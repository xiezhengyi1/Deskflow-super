/*
 * Deskflow -- mouse and keyboard sharing utility
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "ServerProxyTests.h"

#include "base/Event.h"
#include "base/IEventQueue.h"
#include "client/Client.h"
#include "client/ServerProxy.h"
#include "common/Settings.h"
#include "deskflow/AppUtil.h"
#include "deskflow/KeyMap.h"
#include "deskflow/ProtocolTypes.h"
#include "io/IStream.h"

#include <QTest>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace {

class TestAppUtil : public AppUtil
{
public:
  int run() override
  {
    return 0;
  }

  std::vector<std::string> getKeyboardLayoutList() override
  {
    return {"en"};
  }

  std::string getCurrentLanguageCode() override
  {
    return "en";
  }
};

class FakeStream : public deskflow::IStream
{
public:
  void push(const std::string &bytes)
  {
    m_chunks.push_back(bytes);
  }

  void close() override
  {
    m_chunks.clear();
    m_inputShutdown = true;
  }

  uint32_t read(void *buffer, uint32_t size) override
  {
    if (m_inputShutdown || m_chunks.empty() || size == 0) {
      return 0;
    }

    auto &front = m_chunks.front();
    const size_t bytesToRead = std::min(static_cast<size_t>(size), front.size());
    if (buffer != nullptr) {
      std::memcpy(buffer, front.data(), bytesToRead);
    }

    front.erase(0, bytesToRead);
    if (front.empty()) {
      m_chunks.pop_front();
    }
    return static_cast<uint32_t>(bytesToRead);
  }

  void write(const void *, uint32_t) override
  {
  }

  void flush() override
  {
  }

  void shutdownInput() override
  {
    close();
  }

  void shutdownOutput() override
  {
  }

  void *getEventTarget() const override
  {
    return const_cast<FakeStream *>(this);
  }

  bool isReady() const override
  {
    return !m_inputShutdown && !m_chunks.empty();
  }

  uint32_t getSize() const override
  {
    size_t total = 0;
    for (const auto &chunk : m_chunks) {
      total += chunk.size();
    }
    return static_cast<uint32_t>(std::min<size_t>(total, UINT32_MAX));
  }

private:
  std::deque<std::string> m_chunks;
  bool m_inputShutdown = false;
};

class RecordingEventQueue : public IEventQueue
{
public:
  ~RecordingEventQueue() override
  {
    for (const auto &event : m_addedEvents) {
      Event::deleteData(event);
    }
  }

  int loop() override
  {
    return 0;
  }

  void adoptBuffer(IEventQueueBuffer *) override
  {
  }

  bool getEvent(Event &, double = -1.0) override
  {
    return false;
  }

  bool dispatchEvent(const Event &event) override
  {
    const auto handler = m_handlers.find(HandlerKey{event.getType(), event.getTarget()});
    if (handler == m_handlers.end()) {
      return false;
    }

    const auto callback = handler->second;
    callback(event);
    return true;
  }

  void addEvent(Event &&event) override
  {
    m_addedEvents.emplace_back(std::move(event));
  }

  EventQueueTimer *newTimer(double, void *) override
  {
    return timer();
  }

  EventQueueTimer *newOneShotTimer(double, void *) override
  {
    return timer();
  }

  void deleteTimer(EventQueueTimer *) override
  {
  }

  void addHandler(EventTypes type, void *target, const EventHandler &handler) override
  {
    m_handlers[HandlerKey{type, target}] = handler;
  }

  void removeHandler(EventTypes type, void *target) override
  {
    m_handlers.erase(HandlerKey{type, target});
  }

  void removeHandlers(void *target) override
  {
    for (auto it = m_handlers.begin(); it != m_handlers.end();) {
      if (it->first.target == target) {
        it = m_handlers.erase(it);
      } else {
        ++it;
      }
    }
  }

  void waitForReady() const override
  {
  }

  void *getSystemTarget() override
  {
    return this;
  }

  EventQueueTimer *timer()
  {
    return reinterpret_cast<EventQueueTimer *>(&m_timerStorage);
  }

  const std::vector<Event> &addedEvents() const
  {
    return m_addedEvents;
  }

private:
  struct HandlerKey
  {
    EventTypes type;
    void *target;

    bool operator<(const HandlerKey &other) const
    {
      if (type != other.type) {
        return static_cast<uint32_t>(type) < static_cast<uint32_t>(other.type);
      }
      return std::less<void *>{}(target, other.target);
    }
  };

  int m_timerStorage = 0;
  std::map<HandlerKey, EventHandler> m_handlers;
  std::vector<Event> m_addedEvents;
};

class TestServerProxy : public ServerProxy
{
public:
  using ServerProxy::ServerProxy;

  bool parseHandshakeMessageReturnsDisconnect(const uint8_t *code)
  {
    return parseHandshakeMessage(code) == ConnectionResult::Disconnect;
  }
};

Client *undereferenceableClient()
{
  // These paths must queue cleanup without calling through to Client.
  return reinterpret_cast<Client *>(0x1);
}

TestAppUtil &testAppUtil()
{
  static TestAppUtil util;
  return util;
}

const Client::DisconnectRequest *disconnectRequest(const RecordingEventQueue &events)
{
  if (events.addedEvents().size() != 1) {
    return nullptr;
  }
  return static_cast<const Client::DisconnectRequest *>(events.addedEvents().front().getDataObject());
}

} // namespace

void ServerProxyTests::initTestCase()
{
  (void)testAppUtil();
  Settings::setSettingsFile(m_settings.filePath("test.conf"));
  m_log.setFilter(LogLevel::Level::Debug);
}

void ServerProxyTests::handleKeepAliveAlarm_timeout_queuesDisconnectRequest()
{
  RecordingEventQueue events;
  FakeStream stream;
  ServerProxy proxy(undereferenceableClient(), &stream, &events);

  QVERIFY(events.dispatchEvent(Event(EventTypes::Timer, events.timer())));

  QCOMPARE(events.addedEvents().size(), static_cast<size_t>(1));
  const auto &event = events.addedEvents().front();
  QVERIFY(event.getType() == EventTypes::ClientDisconnectRequested);
  QCOMPARE(event.getTarget(), stream.getEventTarget());

  const auto *request = disconnectRequest(events);
  QVERIFY(request != nullptr);
  QVERIFY(request->kind() == Client::DisconnectRequest::Kind::Disconnect);
  QCOMPARE(QString::fromUtf8(request->message()), QStringLiteral("server is not responding"));
}

void ServerProxyTests::handleData_incompleteMessage_queuesDisconnectRequest()
{
  RecordingEventQueue events;
  FakeStream stream;
  stream.push("DF");
  ServerProxy proxy(undereferenceableClient(), &stream, &events);

  QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));

  QCOMPARE(events.addedEvents().size(), static_cast<size_t>(1));
  const auto &event = events.addedEvents().front();
  QVERIFY(event.getType() == EventTypes::ClientDisconnectRequested);
  QCOMPARE(event.getTarget(), stream.getEventTarget());

  const auto *request = disconnectRequest(events);
  QVERIFY(request != nullptr);
  QVERIFY(request->kind() == Client::DisconnectRequest::Kind::Disconnect);
  QCOMPARE(QString::fromUtf8(request->message()), QStringLiteral("incomplete message from server"));
}

void ServerProxyTests::parseHandshakeMessage_protocolError_queuesRefusalRequest()
{
  RecordingEventQueue events;
  FakeStream stream;
  TestServerProxy proxy(undereferenceableClient(), &stream, &events);

  QVERIFY(proxy.parseHandshakeMessageReturnsDisconnect(reinterpret_cast<const uint8_t *>(kMsgEBad)));
  QCOMPARE(events.addedEvents().size(), static_cast<size_t>(1));
  const auto *request = disconnectRequest(events);
  QVERIFY(request != nullptr);
  QVERIFY(request->kind() == Client::DisconnectRequest::Kind::Refuse);
  QVERIFY(request->refusalReason() == deskflow::core::ConnectionRefusal::ProtocolError);
  QCOMPARE(QString::fromUtf8(request->message()), QStringLiteral("server reported a protocol error"));
}

void ServerProxyTests::handleData_largeBurst_yieldsAndPreservesMessageOrder()
{
  class CountingProxy : public ServerProxy
  {
  public:
    using ServerProxy::ServerProxy;
    std::vector<std::string> codes;
  protected:
    ConnectionResult parseHandshakeMessage(const uint8_t *code) override
    {
      codes.emplace_back(reinterpret_cast<const char *>(code), 4);
      return ConnectionResult::Okay;
    }
  };
  RecordingEventQueue events;
  FakeStream stream;
  std::vector<std::string> expected;
  for (int i = 0; i < 200; ++i) {
    expected.push_back(std::to_string(1000 + i));
    stream.push(expected.back());
  }
  CountingProxy proxy(undereferenceableClient(), &stream, &events);
  QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));
  QVERIFY(!proxy.codes.empty());
  QVERIFY(proxy.codes.size() < expected.size());
  size_t next = 0;
  while (next < events.addedEvents().size()) {
    QVERIFY(events.addedEvents()[next].getType() == EventTypes::StreamInputReady);
    QCOMPARE(events.addedEvents()[next].getTarget(), stream.getEventTarget());
    ++next;
    QVERIFY(events.dispatchEvent(Event(EventTypes::StreamInputReady, stream.getEventTarget())));
  }
  QVERIFY(proxy.codes == expected);
  QVERIFY(!stream.isReady());
}

void ServerProxyTests::macShortcuts_disabled_preservesServerMapping()
{
  Settings::setValue(Settings::Client::MacShortcuts, false);
  RecordingEventQueue events;
  FakeStream stream;
  ServerProxy proxy(undereferenceableClient(), &stream, &events);
  QCOMPARE(proxy.translateKey(kKeySuper_L), kKeySuper_L);
  QCOMPARE(proxy.translateKey(kKeySuper_R), kKeySuper_R);
  QCOMPARE(proxy.translateModifierMask(KeyModifierSuper | KeyModifierShift),
           KeyModifierMask(KeyModifierSuper | KeyModifierShift));
  proxy.m_modifierTranslationTable[kKeyModifierIDSuper] = kKeyModifierIDAlt;
  QCOMPARE(proxy.translateKey(kKeySuper_L), kKeyAlt_L);
  QCOMPARE(proxy.translateModifierMask(KeyModifierSuper), KeyModifierMask(KeyModifierAlt));
}

using KeyAction = MacShortcuts::Action;

void ServerProxyTests::macShortcuts_settingEnablesWindowsClient()
{
  Settings::setValue(Settings::Client::MacShortcuts, true);
  RecordingEventQueue events;
  FakeStream stream;
  ServerProxy proxy(undereferenceableClient(), &stream, &events);
#if defined(Q_OS_WIN)
  QVERIFY(proxy.m_macShortcuts);
#else
  QVERIFY(!proxy.m_macShortcuts);
#endif
  Settings::setValue(Settings::Client::MacShortcuts, false);
}

void ServerProxyTests::macShortcuts_common_data()
{
  QTest::addColumn<int>("key");
  for (const char key : std::string("cCvVxXzZaAfFsS"))
    QTest::newRow(std::string(1, key).c_str()) << int(key);
}

void ServerProxyTests::macShortcuts_common()
{
  QFETCH(int, key);
  MacShortcuts mapper;
  QVERIFY(mapper.translate({KeyAction::Down, kKeySuper_L, 0, 55}).empty());
  const KeyModifierMask originalMask = KeyModifierSuper | KeyModifierShift | KeyModifierCapsLock;
  const KeyModifierMask mappedMask = KeyModifierControl | KeyModifierShift | KeyModifierCapsLock;
  auto events = mapper.translate({KeyAction::Down, KeyID(key), originalMask, 8, 1, "en"});
  QCOMPARE(events.size(), size_t(1));
  QCOMPARE(events[0].id, KeyID(key));
  QCOMPARE(events[0].mask, mappedMask);
  QCOMPARE(events[0].button, KeyButton(8));
  QCOMPARE(events[0].language, std::string("en"));
  events = mapper.translate({KeyAction::Repeat, KeyID(key), originalMask, 8, 3, "en"});
  QCOMPARE(events.size(), size_t(1));
  QCOMPARE(events[0].action, KeyAction::Repeat);
  QCOMPARE(events[0].mask, mappedMask);
  QCOMPARE(events[0].count, uint16_t(3));
  events = mapper.translate({KeyAction::Up, KeyID(key), originalMask, 8});
  QCOMPARE(events.size(), size_t(1));
  QCOMPARE(events[0].action, KeyAction::Up);
  QVERIFY(mapper.translate({KeyAction::Up, kKeySuper_L, 0, 55}).empty());
  // A later ordinary key must not inherit Ctrl or Command.
  events = mapper.translate({KeyAction::Down, KeyID('b'), 0, 9});
  QCOMPARE(events[0].mask, KeyModifierMask(0));
}

void ServerProxyTests::macShortcuts_otherCombinationsKeepCommand()
{
  for (const auto key : {kKeyTab, KeyID('q'), kKeyLeft, KeyID(0x0163)}) {
    MacShortcuts mapper;
    mapper.translate({KeyAction::Down, kKeySuper_R, 0, 54});
    auto events = mapper.translate({KeyAction::Down, key, KeyModifierSuper, 8});
    QCOMPARE(events.size(), size_t(2));
    QCOMPARE(events[0].id, kKeySuper_R);
    QCOMPARE(events[0].action, KeyAction::Down);
    QCOMPARE(events[1].id, key);
    QCOMPARE(events[1].mask, KeyModifierMask(KeyModifierSuper));
    events = mapper.translate({KeyAction::Up, key, KeyModifierSuper, 8});
    QCOMPARE(events.size(), size_t(1));
    events = mapper.translate({KeyAction::Up, kKeySuper_R, 0, 54});
    QCOMPARE(events.size(), size_t(1));
    QCOMPARE(events[0].action, KeyAction::Up);
    QCOMPARE(events[0].id, kKeySuper_R);
  }
}

void ServerProxyTests::macShortcuts_mixedCombinationsReleaseOriginalCommand()
{
  MacShortcuts mapper;
  mapper.translate({KeyAction::Down, kKeySuper_L, 0, 55});
  mapper.translate({KeyAction::Down, KeyID('c'), KeyModifierSuper, 8});
  mapper.translate({KeyAction::Up, KeyID('c'), KeyModifierSuper, 8});
  auto events = mapper.translate({KeyAction::Down, kKeyTab, KeyModifierSuper, 48});
  QCOMPARE(events.size(), size_t(2));
  QCOMPARE(events[0].id, kKeySuper_L);
  QCOMPARE(events[1].id, kKeyTab);
  mapper.translate({KeyAction::Up, kKeyTab, KeyModifierSuper, 48});
  events = mapper.translate({KeyAction::Down, KeyID('v'), KeyModifierSuper, 9});
  QCOMPARE(events.size(), size_t(2));
  QCOMPARE(events[0].action, KeyAction::Up);
  QCOMPARE(events[0].id, kKeySuper_L);
  QCOMPARE(events[1].mask, KeyModifierMask(KeyModifierControl));
  // Command may be released before V without synthesizing another Windows tap.
  QVERIFY(mapper.translate({KeyAction::Up, kKeySuper_L, 0, 55}).empty());
  events = mapper.translate({KeyAction::Up, KeyID('v'), 0, 9});
  QCOMPARE(events.size(), size_t(1));
  QCOMPARE(events[0].mask, KeyModifierMask(0));
}

void ServerProxyTests::macShortcuts_bareAndBothCommandKeys()
{
  for (const auto id : {kKeySuper_L, kKeySuper_R, kKeyMeta_L, kKeyMeta_R}) {
    MacShortcuts mapper;
    QVERIFY(mapper.translate({KeyAction::Down, id, 0, 55}).empty());
    const auto events = mapper.translate({KeyAction::Up, id, 0, 55});
    QCOMPARE(events.size(), size_t(2));
    QCOMPARE(events[0].action, KeyAction::Down);
    QCOMPARE(events[1].action, KeyAction::Up);
    QCOMPARE(events[0].id, id);
    QCOMPARE(events[1].button, KeyButton(55));
  }
  MacShortcuts mapper;
  mapper.translate({KeyAction::Down, kKeySuper_L, 0, 55});
  mapper.translate({KeyAction::Down, kKeySuper_R, KeyModifierSuper, 54});
  const auto events = mapper.translate({KeyAction::Down, KeyID('c'), KeyModifierSuper, 8});
  QCOMPARE(events.size(), size_t(1));
  QCOMPARE(events[0].mask, KeyModifierMask(KeyModifierControl));
  QVERIFY(mapper.translate({KeyAction::Up, kKeySuper_L, KeyModifierSuper, 55}).empty());
  QVERIFY(mapper.translate({KeyAction::Up, kKeySuper_R, 0, 54}).empty());
}

void ServerProxyTests::macShortcuts_shiftAndOtherModifiers()
{
  MacShortcuts mapper;
  mapper.translate({KeyAction::Down, kKeySuper_L, 0, 55});
  auto events = mapper.translate({KeyAction::Down, kKeyShift_L, KeyModifierSuper, 56});
  QCOMPARE(events.size(), size_t(1));
  QCOMPARE(events[0].id, kKeyShift_L);
  QCOMPARE(events[0].mask, KeyModifierMask(0));
  events = mapper.translate({KeyAction::Down, KeyID('z'), KeyModifierSuper | KeyModifierShift, 6});
  QCOMPARE(events.size(), size_t(1));
  QCOMPARE(events[0].mask, KeyModifierMask(KeyModifierControl | KeyModifierShift));
  for (const auto modifier : {KeyModifierAlt, KeyModifierControl, KeyModifierAltGr}) {
    MacShortcuts original;
    original.translate({KeyAction::Down, kKeySuper_L, 0, 55});
    events = original.translate({KeyAction::Down, KeyID('c'), KeyModifierMask(KeyModifierSuper | modifier), 8});
    QCOMPARE(events.size(), size_t(2));
    QCOMPARE(events[1].mask, KeyModifierMask(KeyModifierSuper | modifier));
  }
}

void ServerProxyTests::macShortcuts_pointerAndLeave()
{
  MacShortcuts mapper;
  mapper.translate({KeyAction::Down, kKeySuper_L, KeyModifierShift, 55});
  auto events = mapper.pointerModifiers();
  QCOMPARE(events.size(), size_t(1));
  QCOMPARE(events[0].id, kKeySuper_L);
  QCOMPARE(events[0].mask, KeyModifierMask(KeyModifierShift));
  QVERIFY(mapper.pointerModifiers().empty());
  mapper.reset();
  QVERIFY(mapper.pointerModifiers().empty());
  // A release after leaving must not replay a stale Command press.
  QVERIFY(mapper.translate({KeyAction::Up, kKeySuper_L, 0, 55}).empty());
}

void ServerProxyTests::macShortcuts_nativeMappingRestoresCtrl()
{
  deskflow::KeyMap keyMap;
  deskflow::KeyMap::KeyItem control;
  control.m_id = kKeyControl_L;
  control.m_button = 17;
  control.m_generates = KeyModifierControl;
  keyMap.addKeyEntry(control);
  deskflow::KeyMap::KeyItem c;
  c.m_id = 'c';
  c.m_button = 67;
  keyMap.addKeyEntry(c);
  keyMap.finish();
  MacShortcuts mapper;
  mapper.translate({KeyAction::Down, kKeySuper_L, 0, 55});
  const auto mapped = mapper.translate({KeyAction::Down, KeyID('c'), KeyModifierSuper, 8});
  deskflow::KeyMap::Keystrokes strokes;
  deskflow::KeyMap::ModifierToKeys active;
  KeyModifierMask state = 0;
  QVERIFY(keyMap.mapKey(strokes, mapped[0].id, 0, active, state, mapped[0].mask, false, "en") != nullptr);
  QCOMPARE(strokes.size(), size_t(3));
  QCOMPARE(strokes[0].m_data.m_button.m_button, KeyButton(17));
  QVERIFY(strokes[0].m_data.m_button.m_press);
  QCOMPARE(strokes[1].m_data.m_button.m_button, KeyButton(67));
  QVERIFY(strokes[1].m_data.m_button.m_press);
  QCOMPARE(strokes[2].m_data.m_button.m_button, KeyButton(17));
  QVERIFY(!strokes[2].m_data.m_button.m_press);
  QCOMPARE(state, KeyModifierMask(0));
  QVERIFY(active.empty());
}

QTEST_MAIN(ServerProxyTests)
