/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "base/Log.h"
#include "deskflow/FileRelayWindow.h"
#include "deskflow/NativeFileDrag.h"
#include "deskflow/WindowMirror.h"
#include "deskflow/WindowMirrorRouting.h"
#include <QBuffer>
#include <QImage>
#include <QWidget>
#include <QCborArray>
#include "deskflow/ipc/IpcServer.h"
#include <QCborValue>
#include <QCheckBox>
#include <QFile>
#include <QFileInfo>
#include <QLabel>
#include <QListWidget>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QUuid>
#include <deque>
#ifdef Q_OS_MACOS
#include "platform/OSXMainThread.h"
#include <future>
#endif

namespace {
class RelayIpcTestServer : public deskflow::core::ipc::IpcServer
{
public:
  QStringList messages;
  explicit RelayIpcTestServer(const QString &name) : IpcServer(nullptr, name, QStringLiteral("test"))
  {
  }
  void processCommand(QLocalSocket *client, const QString &command, const QStringList &parts) override
  {
    if (command == "subscribeRelay") {
      subscribeFileRelay(client);
      flushFileRelayMessages();
      return;
    }
    messages.append(command + "=" + parts.value(1));
  }
};
struct Wire
{
  FileRelayWindow &a, &b;
  std::deque<std::pair<bool, QCborMap>> packets;
  bool corrupt = false;
  Wire(FileRelayWindow &first, FileRelayWindow &second, bool autoAccept = false) : a(first), b(second)
  {
    a.setAutoAccept(autoAccept);
    b.setAutoAccept(autoAccept);
    a.setPeer("B", [this](const QCborMap &p) { packets.emplace_back(true, p); });
    b.setPeer("A", [this](const QCborMap &p) { packets.emplace_back(false, p); });
  }
  bool pump()
  {
    int count = 0;
    while (!packets.empty() && ++count < 2000) {
      auto [toB, packet] = packets.front();
      packets.pop_front();
      if (corrupt && packet.value(QStringLiteral("type")).toString() == "chunk")
        packet.insert(QStringLiteral("sha256"), QByteArray("wrong hash"));
      // The actual network channel serializes each packet to CBOR.
      packet = QCborValue::fromCbor(QCborValue(packet).toCbor()).toMap();
      if (toB)
        b.receive("A", packet);
      else
        a.receive("B", packet);
    }
    return packets.empty();
  }
};
QString makeFile(const QString &path, const QByteArray &data)
{
  QFile file(path);
  if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size())
    return {};
  return path;
}
QByteArray readFile(const QString &path)
{
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return {};
  return file.readAll();
}
QPushButton *button(FileRelayWindow &window, const char *name)
{
  return window.findChild<QPushButton *>(QString::fromLatin1(name));
}
QString status(FileRelayWindow &window)
{
  return window.findChild<QLabel *>(QStringLiteral("transferStatus"))->text();
}
} // namespace

