// SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
#include "ServerDiscoveryTests.h"
#include "gui/core/ServerDiscovery.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkDatagram>
#include <QSignalSpy>
#include <QUdpSocket>

using deskflow::gui::ServerDiscovery;

void ServerDiscoveryTests::discoverOnlyBoundServer()
{
  QUdpSocket reserve;
  QVERIFY(reserve.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
  const auto port = reserve.localPort();
  reserve.close();
  const QString fingerprint(64, 'a');
  ServerDiscovery server(nullptr, port, false), client(nullptr, port, false, QHostAddress::AnyIPv4, false);
  server.configure(true, true, "test-server", fingerprint, 30000);
  QSignalSpy found(&client, &ServerDiscovery::serverFound);
  client.configure(false, true, {}, QString(64, 'b'), 24800, "127.0.0.1");
  QTest::qWait(150);
  QCOMPARE(found.count(), 0);
  client.configure(false, true, {}, fingerprint, 24800, "127.0.0.1");
  QTRY_VERIFY_WITH_TIMEOUT(found.count() > 0, 2000);
  const auto reply = found.takeFirst();
  QCOMPARE(reply.at(0).toString(), QString("127.0.0.1"));
  QCOMPARE(reply.at(1).value<quint16>(), quint16(30000));
  QCOMPARE(reply.at(2).toString(), fingerprint);
  client.configure(false, false, {}, fingerprint, 24800, "127.0.0.1");
  found.clear();
  client.probeNow();
  QTest::qWait(100);
  QCOMPARE(found.count(), 0);
}

void ServerDiscoveryTests::rejectUnsolicitedAndMalformedReplies()
{
  QUdpSocket fake;
  QVERIFY(fake.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
  const QString fingerprint(64, 'c');
  ServerDiscovery client(nullptr, fake.localPort(), false, QHostAddress::AnyIPv4, false);
  QSignalSpy found(&client, &ServerDiscovery::serverFound);
  client.configure(false, true, {}, fingerprint, 24800, "127.0.0.1");
  QTRY_VERIFY(fake.hasPendingDatagrams());
  const auto query = fake.receiveDatagram();
  auto reply = QJsonDocument::fromJson(query.data()).object();
  reply["type"] = "server";
  reply["port"] = 24800;
  const QJsonValue nonce = reply.value("nonce");
  auto send = [&] {
    fake.writeDatagram(QJsonDocument(reply).toJson(QJsonDocument::Compact), query.senderAddress(), query.senderPort());
    QTest::qWait(30);
  };
  reply["nonce"] = QString(36, 'x'); send();
  QCOMPARE(found.count(), 0);
  reply["nonce"] = nonce;
  reply["port"] = 65536; send();
  QCOMPARE(found.count(), 0);
  reply["port"] = 24800;
  reply["fingerprint"] = QString(64, 'd'); send();
  QCOMPARE(found.count(), 0);
  reply["fingerprint"] = fingerprint; send();
  QCOMPARE(found.count(), 1);
}
void ServerDiscoveryTests::findMovedServerWithoutBroadcast()
{
  QUdpSocket serverReservation, clientReservation;
  QVERIFY(serverReservation.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
  QVERIFY(clientReservation.bind(QHostAddress(QHostAddress::LocalHost), quint16(0)));
  const auto serverPort = serverReservation.localPort();
  const auto clientPort = clientReservation.localPort();
  serverReservation.close();
  clientReservation.close();
  const QString fingerprint(64, 'e');
  ServerDiscovery server(nullptr, serverPort, false, QHostAddress(QHostAddress::LocalHost), true, clientPort);
  ServerDiscovery client(nullptr, serverPort, false, QHostAddress(QHostAddress::LocalHost), true, clientPort);
  // Simulate a restarted server with its last authenticated client's saved IP.
  server.restoreClients({"127.0.0.1"});
  server.configure(true, true, "moved-server", fingerprint, 30001);
  QSignalSpy found(&client, &ServerDiscovery::serverFound);
  // Both endpoints have broadcasting disabled and the client's old server IP is stale.
  client.configure(false, true, {}, fingerprint, 24800, "192.0.2.99");
  server.probeNow();
  QTRY_VERIFY_WITH_TIMEOUT(!found.isEmpty(), 2000);
  QCOMPARE(found.first().at(0).toString(), QString("127.0.0.1"));
  QCOMPARE(found.first().at(1).value<quint16>(), quint16(30001));
}
QTEST_MAIN(ServerDiscoveryTests)
