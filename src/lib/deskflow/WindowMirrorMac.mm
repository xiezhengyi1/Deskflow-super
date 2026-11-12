/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "WindowMirrorPlatform.h"
#include "WindowMirror.h"
#include "NativeFileDragPlatform.h"
#include "WindowMirrorRouting.h"
#include "base/Log.h"
#include <QTimer>
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <unistd.h>

// This file is compiled without ARC; ownership is managed explicitly.

namespace {
struct Pressed
{
  bool valid = false;
  CGWindowID windowId = 0;
  pid_t pid = 0;
  CGRect bounds = CGRectZero;
  CGPoint cursor = CGPointZero;
  bool grab = false;                  // Control+Option grab: Deskflow moves the window
  AXUIElementRef element = nullptr;   // retained, for moving a grabbed window
  CGPoint movedTo = CGPointZero;
};
Pressed s_pressed;
QTimer *s_follow = nullptr;

void clearPressed()
{
  if (s_follow)
    s_follow->stop();
  if (s_pressed.element)
    CFRelease(s_pressed.element);
  s_pressed = {};
}

struct Owned
{
  AXUIElementRef element = nullptr; // retained
  pid_t pid = 0;
};
std::map<CGWindowID, Owned> s_owned;

CGPoint cursorNow()
{
  CGEventRef event = CGEventCreate(nullptr);
  const CGPoint point = CGEventGetLocation(event);
  CFRelease(event);
  return point;
}

NSString *toNSString(const QString &text)
{
  return [NSString stringWithUTF8String:text.toUtf8().constData()];
}

QString fromNSString(NSString *text)
{
  return text ? QString::fromUtf8(text.UTF8String) : QString();
}

CGRect boundsOf(NSDictionary *info)
{
  CGRect bounds = CGRectZero;
  CGRectMakeWithDictionaryRepresentation((CFDictionaryRef)info[(id)kCGWindowBounds], &bounds);
  return bounds;
}

NSDictionary *windowInfo(CGWindowID windowId)
{
  CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionIncludingWindow, windowId);
  if (!list)
    return nil;
  NSArray *windows = (NSArray *)list;
  NSDictionary *info = windows.count ? [[windows.firstObject retain] autorelease] : nil;
  CFRelease(list);
  return info;
}

// Height of the menu bar on the main screen, in points.
CGFloat menuBarHeight()
{
  NSScreen *screen = [NSScreen screens].firstObject;
  return screen ? NSMaxY(screen.frame) - NSMaxY(screen.visibleFrame) : 25;
}

bool axPoint(AXUIElementRef element, CFStringRef attribute, CGPoint &point)
{
  CFTypeRef value = nullptr;
  if (AXUIElementCopyAttributeValue(element, attribute, &value) != kAXErrorSuccess || !value)
    return false;
  const bool ok = AXValueGetValue((AXValueRef)value, (AXValueType)kAXValueCGPointType, &point);
  CFRelease(value);
  return ok;
}

bool axSize(AXUIElementRef element, CGSize &size)
{
  CFTypeRef value = nullptr;
  if (AXUIElementCopyAttributeValue(element, kAXSizeAttribute, &value) != kAXErrorSuccess || !value)
    return false;
  const bool ok = AXValueGetValue((AXValueRef)value, (AXValueType)kAXValueCGSizeType, &size);
  CFRelease(value);
  return ok;
}

void setAxPosition(AXUIElementRef element, CGPoint point)
{
  AXValueRef value = AXValueCreate((AXValueType)kAXValueCGPointType, &point);
  AXUIElementSetAttributeValue(element, kAXPositionAttribute, value);
  CFRelease(value);
}

// The Accessibility element of the window with these CoreGraphics bounds (retained, or null).
AXUIElementRef findAxWindow(pid_t pid, CGRect bounds, NSString *title)
{
  AXUIElementRef app = AXUIElementCreateApplication(pid);
  CFTypeRef windows = nullptr;
  AXUIElementRef found = nullptr;
  if (AXUIElementCopyAttributeValue(app, kAXWindowsAttribute, &windows) == kAXErrorSuccess && windows) {
    for (id candidate in (NSArray *)windows) {
      AXUIElementRef element = (AXUIElementRef)candidate;
      CGPoint position;
      CGSize size;
      if (axPoint(element, kAXPositionAttribute, position) && axSize(element, size) &&
          std::fabs(position.x - bounds.origin.x) < 4 && std::fabs(position.y - bounds.origin.y) < 4 &&
          std::fabs(size.width - bounds.size.width) < 4 && std::fabs(size.height - bounds.size.height) < 4) {
        found = (AXUIElementRef)CFRetain(element);
        break;
      }
    }
    if (!found && title.length) {
      for (id candidate in (NSArray *)windows) {
        CFTypeRef value = nullptr;
        if (AXUIElementCopyAttributeValue((AXUIElementRef)candidate, kAXTitleAttribute, &value) == kAXErrorSuccess &&
            value) {
          const bool match = [(NSString *)value isEqualToString:title];
          CFRelease(value);
          if (match) {
            found = (AXUIElementRef)CFRetain((AXUIElementRef)candidate);
            break;
          }
        }
      }
    }
    CFRelease(windows);
  }
  CFRelease(app);
  return found;
}

