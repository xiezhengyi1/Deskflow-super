/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */

#include "../deskflow/MockEventQueue.h"
#include "base/Log.h"
#include "common/Settings.h"
#include "deskflow/AppUtil.h"
#include "deskflow/LocalCursor.h"
#include "deskflow/OptionTypes.h"
#include "deskflow/ProtocolTypes.h"
#include "deskflow/ProtocolUtil.h"
#include "io/IStream.h"
#include "server/ClientProxy1_0.h"
#include "server/PrimaryClient.h"
#include "server/Server.h"
#include <QTemporaryDir>

#include <QByteArray>
#include <QTest>
#include <cstring>
#include <memory>

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

class MemoryStream : public deskflow::IStream
{
public:
  QByteArray bytes;
  void close() override
  {
  }
  uint32_t read(void *buffer, uint32_t size) override
  {
    const auto count = std::min(size, static_cast<uint32_t>(bytes.size()));
    if (buffer != nullptr)
      std::memcpy(buffer, bytes.constData(), count);
    bytes.remove(0, count);
    return count;
  }
  void write(const void *buffer, uint32_t size) override
  {
    bytes.append(static_cast<const char *>(buffer), size);
  }
  void flush() override
  {
  }
  void shutdownInput() override
  {
  }
  void shutdownOutput() override
  {
  }
  void *getEventTarget() const override
  {
    return const_cast<MemoryStream *>(this);
  }
  bool isReady() const override
  {
    return !bytes.isEmpty();
  }
  uint32_t getSize() const override
  {
    return static_cast<uint32_t>(bytes.size());
  }
};

class RecordingQueue : public MockEventQueue
{
public:
  EventTypes type = EventTypes::Unknown;
  void *target = nullptr;
  std::unique_ptr<LocalCursorPosition> position;
  void addEvent(Event &&event) override
  {
    type = event.getType();
    target = event.getTarget();
    if (type == EventTypes::ClientProxyLocalCursorPosition)
      position.reset(static_cast<LocalCursorPosition *>(event.getDataObject()));
    else
      Event::deleteData(event);
  }
};

class TestProxy : public ClientProxy1_0
{
public:
  using ClientProxy1_0::ClientProxy1_0;
  int absoluteMoves = 0;
  int relativeMoves = 0;
  int lastDx = 0, lastDy = 0;
  int enters = 0;
  int32_t shapeX = -1920, shapeY = 0, shapeWidth = 5760, shapeHeight = 1440;
  void getShape(int32_t &x, int32_t &y, int32_t &w, int32_t &h) const override
  {
    x = shapeX;
    y = shapeY;
    w = shapeWidth;
    h = shapeHeight;
  }
  void getCursorPos(int32_t &x, int32_t &y) const override
  {
    x = 100;
    y = 100;
  }
  void mouseMove(int32_t, int32_t) override
  {
    ++absoluteMoves;
  }
  void mouseRelativeMove(int32_t dx, int32_t dy) override
  {
    ++relativeMoves;
    lastDx = dx;
    lastDy = dy;
  }
  void enter(int32_t, int32_t, uint32_t, KeyModifierMask, bool) override
  {
    ++enters;
  }
  bool readMessage()
  {
    uint8_t code[4];
    getStream()->read(code, sizeof(code));
    return parseMessage(code);
  }
};
class TestPrimary : public PrimaryClient
{
public:
  TestPrimary() : PrimaryClient("mac", nullptr)
  {
  }
  std::string getSecureInputApp() const override
  {
    // macOS checks secure input when switching between remote clients.
    return {};
  }
  void *getEventTarget() const override
  {
    return const_cast<TestPrimary *>(this);
  }
  void enable() override
  {
  }
  void disable() override
  {
  }
  void reconfigure(uint32_t) override
  {
  }
  KeyModifierMask getToggleMask() const override
  {
    return 0;
  }
  void resetOptions() override
  {
  }
  void setOptions(const OptionsList &) override
  {
  }
  void getCursorPos(int32_t &x, int32_t &y) const override
  {
    x = 0;
    y = 0;
  }
};
} // namespace

