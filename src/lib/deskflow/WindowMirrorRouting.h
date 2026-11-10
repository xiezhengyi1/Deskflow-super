/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <atomic>

// Shared between the macOS event tap, the server's core thread and the window-mirror
// controller. Set only on the computer that owns a mirrored window (the Mac server).
struct WindowMirrorRouting
{
  // The mirrored window has keyboard focus on the other computer: the event tap passes key
  // events through to the (activated) Mac app instead of sending them to the other computer.
  inline static std::atomic<bool> keyboardToMac{false};
  // The other computer's cursor is over a mirrored window: buttons and scrolling are also
  // injected into the Mac app.
  inline static std::atomic<bool> pointerInMirror{false};
};