// The usable area (below the menu bar, beside the Dock) of the screen containing point, in
// global top-left coordinates; scale receives that screen's pixels per point.
CGRect visibleAreaAt(CGPoint point, CGFloat *scale = nullptr)
{
  NSArray<NSScreen *> *screens = [NSScreen screens];
  if (!screens.count) {
    if (scale)
      *scale = 2;
    return CGDisplayBounds(CGMainDisplayID());
  }
  const CGFloat primaryHeight = NSHeight(screens.firstObject.frame);
  NSScreen *chosen = screens.firstObject;
  for (NSScreen *screen in screens) {
    const NSRect f = screen.frame;
    if (CGRectContainsPoint(CGRectMake(f.origin.x, primaryHeight - NSMaxY(f), f.size.width, f.size.height), point)) {
      chosen = screen;
      break;
    }
  }
  if (scale)
    *scale = chosen.backingScaleFactor;
  const NSRect v = chosen.visibleFrame;
  return CGRectMake(v.origin.x, primaryHeight - NSMaxY(v), v.size.width, v.size.height);
}

// The spot nearest to wanted where a window of this size is completely on screen.
CGPoint onScreenOrigin(CGPoint wanted, CGSize size)
{
  const CGRect area = visibleAreaAt(CGPointMake(wanted.x + size.width / 2, wanted.y + size.height / 2));
  CGPoint origin = wanted;
  origin.x = std::max(CGRectGetMinX(area), std::min(origin.x, CGRectGetMaxX(area) - size.width));
  origin.y = std::max(CGRectGetMinY(area), std::min(origin.y, CGRectGetMaxY(area) - size.height));
  return origin;
}

// Whether a click at this point reaches the window: no other app's window is in front there.
bool isOnTopAt(CGWindowID windowId, CGPoint point)
{
  CFArrayRef list =
      CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
  if (!list)
    return true;
  const auto owner = s_owned.find(windowId);
  bool onTop = false;
  for (NSDictionary *info in (NSArray *)list) {
    if ([info[(id)kCGWindowLayer] intValue] != 0 || [info[(id)kCGWindowAlpha] doubleValue] <= 0)
      continue;
    if (!CGRectContainsPoint(boundsOf(info), point))
      continue;
    // A sheet or panel of the same app in front of the window is part of it.
    onTop = [info[(id)kCGWindowNumber] unsignedIntValue] == windowId ||
            (owner != s_owned.end() && [info[(id)kCGWindowOwnerPID] intValue] == owner->second.pid);
    break;
  }
  CFRelease(list);
  return onTop;
}

// Input for mirrored windows is real input, delivered by the window server to whatever is at
// that place on screen. Tagged, so Deskflow's own event tap leaves it alone.
CGPoint s_lastPosted = CGPointZero;
std::deque<std::function<void()>> s_pendingInput; // waiting for the window to come to the front
bool s_raising = false;

void postReal(CGEventRef event)
{
  CGEventSetFlags(event, CGEventSourceFlagsState(kCGEventSourceStateHIDSystemState));
  CGEventSetIntegerValueField(event, kCGEventSourceUserData, kNativeFileDragInputTag);
  CGEventPost(kCGSessionEventTap, event);
  CFRelease(event);
}

void postMouse(CGEventType type, CGMouseButton button, CGPoint at, int clicks)
{
  CGEventRef event = CGEventCreateMouseEvent(nullptr, type, at, button);
  if (clicks > 0)
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, clicks);
  CGEventSetDoubleValueField(event, kCGMouseEventDeltaX, at.x - s_lastPosted.x);
  CGEventSetDoubleValueField(event, kCGMouseEventDeltaY, at.y - s_lastPosted.y);
  s_lastPosted = at;
  // Tell Deskflow's event tap where the cursor is being put (see OSXComputer::onMouseMove).
  WindowMirrorRouting::replayX = at.x;
  WindowMirrorRouting::replayY = at.y;
  ++WindowMirrorRouting::replaySeq;
  postReal(event);
}

