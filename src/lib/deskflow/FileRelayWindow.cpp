/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileRelayWindow.h"
#include <QCborArray>
#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <limits>

namespace {
QStringList localPaths(const QMimeData *data)
{
  QStringList paths;
  if (!data->hasUrls())
    return paths;
  for (const auto &url : data->urls()) {
    if (!url.isLocalFile())
      return {};
    paths.append(url.toLocalFile());
  }
  return paths;
}
class ReceivedFileList : public QListWidget
{
public:
  using QListWidget::QListWidget;

protected:
  QStringList mimeTypes() const override
  {
    return {QStringLiteral("text/uri-list")};
  }
  QMimeData *mimeData(const QList<QListWidgetItem *> &items) const override
  {
    auto *data = new QMimeData;
    QList<QUrl> urls;
    for (auto *item : items)
      urls.append(QUrl::fromLocalFile(item->data(Qt::UserRole).toString()));
    data->setUrls(urls);
    return data;
  }
  Qt::DropActions supportedDropActions() const override
  {
    return Qt::CopyAction;
  }
};
qint64 offerSize(const QCborMap &packet)
{
  qint64 total = 0;
  for (const auto &item : packet.value(QStringLiteral("files")).toArray()) {
    const auto size = item.toMap().value(QStringLiteral("size")).toInteger(-1);
    if (size < 0 || size > std::numeric_limits<qint64>::max() - total)
      return -1;
    total += size;
  }
  return total;
}
} // namespace

