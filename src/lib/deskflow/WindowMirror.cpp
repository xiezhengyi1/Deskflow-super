/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "WindowMirror.h"
#include "NativeFileDragPlatform.h"
#include "WindowMirrorPlatform.h"
#include "WindowMirrorRouting.h"
#include "base/Log.h"
#include <QCloseEvent>
#include <QCoreApplication>
#include <QCursor>
#include <QElapsedTimer>
#include <QEvent>
#include <QGuiApplication>
#include <QImage>
#include <QLineF>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>

namespace {
QPointer<QObject> controller;
WindowMirror::Sender sender;

// Packets bigger than this are dropped by the receiving controller (NativeFileDrag::receive).
constexpr qsizetype kMaxFrameBytes = 1800 * 1024;
constexpr int kUnstartedViewerMs = 30 * 1000;
// A mirror that never hears of the button's release stops following the cursor by itself.
constexpr int kMaxFollowMs = 30 * 1000;

void sendMirror(const QString &peer, QCborMap packet)
{
  packet.insert(QStringLiteral("mirror"), true);
  if (sender)
    sender(peer, packet);
}

// ---------------------------------------------------------------- owner side
struct OwnedWindow
{
  QString id;
  QString peer;
  WindowMirrorPlatform::DraggedWindow window;
  bool adopted = false;
  std::unique_ptr<WindowMirrorPlatform::Capture> capture;
  qint64 seq = 0;
  QPointF pointer; // points inside the window (outside it while a drag has left the viewer)
  bool pointerInside = false;
  int heldButtons = 0;
  QElapsedTimer lastDown;
  QPointF lastDownAt;
  int clicks = 0;
  // A drag that left the viewer's window: looking for files to hand over.
  int dragOutTries = 0;
  bool dragOutBusy = false;
  QElapsedTimer lastDragOutTry;
  // Throughput, logged every few seconds while pictures flow.
  QElapsedTimer inFlight;
  QElapsedTimer statClock;
  int statFrames = 0;
  qint64 statBytes = 0;
  qint64 statEncodeMs = 0;
  qint64 statRoundTripMs = 0;
  QSize statPixels;
};
std::map<QString, std::unique_ptr<OwnedWindow>> owned;
QString pointerOwner; // window the viewer's pointer is over
QString buttonOwner;  // window that received the last injected press
QString focusOwner;   // window with keyboard focus on the viewer
QPointer<QTimer> watcher;
WindowMirror::DragOutHandler dragOutHandler;

constexpr int kWatchMs = 250;
constexpr int kStatsMs = 5000;
constexpr int kMaxDragOutTries = 25;
constexpr int kMaxRestMs = 200;

OwnedWindow *ownedWindow(const QString &id)
{
  const auto it = owned.find(id);
  return it == owned.end() ? nullptr : it->second.get();
}

void releaseRouting(const QString &id)
{
  if (pointerOwner == id) {
    pointerOwner.clear();
    WindowMirrorRouting::pointerInMirror = false;
  }
  if (focusOwner == id) {
    focusOwner.clear();
    WindowMirrorRouting::keyboardToMac = false;
  }
  if (buttonOwner == id)
    buttonOwner.clear();
}

void endOwned(const QString &id, bool notifyViewer, const QString &reason)
{
  const auto it = owned.find(id);
  if (it == owned.end())
    return;
  auto window = std::move(it->second);
  owned.erase(it);
  window->capture.reset();
  // Never leave a button pressed in the real window.
  using WindowMirrorPlatform::MouseAction;
  for (int button = 1; button <= 3; ++button)
    if (window->heldButtons & (1 << (button - 1)))
      WindowMirrorPlatform::injectMouse(window->window.windowId, MouseAction::Up, button, window->pointer, 1);
  WindowMirrorPlatform::release(window->window.windowId);
  releaseRouting(id);
  if (owned.empty() && watcher)
    watcher->stop();
  if (notifyViewer)
    sendMirror(window->peer, {{QStringLiteral("type"), QStringLiteral("closed")},
                              {QStringLiteral("id"), id},
                              {QStringLiteral("reason"), reason}});
  LOG_INFO("window mirror: stopped showing \"%s\" on the other computer (%s)", qPrintable(window->window.title),
           qPrintable(reason));
}

void logStats(OwnedWindow &window)
{
  if (!window.statClock.isValid())
    window.statClock.start();
  if (window.statClock.elapsed() < kStatsMs || window.statFrames == 0)
    return;
  LOG_DEBUG(
      "window mirror: %d pictures in %.1f s, %lld KB each, encoding %lld ms, round trip %lld ms, window %dx%d pixels",
      window.statFrames, window.statClock.elapsed() / 1000.0,
      static_cast<long long>(window.statBytes / window.statFrames / 1024),
      static_cast<long long>(window.statEncodeMs / window.statFrames),
      static_cast<long long>(window.statRoundTripMs / window.statFrames), window.statPixels.width(),
      window.statPixels.height()
  );
  window.statClock.restart();
  window.statFrames = 0;
  window.statBytes = window.statEncodeMs = window.statRoundTripMs = 0;
}

void onFrame(const QString &id, const QByteArray &jpeg, QRect patch, QSize full, int encodeMs)
{
  auto *window = ownedWindow(id);
  if (!window)
    return;
  if (jpeg.size() > kMaxFrameBytes) {
    LOG_WARN("window mirror: dropped a %lld byte picture", static_cast<long long>(jpeg.size()));
    window->capture->requestFullFrame();
    return;
  }
  ++window->statFrames;
  window->statBytes += jpeg.size();
  window->statEncodeMs += encodeMs;
  window->statPixels = full;
  window->inFlight.start();
  sendMirror(window->peer, {{QStringLiteral("type"), QStringLiteral("frame")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("seq"), ++window->seq},
                            {QStringLiteral("x"), patch.x()},
                            {QStringLiteral("y"), patch.y()},
                            {QStringLiteral("fw"), full.width()},
                            {QStringLiteral("fh"), full.height()},
                            {QStringLiteral("jpeg"), jpeg}});
}

// The real window may change by itself (resized by its app, closed): keep the viewer in step.
void watchOwned()
{
  QStringList gone;
  for (const auto &[id, window] : owned) {
    const auto frame = WindowMirrorPlatform::frame(window->window.windowId);
    if (!frame) {
      gone.append(id);
      continue;
    }
    const bool resized = std::abs(frame->width() - window->window.frame.width()) > 1 ||
                         std::abs(frame->height() - window->window.frame.height()) > 1;
    window->window.frame = *frame;
    if (resized) {
      if (window->capture)
        window->capture->refreshSize();
      sendMirror(window->peer, {{QStringLiteral("type"), QStringLiteral("size")},
                                {QStringLiteral("id"), id},
                                {QStringLiteral("w"), frame->width()},
                                {QStringLiteral("h"), frame->height()}});
    }
  }
  for (const auto &id : gone)
    endOwned(id, true, QStringLiteral("the window was closed"));
}

// The held left button left the viewer's window: if it is dragging files here, the drag
// continues on the other computer as a real file drag.
void tryDragOut(OwnedWindow &window)
{
  if (!dragOutHandler || window.dragOutBusy || window.dragOutTries >= kMaxDragOutTries)
    return;
  if (window.lastDragOutTry.isValid() && window.lastDragOutTry.elapsed() < 80)
    return;
  ++window.dragOutTries;
  window.lastDragOutTry.start();
  window.dragOutBusy = true;
  const auto id = window.id;
  dragOutHandler(window.peer, [id](WindowMirror::DragOut result) {
    auto *window = ownedWindow(id);
    if (!window)
      return;
    window->dragOutBusy = false;
    if (result == WindowMirror::DragOut::Nothing)
      return;
    window->dragOutTries = kMaxDragOutTries;
    if (result == WindowMirror::DragOut::HandedOver) {
      // The source drag was ended here; the release now belongs to the other computer.
      window->heldButtons &= ~1;
      LOG_INFO("window mirror: files dragged out of \"%s\" continue on the other computer", qPrintable(window->window.title));
    }
  });
}

void handleOwnerPacket(OwnedWindow &window, const QString &type, const QCborMap &packet)
{
  using WindowMirrorPlatform::MouseAction;
  if (type == "ack") {
    // One picture in flight at a time: the rate follows the link, and input never waits
    // behind a backlog of pictures on the shared connection.
    if (packet.value(QStringLiteral("seq")).toInteger() == window.seq && window.capture) {
      const qint64 roundTrip = window.inFlight.isValid() ? window.inFlight.elapsed() : 0;
      window.statRoundTripMs += roundTrip;
      logStats(window);
      // Pictures share the connection with the mouse and keyboard. Resting for as long as
      // the last picture took keeps it free at least half the time, so the pointer on the
      // other computer does not stutter while a window changes a lot.
      const int rest = static_cast<int>(std::min<qint64>(roundTrip, kMaxRestMs));
      if (rest < 4) {
        window.capture->requestFrame();
      } else {
        const auto id = window.id;
        QTimer::singleShot(rest, controller, [id] {
          if (auto *window = ownedWindow(id); window && window->capture)
            window->capture->requestFrame();
        });
      }
    }
  } else if (type == "refresh") {
    if (window.capture)
      window.capture->requestFullFrame();
  } else if (type == "pointer") {
    const bool inside = packet.value(QStringLiteral("inside")).toBool();
    const auto fx = std::clamp(packet.value(QStringLiteral("fx")).toDouble(), -4.0, 5.0);
    const auto fy = std::clamp(packet.value(QStringLiteral("fy")).toDouble(), -4.0, 5.0);
    window.pointer = QPointF(fx * window.window.frame.width(), fy * window.window.frame.height());
    const int dragButton = window.heldButtons & 1 ? 1 : (window.heldButtons & 4 ? 3 : 2);
    if (WindowMirrorRouting::ownerActive) {
      // The pointer has come back to this computer; a report still on its way must not
      // move the real cursor here.
      window.pointerInside = false;
      if (pointerOwner == window.id) {
        pointerOwner.clear();
        WindowMirrorRouting::pointerInMirror = false;
      }
      return;
    }
    if (inside) {
      if (!window.pointerInside) {
        window.pointerInside = true;
        WindowMirrorPlatform::raise(window.window.windowId);
      }
      pointerOwner = window.id;
      WindowMirrorRouting::pointerInMirror = true;
      WindowMirrorPlatform::injectMouse(
          window.window.windowId, window.heldButtons ? MouseAction::Drag : MouseAction::Move, dragButton,
          window.pointer, 0
      );
    } else {
      window.pointerInside = false;
      if (pointerOwner == window.id) {
        pointerOwner.clear();
        WindowMirrorRouting::pointerInMirror = false;
      }
      if (window.heldButtons) {
        // A drag that began in the window goes on outside it.
        WindowMirrorPlatform::injectMouse(window.window.windowId, MouseAction::Drag, dragButton, window.pointer, 0);
        if (window.heldButtons & 1)
          tryDragOut(window);
      }
    }
  } else if (type == "focus") {
    if (packet.value(QStringLiteral("focused")).toBool()) {
      focusOwner = window.id;
      WindowMirrorPlatform::activateForKeyboard(window.window.windowId);
      WindowMirrorRouting::keyboardToMac = true;
    } else if (focusOwner == window.id) {
      focusOwner.clear();
      WindowMirrorRouting::keyboardToMac = false;
    }
  } else if (type == "resize") {
    const QSizeF points(packet.value(QStringLiteral("w")).toDouble(), packet.value(QStringLiteral("h")).toDouble());
    if (points.width() < 40 || points.height() < 40 || points.width() > 16000 || points.height() > 16000)
      return;
    if (std::abs(points.width() - window.window.frame.width()) <= 1 &&
        std::abs(points.height() - window.window.frame.height()) <= 1)
      return;
    // The watcher sees the size the app really took and tells the capture and the viewer.
    WindowMirrorPlatform::resize(window.window.windowId, points);
    // An app that refuses the size (its minimum, a fixed layout) leaves nothing to see, so
    // the viewer is told explicitly to go back to the real size.
    const auto id = window.id;
    QTimer::singleShot(400, controller, [id, points] {
      auto *window = ownedWindow(id);
      if (!window)
        return;
      const auto &actual = window->window.frame;
      if (std::abs(actual.width() - points.width()) > 1 || std::abs(actual.height() - points.height()) > 1)
        sendMirror(window->peer, {{QStringLiteral("type"), QStringLiteral("size")},
                                  {QStringLiteral("id"), id},
                                  {QStringLiteral("w"), actual.width()},
                                  {QStringLiteral("h"), actual.height()}});
    });
  } else if (type == "close") {
    endOwned(window.id, false, QStringLiteral("closed on the other computer"));
  }
}

// ---------------------------------------------------------------- viewer side
void forgetViewer(const QString &id);

class MirrorView : public QWidget
{
public:
  MirrorView(const QString &peer, const QString &id, const QString &title, QSizeF points, QPointF grab)
      : QWidget(nullptr, Qt::Window),
        m_peer(peer),
        m_id(id),
        m_grab(grab)
  {
    setObjectName(QStringLiteral("windowMirror"));
    setWindowTitle(title);
    setAttribute(Qt::WA_DeleteOnClose);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(80, 60);
    resize(points.toSize());
    m_resizeTimer.setSingleShot(true);
    connect(&m_resizeTimer, &QTimer::timeout, this, [this] { sendResize(); });
    m_pointerTimer.setSingleShot(true);
    connect(&m_pointerTimer, &QTimer::timeout, this, [this] { flushPointer(); });
    m_follow.setInterval(8);
    connect(&m_follow, &QTimer::timeout, this, [this] { followCursor(); });
  }