void raiseWindow(const Owned &owned)
{
  // Through Accessibility this works from a background process, without a click.
  AXUIElementRef app = AXUIElementCreateApplication(owned.pid);
  AXUIElementSetAttributeValue(app, kAXFrontmostAttribute, kCFBooleanTrue);
  CFRelease(app);
  AXUIElementPerformAction(owned.element, kAXRaiseAction);
  AXUIElementSetAttributeValue(owned.element, kAXMainAttribute, kCFBooleanTrue);
}

// Runs input now, or after a raise in progress has taken effect (keeping the order).
void deliverInput(std::function<void()> input)
{
  if (s_raising)
    s_pendingInput.push_back(std::move(input));
  else
    input();
}

void raiseThenDeliver(const Owned &owned)
{
  if (s_raising)
    return;
  s_raising = true;
  raiseWindow(owned);
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    s_raising = false;
    auto pending = std::move(s_pendingInput);
    s_pendingInput.clear();
    for (auto &input : pending)
      input();
  });
}

// Global position of a point given inside the window (points), kept on the displays.
bool globalPoint(CGWindowID windowId, QPointF point, CGPoint &out)
{
  NSDictionary *info = windowInfo(windowId);
  if (!info)
    return false;
  const CGRect bounds = boundsOf(info);
  out = CGPointMake(bounds.origin.x + point.x(), bounds.origin.y + point.y());
  CGDirectDisplayID displays[16];
  uint32_t count = 0;
  CGRect all = CGRectZero;
  if (CGGetActiveDisplayList(16, displays, &count) == kCGErrorSuccess)
    for (uint32_t i = 0; i < count; ++i)
      all = i == 0 ? CGDisplayBounds(displays[i]) : CGRectUnion(all, CGDisplayBounds(displays[i]));
  if (!CGRectIsEmpty(all)) {
    out.x = std::max(CGRectGetMinX(all), std::min(out.x, CGRectGetMaxX(all) - 1));
    out.y = std::max(CGRectGetMinY(all), std::min(out.y, CGRectGetMaxY(all) - 1));
  }
  return true;
}

// The window's size in the pixels it is drawn with, limited for the connection.
QSize nativePixels(CGWindowID windowId)
{
  NSDictionary *info = windowInfo(windowId);
  if (!info)
    return {};
  const CGRect bounds = boundsOf(info);
  CGFloat scale = 2;
  visibleAreaAt(CGPointMake(CGRectGetMidX(bounds), CGRectGetMidY(bounds)), &scale);
  double width = bounds.size.width * scale;
  double height = bounds.size.height * scale;
  constexpr double kMaxSide = 2560;
  if (const double longest = std::max(width, height); longest > kMaxSide) {
    width *= kMaxSide / longest;
    height *= kMaxSide / longest;
  }
  return QSize(std::max(2, static_cast<int>(std::lround(width))), std::max(2, static_cast<int>(std::lround(height))));
}

// Moves a grabbed window with the cursor until the button is released or it is taken over.
void followGrab()
{
  if (!s_pressed.grab || !s_pressed.element || !NativeFileDragPlatform::leftButtonDown()) {
    if (s_follow)
      s_follow->stop();
    return;
  }
  const CGPoint cursor = cursorNow();
  const CGPoint target = CGPointMake(
      s_pressed.bounds.origin.x + cursor.x - s_pressed.cursor.x, s_pressed.bounds.origin.y + cursor.y - s_pressed.cursor.y
  );
  if (std::fabs(target.x - s_pressed.movedTo.x) < 0.5 && std::fabs(target.y - s_pressed.movedTo.y) < 0.5)
    return;
  s_pressed.movedTo = target;
  setAxPosition(s_pressed.element, target);
}
} // namespace

// ---------------------------------------------------------------- capture
@interface DFWindowCapture : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
  WindowMirrorPlatform::FrameCallback onFrame;
  WindowMirrorPlatform::StopCallback onStopped;
  CGWindowID windowId;
  SCStream *stream;
  dispatch_queue_t queue;
  CVPixelBufferRef latest;
  BOOL latestSent;
  BOOL wanted;
  BOOL stopped;
  QSize pixels;
  // The picture the viewer has: only what differs from it is sent.
  uint8_t *shown;
  size_t shownWidth, shownHeight, shownStride;
  // Areas last sent at low quality while they were moving.
  QRect blurred;
  BOOL sharpenArmed;
  BOOL patches;
  CFAbsoluteTime lastSent;
}
- (void)start;
- (void)requestFrame;
- (void)requestFullFrame;
- (void)refreshSize;
- (void)setPatches:(BOOL)enabled;
- (void)shutdown;
@end

