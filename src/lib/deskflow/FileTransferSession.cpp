/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "FileTransferSession.h"
#include <QCborArray>
#include <QDir>
#include <QFileInfo>
#include <QSet>
#include <QUuid>

bool FileTransferSession::validName(const QString &name)
{
  if (name.isEmpty() || name == "." || name == ".." || name.endsWith('.') || name.endsWith(' '))
    return false;
  for (QChar c : name) {
    if (c.unicode() < 32 || QStringLiteral("/\\:<>\"|?*").contains(c))
      return false;
  }
  const auto base = name.section('.', 0, 0).toUpper();
  static const QSet<QString> reserved = {"CON",  "PRN",  "AUX",  "NUL",  "COM1", "COM2", "COM3", "COM4",
                                         "COM5", "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3",
                                         "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
  return !reserved.contains(base);
}

void FileTransferSession::emitPacket(QCborMap packet)
{
  packet.insert(QStringLiteral("id"), m_id);
  send(packet);
}

bool FileTransferSession::begin(const QStringList &paths, int edge, QPoint position)
{
  m_id = QUuid::createUuid().toString(QUuid::WithoutBraces);
  m_paths = paths;
  m_edge = edge;
  m_position = position;
  QCborArray files;
  QSet<QString> names;
  if (paths.isEmpty() || paths.size() > 1024) {
    fail("Select between 1 and 1024 regular files.");
    return false;
  }
  for (const auto &path : paths) {
    QFileInfo info(path);
    const auto name = info.fileName().normalized(QString::NormalizationForm_C);
    if (!info.isFile() || info.isSymLink() || !validName(name) || names.contains(name.toCaseFolded())) {
      fail("Only regular files with unique Windows/macOS compatible names can be dragged.");
      return false;
    }
    names.insert(name.toCaseFolded());
    m_names.append(name);
    m_sizes.push_back(info.size());
    files.append(QCborMap{{QStringLiteral("name"), name}, {QStringLiteral("size"), info.size()}});
  }
  m_sending = true;
  emitPacket(
      QCborMap{
          {QStringLiteral("type"), QStringLiteral("offer")},
          {QStringLiteral("files"), files},
          {QStringLiteral("edge"), edge},
          {QStringLiteral("x"), position.x()},
          {QStringLiteral("y"), position.y()}
      }
  );
  return true;
}

void FileTransferSession::nextChunk()
{
  if (!m_source.isOpen()) {
    m_source.setFileName(m_paths.at(m_index));
    if (!m_source.open(QIODevice::ReadOnly)) {
      fail(m_source.errorString());
      return;
    }
    m_hash.reset();
    m_offset = 0;
  }
  const auto data = m_source.read(chunkSize);
  if (m_source.error() != QFileDevice::NoError) {
    fail(m_source.errorString());
    return;
  }
  m_hash.addData(data);
  m_offset += data.size();
  const bool end = m_source.atEnd();
  if (m_offset > m_sizes.at(m_index) || (end && m_offset != m_sizes.at(m_index))) {
    fail("The source file changed size during transfer.");
    return;
  }
  QCborMap packet{
      {QStringLiteral("type"), QStringLiteral("chunk")},
      {QStringLiteral("index"), m_index},
      {QStringLiteral("data"), data},
      {QStringLiteral("end"), end}
  };
  if (end) {
    packet.insert(QStringLiteral("sha256"), m_hash.result());
    m_source.close();
    ++m_index;
    m_offset = 0;
  }
  emitPacket(packet);
}

void FileTransferSession::receive(const QCborMap &packet)
{
  const auto type = packet.value(QStringLiteral("type")).toString();
  if (type == "offer") {
    m_id = packet.value(QStringLiteral("id")).toString();
    m_edge = static_cast<int>(packet.value(QStringLiteral("edge")).toInteger());
    m_position = QPoint(
        static_cast<int>(packet.value(QStringLiteral("x")).toInteger()),
        static_cast<int>(packet.value(QStringLiteral("y")).toInteger())
    );
    const auto files = packet.value(QStringLiteral("files")).toArray();
    if (m_id.isEmpty() || files.isEmpty() || files.size() > 1024 || m_edge < 1 || m_edge > 4) {
      fail("Invalid file offer.");
      return;
    }
    QSet<QString> names;
    for (const auto &entry : files) {
      const auto file = entry.toMap();
      const auto name = file.value(QStringLiteral("name")).toString();
      const auto size = file.value(QStringLiteral("size")).toInteger(-1);
      if (!validName(name) || size < 0 ||
          names.contains(name.normalized(QString::NormalizationForm_C).toCaseFolded())) {
        fail("Invalid or duplicate file name or size.");
        return;
      }
      names.insert(name.normalized(QString::NormalizationForm_C).toCaseFolded());
      m_names.append(name);
      m_sizes.push_back(size);
    }
    m_directory = std::make_unique<QTemporaryDir>(QDir::tempPath() + "/deskflow-drag-XXXXXX");
    if (!m_directory->isValid()) {
      fail("Cannot create the drag staging directory.");
      return;
    }
    emitPacket(QCborMap{{QStringLiteral("type"), QStringLiteral("ack")}});
    return;
  }
  if (packet.value(QStringLiteral("id")).toString() != m_id)
    return; // An earlier cancelled transfer can still have packets in flight.
  if (type == "cancel") {
    m_sending = false;
    m_staged = false;
    m_source.close();
    m_destination.reset();
    m_directory.reset();
    failed(packet.value(QStringLiteral("reason")).toString());
  } else if (type == "ack" && m_sending) {
    if (m_index >= static_cast<int>(m_paths.size())) {
      fail("Unexpected file acknowledgement.");
      return;
    }
    nextChunk();
  } else if (type == "ready" && m_sending) {
    if (m_index != static_cast<int>(m_paths.size())) {
      fail("Premature file completion.");
      return;
    }
    m_sending = false;
    sourceReady();
  } else if (type == "chunk") {
    const int index = static_cast<int>(packet.value(QStringLiteral("index")).toInteger(-1));
    const auto data = packet.value(QStringLiteral("data")).toByteArray();
    if (!m_directory || index != m_index || m_index >= static_cast<int>(m_sizes.size()) || data.size() > chunkSize) {
      fail("Unexpected file chunk.");
      return;
    }
    if (!m_destination) {
      m_destination = std::make_unique<QSaveFile>(m_directory->filePath(m_names.at(m_index)));
      if (!m_destination->open(QIODevice::WriteOnly)) {
        fail(m_destination->errorString());
        return;
      }
      m_hash.reset();
    }
    if (m_offset + data.size() > m_sizes.at(m_index) || m_destination->write(data) != data.size()) {
      fail("File size exceeded or staging write failed.");
      return;
    }
    m_hash.addData(data);
    m_offset += data.size();
    if (packet.value(QStringLiteral("end")).toBool()) {
      if (m_offset != m_sizes.at(m_index) || m_hash.result() != packet.value(QStringLiteral("sha256")).toByteArray()) {
        fail("File size or SHA-256 verification failed.");
        return;
      }
      if (!m_destination->commit()) {
        fail(m_destination->errorString());
        return;
      }
      m_destination.reset();
      m_offset = 0;
      ++m_index;
    }
    m_staged = m_index == static_cast<int>(m_sizes.size());
    emitPacket(QCborMap{{QStringLiteral("type"), m_staged ? QStringLiteral("ready") : QStringLiteral("ack")}});
    if (m_staged)
      staged();
  }
}

void FileTransferSession::cancel(const QString &reason)
{
  fail(reason);
}
void FileTransferSession::fail(const QString &reason)
{
  m_sending = false;
  m_staged = false;
  m_source.close();
  m_destination.reset();
  m_directory.reset();
  emitPacket(QCborMap{{QStringLiteral("type"), QStringLiteral("cancel")}, {QStringLiteral("reason"), reason}});
  failed(reason);
}

QStringList FileTransferSession::saveReceivedFiles(const QString &directory, QString *error)
{
  const auto failSave = [error](const QString &message) -> QStringList {
    if (error) *error = message;
    return {};
  };
  if (!m_staged || !m_directory)
    return failSave(QStringLiteral("The received files have not passed verification."));
  if (!QDir().mkpath(directory))
    return failSave(QStringLiteral("Cannot create the receive folder."));
  QTemporaryDir destination(QDir(directory).filePath(QStringLiteral("Transfer-XXXXXX")));
  if (!destination.isValid())
    return failSave(QStringLiteral("Cannot create a unique receive folder."));
  QStringList paths;
  for (const auto &name : m_names) {
    const auto source = m_directory->filePath(name);
    const auto target = destination.filePath(name);
    if (!QFile::rename(source, target) && !QFile::copy(source, target))
      return failSave(QStringLiteral("Cannot save a received file."));
    paths.append(target);
  }
  destination.setAutoRemove(false);
  return paths;
}

QStringList FileTransferSession::receivedPaths() const
{
  QStringList paths;
  for (const auto &name : m_names)
    paths.append(m_directory->filePath(name));
  return paths;
}
