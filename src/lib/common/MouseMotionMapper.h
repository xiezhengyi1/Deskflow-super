/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once
#include <cmath>
#include <cstdint>
#include <utility>

class MouseMotionMapper
{
public:
  void setFactor(double factor)
  {
    if (factor != m_factor) {
      m_factor = factor;
      reset();
    }
  }
  void reset()
  {
    m_x = 0;
    m_y = 0;
  }
  std::pair<int32_t, int32_t> map(int32_t dx, int32_t dy)
  {
    m_x += dx * m_factor;
    m_y += dy * m_factor;
    const auto x = static_cast<int32_t>(std::round(m_x));
    const auto y = static_cast<int32_t>(std::round(m_y));
    m_x -= x;
    m_y -= y;
    return {x, y};
  }

private:
  double m_factor = 1.0;
  double m_x = 0, m_y = 0;
};