@implementation DFWindowCapture
- (instancetype)init
{
  if ((self = [super init])) {
    queue = dispatch_queue_create("deskflow.window-mirror", DISPATCH_QUEUE_SERIAL);
  }
  return self;
}
- (void)dealloc
{
  if (latest)
    CVPixelBufferRelease(latest);
  free(shown);
  [stream release];
  dispatch_release(queue);
  [super dealloc];
}
- (SCStreamConfiguration *)configuration
{
  SCStreamConfiguration *config = [[[SCStreamConfiguration alloc] init] autorelease];
  config.width = static_cast<size_t>(pixels.width());
  config.height = static_cast<size_t>(pixels.height());
  config.scalesToFit = YES;
  config.minimumFrameInterval = CMTimeMake(1, 30);
  config.pixelFormat = kCVPixelFormatType_32BGRA;
  config.showsCursor = NO;
  config.queueDepth = 3;
  config.ignoreShadowsSingleWindow = YES;
  if (@available(macOS 14.0, *))
    config.captureResolution = SCCaptureResolutionBest;
  return config;
}
- (void)fail:(NSString *)reason
{
  dispatch_async(queue, ^{
    if (stopped)
      return;
    stopped = YES;
    auto callback = std::move(onStopped);
    onFrame = nullptr;
    if (callback)
      callback(fromNSString(reason));
  });
}
- (void)start
{
  [self retain];
  const CGWindowID wanted_ = windowId;
  [SCShareableContent
      getShareableContentExcludingDesktopWindows:YES
                             onScreenWindowsOnly:NO
                               completionHandler:^(SCShareableContent *content, NSError *error) {
                                 SCWindow *target = nil;
                                 for (SCWindow *window in content.windows)
                                   if (window.windowID == wanted_)
                                     target = window;
                                 if (!target) {
                                   LOG_WARN("window mirror: cannot capture the window: %s",
                                            error ? error.localizedDescription.UTF8String : "not found");
                                   [self fail:@"the window cannot be captured"];
                                   [self release];
                                   return;
                                 }
                                 SCContentFilter *filter =
                                     [[[SCContentFilter alloc] initWithDesktopIndependentWindow:target] autorelease];
                                 dispatch_async(queue, ^{
                                   if (stopped) {
                                     [self release];
                                     return;
                                   }
                                   stream = [[SCStream alloc] initWithFilter:filter
                                                               configuration:[self configuration]
                                                                    delegate:self];
                                   NSError *addError = nil;
                                   [stream addStreamOutput:self
                                                      type:SCStreamOutputTypeScreen
                                        sampleHandlerQueue:queue
                                                     error:&addError];
                                   const int width = pixels.width(), height = pixels.height();
                                   [stream startCaptureWithCompletionHandler:^(NSError *startError) {
                                     if (startError) {
                                       LOG_WARN("window mirror: capture did not start: %s",
                                                startError.localizedDescription.UTF8String);
                                       [self fail:@"capture did not start"];
                                     } else {
                                       LOG_INFO("window mirror: capturing the window at %dx%d pixels", width, height);
                                     }
                                   }];
                                   [self release];
                                 });
                               }];
}
- (void)stream:(SCStream *)stream didStopWithError:(NSError *)error
{
  LOG_INFO("window mirror: capture stopped: %s", error.localizedDescription.UTF8String);
  [self fail:@"the window was closed"];
}
- (void)stream:(SCStream *)stream didOutputSampleBuffer:(CMSampleBufferRef)sample ofType:(SCStreamOutputType)type
{
  if (type != SCStreamOutputTypeScreen || stopped)
    return;
  CFArrayRef attachments = CMSampleBufferGetSampleAttachmentsArray(sample, false);
  if (!attachments || CFArrayGetCount(attachments) == 0)
    return;
  NSDictionary *info = (NSDictionary *)CFArrayGetValueAtIndex(attachments, 0);
  if ([info[SCStreamFrameInfoStatus] integerValue] != SCFrameStatusComplete)
    return;
  CVPixelBufferRef buffer = CMSampleBufferGetImageBuffer(sample);
  if (!buffer)
    return;
  if (latest)
    CVPixelBufferRelease(latest);
  latest = CVPixelBufferRetain(buffer);
  latestSent = NO;
  if (wanted)
    [self encodeLatest];
}
// Encodes one area of the newest picture and hands it to the viewer.
- (BOOL)sendArea:(QRect)patch quality:(double)jpegQuality began:(CFAbsoluteTime)began
{
  CVPixelBufferLockBaseAddress(latest, kCVPixelBufferLock_ReadOnly);
  const size_t width = CVPixelBufferGetWidth(latest);
  const size_t height = CVPixelBufferGetHeight(latest);
  const size_t stride = CVPixelBufferGetBytesPerRow(latest);
  uint8_t *base = static_cast<uint8_t *>(CVPixelBufferGetBaseAddress(latest));
  patch = patch.intersected(QRect(0, 0, static_cast<int>(width), static_cast<int>(height)));
  NSMutableData *data = [NSMutableData data];
  if (base && !patch.isEmpty()) {
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        base + static_cast<size_t>(patch.y()) * stride + static_cast<size_t>(patch.x()) * 4,
        static_cast<size_t>(patch.width()), static_cast<size_t>(patch.height()), 8, stride, space,
        static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst) | kCGBitmapByteOrder32Little
    );
    CGImageRef image = context ? CGBitmapContextCreateImage(context) : nullptr;
    if (image) {
      // A picture too big for one packet is encoded again, smaller.
      for (const double attempt : {jpegQuality, 0.4, 0.2}) {
        if (attempt > jpegQuality)
          continue;
        data.length = 0;
        CGImageDestinationRef destination =
            CGImageDestinationCreateWithData((CFMutableDataRef)data, CFSTR("public.jpeg"), 1, nullptr);
        NSDictionary *options = @{(id)kCGImageDestinationLossyCompressionQuality : @(attempt)};
        CGImageDestinationAddImage(destination, image, (CFDictionaryRef)options);
        CGImageDestinationFinalize(destination);
        CFRelease(destination);
        if (data.length <= 1500 * 1024)
          break;
      }
      CGImageRelease(image);
    }
    if (context)
      CGContextRelease(context);
    CGColorSpaceRelease(space);
  }
  if (data.length) {
    // Remember what the viewer now shows.
    if (!shown || shownWidth != width || shownHeight != height || shownStride != stride) {
      free(shown);
      shown = static_cast<uint8_t *>(malloc(stride * height));
      shownWidth = width;
      shownHeight = height;
      shownStride = stride;
      if (shown)
        memcpy(shown, base, stride * height);
    } else {
      for (int y = patch.top(); y <= patch.bottom(); ++y)
        memcpy(shown + static_cast<size_t>(y) * stride + static_cast<size_t>(patch.x()) * 4,
               base + static_cast<size_t>(y) * stride + static_cast<size_t>(patch.x()) * 4,
               static_cast<size_t>(patch.width()) * 4);
    }
  }
  CVPixelBufferUnlockBaseAddress(latest, kCVPixelBufferLock_ReadOnly);
  if (!data.length)
    return NO;
  wanted = NO;
  lastSent = CFAbsoluteTimeGetCurrent();
  onFrame(QByteArray(static_cast<const char *>(data.bytes), static_cast<qsizetype>(data.length)), patch,
          QSize(static_cast<int>(width), static_cast<int>(height)), static_cast<int>((lastSent - began) * 1000));
  return YES;
}
- (void)encodeLatest
{
  if (!latest || latestSent || !onFrame)
    return;
  latestSent = YES;
  const CFAbsoluteTime began = CFAbsoluteTimeGetCurrent();
  const int width = static_cast<int>(CVPixelBufferGetWidth(latest));
  const int height = static_cast<int>(CVPixelBufferGetHeight(latest));
  const size_t stride = CVPixelBufferGetBytesPerRow(latest);
  QRect patch(0, 0, width, height);
  bool known = false;
  if (shown && shownWidth == static_cast<size_t>(width) && shownHeight == static_cast<size_t>(height) &&
      shownStride == stride) {
    CVPixelBufferLockBaseAddress(latest, kCVPixelBufferLock_ReadOnly);
    patch = WindowMirror::changedRect(shown, static_cast<const uint8_t *>(CVPixelBufferGetBaseAddress(latest)), width,
                                      height, static_cast<qsizetype>(stride));
    CVPixelBufferUnlockBaseAddress(latest, kCVPixelBufferLock_ReadOnly);
    known = true;
  }
  if (patch.isEmpty())
    return; // nothing changed: stay `wanted` until the window does
  if (!patches) {
    // An older viewer shows every picture as the whole window.
    [self sendArea:QRect(0, 0, width, height) quality:(known ? 0.55 : 0.8) began:began];
    return;
  }
  // A large area changing over and over is motion (scrolling, video): send it small and
  // quickly so the shared connection stays free for the mouse, and sharpen it once it rests.
  const double share = double(patch.width()) * patch.height() / (double(width) * height);
  const bool motion = known && share > 0.3;
  if ([self sendArea:patch quality:(motion ? 0.45 : 0.85) began:began]) {
    if (motion) {
      blurred = blurred.united(patch);
      [self armSharpen];
    }
  }
}
- (void)armSharpen
{
  if (sharpenArmed)
    return;
  sharpenArmed = YES;
  [self retain];
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 300 * NSEC_PER_MSEC), queue, ^{
    sharpenArmed = NO;
    if (!stopped && !blurred.isEmpty() && latest && onFrame) {
      // Only while the viewer waits for a picture and nothing has changed for a moment.
      if (wanted && CFAbsoluteTimeGetCurrent() - lastSent > 0.25) {
        const QRect area = blurred;
        blurred = QRect();
        [self sendArea:area quality:0.85 began:CFAbsoluteTimeGetCurrent()];
      } else {
        [self armSharpen];
      }
    }
    [self release];
  });
}
- (void)requestFrame
{
  dispatch_async(queue, ^{
    wanted = YES;
    [self encodeLatest];
  });
}
- (void)requestFullFrame
{
  dispatch_async(queue, ^{
    free(shown);
    shown = nullptr;
    blurred = QRect();
    wanted = YES;
    latestSent = NO;
    [self encodeLatest];
  });
}
- (void)setPatches:(BOOL)enabled
{
  dispatch_async(queue, ^{
    patches = enabled;
  });
}
- (void)refreshSize
{
  const QSize size = nativePixels(windowId);
  dispatch_async(queue, ^{
    if (size.isEmpty() || size == pixels || !stream)
      return;
    pixels = size;
    LOG_DEBUG("window mirror: capturing at %dx%d pixels now", size.width(), size.height());
    [stream updateConfiguration:[self configuration] completionHandler:nil];
  });
}
- (void)shutdown
{
  dispatch_sync(queue, ^{
    stopped = YES;
    onFrame = nullptr;
    onStopped = nullptr;
    [stream stopCaptureWithCompletionHandler:nil];
  });
}
@end

