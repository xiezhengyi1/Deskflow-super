/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QCborMap>
#include <QList>
#include <QObject>
#include <QString>
#include <functional>
#include <optional>

class QWidget;

// Shows a window of one computer as a live, controllable window on the other.
//
// Owner (the Mac, where the real window lives): a title-bar drag reaching the screen edge is
// answered with a "window" drag packet; the window is hidden, captured and streamed as JPEG
// frames. Input the viewer reports is injected into the real window.
// Viewer (Windows): shows the frames in a normal window that follows the cursor until the
// button is released, and reports pointer position, focus, resize and close.
//
// All functions except injectButton/injectWheel run on the controller's (Qt main) thread.
class WindowMirror
{
public:
  using Sender = std::function<void(const QString &peer, const QCborMap &packet)>;
  static void init(QObject *controller, Sender send);

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
  // Restores every hidden window (core shutting down).
  static void shutdown();

  // Owner, any thread: the server's mouse buttons / wheel while the pointer is over a mirror.
  static void injectButton(int button, bool down);
  static void injectWheel(int xDelta, int yDelta);

  // Tests.
  static QList<QWidget *> viewerWindows();
};