FileRelayWindow::FileRelayWindow(QWidget *parent) : QWidget(parent, Qt::Window)
{
  setWindowTitle(tr("File relay"));
  setAttribute(Qt::WA_QuitOnClose, false);
  setObjectName(QStringLiteral("fileRelayWindow"));
  setAcceptDrops(true);
  resize(640, 620);
  auto *layout = new QVBoxLayout(this);
  auto *help = new QLabel(
      tr("Drop files here, then choose a connected computer and click Send.\nThe other computer accepts the transfer "
         "in its relay window."),
      this
  );
  help->setWordWrap(true);
  layout->addWidget(help);
  auto *peerRow = new QHBoxLayout;
  peerRow->addWidget(new QLabel(tr("Send to:"), this));
  m_peer = new QComboBox(this);
  m_peer->setObjectName(QStringLiteral("relayPeer"));
  peerRow->addWidget(m_peer, 1);
  layout->addLayout(peerRow);
  m_outgoing = new QListWidget(this);
  m_outgoing->setObjectName(QStringLiteral("outgoingFiles"));
  m_outgoing->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_outgoing->setMinimumHeight(90);
  layout->addWidget(m_outgoing, 1);
  auto *sendRow = new QHBoxLayout;
  auto *add = new QPushButton(tr("Choose files..."), this);
  auto *clear = new QPushButton(tr("Clear"), this);
  m_send = new QPushButton(tr("Send"), this);
  m_send->setObjectName(QStringLiteral("sendFiles"));
  sendRow->addWidget(add);
  sendRow->addWidget(clear);
  sendRow->addStretch();
  sendRow->addWidget(m_send);
  layout->addLayout(sendRow);
  connect(add, &QPushButton::clicked, this, [this] {
    addFiles(QFileDialog::getOpenFileNames(this, tr("Choose files")));
  });
  connect(clear, &QPushButton::clicked, this, [this] {
    if (!busy()) {
      m_outgoing->clear();
      updateControls();
    }
  });
  connect(m_send, &QPushButton::clicked, this, &FileRelayWindow::startSend);
  m_request = new QLabel(this);
  m_request->setObjectName(QStringLiteral("incomingRequest"));
  m_request->setWordWrap(true);
  layout->addWidget(m_request);
  auto *receiveRow = new QHBoxLayout;
  m_accept = new QPushButton(tr("Receive"), this);
  m_accept->setObjectName(QStringLiteral("receiveFiles"));
  m_reject = new QPushButton(tr("Reject"), this);
  m_reject->setObjectName(QStringLiteral("rejectFiles"));
  m_cancel = new QPushButton(tr("Cancel transfer"), this);
  m_cancel->setObjectName(QStringLiteral("cancelTransfer"));
  receiveRow->addWidget(m_accept);
  receiveRow->addWidget(m_reject);
  receiveRow->addStretch();
  receiveRow->addWidget(m_cancel);
  layout->addLayout(receiveRow);
  connect(m_accept, &QPushButton::clicked, this, &FileRelayWindow::acceptOffer);
  connect(m_reject, &QPushButton::clicked, this, [this] { rejectOffer(tr("The receiver rejected the transfer.")); });
  connect(m_cancel, &QPushButton::clicked, this, &FileRelayWindow::cancelTransfer);
  m_progress = new QProgressBar(this);
  m_progress->setObjectName(QStringLiteral("transferProgress"));
  m_progress->setRange(0, 100);
  m_progress->setValue(0);
  layout->addWidget(m_progress);
  m_status = new QLabel(tr("Connect both computers and enable File relay on both ends."), this);
  m_status->setObjectName(QStringLiteral("transferStatus"));
  m_status->setWordWrap(true);
  layout->addWidget(m_status);
  m_folder = new QLabel(this);
  m_folder->setWordWrap(true);
  layout->addWidget(m_folder);
  auto *folderRow = new QHBoxLayout;
  auto *chooseFolder = new QPushButton(tr("Receive folder..."), this);
  auto *openFolder = new QPushButton(tr("Open receive folder"), this);
  folderRow->addWidget(chooseFolder);
  folderRow->addWidget(openFolder);
  folderRow->addStretch();
  layout->addLayout(folderRow);
  connect(chooseFolder, &QPushButton::clicked, this, [this] {
    if (busy())
      return;
    const auto folder = QFileDialog::getExistingDirectory(this, tr("Receive folder"), m_downloadDirectory);
    if (!folder.isEmpty())
      setDownloadDirectory(folder);
  });
  connect(openFolder, &QPushButton::clicked, this, [this] {
    QDir().mkpath(m_downloadDirectory);
    QDesktopServices::openUrl(QUrl::fromLocalFile(m_downloadDirectory));
  });
  layout->addWidget(new QLabel(tr("Received files (double-click to open, or drag to a local folder):"), this));
  m_received = new ReceivedFileList(this);
  m_received->setDragEnabled(true);
  m_received->setSelectionMode(QAbstractItemView::ExtendedSelection);
  m_received->setDragDropMode(QAbstractItemView::DragOnly);
  m_received->setDefaultDropAction(Qt::CopyAction);
  m_received->setObjectName(QStringLiteral("receivedFiles"));
  layout->addWidget(m_received, 1);
  connect(m_received, &QListWidget::itemDoubleClicked, this, [](QListWidgetItem *item) {
    QDesktopServices::openUrl(QUrl::fromLocalFile(item->data(Qt::UserRole).toString()));
  });
  auto downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
  if (downloads.isEmpty())
    downloads = QDir::homePath();
  setDownloadDirectory(QDir(downloads).filePath(QStringLiteral("Deskflow")));
  m_timeout.setSingleShot(true);
  connect(&m_timeout, &QTimer::timeout, this, [this] {
    if (!m_pendingOffer.isEmpty())
      rejectOffer(tr("The transfer timed out."));
    else if (m_active && m_session)
      m_session->cancel(tr("The transfer timed out."));
  });
  m_autoSendTimer.setSingleShot(true);
  connect(&m_autoSendTimer, &QTimer::timeout, this, [this] {
    if (!m_autoSend)
      return;
    m_autoSend = false;
    m_status->setText(tr("No connected computer was found. Connect Deskflow, then click Send."));
  });
  updateControls();
}

