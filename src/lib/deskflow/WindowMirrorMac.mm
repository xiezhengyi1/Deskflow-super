/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "WindowMirrorPlatform.h"
#include "NativeFileDragPlatform.h"
#include "base/Log.h"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <ImageIO/ImageIO.h>
#import <ScreenCaptureKit/ScreenCaptureKit.h>
#include <algorithm>
#include <cmath>
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
};
Pressed s_pressed;

struct Hidden
{
  AXUIElementRef element = nullptr; // retained
  pid_t pid = 0;
  CGPoint original = CGPointZero;
  CGPoint hiddenAt = CGPointZero;
};
std::map<CGWindowID, Hidden> s_hidden;

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

// Bottom-right corner of all displays: macOS keeps a sliver of the window there, out of sight.
CGPoint hiddenOrigin()
{
  CGDirectDisplayID displays[16];
  uint32_t count = 0;
  CGRect all = CGRectZero;
  if (CGGetActiveDisplayList(16, displays, &count) == kCGErrorSuccess)
    for (uint32_t i = 0; i < count; ++i)
      all = i == 0 ? CGDisplayBounds(displays[i]) : CGRectUnion(all, CGDisplayBounds(displays[i]));
  return CGPointMake(CGRectGetMaxX(all) - 1, CGRectGetMaxY(all) - 1);
}

// Where the window is now (its hidden spot if hidden), global top-left points.
CGPoint windowOrigin(CGWindowID windowId)
{
  if (const auto it = s_hidden.find(windowId); it != s_hidden.end())
    return it->second.hiddenAt;
  if (NSDictionary *info = windowInfo(windowId))
    return boundsOf(info).origin;
  return CGPointZero;
}

pid_t windowPid(CGWindowID windowId)
{
  if (const auto it = s_hidden.find(windowId); it != s_hidden.end())
    return it->second.pid;
  if (NSDictionary *info = windowInfo(windowId))
    return [info[(id)kCGWindowOwnerPID] intValue];
  return 0;
}

void postToWindow(CGWindowID windowId, CGEventRef event)
{
  // Address the event to this window explicitly: it is not under the real cursor.
  CGEventSetIntegerValueField(event, kCGMouseEventWindowUnderMousePointer, windowId);
  CGEventSetIntegerValueField(event, kCGMouseEventWindowUnderMousePointerThatCanHandleThisEvent, windowId);
  CGEventSetIntegerValueField(event, kCGEventSourceUserData, kNativeFileDragInputTag);
  if (const pid_t pid = windowPid(windowId))
    CGEventPostToPid(pid, event);
  CFRelease(event);
}
} // namespace

// ---------------------------------------------------------------- capture
@interface DFWindowCapture : NSObject <SCStreamOutput, SCStreamDelegate> {
@public
  WindowMirrorPlatform::FrameCallback onFrame;
  WindowMirrorPlatform::StopCallback onStopped;
  SCStream *stream;
  dispatch_queue_t queue;
  CVPixelBufferRef latest;
  BOOL latestSent;
  BOOL wanted;
  BOOL stopped;
  double quality;
  QSize pixels;
}
- (void)startWithWindow:(CGWindowID)windowId;
- (void)requestFrame;
- (void)setOutputSize:(QSize)size;
- (void)shutdown;
@end

