/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "NativeFileDragPlatform.h"
#include <QCursor>
#include <QDrag>
#include <QMimeData>
#include <QTimer>
#include <QUrl>
#include <QWidget>
#include <Windows.h>

QPoint NativeFileDragPlatform::cursorPosition()
{
  POINT point;
  GetCursorPos(&point);
  return QPoint(point.x, point.y);
}
bool NativeFileDragPlatform::leftButtonDown()
{
  return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}
void NativeFileDragPlatform::cancelSourceDrag()
{
  INPUT input[3]{};
  input[0].type = INPUT_KEYBOARD;
  input[0].ki.wVk = VK_ESCAPE;
  input[0].ki.dwExtraInfo = static_cast<ULONG_PTR>(kNativeFileDragInputTag);
  input[1] = input[0];
  input[1].ki.dwFlags = KEYEVENTF_KEYUP;
  input[2].type = INPUT_MOUSE;
  input[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
  input[2].mi.dwExtraInfo = static_cast<ULONG_PTR>(kNativeFileDragInputTag);
  SendInput(3, input, sizeof(INPUT));
}
void NativeFileDragPlatform::startDrag(const QStringList &paths, std::function<void(bool)> finished)
{
  auto *source = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
  source->setAttribute(Qt::WA_ShowWithoutActivating);
  source->setGeometry(QRect(QCursor::pos(), QSize(1, 1)));
  source->show();
  INPUT down{};
  down.type = INPUT_MOUSE;
  down.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
  down.mi.dwExtraInfo = static_cast<ULONG_PTR>(kNativeFileDragInputTag);
  SendInput(1, &down, sizeof(INPUT));
  QTimer::singleShot(0, source, [source, paths, finished = std::move(finished)] {
    auto *mime = new QMimeData;
    QList<QUrl> urls;
    for (const auto &path : paths)
      urls.append(QUrl::fromLocalFile(path));
    mime->setUrls(urls);
    auto *drag = new QDrag(source);
    drag->setMimeData(mime);
    const bool copied = drag->exec(Qt::CopyAction, Qt::CopyAction) == Qt::CopyAction;
    source->hide();
    source->deleteLater();
    finished(copied);
  });
}