namespace {
class MacCapture : public WindowMirrorPlatform::Capture
{
public:
  explicit MacCapture(DFWindowCapture *impl) : m_impl(impl)
  {
  }
  ~MacCapture() override
  {
    [m_impl shutdown];
    [m_impl release];
  }
  void requestFrame() override
  {
    [m_impl requestFrame];
  }
  void requestFullFrame() override
  {
    [m_impl requestFullFrame];
  }
  void refreshSize() override
  {
    [m_impl refreshSize];
  }
  void setPatches(bool enabled) override
  {
    [m_impl setPatches:enabled];
  }

private:
  DFWindowCapture *m_impl;
};
} // namespace

// ---------------------------------------------------------------- platform API
void WindowMirrorPlatform::noteLeftDown()
{
  clearPressed();
  const CGPoint cursor = cursorNow();
  CFArrayRef list =
      CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
  if (!list)
    return;
  // Front to back: the first ordinary window containing the cursor is the one clicked.
  for (NSDictionary *info in (NSArray *)list) {
    if ([info[(id)kCGWindowLayer] intValue] != 0 || [info[(id)kCGWindowAlpha] doubleValue] <= 0)
      continue;
    const pid_t pid = [info[(id)kCGWindowOwnerPID] intValue];
    const CGRect bounds = boundsOf(info);
    if (!CGRectContainsPoint(bounds, cursor))
      continue;
    if (pid != getpid())
      s_pressed = {true, (CGWindowID)[info[(id)kCGWindowNumber] unsignedIntValue], pid, bounds, cursor};
    break;
  }
  CFRelease(list);

  if (!s_pressed.valid || !WindowMirrorRouting::grabbing)
    return;
  NSDictionary *info = windowInfo(s_pressed.windowId);
  s_pressed.grab = true;
  s_pressed.movedTo = s_pressed.bounds.origin;
  s_pressed.element = findAxWindow(s_pressed.pid, s_pressed.bounds, info ? (NSString *)info[(id)kCGWindowName] : nil);
  LOG_DEBUG(
      "window mirror: Control+Option grab of window %u (%s)%s", s_pressed.windowId,
      info ? [(NSString *)info[(id)kCGWindowOwnerName] UTF8String] : "?", s_pressed.element ? "" : ", cannot move it"
  );
  if (!s_follow) {
    s_follow = new QTimer;
    s_follow->setInterval(8);
    QObject::connect(s_follow, &QTimer::timeout, [] { followGrab(); });
  }
  s_follow->start();
}