  QString id() const
  {
    return m_id;
  }
  bool hasFrame() const
  {
    return !m_image.isNull();
  }

  void start()
  {
    move(QCursor::pos() - m_grab.toPoint());
    show();
    raise();
    activateWindow();
    // The button is still held on the other computer (none is down here): follow the cursor
    // until the server reports its release.
    m_following = true;
    m_releasesAtStart = WindowMirrorRouting::remoteLeftUps;
    m_followClock.start();
    m_follow.start();
    followCursor();
  }

  // A picture of the area at (x, y) of a window `full` pixels large. False if it does not fit
  // the picture shown so far (the owner then sends a complete one).
  bool setFrame(const QByteArray &jpeg, QPoint at, QSize full)
  {
    QImage image;
    if (!image.loadFromData(jpeg, "JPEG"))
      return false;
    if (!full.isValid() || (at.isNull() && image.size() == full)) {
      m_image = image.convertToFormat(QImage::Format_RGB32);
    } else if (m_image.size() == full && QRect(QPoint(0, 0), full).contains(QRect(at, image.size()))) {
      QPainter painter(&m_image);
      painter.setCompositionMode(QPainter::CompositionMode_Source);
      painter.drawImage(at, image);
      painter.end();
      // Repaint only the part of the window that shows this area.
      const double sx = double(width()) / full.width(), sy = double(height()) / full.height();
      update(QRectF(at.x() * sx, at.y() * sy, image.width() * sx, image.height() * sy)
                 .adjusted(-2, -2, 2, 2)
                 .toAlignedRect());
      return true;
    } else {
      return false;
    }
    update();
    return true;
  }

