/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QByteArray>
#include <QPointF>
#include <QRect>
#include <QRectF>
#include <QSize>
#include <QString>
#include <functional>
#include <memory>
#include <optional>

// The computer that owns the real window (macOS). Implemented in WindowMirrorMac.mm; other
// platforms get no-op stubs. Everything runs on the core process's Qt main thread unless noted.
namespace WindowMirrorPlatform {

struct DraggedWindow
{
  quint32 windowId = 0;
  qint64 pid = 0;
  QRectF frame;   // global, top-left origin, points
  QPointF grab;   // cursor position inside the window when the drag began, points
  std::optional<QPointF> home; // where the window was before the drag; it returns there
  QString title;
  QString app;
};

// Called when the left button goes down: remembers the window under the cursor.
void noteLeftDown();
// The remembered window, if it has been moving by its title bar since the press, or was
// grabbed with Control+Option held (WindowMirrorRouting::grabbing).
std::optional<DraggedWindow> draggedWindow();
// Ends the window server's title-bar drag so the window stops following the cursor.
void endWindowDrag();
// Screen Recording permission; asks the user once when it is missing.
bool canCapture();

// Starts mirroring a window. It stays on this computer's screen, because the system only
// delivers clicks to windows that are really on screen: it is moved back to where its drag
// began, fully inside the screen. False if Accessibility cannot reach the window.
bool adopt(const DraggedWindow &window);
// Stops mirroring; the window stays where it is.
void release(quint32 windowId);
// Where an adopted window is now (global, top-left origin, points), or nothing if it is gone.
std::optional<QRectF> frame(quint32 windowId);
void resize(quint32 windowId, QSizeF points);
// Brings the window to the front so that clicks at its position reach it.
void raise(quint32 windowId);
// Makes the owning app frontmost with this window as its key window, so typing reaches it.
void activateForKeyboard(quint32 windowId);

enum class MouseAction
{
  Move,
  Down,
  Up,
  Drag
};
// Real input at the window's place on screen. point is inside the window, in points (a drag
// may go outside). button uses Deskflow ButtonID values.
void injectMouse(quint32 windowId, MouseAction action, int button, QPointF point, int clicks);
void injectWheel(quint32 windowId, QPointF point, int xDelta, int yDelta);

// Live capture of one window at its native resolution. Callbacks arrive on an arbitrary thread.
class Capture
{
public:
  virtual ~Capture() = default;
  // Encode and deliver what changed since the last delivered frame (now if something is
  // waiting, else as soon as the window changes).
  virtual void requestFrame() = 0;
  // The next frame is a complete picture.
  virtual void requestFullFrame() = 0;
  // The window changed size: capture at the new native size.
  virtual void refreshSize() = 0;
  // Whether frames may carry only the changed area. Off (the default), every frame is the
  // complete picture, which is all an older viewer can show.
  virtual void setPatches(bool enabled) = 0;
};
// jpeg holds the area `patch` of a picture `full` pixels large.
using FrameCallback = std::function<void(const QByteArray &jpeg, QRect patch, QSize full, int encodeMs)>;
using StopCallback = std::function<void(const QString &reason)>;
std::unique_ptr<Capture> startCapture(quint32 windowId, FrameCallback onFrame, StopCallback onStopped);

} // namespace WindowMirrorPlatform
