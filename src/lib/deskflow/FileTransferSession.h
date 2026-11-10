/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QCborMap>
#include <QCryptographicHash>
#include <QFile>
#include <QPoint>
#include <QSaveFile>
#include <QStringList>
#include <QTemporaryDir>
#include <functional>
#include <memory>
#include <vector>

// Stop-and-wait chunks bound queued data while leaving the input connection responsive.
class FileTransferSession
{
public:
  static constexpr qint64 chunkSize = 64 * 1024;
  std::function<void(const QCborMap &)> send;
  std::function<void(const QString &)> failed;
  std::function<void()> sourceReady;
  std::function<void()> staged;
  bool begin(const QStringList &paths, int edge, QPoint position);
  void receive(const QCborMap &packet);
  void cancel(const QString &reason);
  QStringList receivedPaths() const;
  // Persist verified files in a unique folder; never overwrite existing files.
  QStringList saveReceivedFiles(const QString &directory, QString *error = nullptr);
  QString id() const
  {
    return m_id;
  }
  int edge() const
  {
    return m_edge;
  }
  QPoint position() const
  {
    return m_position;
  }
  bool isSending() const
  {
    return m_sending;
  }
  bool isStaged() const
  {
    return m_staged;
  }
  static bool validName(const QString &name);

private:
  void nextChunk();
  void fail(const QString &reason);
  void emitPacket(QCborMap packet);
  QString m_id;
  int m_edge = 0;
  QPoint m_position;
  bool m_sending = false;
  bool m_staged = false;
  int m_index = 0;
  qint64 m_offset = 0;
  QStringList m_paths;
  QStringList m_names;
  std::vector<qint64> m_sizes;
  QFile m_source;
  std::unique_ptr<QSaveFile> m_destination;
  std::unique_ptr<QTemporaryDir> m_directory;
  QCryptographicHash m_hash{QCryptographicHash::Sha256};
};
