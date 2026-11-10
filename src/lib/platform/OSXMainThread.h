/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QCoreApplication>
#include <QThread>
#include <type_traits>
#include <utility>

inline bool osxNeedsMainThread()
{
  const auto *app = QCoreApplication::instance();
  return app && QThread::currentThread() != app->thread();
}

// TIS/TSM must share the main thread with Cocoa and Qt's input method handling.
// A mutex among Deskflow callers cannot serialize calls made internally by Qt.
template <class Function> std::invoke_result_t<Function> onOSXMainThread(Function &&function)
{
  if (!osxNeedsMainThread())
    return function();
  if constexpr (std::is_void_v<std::invoke_result_t<Function>>) {
    QMetaObject::invokeMethod(
        QCoreApplication::instance(), std::forward<Function>(function), Qt::BlockingQueuedConnection
    );
  } else {
    std::invoke_result_t<Function> result{};
    QMetaObject::invokeMethod(QCoreApplication::instance(), [&] { result = function(); }, Qt::BlockingQueuedConnection);
    return result;
  }
}