std::optional<WindowMirrorPlatform::DraggedWindow> WindowMirrorPlatform::draggedWindow()
{
  if (!s_pressed.valid || !NativeFileDragPlatform::leftButtonDown())
    return std::nullopt;
  NSDictionary *info = windowInfo(s_pressed.windowId);
  if (!info)
    return std::nullopt;
  const CGRect now = boundsOf(info);
  const CGRect then = s_pressed.bounds;
  if (!s_pressed.grab &&
      (std::fabs(now.size.width - then.size.width) > 1 || std::fabs(now.size.height - then.size.height) > 1))
    return std::nullopt; // resized, not moved
  const double moved = std::hypot(now.origin.x - then.origin.x, now.origin.y - then.origin.y);
  // A window already at the top cannot move further up, so also accept a press on the title
  // bar of a window pinned under the menu bar while the cursor pushed on.
  const CGPoint cursor = cursorNow();
  const bool titleBarPress = s_pressed.cursor.y - then.origin.y < 52;
  const bool pinnedAtTop = now.origin.y <= menuBarHeight() + 2;
  const double pushed = std::hypot(cursor.x - s_pressed.cursor.x, cursor.y - s_pressed.cursor.y);
  if (!s_pressed.grab && moved < 8 && !(titleBarPress && pinnedAtTop && pushed > 20))
    return std::nullopt;
  DraggedWindow window;
  window.windowId = s_pressed.windowId;
  window.pid = s_pressed.pid;
  window.frame = QRectF(now.origin.x, now.origin.y, now.size.width, now.size.height);
  window.grab = QPointF(s_pressed.cursor.x - then.origin.x, s_pressed.cursor.y - then.origin.y);
  window.home = QPointF(then.origin.x, then.origin.y);
  window.title = fromNSString(info[(id)kCGWindowName]);
  window.app = fromNSString(info[(id)kCGWindowOwnerName]);
  return window;
}