  // The real window changed size by itself.
  void setOwnerSize(QSize points)
  {
    if (m_following || points == size())
      return;
    m_ownerSize = points;
    resize(points);
  }

  void closeFromOwner()
  {
    m_ownerClosed = true;
    close();
  }

protected:
  void paintEvent(QPaintEvent *event) override
  {
    QPainter painter(this);
    painter.setClipRect(event->rect());
    if (m_image.isNull()) {
      painter.fillRect(rect(), QColor(40, 40, 40));
      painter.setPen(Qt::lightGray);
      painter.drawText(rect(), Qt::AlignCenter, QCoreApplication::translate("WindowMirror", "Connecting to the window…"));
      return;
    }
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    painter.drawImage(rect(), m_image);
  }
  void resizeEvent(QResizeEvent *event) override
  {
    QWidget::resizeEvent(event);
    // A size that came from the owner is not sent back to it.
    if (!m_following && size() != m_ownerSize)
      m_resizeTimer.start(150);
  }
  void mouseMoveEvent(QMouseEvent *event) override
  {
    // While a button is held Qt keeps sending moves from outside the window: that is how a
    // drag leaves it.
    queuePointer(event->position(), QRectF(rect()).contains(event->position()));
  }
  void mousePressEvent(QMouseEvent *event) override
  {
    // Accepting the press makes this window receive the moves of a drag that leaves it.
    queuePointer(event->position(), true);
    event->accept();
  }
  void mouseReleaseEvent(QMouseEvent *event) override
  {
    queuePointer(event->position(), QRectF(rect()).contains(event->position()));
  }
  void enterEvent(QEnterEvent *event) override
  {
    queuePointer(event->position(), true);
  }
  void leaveEvent(QEvent *) override
  {
    queuePointer(m_pointer, false);
  }
  void changeEvent(QEvent *event) override
  {
    QWidget::changeEvent(event);
    if (event->type() == QEvent::ActivationChange && !m_following)
      send({{QStringLiteral("type"), QStringLiteral("focus")}, {QStringLiteral("focused"), isActiveWindow()}});
  }
  void closeEvent(QCloseEvent *event) override
  {
    if (!m_ownerClosed)
      send({{QStringLiteral("type"), QStringLiteral("close")}});
    forgetViewer(m_id);
    QWidget::closeEvent(event);
  }

private:
  void send(QCborMap packet)
  {
    packet.insert(QStringLiteral("id"), m_id);
    sendMirror(m_peer, packet);
  }
  void followCursor()
  {
    move(QCursor::pos() - m_grab.toPoint());
    if (WindowMirrorRouting::remoteLeftUps == m_releasesAtStart && m_followClock.elapsed() < kMaxFollowMs)
      return;
    // Dropped: from now on this is an ordinary window. It usually arrives at the screen edge
    // it was dragged over, so bring all of it onto the screen.
    m_follow.stop();
    m_following = false;
    if (const auto *screen = QGuiApplication::screenAt(QCursor::pos()) ? QGuiApplication::screenAt(QCursor::pos())
                                                                         : QGuiApplication::primaryScreen()) {
      const QRect area = screen->availableGeometry();
      const QRect frame = frameGeometry();
      const int x = std::max(area.left(), std::min(frame.left(), area.right() - frame.width() + 1));
      const int y = std::max(area.top(), std::min(frame.top(), area.bottom() - frame.height() + 1));
      if (QPoint(x, y) != frame.topLeft())
        move(x, y);
    }
    sendResize();
    send({{QStringLiteral("type"), QStringLiteral("focus")}, {QStringLiteral("focused"), isActiveWindow()}});
  }
  void sendResize()
  {
    // Not what the owner already knows: the size it sent, or the one reported last.
    if (size() == m_ownerSize || size() == m_reportedSize)
      return;
    m_reportedSize = size();
    const auto ratio = devicePixelRatioF();
    send({{QStringLiteral("type"), QStringLiteral("resize")},
          {QStringLiteral("w"), width()},
          {QStringLiteral("h"), height()},
          {QStringLiteral("pw"), qRound(width() * ratio)},
          {QStringLiteral("ph"), qRound(height() * ratio)}});
  }
  void queuePointer(QPointF position, bool inside)
  {
    if (m_following)
      return;
    m_inside = inside;
    m_pointer = position;
    // Throttle to about 60 reports a second; the last position is always sent.
    if (!m_pointerTimer.isActive())
      m_pointerTimer.start(m_pointerClock.isValid() && m_pointerClock.elapsed() < 16 ? 16 : 0);
  }
  void flushPointer()
  {
    m_pointerClock.start();
    // Outside the window (a drag leaving it) the fractions go below 0 or above 1.
    send({{QStringLiteral("type"), QStringLiteral("pointer")},
          {QStringLiteral("inside"), m_inside},
          {QStringLiteral("fx"), width() > 0 ? m_pointer.x() / width() : 0.0},
          {QStringLiteral("fy"), height() > 0 ? m_pointer.y() / height() : 0.0}});
  }

