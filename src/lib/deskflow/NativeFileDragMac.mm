/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "NativeFileDragPlatform.h"
#include "FileTransferSession.h"
#include "base/Log.h"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>

// This file is compiled without ARC; ownership is managed explicitly.

namespace {
NSInteger s_dragChangeAtLeftDown = -1;

void postTagged(CGEventRef event)
{
  CGEventSetIntegerValueField(event, kCGEventSourceUserData, kNativeFileDragInputTag);
  CGEventPost(kCGSessionEventTap, event);
  CFRelease(event);
}

CGPoint cursorPosition()
{
  CGEventRef event = CGEventCreate(nullptr);
  const auto point = CGEventGetLocation(event);
  CFRelease(event);
  return point;
}

NSString *toNSString(const QString &text)
{
  const auto utf8 = text.toUtf8();
  return [NSString stringWithUTF8String:utf8.constData()];
}

constexpr CGFloat kDragWindowSize = 64;

// When no native drag could be started, still deliver: save into Downloads/Deskflow once the
// files are verified, then show them in Finder.
void deliverToDownloads(std::shared_ptr<FileDragPromise> promise)
{
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
    if (promise->wait(6 * 60 * 60 * 1000) != FileDragPromise::State::Ready) {
      LOG_WARN("edge drag: files not saved: %s", qPrintable(promise->error()));
      return;
    }
    NSString *downloads = NSSearchPathForDirectoriesInDomains(NSDownloadsDirectory, NSUserDomainMask, YES).firstObject;
    const QDir folder(QDir(QString::fromNSString(downloads ?: NSHomeDirectory())).filePath(QStringLiteral("Deskflow")));
    QDir().mkpath(folder.path());
    NSMutableArray<NSURL *> *saved = [NSMutableArray array];
    for (int i = 0; i < promise->names.size(); ++i) {
      const auto target = FileTransferSession::uniquePath(folder, promise->names.at(i));
      if (!target.isEmpty() && QFile::copy(promise->stagedPaths.value(i), target))
        [saved addObject:[NSURL fileURLWithPath:toNSString(target)]];
    }
    LOG_INFO("edge drag: saved %d file(s) to %s", static_cast<int>(saved.count), qPrintable(folder.path()));
    [saved retain];
    dispatch_async(dispatch_get_main_queue(), ^{
      [[NSWorkspace sharedWorkspace] activateFileViewerSelectingURLs:saved];
      [saved release];
    });
  });
}
} // namespace

// Writes one promised file once the transfer has verified it. Runs on its own queue so the
// wait never blocks the main thread, which keeps receiving the file's bytes.
@interface DFPromiseWriter : NSObject <NSFilePromiseProviderDelegate> {
@public
  std::shared_ptr<FileDragPromise> promise;
  NSOperationQueue *queue;
  std::atomic<int> unwritten;
}
- (void)releaseWhenWritten;
@end

@implementation DFPromiseWriter
- (instancetype)init
{
  if ((self = [super init])) {
    queue = [[NSOperationQueue alloc] init];
    queue.qualityOfService = NSQualityOfServiceUserInitiated;
  }
  return self;
}
- (void)releaseWhenWritten
{
  // Balances the +1 held for the drag; the providers only reference this writer weakly.
  if (--unwritten == 0)
    dispatch_async(dispatch_get_main_queue(), ^{
      [self release];
    });
}
- (void)dealloc
{
  [queue release];
  [super dealloc];
}
- (NSString *)filePromiseProvider:(NSFilePromiseProvider *)provider fileNameForType:(NSString *)fileType
{
  const auto index = [(NSNumber *)provider.userInfo integerValue];
  return toNSString(promise->names.value(static_cast<int>(index)));
}
- (NSOperationQueue *)operationQueueForFilePromiseProvider:(NSFilePromiseProvider *)provider
{
  return queue;
}
- (void)filePromiseProvider:(NSFilePromiseProvider *)provider
          writePromiseToURL:(NSURL *)url
          completionHandler:(void (^)(NSError *))completionHandler
{
  const auto index = static_cast<int>([(NSNumber *)provider.userInfo integerValue]);
  // Large files over a slow link can take a while; Finder shows its own progress meanwhile.
  const auto state = promise->wait(6 * 60 * 60 * 1000);
  NSError *error = nil;
  if (state == FileDragPromise::State::Ready) {
    NSURL *staged = [NSURL fileURLWithPath:toNSString(promise->stagedPaths.value(index))];
    NSFileManager *files = [NSFileManager defaultManager];
    NSURL *target = url;
    if ([files fileExistsAtPath:target.path]) {
      // Never overwrite: "name (1).ext" next to the requested location.
      const QFileInfo requested(QString::fromNSString(target.path));
      const QString unique = [&] {
        const auto suffix = requested.completeSuffix();
        const auto base = suffix.isEmpty() ? requested.fileName()
                                           : requested.fileName().left(requested.fileName().size() - suffix.size() - 1);
        for (int i = 1; i < 10000; ++i) {
          const auto name = suffix.isEmpty() ? QStringLiteral("%1 (%2)").arg(base).arg(i)
                                             : QStringLiteral("%1 (%2).%3").arg(base).arg(i).arg(suffix);
          const auto path = requested.dir().filePath(name);
          if (!QFileInfo::exists(path))
            return path;
        }
        return QString();
      }();
      target = unique.isEmpty() ? nil : [NSURL fileURLWithPath:toNSString(unique)];
    }
    if (target == nil || ![files copyItemAtURL:staged toURL:target error:&error]) {
      if (error == nil)
        error = [NSError errorWithDomain:NSCocoaErrorDomain code:NSFileWriteFileExistsError userInfo:nil];
    }
  } else {
    const auto reason = state == FileDragPromise::State::Failed ? promise->error()
                                                                : QStringLiteral("The transfer timed out.");
    error = [NSError errorWithDomain:NSCocoaErrorDomain
                                code:NSFileWriteUnknownError
                            userInfo:@{NSLocalizedDescriptionKey : toNSString(reason)}];
  }
  if (error == nil)
    LOG_INFO("edge drag: wrote dropped file to %s", url.URLByDeletingLastPathComponent.path.UTF8String);
  else
    LOG_WARN("edge drag: could not write dropped file: %s", error.localizedDescription.UTF8String);
  completionHandler(error);
  [self releaseWhenWritten];
}
@end

