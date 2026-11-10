/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <QPoint>
#include <QStringList>
#include <cstdint>
#include <functional>
inline constexpr int64_t kNativeFileDragInputTag = 0x44464e4644524147LL;
namespace NativeFileDragPlatform {
QPoint cursorPosition();
bool leftButtonDown();
void cancelSourceDrag();
void startDrag(const QStringList &paths, std::function<void(bool)> finished);
} // namespace NativeFileDragPlatform