  QString m_peer;
  QString m_id;
  QPointF m_grab;
  QImage m_image;
  QTimer m_resizeTimer;
  QTimer m_pointerTimer;
  QTimer m_follow;
  QElapsedTimer m_pointerClock;
  QElapsedTimer m_followClock;
  unsigned m_releasesAtStart = 0;
  QSize m_ownerSize;
  QSize m_reportedSize;
  QPointF m_pointer;
  bool m_inside = false;
  bool m_following = false;
  bool m_ownerClosed = false;
};

std::map<QString, QPointer<MirrorView>> viewers;

void forgetViewer(const QString &id)
{
  viewers.erase(id);
}

MirrorView *viewer(const QString &id)
{
  const auto it = viewers.find(id);
  return it == viewers.end() ? nullptr : it->second.data();
}
} // namespace

QRect WindowMirror::changedRect(const uchar *before, const uchar *after, int width, int height, qsizetype bytesPerLine)
{
  int left = width, right = -1, top = height, bottom = -1;
  const auto rowBytes = static_cast<size_t>(width) * 4;
  for (int y = 0; y < height; ++y) {
    const uchar *a = before + y * bytesPerLine;
    const uchar *b = after + y * bytesPerLine;
    if (std::memcmp(a, b, rowBytes) == 0)
      continue;
    top = std::min(top, y);
    bottom = y;
    const auto *pa = reinterpret_cast<const quint32 *>(a);
    const auto *pb = reinterpret_cast<const quint32 *>(b);
    int first = 0;
    while (first < left && pa[first] == pb[first])
      ++first;
    left = std::min(left, first);
    int last = width - 1;
    while (last > right && pa[last] == pb[last])
      --last;
    right = std::max(right, last);
  }
  if (bottom < 0)
    return {};
  // Whole 8-pixel blocks keep the edges of a JPEG patch clean.
  left &= ~7;
  top &= ~7;
  right = std::min(width - 1, right | 7);
  bottom = std::min(height - 1, bottom | 7);
  return QRect(QPoint(left, top), QPoint(right, bottom));
}