// A tiny window under the cursor. The drag must begin while AppKit handles a real mouse event
// in this window, so it starts from mouseDown:/mouseDragged: rather than from a fabricated event.
@interface DFNativeDragSource : NSView <NSDraggingSource> {
@public
  std::function<void(bool)> completion;
  NSArray<NSDraggingItem *> *items;
  std::shared_ptr<FileDragPromise> promise;
  BOOL started;
}
- (void)beginWithEvent:(NSEvent *)event reason:(const char *)reason;
- (void)finishWithOperation:(NSDragOperation)operation;
@end

@implementation DFNativeDragSource
- (void)dealloc
{
  [items release];
  [super dealloc];
}
- (BOOL)acceptsFirstMouse:(NSEvent *)event
{
  return YES;
}
- (void)beginWithEvent:(NSEvent *)event reason:(const char *)reason
{
  if (started)
    return;
  started = YES;
  NSDraggingSession *session = [self beginDraggingSessionWithItems:items event:event source:self];
  LOG_INFO("edge drag: macOS drag session %s (%s)", session ? "started" : "failed to start", reason);
  if (session == nil) {
    [self finishWithOperation:NSDragOperationNone];
    return;
  }
  session.animatesToStartingPositionsOnCancelOrFail = NO;
  session.draggingFormation = NSDraggingFormationPile;
}
- (void)mouseDown:(NSEvent *)event
{
  [self beginWithEvent:event reason:"mouse down"];
}
- (void)mouseDragged:(NSEvent *)event
{
  [self beginWithEvent:event reason:"mouse dragged"];
}
- (NSDragOperation)draggingSession:(NSDraggingSession *)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
  return context == NSDraggingContextOutsideApplication ? NSDragOperationCopy : NSDragOperationNone;
}
- (BOOL)ignoreModifierKeysForDraggingSession:(NSDraggingSession *)session
{
  return YES;
}
- (void)draggingSession:(NSDraggingSession *)session endedAtPoint:(NSPoint)point operation:(NSDragOperation)operation
{
  [self finishWithOperation:operation];
}
- (void)finishWithOperation:(NSDragOperation)operation
{
  if (!completion)
    return;
  LOG_INFO("edge drag: macOS drag ended, operation=%lu", static_cast<unsigned long>(operation));
  auto done = std::move(completion);
  NSWindow *window = [self window];
  [window orderOut:nil];
  [window close];
  if (done)
    done(operation != NSDragOperationNone);
}
@end

bool NativeFileDragPlatform::leftButtonDown()
{
  return CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft);
}

void NativeFileDragPlatform::noteLeftDown()
{
  // Finder writes the drag pasteboard when a drag begins, i.e. after this button press.
  s_dragChangeAtLeftDown = [[NSPasteboard pasteboardWithName:NSPasteboardNameDrag] changeCount];
}

void NativeFileDragPlatform::probe(std::function<void(const QStringList &)> done)
{
  QStringList paths;
  NSPasteboard *pasteboard = [NSPasteboard pasteboardWithName:NSPasteboardNameDrag];
  // An unchanged drag pasteboard holds an earlier drag: the held button is not dragging files.
  if (s_dragChangeAtLeftDown >= 0 && pasteboard.changeCount != s_dragChangeAtLeftDown && leftButtonDown()) {
    NSArray *urls = [pasteboard readObjectsForClasses:@[ [NSURL class] ]
                                              options:@{NSPasteboardURLReadingFileURLsOnlyKey : @YES}];
    for (NSURL *url in urls)
      paths.append(QString::fromNSString(url.path));
  }
  done(paths);
}

