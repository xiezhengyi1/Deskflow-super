/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "NativeFileDrag.h"
#include "FileTransferSession.h"
#include "NativeFileDragPlatform.h"
#include "WindowMirror.h"
#include "WindowMirrorPlatform.h"
#include "base/Log.h"
#include "ipc/CoreIpc.h"
#include <QCborArray>
#include <QCborValue>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QTimer>
#include <map>

// Edge drag, all on the core process's Qt main thread:
//  1. The server sees the left button held at an edge and sends "probe" to the source computer.
//  2. The source asks the OS what is being dragged. No files: "none". Files: it ends the local
//     drag and starts a FileTransferSession whose "offer" doubles as the answer.
//  3. The server switches to the target and sends it "start". The target has already begun
//     receiving into a staging folder and now starts a native drag of *promised* files, so the
//     cursor never waits for the bytes. The drop target receives the files when they are complete.
namespace {
QPointer<QObject> controller;
QMap<QString, NativeFileDrag::Sender> peers;

// Keep staged files after a drop: Explorer copies them in the background after the drop returns.
constexpr int kKeepStagedMs = 10 * 60 * 1000;
// Received files nobody started a drag for (the server never sent "start").
constexpr int kUnclaimedMs = 60 * 1000;

struct DragTransfer
{
  QString peer;
  bool outgoing = false;
  bool dragStarted = false;
  bool dropped = false;
  bool done = false; // session finished, successfully or not
  std::unique_ptr<FileTransferSession> session;
  std::shared_ptr<FileDragPromise> promise;
};
std::map<QString, std::shared_ptr<DragTransfer>> transfers;
QString probingId;

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

void sendDrag(const QString &peer, QCborMap packet)
{
  packet.insert(QStringLiteral("drag"), true);
  if (const auto send = peers.value(peer))
    send(packet);
}

// Never destroy a session from inside its own callbacks.
void retire(const QString &id, int delayMs)
{
  QTimer::singleShot(delayMs, controller, [id] { transfers.erase(id); });
}

QString checkDraggedFiles(const QStringList &paths)
{
  if (paths.isEmpty())
    return QStringLiteral("nothing dragged");
  if (paths.size() > 1024)
    return QStringLiteral("more than 1024 files");
  QSet<QString> names;
  for (const auto &path : paths) {
    const QFileInfo info(path);
    const auto name = info.fileName().normalized(QString::NormalizationForm_C);
    if (!info.isFile() || info.isSymLink())
      return QStringLiteral("folders and links are not supported: %1").arg(info.fileName());
    if (!FileTransferSession::validName(name) || names.contains(name.toCaseFolded()))
      return QStringLiteral("file name not usable on both systems: %1").arg(info.fileName());
    names.insert(name.toCaseFolded());
  }
  return {};
}

void handleProbe(const QString &peer, const QCborMap &packet)
{
  const auto id = packet.value(QStringLiteral("id")).toString();
  const int edge = static_cast<int>(packet.value(QStringLiteral("edge")).toInteger());
  if (id.isEmpty() || edge < 1 || edge > 4 || transfers.count(id))
    return;
  probingId = id;
  const bool windowsWanted = packet.value(QStringLiteral("windows")).toBool();
  NativeFileDragPlatform::probe([peer, id, edge, windowsWanted](const QStringList &paths) {
    if (probingId != id)
      return; // the server gave up on this probe
    probingId.clear();
    const auto problem = checkDraggedFiles(paths);
    if (!problem.isEmpty() && paths.isEmpty() && windowsWanted) {
      // Not a file drag: maybe a window is being dragged by its title bar.
      if (const auto answer = WindowMirror::takeOverWindowDrag(peer, id)) {
        sendDrag(peer, *answer);
        return;
      }
    }
    if (!problem.isEmpty()) {
      LOG_DEBUG("edge drag: not handing over the drag, %s", qPrintable(problem));
      sendDrag(peer, {{QStringLiteral("type"), QStringLiteral("none")}, {QStringLiteral("id"), id}});
      return;
    }
    LOG_INFO("edge drag: handing %d file(s) to the other computer", static_cast<int>(paths.size()));
    NativeFileDragPlatform::releaseSourceDrag();
    auto transfer = std::make_shared<DragTransfer>();
    transfer->peer = peer;
    transfer->outgoing = true;
    transfer->session = std::make_unique<FileTransferSession>();
    auto *session = transfer->session.get();
    session->send = [peer](const QCborMap &out) { sendDrag(peer, out); };
    session->failed = [id](const QString &reason) {
      LOG_WARN("edge drag: sending stopped: %s", qPrintable(reason));
      retire(id, 0);
    };
    session->sourceReady = [id] {
      LOG_INFO("edge drag: files delivered");
      retire(id, 0);
    };
    session->staged = [] {};
    transfers[id] = transfer;
    session->setId(id);
    // The offer tells the server the drag is ours to hand over; the server then switches screens.
    session->begin(paths, edge, {});
  });
}

void handleOffer(const QString &peer, const QCborMap &packet)
{
  const auto id = packet.value(QStringLiteral("id")).toString();
  if (id.isEmpty() || transfers.count(id))
    return;
  auto transfer = std::make_shared<DragTransfer>();
  transfer->peer = peer;
  transfer->session = std::make_unique<FileTransferSession>();
  auto *session = transfer->session.get();
  std::weak_ptr<DragTransfer> weak = transfer;
  session->send = [peer](const QCborMap &out) { sendDrag(peer, out); };
  session->failed = [weak, id](const QString &reason) {
    const auto t = weak.lock();
    if (!t || t->done)
      return;
    t->done = true;
    LOG_WARN("edge drag: receiving stopped: %s", qPrintable(reason));
    if (t->promise)
      t->promise->reject(reason);
    retire(id, 0);
  };
  session->staged = [weak, id] {
    const auto t = weak.lock();
    if (!t)
      return;
    t->done = true;
    LOG_INFO("edge drag: all dragged files received and verified");
    t->promise->resolve();
    if (t->dropped)
      retire(id, kKeepStagedMs);
  };
  session->sourceReady = [] {};
  transfers[id] = transfer;
  session->receive(packet); // validates the offer, creates the staging folder and asks for data
  if (transfer->done)
    return; // invalid offer; the session already told the sender
  QStringList names;
  qint64 total = 0;
  for (const auto &entry : packet.value(QStringLiteral("files")).toArray()) {
    names.append(entry.toMap().value(QStringLiteral("name")).toString());
    total += entry.toMap().value(QStringLiteral("size")).toInteger();
  }
  const auto staged = session->receivedPaths();
  // Drop targets may look at the paths while hovering, before the bytes arrive.
  for (const auto &path : staged) {
    QFile placeholder(path);
    if (!placeholder.exists() && placeholder.open(QIODevice::WriteOnly))
      placeholder.close();
  }
  transfer->promise = std::make_shared<FileDragPromise>(names, staged, total);
  LOG_INFO("edge drag: receiving %d file(s), %lld bytes", static_cast<int>(names.size()), static_cast<long long>(total));
  QTimer::singleShot(kUnclaimedMs, controller, [weak] {
    const auto t = weak.lock();
    if (t && !t->dragStarted && !t->done)
      t->session->cancel(QStringLiteral("The drag never reached this computer."));
  });
}

void handleStart(const QCborMap &packet, uint64_t inputSequence)
{
  const auto id = packet.value(QStringLiteral("id")).toString();
  const auto it = transfers.find(id);
  if (it == transfers.end()) {
    WindowMirror::handleStart(id);
    return;
  }
  if (it->second->outgoing || !it->second->promise || it->second->dragStarted)
    return;
  const auto transfer = it->second;
  if (transfer->promise->state() == FileDragPromise::State::Failed)
    return;
  transfer->dragStarted = true;
  LOG_INFO("edge drag: starting the drag on this computer");
  std::weak_ptr<DragTransfer> weak = transfer;
  NativeFileDragPlatform::startPromiseDrag(transfer->promise, [weak, id](bool dropped) {
    const auto t = weak.lock();
    if (!t)
      return;
    if (!dropped) {
      LOG_INFO("edge drag: the files were not dropped anywhere");
      if (!t->done)
        t->session->cancel(QStringLiteral("The drag was cancelled on the receiving computer."));
      retire(id, 0);
      return;
    }
    t->dropped = true;
    LOG_INFO("edge drag: the files were dropped on this computer");
    if (t->done)
      retire(id, kKeepStagedMs);
  }, inputSequence);
}

void handleDragPacket(const QString &peer, const QCborMap &packet, bool trusted, uint64_t inputSequence)
{
  const auto type = packet.value(QStringLiteral("type")).toString();
  const auto id = packet.value(QStringLiteral("id")).toString();
  if (type == "probe") {
    if (trusted)
      handleProbe(peer, packet);
    return;
  }
  if (type == "start") {
    if (trusted)
      handleStart(packet, inputSequence);
    return;
  }
  if (type == "offer") {
    handleOffer(peer, packet);
    return;
  }
  if (type == "window") {
    WindowMirror::handleWindowOffer(peer, packet);
    return;
  }
  if (type == "cancel") {
    if (probingId == id)
      probingId.clear();
    WindowMirror::cancel(id);
  }
  const auto it = transfers.find(id);
  if (it == transfers.end() || it->second->peer != peer)
    return;
  const auto transfer = it->second;
  if (type == "chunk" && transfer->promise)
    transfer->promise->addProgress(packet.value(QStringLiteral("data")).toByteArray().size());
  transfer->session->receive(packet);
}
} // namespace