void WindowMirror::setDragOutHandler(DragOutHandler handler)
{
  dragOutHandler = std::move(handler);
}

void WindowMirror::init(QObject *owner, Sender send)
{
  controller = owner;
  sender = std::move(send);
  QObject::connect(qApp, &QCoreApplication::aboutToQuit, owner, [] { WindowMirror::shutdown(); });
}

std::optional<QCborMap> WindowMirror::takeOverWindowDrag(const QString &peer, const QString &id)
{
  const auto dragged = WindowMirrorPlatform::draggedWindow();
  if (!dragged)
    return std::nullopt;
  if (!WindowMirrorPlatform::canCapture()) {
    LOG_WARN("window mirror: Screen Recording permission is needed to move windows to the other computer");
    return std::nullopt;
  }
  WindowMirrorPlatform::endWindowDrag();
  auto window = std::make_unique<OwnedWindow>();
  window->id = id;
  window->peer = peer;
  window->window = *dragged;
  window->adopted = WindowMirrorPlatform::adopt(*dragged);
  if (!window->adopted)
    LOG_WARN("window mirror: \"%s\" cannot be controlled through Accessibility; it will only be shown",
             qPrintable(dragged->title));
  QPointer<QObject> owner = controller;
  window->capture = WindowMirrorPlatform::startCapture(
      dragged->windowId,
      [owner, id](const QByteArray &jpeg, QRect patch, QSize full, int encodeMs) {
        if (owner)
          QMetaObject::invokeMethod(
              owner, [id, jpeg, patch, full, encodeMs] { onFrame(id, jpeg, patch, full, encodeMs); },
              Qt::QueuedConnection
          );
      },
      [owner, id](const QString &reason) {
        if (owner)
          QMetaObject::invokeMethod(owner, [id, reason] { endOwned(id, true, reason); }, Qt::QueuedConnection);
      }
  );
  if (!window->capture) {
    WindowMirrorPlatform::release(dragged->windowId);
    return std::nullopt;
  }
  window->capture->requestFrame();
  LOG_INFO(
      "window mirror: showing \"%s\" (%s, %dx%d) on the other computer", qPrintable(dragged->title),
      qPrintable(dragged->app), qRound(dragged->frame.width()), qRound(dragged->frame.height())
  );
  owned[id] = std::move(window);
  if (!watcher && controller) {
    watcher = new QTimer(controller);
    watcher->setInterval(kWatchMs);
    QObject::connect(watcher, &QTimer::timeout, watcher, [] { watchOwned(); });
  }
  if (watcher)
    watcher->start();
  const auto title = dragged->title.isEmpty() ? dragged->app : QStringLiteral("%1 — %2").arg(dragged->app, dragged->title);
  return QCborMap{
      {QStringLiteral("type"), QStringLiteral("window")},
      {QStringLiteral("id"), id},
      {QStringLiteral("title"), title},
      {QStringLiteral("w"), dragged->frame.width()},
      {QStringLiteral("h"), dragged->frame.height()},
      {QStringLiteral("grabX"), dragged->grab.x()},
      {QStringLiteral("grabY"), dragged->grab.y()}
  };
}