void NativeFileDragPlatform::releaseSourceDrag()
{
  // Escape cancels the Finder drag (the icon slides back; the files stay put). The tagged
  // button release resets the source app; the physical button stays held for the target.
  postTagged(CGEventCreateKeyboardEvent(nullptr, 53, true));
  postTagged(CGEventCreateKeyboardEvent(nullptr, 53, false));
  postTagged(CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseUp, cursorPosition(), kCGMouseButtonLeft));
}

void NativeFileDragPlatform::startPromiseDrag(
    std::shared_ptr<FileDragPromise> promise, std::function<void(bool)> finished
)
{
  // deskflow-core starts as a background-only process without a window-server connection;
  // such a process cannot own a window that receives clicks, so no drag could ever start.
  // An accessory app has no Dock icon or menu bar but can drag like any other app.
  NSApplication *app = [NSApplication sharedApplication];
  if (app.activationPolicy != NSApplicationActivationPolicyAccessory &&
      app.activationPolicy != NSApplicationActivationPolicyRegular) {
    const bool changed = [app setActivationPolicy:NSApplicationActivationPolicyAccessory];
    LOG_INFO("edge drag: switched to accessory app for dragging: %s", changed ? "ok" : "failed");
  }
  const NSPoint location = [NSEvent mouseLocation];
  NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(location.x - kDragWindowSize / 2,
                                                                      location.y - kDragWindowSize / 2,
                                                                      kDragWindowSize, kDragWindowSize)
                                                 styleMask:NSWindowStyleMaskBorderless
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
  [window setReleasedWhenClosed:YES];
  [window setOpaque:NO];
  [window setBackgroundColor:[NSColor colorWithCalibratedWhite:0 alpha:0.01]];
  [window setLevel:NSPopUpMenuWindowLevel];
  [window setIgnoresMouseEvents:NO];
  DFNativeDragSource *source = [[DFNativeDragSource alloc] initWithFrame:NSMakeRect(0, 0, kDragWindowSize, kDragWindowSize)];
  source->promise = promise;
  DFPromiseWriter *writer = [[DFPromiseWriter alloc] init];
  writer->promise = promise;
  // One count per promised file plus one for the drag itself; the writer lives until all
  // promised files were written, or until the drag ends without a drop.
  writer->unwritten = static_cast<int>(promise->names.size()) + 1;
  source->completion = [finished = std::move(finished), writer](bool dropped) {
    finished(dropped);
    if (!dropped)
      writer->unwritten = 1;
    [writer releaseWhenWritten];
  };
  NSMutableArray<NSDraggingItem *> *items = [NSMutableArray array];
  for (int i = 0; i < promise->names.size(); ++i) {
    NSFilePromiseProvider *provider = [[[NSFilePromiseProvider alloc] initWithFileType:@"public.data"
                                                                              delegate:writer] autorelease];
    provider.userInfo = @(i);
    NSDraggingItem *item = [[[NSDraggingItem alloc] initWithPasteboardWriter:provider] autorelease];
    // The staged placeholder carries the real extension, so its icon matches the file type.
    NSImage *icon = [[NSWorkspace sharedWorkspace] iconForFile:toNSString(promise->stagedPaths.value(i))];
    [item setDraggingFrame:NSMakeRect(i * 4, -i * 4, 32, 32) contents:icon];
    [items addObject:item];
  }
  source->items = [items copy];
  [window setContentView:source];
  // Keep the drag window under the moving cursor and press on it there. The button is
  // physically held on this Mac; the tagged press lands on our window and AppKit hands it to
  // mouseDown:, which begins the drag. The cursor keeps moving while the window appears, so
  // re-centre the window right before each press.
  auto centreUnderCursor = ^{
    const NSPoint p = [NSEvent mouseLocation];
    [window setFrameOrigin:NSMakePoint(p.x - kDragWindowSize / 2, p.y - kDragWindowSize / 2)];
    [window orderFrontRegardless];
  };
  centreUnderCursor();
  const int delays[] = {40, 140, 280};
  for (int attempt = 0; attempt < 3; ++attempt) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, delays[attempt] * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
      if (source->started)
        return;
      centreUnderCursor();
      dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        if (source->started)
          return;
        const CGPoint at = cursorPosition();
        const NSRect frame = window.frame;
        const CGFloat screenTop = NSMaxY([NSScreen screens].firstObject.frame);
        const NSPoint cocoa = NSMakePoint(at.x, screenTop - at.y);
        if (!NSPointInRect(cocoa, frame)) {
          LOG_INFO("edge drag: cursor left the drag window before press %d, retrying", attempt + 1);
          return;
        }
        LOG_INFO("edge drag: pressing on the macOS drag window (attempt %d)", attempt + 1);
        postTagged(CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseDown, at, kCGMouseButtonLeft));
      });
    });
  }
  // No press landed: never start an untracked session (its image would stick on screen).
  // Save the files to Downloads instead so the user still gets them.
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 600 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    if (source->started)
      return;
    source->started = YES;
    LOG_WARN("edge drag: could not start a macOS drag; saving the files to Downloads instead");
    deliverToDownloads(source->promise);
    [source finishWithOperation:NSDragOperationCopy];
  });
  [source release];
}
