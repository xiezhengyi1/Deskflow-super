/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2025 Deskflow Developers
 * SPDX-FileCopyrightText: (C) 2012 - 2016, 2026 Synergy App Ltd
 * SPDX-FileCopyrightText: (C) 2002 Chris Schoeneman
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#pragma once

#include "common/Enums.h"
#include "client/MacShortcuts.h"
#include "common/MouseMotionMapper.h"
#include "deskflow/ClipboardChunk.h"
#include "deskflow/ClipboardTypes.h"
#include "deskflow/KeyTypes.h"
#include "deskflow/LocalCursor.h"
#include <chrono>
#include <memory>

class Client;
class ClientInfo;
class EventQueueTimer;
class IClipboard;
namespace deskflow {
class IStream;
}
class IEventQueue;

//! Proxy for server implementing protocol version 1.6
/*!
This class acts a proxy for the server, converting calls into messages
to the server and messages from the server to calls on the client.
It implements 1.6, the oldest version the client supports; later
versions subclass it and add their messages.
*/
class ServerProxy
{
  friend class ServerProxyTests;
public:
  /*!
  Process messages from the server on \p stream and forward to
  \p client.
  */
  ServerProxy(Client *client, deskflow::IStream *stream, IEventQueue *events);
  ServerProxy(ServerProxy const &) = delete;
  ServerProxy(ServerProxy &&) = delete;
  virtual ~ServerProxy();

  ServerProxy &operator=(ServerProxy const &) = delete;
  ServerProxy &operator=(ServerProxy &&) = delete;

  //! @name manipulators
  //@{

  void onInfoChanged();
  bool onGrabClipboard(ClipboardID);
  void onClipboardChanged(ClipboardID, const IClipboard *);

  //@}

protected:
  enum class ConnectionResult
  {
    Okay,
    Unknown,
    Disconnect
  };

  virtual ConnectionResult parseHandshakeMessage(const uint8_t *code);
  virtual ConnectionResult parseMessage(const uint8_t *code);
  void keyDown(uint16_t id, uint16_t mask, uint16_t button, const std::string &lang);
  void keyRepeat(uint16_t id, uint16_t mask, uint16_t count, uint16_t button, const std::string &lang);

  deskflow::IStream *getStream() const
  {
    return m_stream;
  }

private:
  // if compressing mouse motion then send the last motion now
  void flushCompressedMouse();

  void sendInfo(const ClientInfo &);
  void stopLocalCursorTimer();
  void reportLocalCursor();
  void applyRelativeMotion(int32_t dx, int32_t dy);

  void resetKeepAliveAlarm();
  void setKeepAliveRate(double);

  // modifier key translation
  KeyID translateKey(KeyID) const;
  KeyModifierMask translateModifierMask(KeyModifierMask) const;
  void handleKeyEvent(const MacShortcuts::KeyEvent &);
  void forwardKeyEvent(const MacShortcuts::KeyEvent &);
  void forwardPointerModifiers();

  // event handlers
  void handleData();
  ConnectionResult handleMessage(const uint8_t *code);
  void handleKeepAliveAlarm();
  void requestDisconnect(const char *message);
  void requestRefuseConnection(deskflow::core::ConnectionRefusal reason, const char *message);

  // message handlers
  void enter();
  void leave();
  void setClipboard();
  void grabClipboard();
  void keyUp();
  void mouseDown();
  void mouseUp();
  void mouseMove();
  void mouseRelativeMove();
  void mouseWheel();
  void screensaver();
  void resetOptions();
  void setOptions();
  void queryInfo();
  void infoAcknowledgment();

private:
  using MessageParser = ConnectionResult (ServerProxy::*)(const uint8_t *);

  Client *m_client = nullptr;
  deskflow::IStream *m_stream = nullptr;

  uint32_t m_seqNum = 0;
  bool m_replyPending = false;
  bool m_macShortcuts = false;
  MacShortcuts m_macShortcutMapper;
  bool m_localCursor = false;
  bool m_localCursorEdges = false;
  std::vector<LocalCursorMonitor> m_localMonitors;
  std::chrono::steady_clock::time_point m_localMonitorRefresh{};
  bool m_nativeFileDrag = false;
  std::shared_ptr<int> m_nativeDragChannel = std::make_shared<int>(0);
  MouseMotionMapper m_mouseMotion;
  EventQueueTimer *m_localCursorTimer = nullptr;
  // Motion applied since the last report, and the position that report carried.
  int32_t m_localDx = 0;
  int32_t m_localDy = 0;
  bool m_localReported = false;
  int32_t m_localX = 0;
  int32_t m_localY = 0;

  bool m_compressMouse = false;
  bool m_compressMouseRelative = false;
  int32_t m_xMouse = 0;
  int32_t m_yMouse = 0;
  int32_t m_dxMouse = 0;
  int32_t m_dyMouse = 0;

  bool m_ignoreMouse = false;

  KeyModifierID m_modifierTranslationTable[kKeyModifierIDLast];

  double m_keepAliveAlarm = 0.0;
  EventQueueTimer *m_keepAliveAlarmTimer = nullptr;

  MessageParser m_parser = &ServerProxy::parseHandshakeMessage;
  IEventQueue *m_events = nullptr;
  std::string m_clipboardDataCached;
  ClipboardChunkAssemblyState m_clipboardChunkState;
};
