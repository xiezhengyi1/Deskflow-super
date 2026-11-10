/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "MacShortcuts.h"
#include <algorithm>
#include <string_view>

namespace {
constexpr KeyModifierMask commandMask = KeyModifierSuper | KeyModifierMeta;

bool isCommonShortcut(KeyID id, KeyModifierMask mask)
{
  if (!(mask & commandMask) || (mask & (KeyModifierControl | KeyModifierAlt | KeyModifierAltGr)))
    return false;
  // Shift variants use the application's usual Ctrl+Shift shortcut.
  return id < 128 && std::string_view("cCvVxXzZaAfFsS").find(static_cast<char>(id)) != std::string_view::npos;
}

bool isCommand(KeyID id)
{
  return id == kKeySuper_L || id == kKeySuper_R || id == kKeyMeta_L || id == kKeyMeta_R;
}
} // namespace

std::vector<MacShortcuts::KeyEvent> MacShortcuts::translate(KeyEvent event)
{
  m_mask = event.mask;
  std::vector<KeyEvent> events;
  if (isCommand(event.id)) {
    if (event.action == Action::Down) {
      m_commands.push_back({event});
    } else if (event.action == Action::Up) {
      const auto command = std::find_if(m_commands.begin(), m_commands.end(), [&](const auto &held) {
        return held.down.button == event.button;
      });
      if (command != m_commands.end()) {
        // A bare Command tap keeps its original Windows-key behavior.
        if (!command->used && !command->forwarded) {
          auto down = command->down;
          down.mask = event.mask & ~commandMask;
          events.push_back(down);
        }
        if (!command->used || command->forwarded)
          events.push_back(event);
        m_commands.erase(command);
      }
    }
    return events;
  }

  const bool common = isCommonShortcut(event.id, event.mask);
  const bool modifier = event.id >= kKeyShift_L && event.id <= kKeyHyper_R;
  if (event.action != Action::Up && !modifier) {
    if (common)
      releaseCommands(events, event.mask);
    else
      forwardCommands(events, event.mask);
    for (auto &command : m_commands)
      command.used = true;
  }
  if (common) {
    // KeyMap applies Ctrl for this key stroke and restores held modifiers.
    event.mask = (event.mask & ~commandMask) | KeyModifierControl;
  } else if (!m_commands.empty() && std::none_of(m_commands.begin(), m_commands.end(), [](const auto &command) {
               return command.forwarded;
             })) {
    event.mask &= ~commandMask;
  }
  events.push_back(event);
  return events;
}

void MacShortcuts::forwardCommands(std::vector<KeyEvent> &events, KeyModifierMask mask)
{
  for (auto &command : m_commands) {
    if (!command.forwarded) {
      auto down = command.down;
      down.mask = mask & ~commandMask;
      events.push_back(down);
      command.forwarded = true;
    }
    command.used = true;
  }
}

void MacShortcuts::releaseCommands(std::vector<KeyEvent> &events, KeyModifierMask mask)
{
  for (auto &command : m_commands) {
    if (command.forwarded) {
      auto up = command.down;
      up.action = Action::Up;
      up.mask = mask;
      events.push_back(up);
      command.forwarded = false;
    }
  }
}

std::vector<MacShortcuts::KeyEvent> MacShortcuts::pointerModifiers()
{
  std::vector<KeyEvent> events;
  forwardCommands(events, m_mask);
  return events;
}

void MacShortcuts::reset()
{
  m_commands.clear();
  m_mask = 0;
}
