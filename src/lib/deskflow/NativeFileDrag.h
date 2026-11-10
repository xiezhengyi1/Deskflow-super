/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "base/Event.h"
#include <QByteArray>
#include <QCborMap>
#include <QObject>
#include <QString>
#include <functional>
#include <memory>

// Keep a channel's event target alive until queued events and GUI callbacks are retired.
struct NativeFileDragEvent : EventData
{
  QCborMap packet;
  bool localSource = false;
  std::shared_ptr<int> channel;
  NativeFileDragEvent(QCborMap p, bool local, std::shared_ptr<int> target)
      : packet(std::move(p)),
        localSource(local),
        channel(std::move(target))
  {
  }
};

class NativeFileDrag
{
public:
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
  static constexpr bool supported = true;
#else
  static constexpr bool supported = false;
#endif
  using Sender = std::function<void(const QCborMap &)>;
  static void create(QObject *parent);
  static void publishRelayPeers();
  static void sendRelayPacket(const QByteArray &envelope);
  // A connected peer; send() delivers a packet to it over the authenticated Deskflow connection.
  static void attach(const QString &peer, Sender send);
  static void detach(const QString &peer);
  // A packet from or about peer. trusted is true for control packets that originate from
  // this computer's own server logic (or, on a client, from the server it is bound to).
  static void receive(const QString &peer, const QByteArray &bytes, bool trusted);
  // The left button went down on this computer; any file drag starts after this point.
  static void noteLeftDown();
};
