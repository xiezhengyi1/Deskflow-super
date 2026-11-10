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
#include <QImage>
#include <QLineF>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <map>
#include <memory>

namespace {
QPointer<QObject> controller;
WindowMirror::Sender sender;

// Packets bigger than this are dropped by the receiving controller (NativeFileDrag::receive).
constexpr qsizetype kMaxFrameBytes = 1800 * 1024;
constexpr int kMaxCaptureSide = 2560;
constexpr int kUnstartedViewerMs = 30 * 1000;

void sendMirror(const QString &peer, QCborMap packet)
{
  packet.insert(QStringLiteral("mirror"), true);
  if (sender)
    sender(peer, packet);
}

QSize capturePixels(QSizeF points, double scale)
{
  QSizeF pixels = points * scale;
  const double longest = std::max(pixels.width(), pixels.height());
  if (longest > kMaxCaptureSide)
    pixels *= kMaxCaptureSide / longest;
  return QSize(std::max(1, qRound(pixels.width())), std::max(1, qRound(pixels.height())));
}

// ---------------------------------------------------------------- owner side
struct OwnedWindow
{
  QString id;
  QString peer;
  WindowMirrorPlatform::DraggedWindow window;
  bool hidden = false;
  std::unique_ptr<WindowMirrorPlatform::Capture> capture;
  qint64 seq = 0;
  QPointF pointer; // points inside the window
  int heldButtons = 0;
  QElapsedTimer lastDown;
  QPointF lastDownAt;
  int clicks = 0;
};
std::map<QString, std::unique_ptr<OwnedWindow>> owned;
QString pointerOwner; // window the viewer's pointer is over
QString buttonOwner;  // window that received the last injected press
QString focusOwner;   // window with keyboard focus on the viewer

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
  if (window->hidden)
    WindowMirrorPlatform::restore(window->window.windowId);
  releaseRouting(id);
  if (notifyViewer)
    sendMirror(window->peer, {{QStringLiteral("type"), QStringLiteral("closed")},
                              {QStringLiteral("id"), id},
                              {QStringLiteral("reason"), reason}});
  LOG_INFO("window mirror: \"%s\" is back on this computer (%s)", qPrintable(window->window.title), qPrintable(reason));
}

void onFrame(const QString &id, const QByteArray &jpeg, QSize pixels)
{
  auto *window = ownedWindow(id);
  if (!window)
    return;
  if (jpeg.size() > kMaxFrameBytes) {
    LOG_WARN("window mirror: dropped a %lld byte frame", static_cast<long long>(jpeg.size()));
    window->capture->requestFrame();
    return;
  }
  sendMirror(window->peer, {{QStringLiteral("type"), QStringLiteral("frame")},
                            {QStringLiteral("id"), id},
                            {QStringLiteral("seq"), ++window->seq},
                            {QStringLiteral("w"), pixels.width()},
                            {QStringLiteral("h"), pixels.height()},
                            {QStringLiteral("jpeg"), jpeg}});
}

