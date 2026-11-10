/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "NativeFileDragPlatform.h"
#include <QCursor>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QGuiApplication>
#include <QLabel>
#include <QMimeData>
#include <QPointer>
#include <QScreen>
#include <QTimer>
#include <QUrl>
#include <QVariant>
#include <QWidget>
#include <Windows.h>
#include <algorithm>

namespace {
void sendTagged(INPUT *inputs, UINT count)
{
  for (UINT i = 0; i < count; ++i) {
    if (inputs[i].type == INPUT_MOUSE)
      inputs[i].mi.dwExtraInfo = static_cast<ULONG_PTR>(kNativeFileDragInputTag);
    else if (inputs[i].type == INPUT_KEYBOARD)
      inputs[i].ki.dwExtraInfo = static_cast<ULONG_PTR>(kNativeFileDragInputTag);
  }
  SendInput(count, inputs, sizeof(INPUT));
}

void sendButton(DWORD flag)
{
  INPUT input{};
  input.type = INPUT_MOUSE;
  input.mi.dwFlags = flag;
  sendTagged(&input, 1);
}

// OLE re-checks the window under the cursor only when the mouse moves; nudge it so a window
// that appeared under a cursor resting on the screen edge receives DragEnter.
void nudgeCursor()
{
  INPUT moves[2]{};
  moves[0].type = INPUT_MOUSE;
  moves[0].mi.dwFlags = MOUSEEVENTF_MOVE;
  moves[0].mi.dx = 1;
  moves[1] = moves[0];
  moves[1].mi.dx = -1;
  sendTagged(moves, 2);
}

QStringList localFiles(const QMimeData *data)
{
  QStringList paths;
  if (data == nullptr || !data->hasUrls())
    return paths;
  for (const auto &url : data->urls()) {
    if (!url.isLocalFile())
      return {};
    paths.append(url.toLocalFile());
  }
  return paths;
}

// A nearly invisible window under the cursor. Explorer's drag (running in Explorer's process)
// enters it, which is the only way to learn which files are being dragged. When the drag is
// handed over, the injected button release drops it here, ending Explorer's drag as a copy.
class DropCatcher : public QWidget
{
public:
  DropCatcher() : QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
  {
    setAttribute(Qt::WA_ShowWithoutActivating);
    setAcceptDrops(true);
    // Fully transparent layered windows are skipped by hit-testing; 1% alpha is not.
    setWindowOpacity(0.01);
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this, [this] {
      if (m_done)
        report({});
      else
        hideSoon();
    });
  }

  void start(std::function<void(const QStringList &)> done)
  {
    if (m_done)
      report({});
    m_done = std::move(done);
    m_claimed = false;
    const auto cursor = QCursor::pos();
    QRect area(cursor - QPoint(32, 32), QSize(64, 64));
    if (const auto *screen = QGuiApplication::screenAt(cursor)) {
      const auto bounds = screen->geometry();
      area.moveLeft(std::clamp(area.left(), bounds.left(), bounds.right() - area.width() + 1));
      area.moveTop(std::clamp(area.top(), bounds.top(), bounds.bottom() - area.height() + 1));
    }
    setGeometry(area);
    show();
    raise();
    nudgeCursor();
    m_timeout.start(700);
  }

