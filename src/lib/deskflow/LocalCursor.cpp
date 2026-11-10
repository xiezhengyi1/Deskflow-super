/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "LocalCursor.h"

#ifdef _WIN32
#include <Windows.h>
namespace {
BOOL CALLBACK collectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data)
{
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfo(monitor, &info)) {
    const auto &r = info.rcMonitor;
    auto &monitors = *reinterpret_cast<std::vector<LocalCursorMonitor> *>(data);
    monitors.push_back({r.left, r.top, r.right - r.left, r.bottom - r.top});
  }
  return TRUE;
}
} // namespace
#endif

std::vector<LocalCursorMonitor> localCursorMonitors()
{
  std::vector<LocalCursorMonitor> monitors;
#ifdef _WIN32
  if (!EnumDisplayMonitors(nullptr, nullptr, collectMonitor, reinterpret_cast<LPARAM>(&monitors)))
    monitors.clear();
#endif
  return monitors;
}