bool FileRelayWindow::busy() const
{
  return m_active || !m_pendingOffer.isEmpty();
}
void FileRelayWindow::setDownloadDirectory(const QString &path)
{
  if (busy())
    return;
  m_downloadDirectory = QDir(path).absolutePath();
  m_folder->setText(tr("Receive folder: %1").arg(QDir::toNativeSeparators(m_downloadDirectory)));
}
void FileRelayWindow::setPeer(const QString &name, Sender send)
{
  m_peers.insert(name, std::move(send));
  refreshPeers();
}
void FileRelayWindow::clearPeers()
{
  const auto names = m_peers.keys();
  for (const auto &name : names)
    removePeer(name);
}
void FileRelayWindow::removePeer(const QString &name)
{
  m_peers.remove(name);
  if (m_pendingPeer == name && !m_pendingOffer.isEmpty()) {
    m_pendingOffer.clear();
    m_pendingPeer.clear();
    finish(tr("The other computer disconnected."), false);
  }
  if (m_activePeer == name && m_active && m_session)
    m_session->cancel(tr("The other computer disconnected."));
  refreshPeers();
}
void FileRelayWindow::refreshPeers()
{
  const auto selected = m_peer->currentText();
  m_peer->clear();
  m_peer->addItems(m_peers.keys());
  if (m_peers.contains(selected))
    m_peer->setCurrentText(selected);
  updateControls();
  tryAutoSend();
}
void FileRelayWindow::updateControls()
{
  m_send->setEnabled(!busy() && !m_peers.isEmpty() && m_outgoing->count() > 0);
  m_peer->setEnabled(!busy());
  m_accept->setVisible(!m_pendingOffer.isEmpty());
  m_reject->setVisible(!m_pendingOffer.isEmpty());
  m_request->setVisible(!m_pendingOffer.isEmpty());
  m_cancel->setEnabled(busy());
}
void FileRelayWindow::addFiles(const QStringList &paths)
{
  if (busy())
    return;
  for (const auto &path : paths) {
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink()) {
      m_status->setText(tr("Choose regular files; folders and symbolic links are not supported."));
      continue;
    }
    const auto absolute = info.absoluteFilePath();
    bool duplicate = false;
    for (int i = 0; i < m_outgoing->count(); ++i)
      if (m_outgoing->item(i)->data(Qt::UserRole).toString() == absolute)
        duplicate = true;
    if (duplicate)
      continue;
    auto *item = new QListWidgetItem(info.fileName(), m_outgoing);
    item->setToolTip(absolute);
    item->setData(Qt::UserRole, absolute);
  }
  updateControls();
}
void FileRelayWindow::sendToConnected(const QStringList &paths)
{
  show();
  raise();
  activateWindow();
  if (busy()) {
    m_status->setText(tr("A transfer is already in progress. Send the files again when it finishes."));
    return;
  }
  m_autoSend = false;
  m_autoSendTimer.stop();
  m_outgoing->clear();
  addFiles(paths);
  if (m_outgoing->count() == 0) {
    if (paths.isEmpty())
      m_status->setText(tr("No files to send."));
    return;
  }
  m_autoSend = true;
  m_autoSendTimer.start(30000);
  m_status->setText(tr("Waiting for a connected computer..."));
  tryAutoSend();
}
void FileRelayWindow::tryAutoSend()
{
  if (!m_autoSend || busy() || m_peers.isEmpty())
    return;
  m_autoSend = false;
  m_autoSendTimer.stop();
  if (m_peers.size() > 1) {
    m_status->setText(tr("Several computers are connected. Choose one and click Send."));
    return;
  }
  m_peer->setCurrentText(m_peers.firstKey());
  startSend();
}
void FileRelayWindow::dragEnterEvent(QDragEnterEvent *event)
{
  if (!busy() && !localPaths(event->mimeData()).isEmpty())
    event->acceptProposedAction();
}
void FileRelayWindow::dropEvent(QDropEvent *event)
{
  if (busy())
    return;
  const auto paths = localPaths(event->mimeData());
  if (paths.isEmpty())
    return;
  addFiles(paths);
  event->setDropAction(Qt::CopyAction);
  event->accept();
}
void FileRelayWindow::transmit(const QString &peer, QCborMap packet)
{
  packet.insert(QStringLiteral("relay"), true);
  const auto send = m_peers.value(peer);
  if (send)
    send(packet);
}
void FileRelayWindow::setupSession(const QString &peer)
{
  m_session = std::make_unique<FileTransferSession>();
  m_activePeer = peer;
  m_active = true;
  m_done = 0;
  m_session->send = [this, peer](const QCborMap &packet) {
    const auto type = packet.value(QStringLiteral("type")).toString();
    if (type == "chunk") {
      m_done += packet.value(QStringLiteral("data")).toByteArray().size();
      updateProgress();
    }
    if (type == "ready") {
      QString error;
      const auto paths = m_session->saveReceivedFiles(m_downloadDirectory, &error);
      if (paths.isEmpty()) {
        m_session->cancel(error);
        return;
      }
      m_savedFiles.append(paths);
      for (const auto &path : paths) {
        auto *item = new QListWidgetItem(QFileInfo(path).fileName(), m_received);
        item->setToolTip(path);
        item->setData(Qt::UserRole, path);
      }
      transmit(peer, packet);
      finish(
          tr("Received %1 file(s). Saved to %2").arg(paths.size()).arg(QFileInfo(paths.first()).absolutePath()), true
      );
      return;
    }
    transmit(peer, packet);
  };
  m_session->failed = [this](const QString &reason) { finish(tr("Transfer stopped: %1").arg(reason), false); };
  m_session->sourceReady = [this] { finish(tr("Sent successfully. The other computer saved the files."), true); };
  m_session->staged = [] {};
  m_timeout.start(120000);
  updateControls();
}
void FileRelayWindow::startSend()
{
  if (busy() || !m_peers.contains(m_peer->currentText()))
    return;
  QStringList paths;
  m_total = 0;
  for (int i = 0; i < m_outgoing->count(); ++i) {
    const auto path = m_outgoing->item(i)->data(Qt::UserRole).toString();
    const auto size = QFileInfo(path).size();
    if (size < 0 || size > std::numeric_limits<qint64>::max() - m_total)
      return;
    paths.append(path);
    m_total += size;
  }
  if (paths.isEmpty())
    return;
  setupSession(m_peer->currentText());
  m_status->setText(tr("Waiting for the other computer to click Receive..."));
  m_progress->setValue(0);
  m_session->begin(paths, 1, {});
}
void FileRelayWindow::acceptOffer()
{
  if (m_pendingOffer.isEmpty() || !m_peers.contains(m_pendingPeer))
    return;
  const auto packet = m_pendingOffer;
  const auto peer = m_pendingPeer;
  m_pendingOffer.clear();
  m_pendingPeer.clear();
  m_total = offerSize(packet);
  setupSession(peer);
  m_status->setText(tr("Receiving and verifying files..."));
  m_progress->setValue(0);
  m_session->receive(packet);
}
void FileRelayWindow::rejectOffer(const QString &reason)
{
  if (m_pendingOffer.isEmpty())
    return;
  transmit(
      m_pendingPeer, {{QStringLiteral("type"), QStringLiteral("cancel")},
                      {QStringLiteral("id"), m_pendingOffer.value(QStringLiteral("id"))},
                      {QStringLiteral("reason"), reason}}
  );
  m_pendingOffer.clear();
  m_pendingPeer.clear();
  finish(reason, false);
}
void FileRelayWindow::cancelTransfer()
{
  if (!m_pendingOffer.isEmpty())
    rejectOffer(tr("The transfer was cancelled."));
  else if (m_active && m_session)
    m_session->cancel(tr("The transfer was cancelled."));
}
void FileRelayWindow::finish(const QString &message, bool success)
{
  m_active = false;
  m_timeout.stop();
  m_status->setText(message);
  if (success)
    m_progress->setValue(100);
  updateControls();
}
void FileRelayWindow::updateProgress()
{
  const auto percent = m_total > 0 ? int(std::min(99.0, double(m_done) * 100.0 / double(m_total))) : 0;
  m_progress->setValue(percent);
}
void FileRelayWindow::receive(const QString &name, const QCborMap &packet)
{
  if (!m_peers.contains(name) || !packet.value(QStringLiteral("relay")).toBool())
    return;
  const auto type = packet.value(QStringLiteral("type")).toString();
  if (type == "offer") {
    const auto files = packet.value(QStringLiteral("files")).toArray();
    if (busy() || packet.value(QStringLiteral("id")).toString().isEmpty() || files.isEmpty() || files.size() > 1024 ||
        offerSize(packet) < 0) {
      transmit(
          name, {{QStringLiteral("type"), QStringLiteral("cancel")},
                 {QStringLiteral("id"), packet.value(QStringLiteral("id"))},
                 {QStringLiteral("reason"), tr("The receiver is busy or the file offer is invalid.")}}
      );
      return;
    }
    m_pendingOffer = packet;
    m_pendingPeer = name;
    m_request->setText(tr("%1 wants to send %2 file(s), %3 bytes. Click Receive to save them.")
                           .arg(name)
                           .arg(files.size())
                           .arg(offerSize(packet)));
    m_status->setText(tr("Incoming file transfer."));
    m_timeout.start(120000);
    updateControls();
    show();
    return;
  }
  if (!m_pendingOffer.isEmpty() && name == m_pendingPeer &&
      packet.value(QStringLiteral("id")) == m_pendingOffer.value(QStringLiteral("id")) && type == "cancel") {
    m_pendingOffer.clear();
    m_pendingPeer.clear();
    finish(tr("The transfer was cancelled."), false);
    return;
  }
  if (!m_active || !m_session || name != m_activePeer ||
      packet.value(QStringLiteral("id")).toString() != m_session->id())
    return;
  m_timeout.start(120000);
  if (type == "chunk") {
    m_done += packet.value(QStringLiteral("data")).toByteArray().size();
    updateProgress();
  }
  if (type == "ack")
    m_status->setText(tr("Sending files..."));
  m_session->receive(packet);
}
