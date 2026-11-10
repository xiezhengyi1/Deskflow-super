/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "common/MouseMotionMapper.h"
#include "deskflow/FileTransferSession.h"
#include <QCborValue>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>
#include <deque>

class FileTransferTests : public QObject
{
  Q_OBJECT
private Q_SLOTS:
  void multiFileRoundTripIncludingUnicodeAndEmptyFile()
  {
    QTemporaryDir directory;
    const QStringList names{QStringLiteral("文件测试.txt"), QStringLiteral("empty.bin"), QStringLiteral("larger.bin")};
    const QList<QByteArray> contents{
        QByteArray("hello\0world", 11), QByteArray{}, QByteArray(2 * FileTransferSession::chunkSize + 17, 'X')
    };
    QStringList paths;
    for (int i = 0; i < names.size(); ++i) {
      paths.append(directory.filePath(names.at(i)));
      QFile file(paths.last());
      QVERIFY(file.open(QIODevice::WriteOnly));
      QCOMPARE(file.write(contents.at(i)), contents.at(i).size());
    }
    FileTransferSession sender, receiver;
    struct Packet
    {
      bool toReceiver;
      QCborMap data;
    };
    std::deque<Packet> packets;
    QString error;
    bool ready = false, staged = false;
    sender.send = [&](const QCborMap &packet) { packets.push_back({true, packet}); };
    receiver.send = [&](const QCborMap &packet) { packets.push_back({false, packet}); };
    sender.failed = receiver.failed = [&](const QString &reason) { error = reason; };
    sender.sourceReady = [&] { ready = true; };
    receiver.sourceReady = [] {};
    sender.staged = [] {};
    receiver.staged = [&] { staged = true; };
    QVERIFY(sender.begin(paths, 1, QPoint(-1920, 600)));
    int count = 0;
    while (!packets.empty() && count++ < 100) {
      const auto packet = packets.front();
      packets.pop_front();
      // Exercise the actual byte encoding used on the Deskflow TLS stream.
      const auto decoded = QCborValue::fromCbor(QCborValue(packet.data).toCbor()).toMap();
      if (packet.toReceiver)
        receiver.receive(decoded);
      else
        sender.receive(decoded);
    }
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QVERIFY(ready);
    QVERIFY(staged);
    QVERIFY(receiver.isStaged());
    QVERIFY(packets.empty());
    QCOMPARE(receiver.position(), QPoint(-1920, 600));
    const auto received = receiver.receivedPaths();
    QCOMPARE(received.size(), names.size());
    for (int i = 0; i < received.size(); ++i) {
      QFile file(received.at(i));
      QVERIFY(file.open(QIODevice::ReadOnly));
      QCOMPARE(file.readAll(), contents.at(i));
      QVERIFY(QFile::exists(paths.at(i))); // Copy semantics preserve the originals.
    }
    receiver.cancel(QStringLiteral("test cleanup"));
    QVERIFY(!receiver.isStaged());
    for (const auto &path : received)
      QVERIFY(!QFile::exists(path));
  }
  void corruptChunkNeverBecomesAStagedFile()
  {
    QTemporaryDir directory;
    QFile file(directory.filePath("data.bin"));
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write("original");
    file.close();
    FileTransferSession sender, receiver;
    std::deque<std::pair<bool, QCborMap>> queue;
    sender.send = [&](const QCborMap &p) { queue.emplace_back(true, p); };
    receiver.send = [&](const QCborMap &p) { queue.emplace_back(false, p); };
    int errors = 0;
    sender.failed = receiver.failed = [&](const QString &) { ++errors; };
    sender.sourceReady = receiver.sourceReady = [] { QFAIL("Corrupt data must not complete"); };
    sender.staged = receiver.staged = [] { QFAIL("Corrupt data must not be staged"); };
    QVERIFY(sender.begin({file.fileName()}, 2, QPoint(3839, 500)));
    int count = 0;
    while (!queue.empty() && count++ < 30) {
      auto p = queue.front();
      queue.pop_front();
      if (p.first && p.second.value(QStringLiteral("type")).toString() == "chunk")
        p.second.insert(QStringLiteral("data"), QByteArray("modified"));
      if (p.first)
        receiver.receive(p.second);
      else
        sender.receive(p.second);
    }
    QCOMPARE(errors, 2);
    QVERIFY(!receiver.isStaged());
    QVERIFY(!sender.isSending());
    QVERIFY(QFile::exists(file.fileName()));
  }
  void cancellingMidTransferRemovesStagingAndPreservesSource()
  {
    QTemporaryDir directory;
    QFile original(directory.filePath("large.bin"));
    const QByteArray content(3 * FileTransferSession::chunkSize, 'Y');
    QVERIFY(original.open(QIODevice::WriteOnly));
    QCOMPARE(original.write(content), content.size());
    original.close();
    FileTransferSession sender, receiver;
    std::deque<std::pair<bool, QCborMap>> queue;
    sender.send = [&](const QCborMap &p) { queue.emplace_back(true, p); };
    receiver.send = [&](const QCborMap &p) { queue.emplace_back(false, p); };
    int errors = 0;
    sender.failed = receiver.failed = [&](const QString &) { ++errors; };
    sender.sourceReady = receiver.sourceReady = [] { QFAIL("Cancelled transfer must not complete"); };
    sender.staged = receiver.staged = [] { QFAIL("Cancelled transfer must not be staged"); };
    QVERIFY(sender.begin({original.fileName()}, 1, QPoint(-1920, 400)));
    // Offer, acknowledgement, and the first data chunk have crossed the wire.
    for (int i = 0; i < 3; ++i) {
      auto packet = queue.front();
      queue.pop_front();
      if (packet.first)
        receiver.receive(packet.second);
      else
        sender.receive(packet.second);
    }
    const auto stagingFile = receiver.receivedPaths().first();
    const auto stagingDirectory = QFileInfo(stagingFile).absolutePath();
    sender.cancel(QStringLiteral("Mouse released"));
    while (!queue.empty()) {
      auto packet = queue.front();
      queue.pop_front();
      if (packet.first)
        receiver.receive(packet.second);
      else
        sender.receive(packet.second);
    }
    QCOMPARE(errors, 2);
    QVERIFY(!receiver.isStaged());
    QVERIFY(!QDir(stagingDirectory).exists());
    QVERIFY(original.open(QIODevice::ReadOnly));
    QCOMPARE(original.readAll(), content);
  }