class LocalCursorTests : public QObject
{
  Q_OBJECT
  Log m_log;
  QTemporaryDir m_settings;
private Q_SLOTS:
  void initTestCase()
  {
    Settings::setSettingsFile(m_settings.filePath("test.conf"));
  }
  void serverUsesCorrectedPositionAndRejectsLateReports()
  {
    TestAppUtil app;
    RecordingQueue events;
    deskflow::server::Config config(&events);
    config.addComputer("mac");
    config.addComputer("windows");
    config.addComputer("other");
    config.addOption("", kOptionDisableLockToComputer, 1);
    config.addOption("", kOptionClipboardSharing, 0);
    QVERIFY(config.connect("windows", Direction::Right, 0, 1, "other", 0, 1));
    TestPrimary primary;
    TestProxy windows("windows", new MemoryStream, &events);
    TestProxy other("other", new MemoryStream, &events);
    // Server stores this dependency but the paths exercised here use client proxies only.
    Server server(config, &primary, reinterpret_cast<deskflow::Computer *>(0x1), &events);
    server.addClient(&windows);
    server.addClient(&other);
    // Server owns registered clients. Detach our stack fixtures even if a QVERIFY returns early.
    struct Detach
    {
      Server &server;
      TestPrimary &primary;
      TestProxy &windows;
      TestProxy &other;
      ~Detach()
      {
        server.m_active = &primary;
        server.removeClient(&windows);
        server.removeClient(&other);
      }
    } detach{server, primary, windows, other};
    windows.setLocalCursor(true);
    server.m_active = &windows;
    server.m_seqNum = 7;
    server.m_x = 100;
    server.m_y = 100;
    server.onMouseMoveSecondary(8, 2);
    QCOMPARE(windows.relativeMoves, 1);
    QCOMPARE(windows.lastDx, 8);
    QCOMPARE(windows.lastDy, 2);
    QCOMPARE(server.m_x, 100);
    QCOMPARE(windows.absoluteMoves, 0);

    LocalCursorPosition position;
    position.sequence = 7;
    position.x = 2100;
    position.y = 600;
    position.dx = 8;
    Event report(EventTypes::ClientProxyLocalCursorPosition, windows.getEventTarget(), &position);
    server.handleLocalCursorPosition(report, &windows);
    QCOMPARE(server.m_x, 2100);
    QCOMPARE(server.m_y, 600);
    QCOMPARE(server.m_xDelta, 8);
    QCOMPARE(server.m_active, &windows);
    QCOMPARE(windows.absoluteMoves, 0);

    position.sequence = 6;
    position.x = 3839;
    server.handleLocalCursorPosition(report, &windows);
    QCOMPARE(server.m_x, 2100);
    QCOMPARE(server.m_active, &windows);
    position.sequence = 7;
    server.handleLocalCursorPosition(report, &other);
    QCOMPARE(server.m_x, 2100);
    QCOMPARE(server.m_active, &windows);

    // Reversing from the edge must not switch computers.
    position.dx = -8;
    server.handleLocalCursorPosition(report, &windows);
    QCOMPARE(server.m_active, &windows);
    QCOMPARE(windows.absoluteMoves, 0);
    position.dx = 8;
    server.handleLocalCursorPosition(report, &windows);
    QCOMPARE(server.m_active, &other);
    QCOMPARE(other.enters, 1);
    QCOMPARE(server.m_seqNum, 8U);
    // Late packets from the previous computer must not bounce us back.
    server.handleLocalCursorPosition(report, &windows);
    QCOMPARE(server.m_active, &other);
    QCOMPARE(other.enters, 1);
  }
  void physicalMonitorEdges()
  {
    // The user's layout: 1920x1080 on the left, 2000x3200 on the right.
    const std::vector<LocalCursorMonitor> monitors{{0, 0, 1920, 1080}, {1920, 0, 2000, 3200}};
    QCOMPARE(localCursorExposedEdges(960, 1079, monitors), kLocalCursorBottom);
    QCOMPARE(localCursorExposedEdges(2920, 3199, monitors), kLocalCursorBottom);
    QCOMPARE(localCursorExposedEdges(1919, 500, monitors), 0);
    QCOMPARE(localCursorExposedEdges(1920, 500, monitors), 0);
    QCOMPARE(localCursorExposedEdges(1920, 2000, monitors), kLocalCursorLeft);
    QCOMPARE(localCursorExposedEdges(960, 500, monitors), 0);
    QCOMPARE(localCursorExposedEdges(960, 1500, monitors), -1);
    QCOMPARE(localCursorExposedEdges(960, 1079, {}), -1);
    // Reversed order and negative coordinates must have the same exposed bottom.
    const std::vector<LocalCursorMonitor> reversed{{-2000, 0, 2000, 3200}, {0, 0, 1920, 1080}};
    QCOMPARE(localCursorExposedEdges(960, 1079, reversed), kLocalCursorBottom);
    QCOMPARE(localCursorExposedEdges(0, 500, reversed), 0);
    // A bottom-aligned short monitor and a directly stacked monitor share a seam.
    const std::vector<LocalCursorMonitor> aligned{{0, 2120, 1920, 1080}, {1920, 0, 2000, 3200}};
    QCOMPARE(localCursorExposedEdges(960, 3199, aligned), kLocalCursorBottom);
    const std::vector<LocalCursorMonitor> stacked{{0, 0, 1920, 1080}, {0, 1080, 1920, 1080}};
    QCOMPARE(localCursorExposedEdges(960, 1079, stacked), 0);
  }
  void entryPoint_data()
  {
    QTest::addColumn<int>("x");
    QTest::addColumn<int>("y");
    QTest::addColumn<int>("expectedX");
    QTest::addColumn<int>("expectedY");
    // Bounding box 0,0 3920x3200: 1920x1080 top-left, 2000x3200 on the right.
    QTest::newRow("tall-bottom-unchanged") << 2920 << 3199 << 2920 << 3199;
    QTest::newRow("below-short-monitor") << 960 << 3199 << 960 << 1079;
    QTest::newRow("below-short-monitor-near-seam") << 1900 << 3199 << 1900 << 1079;
    QTest::newRow("short-top-unchanged") << 960 << 0 << 960 << 0;
    QTest::newRow("left-below-short-monitor") << 0 << 2000 << 1920 << 2000;
    QTest::newRow("left-beside-short-monitor") << 0 << 500 << 0 << 500;
    QTest::newRow("right-unchanged") << 3919 << 2000 << 3919 << 2000;
    QTest::newRow("interior-gap") << 1000 << 1500 << 1000 << 1079;
  }
  void entryPoint()
  {
    QFETCH(int, x);
    QFETCH(int, y);
    QFETCH(int, expectedX);
    QFETCH(int, expectedY);
    const std::vector<LocalCursorMonitor> monitors{{0, 0, 1920, 1080}, {1920, 0, 2000, 3200}};
    int32_t ex = x, ey = y;
    localCursorEntryPoint(ex, ey, 0, 0, 3920, 3200, monitors);
    QCOMPARE(ex, expectedX);
    QCOMPARE(ey, expectedY);
  }
  void entryPointOffsetLayouts()
  {
    // Monitors on both sides of the origin; the short one is bottom-aligned.
    const std::vector<LocalCursorMonitor> offset{{-1920, 2120, 1920, 1080}, {0, 0, 2000, 3200}};
    int32_t x = -960, y = 0;
    localCursorEntryPoint(x, y, -1920, 0, 3920, 3200, offset);
    QCOMPARE(x, -960);
    QCOMPARE(y, 2120);
    // A column covered twice enters the monitor nearest the entry edge.
    const std::vector<LocalCursorMonitor> stacked{{0, 0, 1920, 1080}, {0, 1080, 1920, 1080}, {1920, 0, 2000, 3200}};
    x = 960;
    y = 3199;
    localCursorEntryPoint(x, y, 0, 0, 3920, 3200, stacked);
    QCOMPARE(x, 960);
    QCOMPARE(y, 2159);
    // A gap between monitors enters the closest one sideways, not through the gap.
    const std::vector<LocalCursorMonitor> gap{{0, 0, 1000, 1000}, {1500, 0, 1000, 1000}};
    x = 1100;
    y = 999;
    localCursorEntryPoint(x, y, 0, 0, 2500, 1000, gap);
    QCOMPARE(x, 999);
    QCOMPARE(y, 999);
    x = 5;
    y = 7;
    localCursorEntryPoint(x, y, 0, 0, 2500, 1000, {});
    QCOMPARE(x, 5);
    QCOMPARE(y, 7);
  }
  void exposedEdgesSwitchToNeighbor_data()
  {
    QTest::addColumn<int>("x");
    QTest::addColumn<int>("y");
    QTest::addColumn<int>("dx");
    QTest::addColumn<int>("dy");
    QTest::addColumn<int>("edges");
    QTest::addColumn<bool>("switches");
    QTest::newRow("short-monitor-bottom") << 960 << 1079 << 0 << 8 << kLocalCursorBottom << true;
    QTest::newRow("tall-monitor-bottom") << 2920 << 3199 << 0 << 8 << kLocalCursorBottom << true;
    QTest::newRow("bottom-inward") << 960 << 1079 << 0 << -8 << kLocalCursorBottom << false;
    QTest::newRow("bottom-no-motion") << 960 << 1079 << 0 << 0 << kLocalCursorBottom << false;
    QTest::newRow("horizontal-internal-seam") << 1919 << 500 << 8 << 0 << 0 << false;
    QTest::newRow("legacy-interior") << 960 << 1079 << 0 << 8 << -1 << false;
  }
  void exposedEdgesSwitchToNeighbor()
  {
    QFETCH(int, x); QFETCH(int, y); QFETCH(int, dx); QFETCH(int, dy);
    QFETCH(int, edges); QFETCH(bool, switches);
    TestAppUtil app;
    RecordingQueue events;
    deskflow::server::Config config(&events);
    config.addComputer("mac"); config.addComputer("windows"); config.addComputer("other");
    config.addOption("", kOptionDisableLockToComputer, 1);
    config.addOption("", kOptionClipboardSharing, 0);
    QVERIFY(config.connect("windows", Direction::Bottom, 0, 1, "other", 0, 1));
    QVERIFY(config.connect("windows", Direction::Right, 0, 1, "other", 0, 1));
    TestPrimary primary;
    TestProxy windows("windows", new MemoryStream, &events);
    TestProxy other("other", new MemoryStream, &events);
    windows.shapeX = windows.shapeY = 0;
    windows.shapeWidth = 3920; windows.shapeHeight = 3200;
    Server server(config, &primary, reinterpret_cast<deskflow::Computer *>(0x1), &events);
    server.addClient(&windows); server.addClient(&other);
    struct Detach {
      Server &server; TestPrimary &primary; TestProxy &windows; TestProxy &other;
      ~Detach() { server.m_active = &primary; server.removeClient(&windows); server.removeClient(&other); }
    } detach{server, primary, windows, other};
    windows.setLocalCursor(true);
    server.m_active = &windows; server.m_seqNum = 7;
    LocalCursorPosition position;
    position.sequence = 7; position.x = x; position.y = y;
    position.dx = dx; position.dy = dy; position.edges = edges;
    Event report(EventTypes::ClientProxyLocalCursorPosition, windows.getEventTarget(), &position);
    server.handleLocalCursorPosition(report, &windows);
    QCOMPARE(server.m_active == &other, switches);
    QCOMPARE(other.enters, switches ? 1 : 0);
    if (!switches) { QCOMPARE(server.m_x, x); QCOMPARE(server.m_y, y); }
    QCOMPARE(windows.absoluteMoves, 0);
    // The report from the previous enter sequence must not cause a second switch.
    server.handleLocalCursorPosition(report, &windows);
    QCOMPARE(other.enters, switches ? 1 : 0);
  }
  void edgeFeedbackProtocol()
  {
    TestAppUtil app;
    RecordingQueue events;
    auto *stream = new MemoryStream;
    TestProxy proxy("windows", stream, &events);
    stream->bytes.clear();
    ProtocolUtil::writef(stream, kMsgDLocalCursorPositionEdges, 7U, -960, 1079, 0, 8, kLocalCursorBottom);
    QCOMPARE(stream->bytes.size(), 28);
    QVERIFY(proxy.readMessage());
    QVERIFY(events.position != nullptr);
    QCOMPARE(events.position->edges, kLocalCursorBottom);
    QCOMPARE(events.position->x, -960);
    QCOMPARE(events.position->y, 1079);
    QCOMPARE(events.position->sequence, 7U);
    QVERIFY(stream->bytes.isEmpty());
    events.position.reset();
    ProtocolUtil::writef(stream, kMsgDLocalCursorPositionEdges, 7U, 960, 1079, 0, 8, 16);
    QVERIFY(!proxy.readMessage());
    QVERIFY(events.position == nullptr);
  }
  void edgeMotion_data()
  {
    QTest::addColumn<int>("position");
    QTest::addColumn<int>("delta");
    QTest::addColumn<int>("expected");
    // A desktop with monitors on both sides of the origin: [-1920, 3839].
    QTest::newRow("left-outward") << -1920 << -8 << -1;
    QTest::newRow("left-inward") << -1920 << 8 << 0;
    QTest::newRow("right-outward") << 3839 << 8 << 1;
    QTest::newRow("right-inward") << 3839 << -8 << 0;
    QTest::newRow("internal-seam-left") << -1 << 8 << 0;
    QTest::newRow("internal-seam-right") << 0 << -8 << 0;
    QTest::newRow("lbm-corrected-interior") << 2100 << 300 << 0;
    QTest::newRow("entry-on-edge-without-motion") << -1920 << 0 << 0;
  }
  void edgeMotion()
  {
    QFETCH(int, position);
    QFETCH(int, delta);
    QFETCH(int, expected);
    QCOMPARE(localCursorEdgeMotion(position, delta, -1920, 5760), expected);
  }
  void explicitOptIn()
  {
    TestAppUtil app;
    RecordingQueue events;
    auto *stream = new MemoryStream;
    TestProxy proxy("windows", stream, &events);
    stream->bytes.clear(); // discard initial QINF
    QVERIFY(!proxy.usesLocalCursor());
    ProtocolUtil::writef(stream, kMsgDLocalCursor, 1);
    QVERIFY(proxy.readMessage());
    QVERIFY(proxy.usesLocalCursor());
    ProtocolUtil::writef(stream, kMsgDLocalCursor, 0);
    QVERIFY(proxy.readMessage());
    QVERIFY(!proxy.usesLocalCursor());
  }
  void feedbackRoundTripPreservesCoordinatesAndEnterSequence()
  {
    TestAppUtil app;
    RecordingQueue events;
    auto *stream = new MemoryStream;
    TestProxy proxy("windows", stream, &events);
    stream->bytes.clear();
    ProtocolUtil::writef(stream, kMsgDLocalCursorPosition, 0xfedcba98U, -70000, 80000, -17, 23);
    QCOMPARE(stream->bytes.size(), 24);
    QVERIFY(proxy.readMessage());
    QVERIFY(events.type == EventTypes::ClientProxyLocalCursorPosition);
    QCOMPARE(events.target, proxy.getEventTarget());
    QVERIFY(events.position != nullptr);
    QCOMPARE(events.position->edges, -1);
    QCOMPARE(events.position->sequence, 0xfedcba98U);
    QCOMPARE(events.position->x, -70000);
    QCOMPARE(events.position->y, 80000);
    QCOMPARE(events.position->dx, -17);
    QCOMPARE(events.position->dy, 23);
    QVERIFY(stream->bytes.isEmpty());
  }
};

QTEST_GUILESS_MAIN(LocalCursorTests)
#include "LocalCursorTests.moc"