void WindowMirror::handleWindowOffer(const QString &peer, const QCborMap &packet)
{
  const auto id = packet.value(QStringLiteral("id")).toString();
  const QSizeF points(packet.value(QStringLiteral("w")).toDouble(), packet.value(QStringLiteral("h")).toDouble());
  if (id.isEmpty() || viewers.count(id) || points.width() < 1 || points.height() < 1 || points.width() > 16000 ||
      points.height() > 16000)
    return;
  const QPointF grab(packet.value(QStringLiteral("grabX")).toDouble(), packet.value(QStringLiteral("grabY")).toDouble());
  const auto title = QCoreApplication::translate("WindowMirror", "%1 (from the other computer)")
                         .arg(packet.value(QStringLiteral("title")).toString());
  auto *view = new MirrorView(peer, id, title, points, grab);
  viewers[id] = view;
  LOG_INFO("window mirror: receiving \"%s\"", qPrintable(title));
  // The server normally switches here within milliseconds; drop the window if it never does.
  QTimer::singleShot(kUnstartedViewerMs, view, [view] {
    if (!view->isVisible())
      view->close();
  });
}

bool WindowMirror::handleStart(const QString &id)
{
  auto *view = viewer(id);
  if (!view)
    return false;
  LOG_INFO("window mirror: showing the window here");
  view->start();
  return true;
}