  void rejectUnsafeOrUnrepresentableNames_data()
  {
    QTest::addColumn<QString>("name");
    for (const auto &name : {"../secret", "..", "a/b", "a\\b", "CON", "nul.txt", "LPT1", "a:b", "name.", "name "})
      QTest::newRow(name) << QString::fromUtf8(name);
  }
  void rejectUnsafeOrUnrepresentableNames()
  {
    QFETCH(QString, name);
    QVERIFY(!FileTransferSession::validName(name));
    QVERIFY(FileTransferSession::validName(QStringLiteral("正常文件.txt")));
  }
  void fractionalMouseSpeedDoesNotLoseSlowMovement()
  {
    MouseMotionMapper mapper;
    mapper.setFactor(0.1);
    int x = 0, y = 0;
    for (int i = 0; i < 10000; ++i) {
      const auto motion = mapper.map(1, -1);
      x += motion.first;
      y += motion.second;
    }
    QCOMPARE(x, 1000);
    QCOMPARE(y, -1000);
    for (int i = 0; i < 10000; ++i) {
      const auto motion = mapper.map(-1, 1);
      x += motion.first;
      y += motion.second;
    }
    QCOMPARE(x, 0);
    QCOMPARE(y, 0);
  }
  void serverAndClientMouseSpeedsComposeIndependently()
  {
    MouseMotionMapper server, client;
    server.setFactor(0.5);
    client.setFactor(2.5);
    int total = 0;
    for (int i = 0; i < 1000; ++i) {
      const auto transmitted = server.map(1, 0);
      total += client.map(transmitted.first, transmitted.second).first;
    }
    QCOMPARE(total, 1250);
    client.setFactor(1.0);
    QCOMPARE(client.map(8, -4), std::make_pair(int32_t(8), int32_t(-4)));
  }
};
QTEST_GUILESS_MAIN(FileTransferTests)
#include "FileTransferTests.moc"
