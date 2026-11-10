/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include "FileTransferSession.h"
#include <QMap>
#include <QTimer>
#include <QWidget>
#include <functional>
#include <memory>
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QProgressBar;
class QPushButton;

// Runs on the Qt GUI thread; packets travel on the existing authenticated connection.
class FileRelayWindow : public QWidget
{
  Q_OBJECT
public:
  using Sender = std::function<void(const QCborMap &)>;
  explicit FileRelayWindow(QWidget *parent = nullptr);
  void setPeer(const QString &name, Sender send);
  void removePeer(const QString &name);
  void clearPeers();
  void receive(const QString &name, const QCborMap &packet);
  void addFiles(const QStringList &paths);
  // Replaces the outgoing list and sends it to the connected computer, waiting briefly for a connection.
  void sendToConnected(const QStringList &paths);
  void setDownloadDirectory(const QString &path);
  QStringList savedFiles() const
  {
    return m_savedFiles;
  }
  // Accept offers from connected computers without a click. Connected peers are already TLS-authenticated.
  void setAutoAccept(bool enabled);
  bool autoAccept() const
  {
    return m_autoAcceptEnabled;
  }

Q_SIGNALS:
  void autoAcceptChanged(bool enabled);
  // Emitted when a transfer ends; savedPaths is non-empty only for successfully received files.
  void transferFinished(const QString &message, bool success, const QStringList &savedPaths);

protected:
  void dragEnterEvent(QDragEnterEvent *event) override;
  void dropEvent(QDropEvent *event) override;

private:
  void updateControls();
  void refreshPeers();
  void startSend();
  void tryAutoSend();
  void acceptOffer();
  void rejectOffer(const QString &reason);
  void cancelTransfer();
  void setupSession(const QString &peer);
  void transmit(const QString &peer, QCborMap packet);
  void finish(const QString &message, bool success);
  void updateProgress();
  bool busy() const;
  QMap<QString, Sender> m_peers;
  QComboBox *m_peer;
  QCheckBox *m_autoAccept;
  QListWidget *m_outgoing;
  QListWidget *m_received;
  QLabel *m_status;
  QLabel *m_request;
  QLabel *m_folder;
  QProgressBar *m_progress;
  QPushButton *m_send;
  QPushButton *m_accept;
  QPushButton *m_reject;
  QPushButton *m_cancel;
  QTimer m_timeout;
  QTimer m_autoSendTimer;
  QString m_downloadDirectory;
  QString m_activePeer;
  QString m_pendingPeer;
  QCborMap m_pendingOffer;
  std::unique_ptr<FileTransferSession> m_session;
  QStringList m_savedFiles;
  qint64 m_total = 0;
  qint64 m_done = 0;
  bool m_active = false;
  bool m_autoSend = false;
  bool m_autoAcceptEnabled = true;
  QStringList m_lastSaved;
};