class FileRelayTests : public QObject
{
  Q_OBJECT
  Log m_log;
private Q_SLOTS:
#ifdef Q_OS_MACOS
  void macInputServicesShareTheGuiThread()
  {
    bool ranOnMainThread = false;
    auto result = std::async(std::launch::async, [&] {
      return onOSXMainThread([&] {
        ranOnMainThread = QThread::currentThread() == QCoreApplication::instance()->thread();
        return 42;
      });
    });
    QTRY_VERIFY_WITH_TIMEOUT(result.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 3000);
    QCOMPARE(result.get(), 42);
    QVERIFY(ranOnMainThread);
  }
#endif
  void bidirectionalRelayRequiresAcceptanceAndPreservesFiles()
  {
    QTemporaryDir source, downloadsA, downloadsB;
    const QStringList files{
        makeFile(source.filePath(QStringLiteral("中文文件.txt")), QByteArray("hello\0world", 11)),
        makeFile(source.filePath("empty.bin"), {}),
        makeFile(source.filePath("large.bin"), QByteArray(2 * FileTransferSession::chunkSize + 19, 'X'))
    };
    FileRelayWindow a, b;
    a.setDownloadDirectory(downloadsA.path());
    b.setDownloadDirectory(downloadsB.path());
    Wire wire(a, b);
    a.addFiles(files);
    button(a, "sendFiles")->click();
    QVERIFY(wire.pump());
    QVERIFY(b.savedFiles().isEmpty());
    QVERIFY(button(b, "receiveFiles")->isVisible());
    QVERIFY(status(a).contains("Waiting"));
    button(b, "receiveFiles")->click();
    QVERIFY(wire.pump());
    QCOMPARE(b.savedFiles().size(), 3);
    QVERIFY(status(a).contains("Sent successfully"));
    for (int i = 0; i < files.size(); ++i) {
      QCOMPARE(readFile(b.savedFiles().at(i)), readFile(files.at(i)));
      QVERIFY(QFile::exists(files.at(i)));
    }
    // A second transfer with the same names is saved next to the first one under new names.
    const auto first = b.savedFiles();
    QCOMPARE(QFileInfo(first.first()).absolutePath(), QFileInfo(downloadsB.path()).absoluteFilePath());
    button(a, "sendFiles")->click();
    QVERIFY(wire.pump());
    button(b, "receiveFiles")->click();
    QVERIFY(wire.pump());
    QCOMPARE(b.savedFiles().size(), 6);
    QCOMPARE(QFileInfo(b.savedFiles().at(3)).fileName(), QStringLiteral("中文文件 (1).txt"));
    QCOMPARE(QFileInfo(b.savedFiles().at(4)).fileName(), QStringLiteral("empty (1).bin"));
    for (int i = 0; i < files.size(); ++i) {
      QVERIFY(QFile::exists(first.at(i)));
      QCOMPARE(readFile(b.savedFiles().at(3 + i)), readFile(files.at(i)));
    }
    // Reverse the same wire; native OS drag state is never involved.
    b.addFiles(first);
    button(b, "sendFiles")->click();
    QVERIFY(wire.pump());
    button(a, "receiveFiles")->click();
    QVERIFY(wire.pump());
    QCOMPARE(a.savedFiles().size(), 3);
    for (int i = 0; i < files.size(); ++i)
      QCOMPARE(readFile(a.savedFiles().at(i)), readFile(files.at(i)));
    // Already saved files survive a disconnect and session cleanup.
    a.removePeer("B");
    b.removePeer("A");
    for (const auto &path : b.savedFiles())
      QVERIFY(QFile::exists(path));
  }
  void edgeDragTargetAcknowledgesOffer()
  {
    QObject parent;
    NativeFileDrag::create(&parent);
    QList<QCborMap> sent;
    NativeFileDrag::attach("server", [&](const QCborMap &p) { sent.append(p); });
    QCoreApplication::processEvents();
    const QCborMap offer{
        {QStringLiteral("drag"), true},
        {QStringLiteral("type"), QStringLiteral("offer")},
        {QStringLiteral("id"), QStringLiteral("t1")},
        {QStringLiteral("files"), QCborArray{QCborMap{{QStringLiteral("name"), QStringLiteral("a.txt")},
                                                      {QStringLiteral("size"), 3}}}},
        {QStringLiteral("edge"), 3},
        {QStringLiteral("x"), 0},
        {QStringLiteral("y"), 0}
    };
    NativeFileDrag::receive("server", QCborValue(offer).toCbor(), true);
    QTRY_VERIFY_WITH_TIMEOUT(!sent.isEmpty(), 2000);
    QCOMPARE(sent.first().value("type").toString(), QStringLiteral("ack"));
    QCOMPARE(sent.first().value("id").toString(), QStringLiteral("t1"));
    QVERIFY(sent.first().value("drag").toBool());
  }
  void windowMirrorViewerShowsFramesAndReportsBack()
  {
    QObject owner;
    QList<QPair<QString, QCborMap>> sent;
    WindowMirror::init(&owner, [&](const QString &peer, const QCborMap &packet) { sent.append({peer, packet}); });
    const QCborMap offer{
        {QStringLiteral("drag"), true},
        {QStringLiteral("type"), QStringLiteral("window")},
        {QStringLiteral("id"), QStringLiteral("w1")},
        {QStringLiteral("name"), QStringLiteral("TextEdit — Notes")},
        {QStringLiteral("title"), QStringLiteral("TextEdit — Notes [update Deskflow on this computer]")},
        {QStringLiteral("w"), 400},
        {QStringLiteral("h"), 300},
        {QStringLiteral("grabX"), 120},
        {QStringLiteral("grabY"), 10}
    };
    WindowMirror::handleWindowOffer("server", offer);
    QCOMPARE(WindowMirror::viewerWindows().size(), 1);
    // The viewer says at once what it can show; the owner sends partial pictures only then.
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first().second.value("type").toString(), QStringLiteral("hello"));
    QVERIFY(sent.first().second.value("v").toInteger() >= 2);
    auto *view = WindowMirror::viewerWindows().first();
    QVERIFY(view->windowTitle().contains("TextEdit — Notes"));
    QVERIFY(!view->windowTitle().contains("update Deskflow")); // that notice is for viewers older than "name"
    QCOMPARE(view->size(), QSize(400, 300));
    QVERIFY(!view->isVisible());
    QVERIFY(!WindowMirror::handleStart("unknown"));
    QVERIFY(WindowMirror::handleStart("w1"));
    QVERIFY(view->isVisible());
    // The window follows the cursor until the server reports the button's release.
    QTest::qWait(50);
    QVERIFY(std::none_of(sent.cbegin(), sent.cend(), [](const auto &p) { return p.second.value("type").toString() == "resize"; }));
    ++WindowMirrorRouting::remoteLeftUps;
    // Dropped: size and focus are reported.
    QTRY_VERIFY(std::any_of(sent.cbegin(), sent.cend(), [](const auto &p) {
      return p.second.value("type").toString() == "resize" && p.second.value("w").toInteger() == 400;
    }));
    QVERIFY(std::all_of(sent.cbegin(), sent.cend(), [](const auto &p) {
      return p.first == "server" && p.second.value("mirror").toBool() && p.second.value("id").toString() == "w1";
    }));
    // A frame is shown and acknowledged so the next one can be sent.
    QImage image(40, 30, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    buffer.open(QIODevice::WriteOnly);
    QVERIFY(image.save(&buffer, "JPEG"));
    sent.clear();
    WindowMirror::handlePacket(
        "server", {{QStringLiteral("mirror"), true},
                   {QStringLiteral("type"), QStringLiteral("frame")},
                   {QStringLiteral("id"), QStringLiteral("w1")},
                   {QStringLiteral("seq"), 7},
                   {QStringLiteral("jpeg"), jpeg}}
    );
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first().second.value("type").toString(), QStringLiteral("ack"));
    QCOMPARE(sent.first().second.value("seq").toInteger(), 7);
    // Frames for unknown windows or from another peer are ignored.
    sent.clear();
    WindowMirror::handlePacket("other", {{QStringLiteral("type"), QStringLiteral("frame")}, {QStringLiteral("id"), QStringLiteral("nope")}});
    QVERIFY(sent.isEmpty());
    // Later pictures only carry the area that changed.
    QImage patch(16, 8, QImage::Format_RGB32);
    patch.fill(Qt::blue);
    QByteArray patchJpeg;
    QBuffer patchBuffer(&patchJpeg);
    patchBuffer.open(QIODevice::WriteOnly);
    QVERIFY(patch.save(&patchBuffer, "JPEG"));
    const auto framePacket = [&](int seq, int x, int y, int fw, int fh) {
      return QCborMap{{QStringLiteral("mirror"), true},  {QStringLiteral("type"), QStringLiteral("frame")},
                      {QStringLiteral("id"), QStringLiteral("w1")}, {QStringLiteral("seq"), seq},
                      {QStringLiteral("x"), x},          {QStringLiteral("y"), y},
                      {QStringLiteral("fw"), fw},        {QStringLiteral("fh"), fh},
                      {QStringLiteral("jpeg"), patchJpeg}};
    };
    sent.clear();
    WindowMirror::handlePacket("server", framePacket(8, 16, 8, 40, 30));
    QCOMPARE(sent.size(), 1);
    QCOMPARE(sent.first().second.value("type").toString(), QStringLiteral("ack"));
    const auto shown = view->grab().toImage();
    const QColor inPatch = shown.pixelColor(240, 120), outside = shown.pixelColor(50, 250);
    QVERIFY2(inPatch.blue() > 180 && inPatch.red() < 90, qPrintable(inPatch.name()));
    QVERIFY2(outside.red() > 180 && outside.blue() < 90, qPrintable(outside.name()));
    // A patch for a picture of another size cannot be shown: a complete one is requested.
    sent.clear();
    WindowMirror::handlePacket("server", framePacket(9, 16, 8, 80, 60));
    QCOMPARE(sent.size(), 2);
    QCOMPARE(sent.first().second.value("type").toString(), QStringLiteral("refresh"));
    QCOMPARE(sent.last().second.value("type").toString(), QStringLiteral("ack"));
    // The real window changing size by itself resizes the copy, without an echo.
    sent.clear();
    WindowMirror::handlePacket("server", {{QStringLiteral("type"), QStringLiteral("size")},
                                          {QStringLiteral("id"), QStringLiteral("w1")},
                                          {QStringLiteral("w"), 500},
                                          {QStringLiteral("h"), 350}});
    QCOMPARE(view->size(), QSize(500, 350));
    QTest::qWait(250);
    QVERIFY(std::none_of(sent.cbegin(), sent.cend(), [](const auto &p) { return p.second.value("type").toString() == "resize"; }));
    // Closing the copy tells the owner to bring its window back.
    view->close();
    QTRY_VERIFY(WindowMirror::viewerWindows().isEmpty());
    QVERIFY(std::any_of(sent.cbegin(), sent.cend(), [](const auto &p) { return p.second.value("type").toString() == "close"; }));
  }
  void windowMirrorSendsOnlyWhatChanged()
  {
    QImage before(100, 60, QImage::Format_RGB32), after;
    before.fill(Qt::white);
    after = before;
    QVERIFY(WindowMirror::changedRect(before.constBits(), after.constBits(), 100, 60, before.bytesPerLine()).isEmpty());
    after.setPixel(21, 10, 0xff000000);
    after.setPixel(45, 33, 0xff000000);
    QCOMPARE(
        WindowMirror::changedRect(before.constBits(), after.constBits(), 100, 60, before.bytesPerLine()),
        QRect(QPoint(16, 8), QPoint(47, 39))
    );
    // Blocks are cut off at the picture's edge.
    after = before;
    after.setPixel(99, 59, 0xff000000);
    QCOMPARE(
        WindowMirror::changedRect(before.constBits(), after.constBits(), 100, 60, before.bytesPerLine()),
        QRect(QPoint(96, 56), QPoint(99, 59))
    );
  }
  void windowMirrorOwnerClosingDoesNotEcho()
  {
    QObject owner;
    QList<QCborMap> sent;
    WindowMirror::init(&owner, [&](const QString &, const QCborMap &packet) { sent.append(packet); });
    WindowMirror::handleWindowOffer(
        "server", {{QStringLiteral("type"), QStringLiteral("window")},
                   {QStringLiteral("id"), QStringLiteral("w2")},
                   {QStringLiteral("title"), QStringLiteral("Safari")},
                   {QStringLiteral("w"), 300},
                   {QStringLiteral("h"), 200}}
    );
    QVERIFY(WindowMirror::handleStart("w2"));
    ++WindowMirrorRouting::remoteLeftUps;
    QCoreApplication::processEvents();
    sent.clear();
    WindowMirror::handlePacket("server", {{QStringLiteral("type"), QStringLiteral("closed")}, {QStringLiteral("id"), QStringLiteral("w2")}});
    QTRY_VERIFY(WindowMirror::viewerWindows().isEmpty());
    QVERIFY(std::none_of(sent.cbegin(), sent.cend(), [](const QCborMap &p) { return p.value("type").toString() == "close"; }));
    // An offer the server never started is dropped on cancel.
    WindowMirror::handleWindowOffer(
        "server", {{QStringLiteral("type"), QStringLiteral("window")},
                   {QStringLiteral("id"), QStringLiteral("w3")},
                   {QStringLiteral("w"), 300},
                   {QStringLiteral("h"), 200}}
    );
    WindowMirror::cancel("w3");
    QTRY_VERIFY(WindowMirror::viewerWindows().isEmpty());
    // Button and wheel injection with no mirrored window are harmless.
    WindowMirror::injectButton(1, true);
    WindowMirror::injectWheel(0, 120);
    QCoreApplication::processEvents();
  }
  void autoAcceptReceivesInTheBackground()
  {
    QTemporaryDir source, downloadsB;
    const auto path = makeFile(source.filePath("photo.jpg"), QByteArray(FileTransferSession::chunkSize + 5, 'P'));
    FileRelayWindow a, b;
    b.setDownloadDirectory(downloadsB.path());
    Wire wire(a, b, true);
    QSignalSpy finished(&b, &FileRelayWindow::transferFinished);
    a.addFiles({path});
    button(a, "sendFiles")->click();
    QVERIFY(wire.pump());
    QVERIFY(!b.isVisible());
    QVERIFY(!button(b, "receiveFiles")->isVisible());
    QCOMPARE(b.savedFiles().size(), 1);
    QCOMPARE(readFile(b.savedFiles().first()), readFile(path));
    QVERIFY(status(a).contains("Sent successfully"));
    QCOMPARE(finished.count(), 1);
    QVERIFY(finished.first().at(1).toBool());
    QCOMPARE(finished.first().at(2).toStringList(), b.savedFiles());
  }
  void autoAcceptCanBeTurnedOff()
  {
    FileRelayWindow window;
    QSignalSpy changed(&window, &FileRelayWindow::autoAcceptChanged);
    QVERIFY(window.autoAccept());
    window.findChild<QCheckBox *>(QStringLiteral("autoAccept"))->setChecked(false);
    QVERIFY(!window.autoAccept());
    QCOMPARE(changed.count(), 1);
    window.setAutoAccept(true);
    QVERIFY(window.autoAccept());
  }
  void uniquePathNeverOverwrites()
  {
    QTemporaryDir directory;
    const QDir dir(directory.path());
    QCOMPARE(FileTransferSession::uniquePath(dir, "a.tar.gz"), dir.filePath("a.tar.gz"));
    makeFile(dir.filePath("a.tar.gz"), "1");
    QCOMPARE(FileTransferSession::uniquePath(dir, "a.tar.gz"), dir.filePath("a (1).tar.gz"));
    makeFile(dir.filePath("README"), "1");
    makeFile(dir.filePath("README (1)"), "2");
    QCOMPARE(FileTransferSession::uniquePath(dir, "README"), dir.filePath("README (2)"));
  }
  void rejectAndCancelDoNotSaveFiles()
  {
    QTemporaryDir directory, downloads;
    const auto path = makeFile(directory.filePath("sample.txt"), "original");
    FileRelayWindow a, b;
    b.setDownloadDirectory(downloads.path());
    Wire wire(a, b);
    a.addFiles({path});
    button(a, "sendFiles")->click();
    QVERIFY(wire.pump());
    button(b, "rejectFiles")->click();
    QVERIFY(wire.pump());
    QVERIFY(b.savedFiles().isEmpty());
    QVERIFY(status(a).contains("rejected"));
    button(a, "sendFiles")->click();
    QVERIFY(wire.pump());
    button(a, "cancelTransfer")->click();
    QVERIFY(wire.pump());
    QVERIFY(b.savedFiles().isEmpty());
    QVERIFY(!button(b, "receiveFiles")->isVisible());
    QVERIFY(QFile::exists(path));
  }
  void cancelAfterFirstChunkRemovesIncompleteTransfer()
  {
    QTemporaryDir directory, downloads;
    const auto path = makeFile(directory.filePath("large.bin"), QByteArray(3 * FileTransferSession::chunkSize, 'Y'));
    FileRelayWindow a, b;
    b.setDownloadDirectory(downloads.path());
    Wire wire(a, b);
    a.addFiles({path});
    button(a, "sendFiles")->click();
    QVERIFY(wire.pump());
    button(b, "receiveFiles")->click();
    // Deliver ack and first chunk, then cancel before the next queued ack.
    auto p = wire.packets.front();
    wire.packets.pop_front();
    a.receive("B", p.second);
    p = wire.packets.front();
    wire.packets.pop_front();
    b.receive("A", p.second);
    button(b, "cancelTransfer")->click();
    QVERIFY(wire.pump());
    QVERIFY(b.savedFiles().isEmpty());
    QVERIFY(QFile::exists(path));
    QVERIFY(status(a).contains("stopped"));
  }
  void corruptDataAndSaveFailuresCannotReportSuccess()
  {
    QTemporaryDir directory, downloads;
    const auto path = makeFile(directory.filePath("sample.txt"), "original");
    FileRelayWindow a, b;
    b.setDownloadDirectory(downloads.path());
    Wire wire(a, b);
    wire.corrupt = true;
    a.addFiles({path});
    button(a, "sendFiles")->click();
    QVERIFY(wire.pump());
    button(b, "receiveFiles")->click();
    QVERIFY(wire.pump());
    QVERIFY(b.savedFiles().isEmpty());
    QVERIFY(status(a).contains("stopped"));
    wire.corrupt = false;
    // A regular file cannot be used as the receive directory.
    b.setDownloadDirectory(path);
    button(a, "sendFiles")->click();
    QVERIFY(wire.pump());
    button(b, "receiveFiles")->click();
    QVERIFY(wire.pump());
    QVERIFY(b.savedFiles().isEmpty());
    QVERIFY(status(a).contains("stopped"));
    QCOMPARE(readFile(path), QByteArray("original"));
  }
  void filePacketsWaitForTheGuiSubscriber()
  {
    const auto name = QStringLiteral("dr-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    RelayIpcTestServer server(name);
    server.listen();
    server.broadcastCommand(QStringLiteral("fileRelayPacket"), QStringLiteral("queued-data"));
    QLocalSocket socket;
    socket.connectToServer(name);
    QVERIFY(socket.waitForConnected(2000));
    socket.write("noop\n");
    socket.flush();
    QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 3000);
    const auto probe = socket.readAll();
    QVERIFY(probe.contains("ok\n"));
    QVERIFY(!probe.contains("queued-data"));
    socket.write("subscribeRelay\n");
    socket.flush();
    QTRY_VERIFY_WITH_TIMEOUT(socket.bytesAvailable() > 0, 3000);
    QCOMPARE(socket.readAll(), QByteArray("fileRelayPacket=queued-data\n"));
    socket.disconnectFromServer();
    QLocalServer::removeServer(name);
  }
  void localIpcRetainsPartialFileFrames()
  {
    const auto name = QStringLiteral("dr-") + QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    RelayIpcTestServer server(name);
    server.listen();
    QLocalSocket socket;
    socket.connectToServer(name);
    QVERIFY(socket.waitForConnected(2000));
    const auto payload =
        QByteArray(160000, 'X').toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals);
    const auto frame = QByteArray("fileRelaySend=") + payload + "\n";
    QCOMPARE(socket.write(frame.left(17)), 17);
    socket.flush();
    QTest::qWait(20);
    QVERIFY(server.messages.isEmpty());
    const auto middle = frame.mid(17, 100000);
    QCOMPARE(socket.write(middle), middle.size());
    socket.flush();
    QTest::qWait(20);
    QVERIFY(server.messages.isEmpty());
    const auto tail = frame.mid(100017) + "fileRelayList=next\n";
    QCOMPARE(socket.write(tail), tail.size());
    socket.flush();
    QTRY_COMPARE_WITH_TIMEOUT(server.messages.size(), 2, 3000);
    QCOMPARE(server.messages.first(), QString::fromLatin1(frame.chopped(1)));
    QCOMPARE(server.messages.last(), QStringLiteral("fileRelayList=next"));
    socket.disconnectFromServer();
    QLocalServer::removeServer(name);
  }
  void sendToConnectedWaitsForPeerThenStartsAutomatically()
  {
    QTemporaryDir source, downloadsB;
    const auto path = makeFile(source.filePath("right-click.txt"), "from the Send to menu");
    FileRelayWindow a, b;
    b.setDownloadDirectory(downloadsB.path());
    a.sendToConnected({path});
    QVERIFY(status(a).contains("Waiting for a connected computer"));
    Wire wire(a, b);
    QVERIFY(wire.pump());
    QVERIFY(status(a).contains("Waiting for the other computer"));
    QVERIFY(button(b, "receiveFiles")->isVisible());
    button(b, "receiveFiles")->click();
    QVERIFY(wire.pump());
    QCOMPARE(b.savedFiles().size(), 1);
    QCOMPARE(readFile(b.savedFiles().first()), QByteArray("from the Send to menu"));
  }
  void sendToConnectedAsksWhenSeveralComputersAreConnected()
  {
    QTemporaryDir source;
    const auto path = makeFile(source.filePath("two-peers.txt"), "data");
    int sent = 0;
    FileRelayWindow window;
    window.setPeer("B", [&](const QCborMap &) { ++sent; });
    window.setPeer("C", [&](const QCborMap &) { ++sent; });
    window.sendToConnected({path});
    QVERIFY(status(window).contains("Several computers"));
    QCOMPARE(sent, 0);
    QVERIFY(button(window, "sendFiles")->isEnabled());
  }
  void disconnectedAndLegacyPeersCannotStartRelay()
  {
    QTemporaryDir directory;
    const auto path = makeFile(directory.filePath("sample.txt"), "original");
    FileRelayWindow window;
    window.addFiles({path});
    QVERIFY(!button(window, "sendFiles")->isEnabled());
    QCborMap legacy{{QStringLiteral("type"), QStringLiteral("offer")}, {QStringLiteral("id"), QStringLiteral("old")}};
    window.setPeer("old", [](const QCborMap &) {});
    window.receive("old", legacy);
    QVERIFY(!button(window, "receiveFiles")->isVisible());
    window.removePeer("old");
    QVERIFY(!button(window, "sendFiles")->isEnabled());
    FileTransferSession session;
    QString error;
    QVERIFY(session.saveReceivedFiles(directory.path(), &error).isEmpty());
    QVERIFY(!error.isEmpty());
  }
};
QTEST_MAIN(FileRelayTests)
#include "FileRelayTests.moc"
