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
  // A connected computer can show mirrored windows: holding Control+Option while pressing
  // the left button grabs the window under the cursor (see OSXComputer's event tap).
  inline static std::atomic<bool> grabEnabled{false};
  // A Control+Option window grab is in progress: the press never reached the app, and
  // Deskflow moves the window with the cursor until the button is released.
  inline static std::atomic<bool> grabbing{false};
  // Viewer side: counts left-button releases sent by the server. A mirror window that arrived
  // with the button held follows the cursor until the next one (no button is down locally).
  inline static std::atomic<unsigned> remoteLeftUps{0};
  // The pointer is on the computer that owns the mirrored windows (set by its screen). Input
  // the viewer reported a moment ago is then stale and must not move the real cursor.
  inline static std::atomic<bool> ownerActive{true};
};