void NativeFileDrag::create(QObject *parent)
{
  controller = new QObject(parent);
  WindowMirror::init(controller, [](const QString &peer, const QCborMap &packet) {
    if (const auto send = peers.value(peer))
      send(packet);
  });
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

void NativeFileDrag::attach(const QString &peer, Sender send)
{
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
        WindowMirror::detach(peer);
        for (const auto &[id, transfer] : transfers) {
          if (transfer->peer == peer)
            transfer->session->receive(
                {{QStringLiteral("type"), QStringLiteral("cancel")},
                 {QStringLiteral("id"), id},
                 {QStringLiteral("reason"), QStringLiteral("The other computer disconnected.")}}
            );
        }
      },
      Qt::QueuedConnection
  );
}

void NativeFileDrag::receive(const QString &peer, const QByteArray &bytes, bool trusted)
{
  if (!controller || bytes.size() > 2 * 1024 * 1024)
    return;
#ifdef Q_OS_WIN
  const auto inputSequence = NativeFileDragPlatform::dragInputSequence();
#else
  constexpr uint64_t inputSequence = 0;
#endif
  QMetaObject::invokeMethod(
      controller,
      [peer, bytes, trusted, inputSequence] {
        QCborParserError error;
        const auto value = QCborValue::fromCbor(bytes, &error);
        if (error.error != QCborError::NoError || !value.isMap() || !peers.contains(peer))
          return;
        const auto packet = value.toMap();
        if (packet.value(QStringLiteral("drag")).toBool()) {
          handleDragPacket(peer, packet, trusted, inputSequence);
          return;
        }
        if (packet.value(QStringLiteral("mirror")).toBool()) {
          WindowMirror::handlePacket(peer, packet);
          return;
        }
        if (!packet.value(QStringLiteral("relay")).toBool())
          return;
        ipcSendToClient(
            QStringLiteral("fileRelayPacket"),
            encoded({{QStringLiteral("peer"), peer}, {QStringLiteral("packet"), value}})
        );
      },
      Qt::QueuedConnection
  );
}

void NativeFileDrag::noteLeftDown()
{
  if (!controller)
    return;
  QMetaObject::invokeMethod(
      controller,
      [] {
        NativeFileDragPlatform::noteLeftDown();
        WindowMirrorPlatform::noteLeftDown();
      },
      Qt::QueuedConnection
  );
}

#if !defined(Q_OS_WIN) && !defined(Q_OS_MACOS)
// Edge drag is implemented for Windows and macOS only.
bool NativeFileDragPlatform::leftButtonDown()
{
  return false;
}
void NativeFileDragPlatform::noteLeftDown()
{
}
void NativeFileDragPlatform::probe(std::function<void(const QStringList &)> done)
{
  done({});
}
void NativeFileDragPlatform::releaseSourceDrag()
{
}
void NativeFileDragPlatform::startPromiseDrag(std::shared_ptr<FileDragPromise>, std::function<void(bool)> finished, uint64_t)
{
  finished(false);
}
#endif
