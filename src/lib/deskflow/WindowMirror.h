/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QCborMap>
#include <QList>
#include <QObject>
#include <QRect>
#include <QString>
#include <functional>
#include <optional>

class QWidget;

// Shows a window of one computer as a live, controllable window on the other.
//
// Owner (the Mac, where the real window lives): a title-bar drag reaching the screen edge is
// answered with a "window" drag packet. The window stays on the owner's screen (the system
// only delivers clicks to windows that are really there), is captured at its native
// resolution and streamed as JPEG pictures of the areas that changed. Input the viewer
// reports is replayed as real input at the window's place on screen, so everything works,
// including dragging files out of it.
// Viewer (Windows): shows the picture in a normal window that follows the cursor until the
// button is released, and reports pointer position, focus, resize and close.
//
// All functions except injectButton/injectWheel run on the controller's (Qt main) thread.
class WindowMirror
{
public:
  using Sender = std::function<void(const QString &peer, const QCborMap &packet)>;
  static void init(QObject *controller, Sender send);

  // Owner: a drag that began inside a mirrored window left the viewer's window with the
  // button held. The handler looks for files dragged on this computer and, if there are any,
  // continues the drag on the peer.
  enum class DragOut
  {
    Nothing,    // nothing is being dragged (yet)
    HandedOver, // the drag continues on the other computer
    Unsupported // dragged items that cannot be sent
  };
  using DragOutHandler = std::function<void(const QString &peer, std::function<void(DragOut)> done)>;
  static void setDragOutHandler(DragOutHandler handler);

  // Owner: called by the edge-drag probe when no files are dragged. If the held button is
  // dragging a window, takes it over and returns the "window" answer to send to peer.
  static std::optional<QCborMap> takeOverWindowDrag(const QString &peer, const QString &id);
  // Viewer: the owner's "window" answer. Prepares a hidden mirror window.
  static void handleWindowOffer(const QString &peer, const QCborMap &packet);
  // Viewer: the server switched here; show the mirror under the cursor. False if id is unknown.
  static bool handleStart(const QString &id);
  // Either side: the server gave up before switching.
  static void cancel(const QString &id);
  // Packets marked "mirror": true.
  static void handlePacket(const QString &peer, const QCborMap &packet);
  static void detach(const QString &peer);
  // Stops mirroring every window (core shutting down).
  static void shutdown();

  // Owner, any thread: the server's mouse buttons / wheel while the pointer is over a mirror.
  static void injectButton(int button, bool down);
  static void injectWheel(int xDelta, int yDelta);

  // The smallest rectangle containing every pixel that differs between two 32-bit pictures
  // of the same size (empty if they are equal), grown to whole 8-pixel blocks.
  static QRect changedRect(const uchar *before, const uchar *after, int width, int height, qsizetype bytesPerLine);

  // Tests.
  static QList<QWidget *> viewerWindows();
};