void WindowMirrorPlatform::endWindowDrag()
{
  if (s_pressed.grab) {
    // The window server is not dragging anything: just stop moving the window.
    clearPressed();
    return;
  }
  CGEventRef up = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseUp, cursorNow(), kCGMouseButtonLeft);
  CGEventSetIntegerValueField(up, kCGEventSourceUserData, kNativeFileDragInputTag);
  CGEventPost(kCGSessionEventTap, up);
  CFRelease(up);
  clearPressed();
}

bool WindowMirrorPlatform::canCapture()
{
  if (CGPreflightScreenCaptureAccess())
    return true;
  static bool asked = false;
  if (!asked) {
    asked = true;
    CGRequestScreenCaptureAccess();
  }
  return false;
}

bool WindowMirrorPlatform::adopt(const DraggedWindow &window)
{
  const CGRect bounds = CGRectMake(window.frame.x(), window.frame.y(), window.frame.width(), window.frame.height());
  AXUIElementRef element = findAxWindow(static_cast<pid_t>(window.pid), bounds, toNSString(window.title));
  if (!element)
    return false;
  release(window.windowId);
  s_owned[window.windowId] = Owned{element, static_cast<pid_t>(window.pid)};
  // Back to where the drag began, completely on screen, so every part of it can be clicked.
  const CGPoint home = window.home ? CGPointMake(window.home->x(), window.home->y()) : bounds.origin;
  const CGPoint place = onScreenOrigin(home, bounds.size);
  setAxPosition(element, place);
  // The window server may still be finishing the title-bar drag; move it again shortly.
  CFRetain(element);
  const CGWindowID windowId = window.windowId;
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    if (const auto it = s_owned.find(windowId); it != s_owned.end() && it->second.element == element)
      setAxPosition(element, place);
    CFRelease(element);
  });
  return true;
}

void WindowMirrorPlatform::release(quint32 windowId)
{
  const auto it = s_owned.find(windowId);
  if (it == s_owned.end())
    return;
  CFRelease(it->second.element);
  s_owned.erase(it);
}

std::optional<QRectF> WindowMirrorPlatform::frame(quint32 windowId)
{
  if (!s_owned.count(windowId))
    return std::nullopt;
  NSDictionary *info = windowInfo(windowId);
  if (!info)
    return std::nullopt;
  const CGRect bounds = boundsOf(info);
  return QRectF(bounds.origin.x, bounds.origin.y, bounds.size.width, bounds.size.height);
}

