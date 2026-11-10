/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "WindowMirrorPlatform.h"

// Only macOS can hand a window over for now; this computer can still show mirrors of the
// other computer's windows (that part is plain Qt in WindowMirror.cpp).
void WindowMirrorPlatform::noteLeftDown()
{
}
std::optional<WindowMirrorPlatform::DraggedWindow> WindowMirrorPlatform::draggedWindow()
{
  return std::nullopt;
}
void WindowMirrorPlatform::endWindowDrag()
{
}
bool WindowMirrorPlatform::canCapture()
{
  return false;
}
bool WindowMirrorPlatform::hide(const DraggedWindow &)
{
  return false;
}
void WindowMirrorPlatform::restore(quint32)
{
}
void WindowMirrorPlatform::resize(quint32, QSizeF)
{
}
void WindowMirrorPlatform::activateForKeyboard(quint32)
{
}
void WindowMirrorPlatform::injectMouse(quint32, MouseAction, int, QPointF, int)
{
}
void WindowMirrorPlatform::injectWheel(quint32, QPointF, int, int)
{
}
std::unique_ptr<WindowMirrorPlatform::Capture>
WindowMirrorPlatform::startCapture(quint32, QSize, FrameCallback, StopCallback)
{
  return nullptr;
}