@implementation DFWindowCapture
- (instancetype)init
{
  if ((self = [super init])) {
    queue = dispatch_queue_create("deskflow.window-mirror", DISPATCH_QUEUE_SERIAL);
    quality = 0.6;
  }
  return self;
}
- (void)dealloc
{
  if (latest)
    CVPixelBufferRelease(latest);
  [stream release];
  dispatch_release(queue);
  [super dealloc];
}
- (SCStreamConfiguration *)configuration
{
  SCStreamConfiguration *config = [[[SCStreamConfiguration alloc] init] autorelease];
  config.width = static_cast<size_t>(pixels.width());
  config.height = static_cast<size_t>(pixels.height());
  config.minimumFrameInterval = CMTimeMake(1, 30);
  config.pixelFormat = kCVPixelFormatType_32BGRA;
  config.showsCursor = NO;
  config.queueDepth = 3;
  config.ignoreShadowsSingleWindow = YES;
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
- (void)startWithWindow:(CGWindowID)windowId
{
  [self retain];
  [SCShareableContent
      getShareableContentExcludingDesktopWindows:YES
                             onScreenWindowsOnly:NO
                               completionHandler:^(SCShareableContent *content, NSError *error) {
                                 SCWindow *target = nil;
                                 for (SCWindow *window in content.windows)
                                   if (window.windowID == windowId)
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
                                   [stream startCaptureWithCompletionHandler:^(NSError *startError) {
                                     if (startError) {
                                       LOG_WARN("window mirror: capture did not start: %s",
                                                startError.localizedDescription.UTF8String);
                                       [self fail:@"capture did not start"];
                                     } else {
                                       LOG_INFO("window mirror: capturing the window");
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
- (void)encodeLatest
{
  if (!latest || latestSent || !onFrame)
    return;
  wanted = NO;
  latestSent = YES;
  CVPixelBufferLockBaseAddress(latest, kCVPixelBufferLock_ReadOnly);
  const size_t width = CVPixelBufferGetWidth(latest);
  const size_t height = CVPixelBufferGetHeight(latest);
  CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
  CGContextRef context = CGBitmapContextCreate(
      CVPixelBufferGetBaseAddress(latest), width, height, 8, CVPixelBufferGetBytesPerRow(latest), space,
      static_cast<CGBitmapInfo>(kCGImageAlphaNoneSkipFirst) | kCGBitmapByteOrder32Little
  );
  CGImageRef image = context ? CGBitmapContextCreateImage(context) : nullptr;
  CVPixelBufferUnlockBaseAddress(latest, kCVPixelBufferLock_ReadOnly);
  NSMutableData *data = [NSMutableData data];
  if (image) {
    CGImageDestinationRef destination =
        CGImageDestinationCreateWithData((CFMutableDataRef)data, CFSTR("public.jpeg"), 1, nullptr);
    NSDictionary *options = @{(id)kCGImageDestinationLossyCompressionQuality : @(quality)};
    CGImageDestinationAddImage(destination, image, (CFDictionaryRef)options);
    CGImageDestinationFinalize(destination);
    CFRelease(destination);
    CGImageRelease(image);
  }
  if (context)
    CGContextRelease(context);
  CGColorSpaceRelease(space);
  // Keep frames small enough for the shared connection.
  if (data.length > 900 * 1024)
    quality = std::max(0.3, quality - 0.1);
  else if (data.length < 250 * 1024)
    quality = std::min(0.75, quality + 0.05);
  if (data.length)
    onFrame(QByteArray(static_cast<const char *>(data.bytes), static_cast<qsizetype>(data.length)),
            QSize(static_cast<int>(width), static_cast<int>(height)));
}
- (void)requestFrame
{
  dispatch_async(queue, ^{
    wanted = YES;
    [self encodeLatest];
  });
}
- (void)setOutputSize:(QSize)size
{
  dispatch_async(queue, ^{
    if (size == pixels)
      return;
    pixels = size;
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
  void setOutputSize(QSize pixels) override
  {
    [m_impl setOutputSize:pixels];
  }

private:
  DFWindowCapture *m_impl;
};
} // namespace

// ---------------------------------------------------------------- platform API
void WindowMirrorPlatform::noteLeftDown()
{
  s_pressed = {};
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
  if (std::fabs(now.size.width - then.size.width) > 1 || std::fabs(now.size.height - then.size.height) > 1)
    return std::nullopt; // resized, not moved
  const double moved = std::hypot(now.origin.x - then.origin.x, now.origin.y - then.origin.y);
  // A window already at the top cannot move further up, so also accept a press on the title
  // bar of a window pinned under the menu bar while the cursor pushed on.
  const CGPoint cursor = cursorNow();
  const bool titleBarPress = s_pressed.cursor.y - then.origin.y < 52;
  const bool pinnedAtTop = now.origin.y <= menuBarHeight() + 2;
  const double pushed = std::hypot(cursor.x - s_pressed.cursor.x, cursor.y - s_pressed.cursor.y);
  if (moved < 8 && !(titleBarPress && pinnedAtTop && pushed > 20))
    return std::nullopt;
  DraggedWindow window;
  window.windowId = s_pressed.windowId;
  window.pid = s_pressed.pid;
  window.frame = QRectF(now.origin.x, now.origin.y, now.size.width, now.size.height);
  window.grab = QPointF(s_pressed.cursor.x - then.origin.x, s_pressed.cursor.y - then.origin.y);
  window.title = fromNSString(info[(id)kCGWindowName]);
  window.app = fromNSString(info[(id)kCGWindowOwnerName]);
  return window;
}

void WindowMirrorPlatform::endWindowDrag()
{
  CGEventRef up = CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseUp, cursorNow(), kCGMouseButtonLeft);
  CGEventSetIntegerValueField(up, kCGEventSourceUserData, kNativeFileDragInputTag);
  CGEventPost(kCGSessionEventTap, up);
  CFRelease(up);
  s_pressed = {};
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

bool WindowMirrorPlatform::hide(const DraggedWindow &window)
{
  const CGRect bounds = CGRectMake(window.frame.x(), window.frame.y(), window.frame.width(), window.frame.height());
  AXUIElementRef element = findAxWindow(static_cast<pid_t>(window.pid), bounds, toNSString(window.title));
  if (!element)
    return false;
  Hidden hidden;
  hidden.element = element;
  hidden.pid = static_cast<pid_t>(window.pid);
  hidden.original = bounds.origin;
  hidden.hiddenAt = hiddenOrigin();
  s_hidden[window.windowId] = hidden;
  setAxPosition(element, hidden.hiddenAt);
  // The window server may still be finishing the title-bar drag; move it again shortly.
  CFRetain(element);
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 120 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    if (const auto it = s_hidden.find(window.windowId); it != s_hidden.end() && it->second.element == element) {
      setAxPosition(element, it->second.hiddenAt);
      CGPoint actual;
      if (axPoint(element, kAXPositionAttribute, actual))
        it->second.hiddenAt = actual;
    }
    CFRelease(element);
  });
  return true;
}

void WindowMirrorPlatform::restore(quint32 windowId)
{
  const auto it = s_hidden.find(windowId);
  if (it == s_hidden.end())
    return;
  setAxPosition(it->second.element, it->second.original);
  CFRelease(it->second.element);
  s_hidden.erase(it);
}

void WindowMirrorPlatform::resize(quint32 windowId, QSizeF points)
{
  const auto it = s_hidden.find(windowId);
  if (it == s_hidden.end())
    return;
  CGSize size = CGSizeMake(points.width(), points.height());
  AXValueRef value = AXValueCreate((AXValueType)kAXValueCGSizeType, &size);
  AXUIElementSetAttributeValue(it->second.element, kAXSizeAttribute, value);
  CFRelease(value);
}

void WindowMirrorPlatform::activateForKeyboard(quint32 windowId)
{
  const auto it = s_hidden.find(windowId);
  if (it == s_hidden.end())
    return;
  // Make the app frontmost through Accessibility (works from a background process) and its
  // window the key window, so key events passed through by the event tap reach it.
  AXUIElementRef app = AXUIElementCreateApplication(it->second.pid);
  AXUIElementSetAttributeValue(app, kAXFrontmostAttribute, kCFBooleanTrue);
  CFRelease(app);
  AXUIElementPerformAction(it->second.element, kAXRaiseAction);
  AXUIElementSetAttributeValue(it->second.element, kAXMainAttribute, kCFBooleanTrue);
  [[NSRunningApplication runningApplicationWithProcessIdentifier:it->second.pid] activateWithOptions:0];
}

void WindowMirrorPlatform::injectMouse(quint32 windowId, MouseAction action, int button, QPointF point, int clicks)
{
  const CGPoint origin = windowOrigin(windowId);
  const CGPoint at = CGPointMake(origin.x + point.x(), origin.y + point.y());
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
  CGEventRef event = CGEventCreateMouseEvent(nullptr, type, at, cgButton);
  if (action == MouseAction::Down || action == MouseAction::Up)
    CGEventSetIntegerValueField(event, kCGMouseEventClickState, std::max(1, clicks));
  postToWindow(windowId, event);
}

void WindowMirrorPlatform::injectWheel(quint32 windowId, QPointF point, int xDelta, int yDelta)
{
  const CGPoint origin = windowOrigin(windowId);
  // Deskflow uses 120 per notch; a quarter of that in pixels scrolls like a trackpad.
  CGEventRef event = CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitPixel, 2, yDelta / 4, xDelta / 4);
  CGEventSetLocation(event, CGPointMake(origin.x + point.x(), origin.y + point.y()));
  postToWindow(windowId, event);
}

std::unique_ptr<WindowMirrorPlatform::Capture>
WindowMirrorPlatform::startCapture(quint32 windowId, QSize pixels, FrameCallback onFrame, StopCallback onStopped)
{
  DFWindowCapture *impl = [[DFWindowCapture alloc] init];
  impl->onFrame = std::move(onFrame);
  impl->onStopped = std::move(onStopped);
  impl->pixels = pixels;
  [impl startWithWindow:windowId];
  return std::make_unique<MacCapture>(impl);
}
