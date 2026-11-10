/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "base/Event.h"

#include <algorithm>
#include <cstdint>
#include <vector>

// A position sampled after the client's local cursor tools have processed input.
struct LocalCursorPosition : EventData
{
  uint32_t sequence = 0;
  int32_t x = 0;
  int32_t y = 0;
  int32_t dx = 0;
  int32_t dy = 0;
  // -1 denotes a legacy report with no physical monitor information.
  int32_t edges = -1;
};

struct LocalCursorMonitor
{
  int32_t x, y, width, height;
};

inline constexpr int32_t kLocalCursorLeft = 1;
inline constexpr int32_t kLocalCursorRight = 2;
inline constexpr int32_t kLocalCursorTop = 4;
inline constexpr int32_t kLocalCursorBottom = 8;

// Monitor coordinates must use the same native pixel space as the cursor.
std::vector<LocalCursorMonitor> localCursorMonitors();

// A neighboring pixel occupied by another monitor is an internal seam.
inline int32_t localCursorExposedEdges(int32_t x, int32_t y, const std::vector<LocalCursorMonitor> &monitors)
{
  const auto covered = [&](int64_t px, int64_t py) {
    for (const auto &m : monitors)
      if (m.width > 0 && m.height > 0 && px >= m.x && py >= m.y &&
          px < int64_t(m.x) + m.width && py < int64_t(m.y) + m.height)
        return true;
    return false;
  };
  if (!covered(x, y))
    return -1;
  int32_t edges = 0;
  if (!covered(int64_t(x) - 1, y)) edges |= kLocalCursorLeft;
  if (!covered(int64_t(x) + 1, y)) edges |= kLocalCursorRight;
  if (!covered(x, int64_t(y) - 1)) edges |= kLocalCursorTop;
  if (!covered(x, int64_t(y) + 1)) edges |= kLocalCursorBottom;
  return edges;
}

// The server maps an entry onto the virtual desktop's bounding box, which can lie
// beside a shorter or offset monitor where no pixel exists. Move such a point onto
// the monitor facing the entry side, keeping its proportional position along that
// side where a monitor covers it. A point on a top/bottom row is treated as a
// vertical entry, one on a left/right column as a horizontal entry.
inline void localCursorEntryPoint(
    int32_t &x, int32_t &y, int32_t sx, int32_t sy, int32_t sw, int32_t sh,
    const std::vector<LocalCursorMonitor> &monitors
)
{
  if (localCursorExposedEdges(x, y, monitors) >= 0)
    return;
  const bool vertical = y == sy || int64_t(y) == int64_t(sy) + sh - 1;
  const bool horizontal = !vertical && (x == sx || int64_t(x) == int64_t(sx) + sw - 1);
  const LocalCursorMonitor *best = nullptr;
  int64_t bestX = x, bestY = y, bestAlong = 0, bestAcross = 0;
  for (const auto &m : monitors) {
    if (m.width <= 0 || m.height <= 0)
      continue;
    const int64_t cx = std::clamp<int64_t>(x, m.x, int64_t(m.x) + m.width - 1);
    const int64_t cy = std::clamp<int64_t>(y, m.y, int64_t(m.y) + m.height - 1);
    const int64_t ax = cx > x ? cx - x : x - cx;
    const int64_t ay = cy > y ? cy - y : y - cy;
    // "along" moves the point sideways on the entry edge; "across" moves it inward.
    const int64_t along = vertical ? ax : horizontal ? ay : ax * ax + ay * ay;
    const int64_t across = vertical ? ay : horizontal ? ax : 0;
    if (best == nullptr || along < bestAlong || (along == bestAlong && across < bestAcross)) {
      best = &m;
      bestX = cx;
      bestY = cy;
      bestAlong = along;
      bestAcross = across;
    }
  }
  if (best != nullptr) {
    x = static_cast<int32_t>(bestX);
    y = static_cast<int32_t>(bestY);
  }
}

// Only outward intent at the desktop's outer edge can switch computers.
// Interior monitor seams and movement back from an edge stay on the client.
inline int32_t localCursorEdgeMotion(int32_t position, int32_t delta, int32_t origin, int32_t size)
{
  if (position == origin && delta < 0)
    return -1;
  if (position == origin + size - 1 && delta > 0)
    return 1;
  return 0;
}
