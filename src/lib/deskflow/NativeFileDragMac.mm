/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "NativeFileDragPlatform.h"
#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

@interface DFNativeDragSource : NSView <NSDraggingSource> {
@public
  std::function<void(bool)> completion;
}
@end
@implementation DFNativeDragSource
- (NSDragOperation)draggingSession:(NSDraggingSession *)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
  return NSDragOperationCopy;
}
- (BOOL)ignoreModifierKeysForDraggingSession:(NSDraggingSession *)session
{
  return YES;
}
- (void)draggingSession:(NSDraggingSession *)session endedAtPoint:(NSPoint)point operation:(NSDragOperation)operation
{
  auto done = std::move(completion);
  NSWindow *window = [self window];
  [window orderOut:nil];
  [window close];
  done((operation & NSDragOperationCopy) != 0);
}
@end

static void postTagged(CGEventRef event)
{
  CGEventSetIntegerValueField(event, kCGEventSourceUserData, kNativeFileDragInputTag);
  CGEventPost(kCGSessionEventTap, event);
  CFRelease(event);
}
QPoint NativeFileDragPlatform::cursorPosition()
{
  CGEventRef event = CGEventCreate(nullptr);
  const auto p = CGEventGetLocation(event);
  CFRelease(event);
  return QPoint(static_cast<int>(p.x), static_cast<int>(p.y));
}
bool NativeFileDragPlatform::leftButtonDown()
{
  return CGEventSourceButtonState(kCGEventSourceStateCombinedSessionState, kCGMouseButtonLeft);
}
void NativeFileDragPlatform::cancelSourceDrag()
{
  postTagged(CGEventCreateKeyboardEvent(nullptr, 53, true));
  postTagged(CGEventCreateKeyboardEvent(nullptr, 53, false));
  const auto p = cursorPosition();
  postTagged(CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseUp, CGPointMake(p.x(), p.y()), kCGMouseButtonLeft));
}
void NativeFileDragPlatform::startDrag(const QStringList &paths, std::function<void(bool)> finished)
{
  const NSPoint location = [NSEvent mouseLocation];
  NSWindow *window = [[NSWindow alloc] initWithContentRect:NSMakeRect(location.x, location.y, 1, 1)
                                                 styleMask:NSWindowStyleMaskBorderless
                                                   backing:NSBackingStoreBuffered
                                                     defer:NO];
  [window setReleasedWhenClosed:YES];
  [window setOpaque:NO];
  [window setBackgroundColor:[NSColor clearColor]];
  [window setLevel:NSPopUpMenuWindowLevel];
  DFNativeDragSource *source = [[DFNativeDragSource alloc] initWithFrame:NSMakeRect(0, 0, 1, 1)];
  source->completion = std::move(finished);
  [window setContentView:source];
  [source release];
  [window orderFrontRegardless];
  const auto point = cursorPosition();
  postTagged(
      CGEventCreateMouseEvent(nullptr, kCGEventLeftMouseDown, CGPointMake(point.x(), point.y()), kCGMouseButtonLeft)
  );
  NSMutableArray<NSDraggingItem *> *items = [NSMutableArray array];
  for (const auto &path : paths) {
    const auto utf8 = path.toUtf8();
    NSString *file = [NSString stringWithUTF8String:utf8.constData()];
    NSURL *url = [NSURL fileURLWithPath:file];
    NSDraggingItem *item = [[[NSDraggingItem alloc] initWithPasteboardWriter:url] autorelease];
    NSImage *icon = [[NSWorkspace sharedWorkspace] iconForFile:file];
    [item setDraggingFrame:NSMakeRect(0, 0, 32, 32) contents:icon];
    [items addObject:item];
  }
  NSEvent *event = [NSEvent mouseEventWithType:NSEventTypeLeftMouseDragged
                                      location:NSMakePoint(0, 0)
                                 modifierFlags:0
                                     timestamp:[NSProcessInfo processInfo].systemUptime
                                  windowNumber:window.windowNumber
                                       context:nil
                                   eventNumber:0
                                    clickCount:1
                                      pressure:1.0];
  NSDraggingSession *session = [source beginDraggingSessionWithItems:items event:event source:source];
  session.animatesToStartingPositionsOnCancelOrFail = NO;
}
