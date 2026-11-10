/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#pragma once

#include "deskflow/KeyTypes.h"
#include <string>
#include <vector>

class MacShortcuts
{
public:
  enum class Action { Down, Repeat, Up };
  struct KeyEvent
  {
    Action action;
    KeyID id;
    KeyModifierMask mask;
    KeyButton button;
    uint16_t count = 1;
    std::string language;
  };

  std::vector<KeyEvent> translate(KeyEvent event);
  std::vector<KeyEvent> pointerModifiers();
  void reset();

private:
  struct Command
  {
    KeyEvent down;
    bool forwarded = false;
    bool used = false;
  };
  void forwardCommands(std::vector<KeyEvent> &events, KeyModifierMask mask);
  void releaseCommands(std::vector<KeyEvent> &events, KeyModifierMask mask);

  std::vector<Command> m_commands;
  KeyModifierMask m_mask = 0;
};