  void claimDrop()
  {
    // Drop Explorer's drag onto this window; it ends as a copy, the source files are untouched.
    m_claimed = true;
    sendButton(MOUSEEVENTF_LEFTUP);
    m_timeout.start(1500);
  }

protected:
  void dragEnterEvent(QDragEnterEvent *event) override
  {
    const auto paths = localFiles(event->mimeData());
    // The controller decides synchronously; it calls claimDrop() when it takes the files over.
    if (m_done)
      report((event->possibleActions() & Qt::CopyAction) ? paths : QStringList{});
    if (m_claimed) {
      event->setDropAction(Qt::CopyAction);
      event->accept();
    } else {
      // Not ours: get out of the way so the user's drag carries on normally.
      event->ignore();
      m_timeout.stop();
      hideSoon();
    }
  }
  void dragMoveEvent(QDragMoveEvent *event) override
  {
    if (!m_claimed) {
      event->ignore();
      return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
  }
  void dropEvent(QDropEvent *event) override
  {
    event->setDropAction(Qt::CopyAction);
    event->accept();
    m_timeout.stop();
    m_claimed = false;
    hideSoon();
  }

private:
  // Never hide the window from inside an OLE drag callback.
  void hideSoon()
  {
    QTimer::singleShot(0, this, &QWidget::hide);
  }
  void report(const QStringList &paths)
  {
    auto done = std::move(m_done);
    m_done = nullptr;
    if (paths.isEmpty()) {
      m_timeout.stop();
      hideSoon();
    }
    done(paths);
  }

  std::function<void(const QStringList &)> m_done;
  QTimer m_timeout;
  bool m_claimed = false;
};

QPointer<DropCatcher> catcher;

// Waiting for the files at drop time keeps Explorer's drop pending; show how far along we are.
class ReceiveProgress : public QLabel
{
public:
  ReceiveProgress() : QLabel(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint)
  {
    setAttribute(Qt::WA_ShowWithoutActivating);
    setMargin(8);
    setStyleSheet(QStringLiteral("QLabel { background: palette(window); border: 1px solid palette(mid); }"));
  }
  void showProgress(const FileDragPromise &promise)
  {
    const auto percent = promise.total > 0 ? int(100.0 * double(promise.received()) / double(promise.total)) : 0;
    setText(QCoreApplication::translate("NativeFileDrag", "Receiving %n file(s) from the other computer... %1%", nullptr,
                                        static_cast<int>(promise.names.size()))
                .arg(std::min(percent, 99)));
    adjustSize();
    move(QCursor::pos() + QPoint(16, 16));
    show();
  }
};

// Offers the staged files as a regular file drop (CF_HDROP), so any app accepts them. The paths
// are handed out right away while hovering; when the drop target asks after the button was
// released, the answer waits until every file has been received and verified.
class PromisedFiles : public QMimeData
{
public:
  explicit PromisedFiles(std::shared_ptr<FileDragPromise> promise) : m_promise(std::move(promise))
  {
  }
  QStringList formats() const override
  {
    return {QStringLiteral("text/uri-list")};
  }
  bool hasFormat(const QString &mimeType) const override
  {
    return mimeType == QStringLiteral("text/uri-list");
  }

protected:
  QVariant retrieveData(const QString &mimeType, QMetaType type) const override
  {
    Q_UNUSED(type)
    if (mimeType != QStringLiteral("text/uri-list"))
      return {};
    if (!NativeFileDragPlatform::leftButtonDown())
      waitForFiles();
    if (m_promise->state() == FileDragPromise::State::Failed)
      return {};
    QList<QVariant> urls;
    for (const auto &path : m_promise->stagedPaths)
      urls.append(QUrl::fromLocalFile(path));
    return urls;
  }

private:
  void waitForFiles() const
  {
    if (m_promise->state() != FileDragPromise::State::Pending)
      return;
    // Bytes arrive through queued events on this thread, so keep the event loop running.
    ReceiveProgress progress;
    QEventLoop loop;
    QTimer poll;
    poll.setInterval(100);
    QObject::connect(&poll, &QTimer::timeout, &loop, [&] {
      if (m_promise->state() != FileDragPromise::State::Pending)
        loop.quit();
      else
        progress.showProgress(*m_promise);
    });
    poll.start();
    progress.showProgress(*m_promise);
    loop.exec();
  }

  std::shared_ptr<FileDragPromise> m_promise;
};
} // namespace

bool NativeFileDragPlatform::leftButtonDown()
{
  // Physical left button, matching the MOUSEEVENTF_LEFTDOWN/UP this file injects.
  return (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
}

void NativeFileDragPlatform::noteLeftDown()
{
}

void NativeFileDragPlatform::probe(std::function<void(const QStringList &)> done)
{
  if (!leftButtonDown()) {
    done({});
    return;
  }
  if (!catcher)
    catcher = new DropCatcher;
  catcher->start(std::move(done));
}

void NativeFileDragPlatform::releaseSourceDrag()
{
  if (catcher)
    catcher->claimDrop();
}

void NativeFileDragPlatform::startPromiseDrag(
    std::shared_ptr<FileDragPromise> promise, std::function<void(bool)> finished
)
{
  auto *source = new QWidget(nullptr, Qt::Tool | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
  source->setAttribute(Qt::WA_ShowWithoutActivating);
  source->setGeometry(QRect(QCursor::pos(), QSize(1, 1)));
  source->show();
  // The physical button is held on the other computer; press it here so the drag has a button.
  sendButton(MOUSEEVENTF_LEFTDOWN);
  auto begin = [source, promise, finished = std::move(finished)] {
    auto *drag = new QDrag(source);
    drag->setMimeData(new PromisedFiles(promise));
    // Qt's Windows drag waits in GetMessage for a mouse message carrying the button state
    // before it calls DoDragDrop. The press was injected for a cursor that is not moving, so
    // that message may never come and the main thread blocks. Hand it one.
    HWND target = GetCapture();
    if (target == nullptr)
      target = GetFocus();
    if (target == nullptr)
      target = reinterpret_cast<HWND>(source->winId());
    POINT point;
    GetCursorPos(&point);
    ScreenToClient(target, &point);
    PostMessageW(target, WM_MOUSEMOVE, MK_LBUTTON, MAKELPARAM(point.x, point.y));
    const bool dropped = drag->exec(Qt::CopyAction, Qt::CopyAction) == Qt::CopyAction;
    source->hide();
    source->deleteLater();
    finished(dropped);
  };
  // Start the drag only once Windows reports the injected press, or it would drop at once.
  auto *wait = new QTimer(source);
  auto elapsed = std::make_shared<QElapsedTimer>();
  elapsed->start();
  QObject::connect(wait, &QTimer::timeout, source, [wait, elapsed, begin = std::move(begin)] {
    if (!leftButtonDown() && elapsed->elapsed() < 300)
      return;
    wait->stop();
    wait->deleteLater();
    begin();
  });
  wait->start(10);
}
