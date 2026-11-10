/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QString>
#include <QStringList>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

// Input injected by the drag code carries this tag so the Deskflow input hooks ignore it.
inline constexpr int64_t kNativeFileDragInputTag = 0x44464e4644524147LL;

// Files handed to a native drop target before all of their bytes have arrived.
// Resolved on the controller thread; waited on by the platform drop code from any thread.
class FileDragPromise
{
public:
  enum class State
  {
    Pending,
    Ready,
    Failed
  };

  FileDragPromise(QStringList fileNames, QStringList staged, qint64 totalBytes)
      : names(std::move(fileNames)),
        stagedPaths(std::move(staged)),
        total(totalBytes)
  {
  }

  const QStringList names;
  const QStringList stagedPaths; // same order as names; complete only once Ready
  const qint64 total;

  void addProgress(qint64 bytes)
  {
    m_received += bytes;
  }
  qint64 received() const
  {
    return m_received;
  }
  void resolve()
  {
    settle(State::Ready, {});
  }
  void reject(const QString &reason)
  {
    settle(State::Failed, reason);
  }
  State state() const
  {
    std::scoped_lock lock(m_mutex);
    return m_state;
  }
  QString error() const
  {
    std::scoped_lock lock(m_mutex);
    return m_error;
  }
  // Blocks until the promise settles or the timeout passes; returns the state at that point.
  State wait(int timeoutMs) const
  {
    std::unique_lock lock(m_mutex);
    m_settled.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] { return m_state != State::Pending; });
    return m_state;
  }

private:
  void settle(State state, const QString &error)
  {
    {
      std::scoped_lock lock(m_mutex);
      if (m_state != State::Pending)
        return;
      m_state = state;
      m_error = error;
    }
    m_settled.notify_all();
  }

  mutable std::mutex m_mutex;
  mutable std::condition_variable m_settled;
  State m_state = State::Pending;
  QString m_error;
  std::atomic<qint64> m_received{0};
};

// Implemented per platform; every function runs on the core process's Qt main thread.
namespace NativeFileDragPlatform {
bool leftButtonDown();
// Called when the left button goes down, before any drag can start on this computer.
void noteLeftDown();
// Reports the local files the user is dragging right now, or an empty list when the
// held button is not a file drag. May answer asynchronously.
void probe(std::function<void(const QStringList &paths)> done);
// Ends the local drag whose files were claimed by probe(), without dropping them anywhere.
void releaseSourceDrag();
// Starts a native drag of the promised files under the cursor while the button is held.
// finished(true) means the files were dropped on a target that accepted them.
void startPromiseDrag(
    std::shared_ptr<FileDragPromise> promise, std::function<void(bool dropped)> finished, uint64_t inputSequence
);
#ifdef Q_OS_WIN
// Input threads forward remote drag input to the active OLE message queue.
uint64_t dragInputSequence();
void moveReceivedDrag();
bool releaseReceivedDrag();
void cancelReceivedDrag();
#endif
} // namespace NativeFileDragPlatform
