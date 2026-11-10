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
  static void attach(const QString &peer, int edges, bool server, Sender send, Sender activate);
  static void detach(const QString &peer);
  static void receive(const QString &peer, const QByteArray &bytes);
};
