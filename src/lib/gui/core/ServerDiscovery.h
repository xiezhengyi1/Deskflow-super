// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#pragma once

#include <QObject>
#include <QTimer>
#include <QUdpSocket>

namespace deskflow::gui {

// Discovery only supplies candidate addresses. The core authenticates the bound
// SHA-256 certificate before allowing any input or clipboard traffic.
class ServerDiscovery : public QObject
{
  Q_OBJECT
public:
  explicit ServerDiscovery(QObject *parent = nullptr, quint16 discoveryPort = 24801, bool broadcast = true,
                           QHostAddress bindAddress = QHostAddress::AnyIPv4, bool fixedClientPort = true,
                           quint16 notificationPort = 24802);
  void configure(bool server, bool enabled, const QString &name, const QString &fingerprint,
                 quint16 serverPort, const QString &lastHost = {});
  void probeNow();
  void rememberClient(const QString &host);
  void restoreClients(const QStringList &clients);
  static bool validFingerprint(const QString &fingerprint);

Q_SIGNALS:
  void serverFound(const QString &host, quint16 port, const QString &fingerprint);
  void clientsChanged(const QStringList &clients);

private:
  void receive();
  void sendQuery(const QHostAddress &address);
  QUdpSocket m_socket;
  QTimer m_timer;
  quint16 m_discoveryPort;
  quint16 m_serverPort = 24800;
  quint16 m_notificationPort;
  bool m_broadcast;
  QHostAddress m_bindAddress;
  bool m_fixedClientPort;
  bool m_server = false;
  bool m_enabled = false;
  QString m_name;
  QString m_fingerprint;
  QString m_lastHost;
  QString m_nonce;
  qint64 m_lastResponse = 0;
  qint64 m_lastHintQuery = 0;
  QStringList m_clients;
};
}