void WindowMirrorPlatform::resize(quint32 windowId, QSizeF points)
{
  const auto it = s_owned.find(windowId);
  if (it == s_owned.end())
    return;
  CGSize size = CGSizeMake(points.width(), points.height());
  AXValueRef value = AXValueCreate((AXValueType)kAXValueCGSizeType, &size);
  AXUIElementSetAttributeValue(it->second.element, kAXSizeAttribute, value);
  CFRelease(value);
  // A bigger window may now hang off the screen, where it cannot be clicked.
  CGPoint position;
  CGSize actual;
  if (axPoint(it->second.element, kAXPositionAttribute, position) && axSize(it->second.element, actual)) {
    const CGPoint place = onScreenOrigin(position, actual);
    if (std::fabs(place.x - position.x) > 0.5 || std::fabs(place.y - position.y) > 0.5)
      setAxPosition(it->second.element, place);
  }
}

void WindowMirrorPlatform::raise(quint32 windowId)
{
  const auto it = s_owned.find(windowId);
  if (it == s_owned.end())
    return;
  NSDictionary *info = windowInfo(windowId);
  if (info) {
    const CGRect bounds = boundsOf(info);
    if (isOnTopAt(windowId, CGPointMake(CGRectGetMidX(bounds), CGRectGetMidY(bounds))))
      return;
  }
  raiseThenDeliver(it->second);
}

void WindowMirrorPlatform::activateForKeyboard(quint32 windowId)
{
  const auto it = s_owned.find(windowId);
  if (it == s_owned.end())
    return;
  // Frontmost app and key window, so key events passed through by the event tap reach it.
  raiseWindow(it->second);
  [[NSRunningApplication runningApplicationWithProcessIdentifier:it->second.pid] activateWithOptions:0];
}

void WindowMirrorPlatform::injectMouse(quint32 windowId, MouseAction action, int button, QPointF point, int clicks)
{
  const auto it = s_owned.find(windowId);
  CGPoint at;
  if (it == s_owned.end() || !globalPoint(windowId, point, at))
    return;
  CGMouseButton cgButton = kCGMouseButtonLeft;
  CGEventType type = kCGEventMouseMoved;
  switch (button) {
  case 3:
    cgButton = kCGMouseButtonRight;
    type = action == MouseAction::Down ? kCGEventRightMouseDown
           : action == MouseAction::Up ? kCGEventRightMouseUp
                                       : kCGEventRightMouseDragged;
    break;
  case 2:
    cgButton = kCGMouseButtonCenter;
    type = action == MouseAction::Down ? kCGEventOtherMouseDown
           : action == MouseAction::Up ? kCGEventOtherMouseUp
                                       : kCGEventOtherMouseDragged;
    break;
  default:
    type = action == MouseAction::Down ? kCGEventLeftMouseDown
           : action == MouseAction::Up ? kCGEventLeftMouseUp
                                       : kCGEventLeftMouseDragged;
    break;
  }
  if (action == MouseAction::Move)
    type = kCGEventMouseMoved;
  // A press must land on this window, not on another app's window covering that spot.
  if (action == MouseAction::Down && !isOnTopAt(windowId, at))
    raiseThenDeliver(it->second);
  const int clickState = action == MouseAction::Move ? 0 : std::max(1, clicks);
  deliverInput([type, cgButton, at, clickState, action] {
    if (action == MouseAction::Down)
      postMouse(kCGEventMouseMoved, cgButton, at, 0); // the pointer arrives before it presses
    postMouse(type, cgButton, at, clickState);
  });
}

void WindowMirrorPlatform::injectWheel(quint32 windowId, QPointF point, int xDelta, int yDelta)
{
  CGPoint at;
  if (!s_owned.count(windowId) || !globalPoint(windowId, point, at))
    return;
  deliverInput([at, xDelta, yDelta] {
    // Scrolling goes to the window under the cursor.
    if (std::fabs(at.x - s_lastPosted.x) > 2 || std::fabs(at.y - s_lastPosted.y) > 2)
      postMouse(kCGEventMouseMoved, kCGMouseButtonLeft, at, 0);
    // Deskflow uses 120 per notch; a quarter of that in pixels scrolls like a trackpad.
    CGEventRef event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 2, yDelta / 4, xDelta / 4);
    CGEventSetLocation(event, at);
    postReal(event);
  });
}

std::unique_ptr<WindowMirrorPlatform::Capture>
WindowMirrorPlatform::startCapture(quint32 windowId, FrameCallback onFrame, StopCallback onStopped)
{
  const QSize pixels = nativePixels(windowId);
  if (pixels.isEmpty())
    return nullptr;
  DFWindowCapture *impl = [[DFWindowCapture alloc] init];
  impl->onFrame = std::move(onFrame);
  impl->onStopped = std::move(onStopped);
  impl->windowId = windowId;
  impl->pixels = pixels;
  [impl start];
  return std::make_unique<MacCapture>(impl);
}