void handleOwnerPacket(OwnedWindow &window, const QString &type, const QCborMap &packet)
{
  using WindowMirrorPlatform::MouseAction;
  if (type == "ack") {
    // One frame in flight at a time: the frame rate follows the link, and input never waits
    // behind a backlog of frames on the shared connection.
    if (packet.value(QStringLiteral("seq")).toInteger() == window.seq && window.capture)
      window.capture->requestFrame();
  } else if (type == "pointer") {
    const bool inside = packet.value(QStringLiteral("inside")).toBool();
    const auto fx = std::clamp(packet.value(QStringLiteral("fx")).toDouble(), 0.0, 1.0);
    const auto fy = std::clamp(packet.value(QStringLiteral("fy")).toDouble(), 0.0, 1.0);
    window.pointer = QPointF(fx * window.window.frame.width(), fy * window.window.frame.height());
    if (inside) {
      pointerOwner = window.id;
      WindowMirrorRouting::pointerInMirror = true;
      WindowMirrorPlatform::injectMouse(
          window.window.windowId, window.heldButtons ? MouseAction::Drag : MouseAction::Move,
          window.heldButtons & 1 ? 1 : (window.heldButtons & 4 ? 3 : 2), window.pointer, 0
      );
    } else if (pointerOwner == window.id) {
      pointerOwner.clear();
      WindowMirrorRouting::pointerInMirror = false;
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
    WindowMirrorPlatform::resize(window.window.windowId, points);
    window.window.frame.setSize(points);
    const QSize pixels(
        static_cast<int>(packet.value(QStringLiteral("pw")).toInteger()),
        static_cast<int>(packet.value(QStringLiteral("ph")).toInteger())
    );
    if (window.capture && pixels.width() > 0 && pixels.height() > 0)
      window.capture->setOutputSize(capturePixels(QSizeF(pixels), 1.0));
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
    // The button is still held from the drag on the other computer: keep following it.
    m_following = true;
    m_follow.start();
    followCursor();
  }

  void setFrame(const QByteArray &jpeg)
  {
    QImage image;
    if (!image.loadFromData(jpeg, "JPEG"))
      return;
    m_image = std::move(image);
    update();
  }

  void closeFromOwner()
  {
    m_ownerClosed = true;
    close();
  }

protected:
  void paintEvent(QPaintEvent *) override
  {
    QPainter painter(this);
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
    if (!m_following)
      m_resizeTimer.start(150);
  }
  void mouseMoveEvent(QMouseEvent *event) override
  {
    queuePointer(event->position(), true);
  }
  void enterEvent(QEnterEvent *event) override
  {
    queuePointer(event->position(), true);
  }
  void leaveEvent(QEvent *) override
  {
    m_inside = false;
    m_pointerTimer.stop();
    if (!m_following)
      send({{QStringLiteral("type"), QStringLiteral("pointer")}, {QStringLiteral("inside"), false}});
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
    if (NativeFileDragPlatform::leftButtonDown())
      return;
    // Dropped: from now on this is an ordinary window.
    m_follow.stop();
    m_following = false;
    sendResize();
    send({{QStringLiteral("type"), QStringLiteral("focus")}, {QStringLiteral("focused"), isActiveWindow()}});
  }
  void sendResize()
  {
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
  window->hidden = WindowMirrorPlatform::hide(*dragged);
  if (!window->hidden)
    LOG_WARN("window mirror: could not hide \"%s\" on this computer", qPrintable(dragged->title));
  QPointer<QObject> owner = controller;
  window->capture = WindowMirrorPlatform::startCapture(
      dragged->windowId, capturePixels(dragged->frame.size(), 1.5),
      [owner, id](const QByteArray &jpeg, QSize pixels) {
        if (owner)
          QMetaObject::invokeMethod(owner, [id, jpeg, pixels] { onFrame(id, jpeg, pixels); }, Qt::QueuedConnection);
      },
      [owner, id](const QString &reason) {
        if (owner)
          QMetaObject::invokeMethod(owner, [id, reason] { endOwned(id, true, reason); }, Qt::QueuedConnection);
      }
  );
  if (!window->capture) {
    if (window->hidden)
      WindowMirrorPlatform::restore(dragged->windowId);
    return std::nullopt;
  }
  window->capture->requestFrame();
  LOG_INFO(
      "window mirror: moving \"%s\" (%s, %dx%d) to the other computer", qPrintable(dragged->title),
      qPrintable(dragged->app), qRound(dragged->frame.width()), qRound(dragged->frame.height())
  );
  owned[id] = std::move(window);
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
    view->setFrame(packet.value(QStringLiteral("jpeg")).toByteArray());
    sendMirror(peer, {{QStringLiteral("type"), QStringLiteral("ack")},
                      {QStringLiteral("id"), id},
                      {QStringLiteral("seq"), packet.value(QStringLiteral("seq"))}});
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