void WindowMirror::cancel(const QString &id)
{
  if (ownedWindow(id))
    endOwned(id, false, QStringLiteral("the drag was cancelled"));
  if (auto *view = viewer(id); view && !view->isVisible())
    view->closeFromOwner();
}

void WindowMirror::handlePacket(const QString &peer, const QCborMap &packet)
{
  const auto type = packet.value(QStringLiteral("type")).toString();
  const auto id = packet.value(QStringLiteral("id")).toString();
  if (auto *window = ownedWindow(id); window && window->peer == peer) {
    handleOwnerPacket(*window, type, packet);
    return;
  }
  auto *view = viewer(id);
  if (!view)
    return;
  if (type == "frame") {
    const QPoint at(static_cast<int>(packet.value(QStringLiteral("x")).toInteger()),
                    static_cast<int>(packet.value(QStringLiteral("y")).toInteger()));
    const QSize full(static_cast<int>(packet.value(QStringLiteral("fw")).toInteger(-1)),
                     static_cast<int>(packet.value(QStringLiteral("fh")).toInteger(-1)));
    if (!view->setFrame(packet.value(QStringLiteral("jpeg")).toByteArray(), at, full))
      sendMirror(peer, {{QStringLiteral("type"), QStringLiteral("refresh")}, {QStringLiteral("id"), id}});
    sendMirror(peer, {{QStringLiteral("type"), QStringLiteral("ack")},
                      {QStringLiteral("id"), id},
                      {QStringLiteral("seq"), packet.value(QStringLiteral("seq"))}});
  } else if (type == "size") {
    const QSize points(qRound(packet.value(QStringLiteral("w")).toDouble()), qRound(packet.value(QStringLiteral("h")).toDouble()));
    if (points.width() >= 40 && points.height() >= 40 && points.width() <= 16000 && points.height() <= 16000)
      view->setOwnerSize(points);
  } else if (type == "closed") {
    view->closeFromOwner();
  }
}

