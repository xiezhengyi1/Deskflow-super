// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "ServerDiscovery.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QNetworkInterface>
#include <QRegularExpression>
#include <QSet>
#include <QUuid>
#include <QUrl>

namespace deskflow::gui {
namespace {
const QString protocol = QStringLiteral("deskflow-pairing-v1");
}

ServerDiscovery::ServerDiscovery(QObject *parent, quint16 discoveryPort, bool broadcast,
                                 QHostAddress bindAddress, bool fixedClientPort, quint16 notificationPort)
    : QObject(parent), m_discoveryPort(discoveryPort),
      m_notificationPort(notificationPort ? notificationPort : discoveryPort), m_broadcast(broadcast),
      m_bindAddress(bindAddress), m_fixedClientPort(fixedClientPort)
{
  connect(&m_socket, &QUdpSocket::readyRead, this, &ServerDiscovery::receive);
  connect(&m_timer, &QTimer::timeout, this, &ServerDiscovery::probeNow);
  m_timer.setInterval(5000);
}

bool ServerDiscovery::validFingerprint(const QString &fingerprint)
{
  static const QRegularExpression hex(QStringLiteral("^[0-9a-f]{64}$"));
  return hex.match(fingerprint).hasMatch();
}

void ServerDiscovery::configure(bool server, bool enabled, const QString &name,
                                const QString &fingerprint, quint16 serverPort, const QString &lastHost)
{
  const bool rebind = m_server != server || m_socket.state() != QAbstractSocket::BoundState;
  m_server = server;
  m_enabled = enabled && validFingerprint(fingerprint);
  m_name = name.left(128);
  m_fingerprint = fingerprint;
  m_serverPort = serverPort;
  m_lastHost = QUrl(QStringLiteral("tcp://") + lastHost).host();
  if (!m_enabled) {
    m_timer.stop();
    m_socket.close();
    return;
  }
  if (rebind) {
    m_socket.close();
    const auto port = server ? m_discoveryPort : (m_fixedClientPort ? m_notificationPort : quint16(0));
    if (!m_socket.bind(m_bindAddress, port, QUdpSocket::ShareAddress | QUdpSocket::ReuseAddressHint)) {
      qWarning("cannot bind device discovery socket");
      m_timer.stop();
      return;
    }
  }
  m_timer.start();
  probeNow();
}

void ServerDiscovery::restoreClients(const QStringList &clients)
{
  m_clients.clear();
  for (const auto &client : clients) {
    const QHostAddress address(client);
    if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isNull() || address.isMulticast() ||
        address == QHostAddress::Broadcast || m_clients.contains(address.toString())) continue;
    m_clients.append(address.toString());
    if (m_clients.size() >= 8) break;
  }
}

void ServerDiscovery::rememberClient(const QString &host)
{
  const QHostAddress address(host);
  if (!m_server || address.protocol() != QAbstractSocket::IPv4Protocol || address.isNull() || address.isMulticast() ||
      address == QHostAddress::Broadcast || m_clients.contains(address.toString())) return;
  m_clients.prepend(address.toString());
  while (m_clients.size() > 8) m_clients.removeLast();
  Q_EMIT clientsChanged(m_clients);
}

void ServerDiscovery::sendQuery(const QHostAddress &address)
{
  if (m_nonce.isEmpty()) m_nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto bytes = QJsonDocument(QJsonObject{{"protocol", protocol}, {"type", "find"},
      {"nonce", m_nonce}, {"fingerprint", m_fingerprint}}).toJson(QJsonDocument::Compact);
  m_socket.writeDatagram(bytes, address, m_discoveryPort);
}

void ServerDiscovery::probeNow()
{
  if (!m_enabled || m_socket.state() != QAbstractSocket::BoundState) return;
  if (m_server) {
    const auto bytes = QJsonDocument(QJsonObject{{"protocol", protocol}, {"type", "hint"},
        {"fingerprint", m_fingerprint}}).toJson(QJsonDocument::Compact);
    for (const auto &client : m_clients) m_socket.writeDatagram(bytes, QHostAddress(client), m_notificationPort);
    return;
  }
  m_nonce = QUuid::createUuid().toString(QUuid::WithoutBraces);
  QSet<QHostAddress> destinations;
  const QHostAddress last(m_lastHost);
  if (last.protocol() == QAbstractSocket::IPv4Protocol) destinations.insert(last);
  if (m_broadcast) for (const auto &iface : QNetworkInterface::allInterfaces()) {
    if (!(iface.flags() & QNetworkInterface::IsUp) || !(iface.flags() & QNetworkInterface::IsRunning) ||
        (iface.flags() & QNetworkInterface::IsLoopBack) || (iface.flags() & QNetworkInterface::IsPointToPoint)) continue;
    for (const auto &entry : iface.addressEntries()) {
      if (entry.ip().protocol() == QAbstractSocket::IPv4Protocol && !entry.broadcast().isNull())
        destinations.insert(entry.broadcast());
    }
  }
  for (const auto &address : destinations) sendQuery(address);
}

void ServerDiscovery::receive()
{
  while (m_socket.hasPendingDatagrams()) {
    const auto datagram = m_socket.receiveDatagram(1401);
    if (!m_enabled || datagram.data().size() > 1400) continue;
    const auto object = QJsonDocument::fromJson(datagram.data()).object();
    if (object.value("protocol").toString() != protocol ||
        object.value("fingerprint").toString() != m_fingerprint) continue;
    if (!m_server && object.value("type").toString() == "hint") {
      // A hint supplies no identity proof and never changes the saved address.
      // Challenge its source first; only the core's TLS pin authorizes a connection.
      const auto now = QDateTime::currentMSecsSinceEpoch();
      if (now - m_lastHintQuery >= 1000) {
        m_lastHintQuery = now;
        sendQuery(datagram.senderAddress());
      }
      continue;
    }
    const auto nonce = object.value("nonce").toString();
    if (nonce.size() != 36) continue;
    if (m_server && object.value("type").toString() == "find") {
      const auto now = QDateTime::currentMSecsSinceEpoch();
      if (now - m_lastResponse < 100) continue;
      m_lastResponse = now;
      const auto reply = QJsonDocument(QJsonObject{{"protocol", protocol}, {"type", "server"},
          {"nonce", nonce}, {"fingerprint", m_fingerprint}, {"name", m_name},
          {"port", int(m_serverPort)}}).toJson(QJsonDocument::Compact);
      m_socket.writeDatagram(reply, datagram.senderAddress(), datagram.senderPort());
    } else if (!m_server && object.value("type").toString() == "server" && nonce == m_nonce) {
      const int port = object.value("port").toInt();
      if (port < 1 || port > 65535 || datagram.senderAddress().isNull()) continue;
      Q_EMIT serverFound(datagram.senderAddress().toString(), quint16(port), m_fingerprint);
    }
  }
}
}
