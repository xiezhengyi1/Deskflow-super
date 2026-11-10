/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QByteArray>
#include <QPointF>
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
  QString title;
  QString app;
};

// Called when the left button goes down: remembers the window under the cursor.
void noteLeftDown();
// The remembered window, if it has been moving by its title bar since the press.
std::optional<DraggedWindow> draggedWindow();
// Ends the window server's title-bar drag so the window stops following the cursor.
void endWindowDrag();
// Screen Recording permission; asks the user once when it is missing.
bool canCapture();

// Moves the window out of sight, remembering where it was. False if it could not be found.
bool hide(const DraggedWindow &window);
// Puts a hidden window back where it was.
void restore(quint32 windowId);
void resize(quint32 windowId, QSizeF points);
// Makes the owning app frontmost with this window as its key window, so typing reaches it.
void activateForKeyboard(quint32 windowId);

enum class MouseAction
{
  Move,
  Down,
  Up,
  Drag
};
// point is inside the window, in points. button uses Deskflow ButtonID values.
void injectMouse(quint32 windowId, MouseAction action, int button, QPointF point, int clicks);
void injectWheel(quint32 windowId, QPointF point, int xDelta, int yDelta);

// Live capture of one window. Callbacks arrive on an arbitrary thread.
class Capture
{
public:
  virtual ~Capture() = default;
  // Encode and deliver the newest frame (now if one is waiting, else as soon as one arrives).
  virtual void requestFrame() = 0;
  virtual void setOutputSize(QSize pixels) = 0;
};
using FrameCallback = std::function<void(const QByteArray &jpeg, QSize pixels)>;
using StopCallback = std::function<void(const QString &reason)>;
std::unique_ptr<Capture> startCapture(quint32 windowId, QSize pixels, FrameCallback onFrame, StopCallback onStopped);

} // namespace WindowMirrorPlatform