void WindowMirror::detach(const QString &peer)
{
  QStringList ids;
  for (const auto &[id, window] : owned)
    if (window->peer == peer)
      ids.append(id);
  for (const auto &id : ids)
    endOwned(id, false, QStringLiteral("the other computer disconnected"));
  QList<QPointer<MirrorView>> views;
  for (const auto &[id, view] : viewers)
    views.append(view);
  for (const auto &view : views)
    if (view)
      view->closeFromOwner();
}

void WindowMirror::shutdown()
{
  QStringList ids;
  for (const auto &[id, window] : owned)
    ids.append(id);
  for (const auto &id : ids)
    endOwned(id, true, QStringLiteral("Deskflow is quitting"));
}

void WindowMirror::injectButton(int button, bool down)
{
  if (!controller)
    return;
  QMetaObject::invokeMethod(
      controller,
      [button, down] {
        using WindowMirrorPlatform::MouseAction;
        const int bit = 1 << (std::clamp(button, 1, 3) - 1);
        if (down) {
          auto *window = ownedWindow(pointerOwner);
          if (!window)
            return;
          const bool repeat = window->lastDown.isValid() && window->lastDown.elapsed() < 450 &&
                              QLineF(window->lastDownAt, window->pointer).length() < 5;
          window->clicks = repeat ? window->clicks + 1 : 1;
          window->lastDown.start();
          window->lastDownAt = window->pointer;
          window->heldButtons |= bit;
          buttonOwner = window->id;
          if (bit == 1) {
            // A file drag starting with this press shows up on the drag pasteboard.
            NativeFileDragPlatform::noteLeftDown();
            window->dragOutTries = 0;
          }
          WindowMirrorPlatform::injectMouse(window->window.windowId, MouseAction::Down, button, window->pointer, window->clicks);
        } else {
          // The release belongs to the window that got the press, even if the pointer left it.
          auto *window = ownedWindow(buttonOwner);
          if (!window || !(window->heldButtons & bit))
            return;
          window->heldButtons &= ~bit;
          WindowMirrorPlatform::injectMouse(window->window.windowId, MouseAction::Up, button, window->pointer, window->clicks);
        }
      },
      Qt::QueuedConnection
  );
}

void WindowMirror::injectWheel(int xDelta, int yDelta)
{
  if (!controller)
    return;
  QMetaObject::invokeMethod(
      controller,
      [xDelta, yDelta] {
        if (auto *window = ownedWindow(pointerOwner))
          WindowMirrorPlatform::injectWheel(window->window.windowId, window->pointer, xDelta, yDelta);
      },
      Qt::QueuedConnection
  );
}

QList<QWidget *> WindowMirror::viewerWindows()
{
  QList<QWidget *> windows;
  for (const auto &[id, view] : viewers)
    if (view)
      windows.append(view);
  return windows;
}
