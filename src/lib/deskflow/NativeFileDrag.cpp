/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "NativeFileDrag.h"
#include "ipc/CoreIpc.h"
#include <QCborValue>
#include <QMap>
#include <QPointer>
namespace {
QPointer<QObject> controller;
QMap<QString, NativeFileDrag::Sender> peers;
QString encoded(QCborMap packet)
{
  return QString::fromLatin1(
      QCborValue(packet).toCbor().toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals)
  );
}
void publishPeer(const QString &peer, bool connected)
{
  ipcSendToClient(
      QStringLiteral("fileRelayPeer"),
      encoded({{QStringLiteral("peer"), peer}, {QStringLiteral("connected"), connected}})
  );
}
} // namespace
void NativeFileDrag::create(QObject *parent)
{
  controller = new QObject(parent);
}
void NativeFileDrag::publishRelayPeers()
{
  if (!controller)
    return;
  // Called by the IPC handler on the controller's main thread. Publish the
  // snapshot before flushing queued file packets; never reset an active offer.
  for (auto it = peers.cbegin(); it != peers.cend(); ++it)
    publishPeer(it.key(), true);
}
void NativeFileDrag::sendRelayPacket(const QByteArray &envelope)
{
  if (!controller || envelope.size() > 2 * 1024 * 1024)
    return;
  QMetaObject::invokeMethod(
      controller,
      [envelope] {
        QCborParserError error;
        const auto decoded = QCborValue::fromCbor(envelope, &error);
        if (error.error != QCborError::NoError || !decoded.isMap())
          return;
        const auto message = decoded.toMap();
        const auto peer = message.value(QStringLiteral("peer")).toString();
        const auto packet = message.value(QStringLiteral("packet")).toMap();
        const auto send = peers.value(peer);
        if (send && packet.value(QStringLiteral("relay")).toBool())
          send(packet);
      },
      Qt::QueuedConnection
  );
}
void NativeFileDrag::attach(const QString &peer, int edges, bool server, Sender send, Sender activate)
{
  Q_UNUSED(edges)
  Q_UNUSED(server)
  Q_UNUSED(activate)
  if (!controller)
    return;
  QMetaObject::invokeMethod(
      controller,
      [peer, send = std::move(send)] {
        peers.insert(peer, send);
        publishPeer(peer, true);
      },
      Qt::QueuedConnection
  );
}
void NativeFileDrag::detach(const QString &peer)
{
  if (!controller)
    return;
  QMetaObject::invokeMethod(
      controller,
      [peer] {
        peers.remove(peer);
        publishPeer(peer, false);
      },
      Qt::QueuedConnection
  );
}
void NativeFileDrag::receive(const QString &peer, const QByteArray &bytes)
{
  if (!controller || bytes.size() > 2 * 1024 * 1024)
    return;
  QMetaObject::invokeMethod(
      controller,
      [peer, bytes] {
        QCborParserError error;
        const auto value = QCborValue::fromCbor(bytes, &error);
        if (error.error != QCborError::NoError || !value.isMap() || !peers.contains(peer) ||
            !value.toMap().value(QStringLiteral("relay")).toBool())
          return;
        ipcSendToClient(
            QStringLiteral("fileRelayPacket"),
            encoded({{QStringLiteral("peer"), peer}, {QStringLiteral("packet"), value}})
        );
      },
      Qt::QueuedConnection
  );
}
