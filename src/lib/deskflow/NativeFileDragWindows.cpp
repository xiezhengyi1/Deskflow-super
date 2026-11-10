/*
 * SPDX-FileCopyrightText: (C) 2026 Deskflow Developers
 * SPDX-License-Identifier: GPL-2.0-only WITH LicenseRef-OpenSSL-Exception
 */
#include "NativeFileDragPlatform.h"
#include "base/Log.h"
#include <QApplication>
#include <QCursor>
#include <QDir>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QEventLoop>
#include <QGuiApplication>
#include <QLabel>
#include <QMimeData>
#include <QPointer>
#include <QScreen>
#include <QScopeGuard>
#include <QStyle>
#include <QTimer>
#include <QWidget>
#include <Windows.h>
#include <ShlObj.h>
#include <wrl.h>
#include <algorithm>
#include <cstring>

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
                .arg((std::min)(percent, 99)));
    adjustSize();
    move(QCursor::pos() + QPoint(16, 16));
    show();
  }
};

// Offers the staged files as a regular file drop (CF_HDROP), so any app accepts them. The paths
// are handed out right away while hovering; when the drop target asks after the button was
// released, the answer waits until every file has been received and verified.
std::atomic<HWND> receiveWindow{};
std::atomic<bool> remoteReleased{false};
std::atomic<uint64_t> inputSequence{0};

using Microsoft::WRL::ClassicCom;
using Microsoft::WRL::Make;
using Microsoft::WRL::RuntimeClass;
using Microsoft::WRL::RuntimeClassFlags;

class PromisedFiles final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IDataObject>
{
public:
  explicit PromisedFiles(std::shared_ptr<FileDragPromise> promise) : m_promise(std::move(promise))
  {
  }
  HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *format) override
  {
    return format->cfFormat == CF_HDROP && (format->tymed & TYMED_HGLOBAL) && format->dwAspect == DVASPECT_CONTENT
               ? S_OK
               : DV_E_FORMATETC;
  }

  HRESULT STDMETHODCALLTYPE GetData(FORMATETC *format, STGMEDIUM *medium) override
  {
    if (const auto result = QueryGetData(format); FAILED(result))
      return result;
    if (remoteReleased)
      waitForFiles();
    if (m_promise->state() == FileDragPromise::State::Failed)
      return E_ABORT;

    std::wstring paths;
    for (const auto &path : m_promise->stagedPaths) {
      paths += QDir::toNativeSeparators(path).toStdWString();
      paths.push_back(L'\0');
    }
    const auto size = (paths.size() + 1) * sizeof(wchar_t);
    const auto data = GlobalAlloc(GMEM_MOVEABLE, sizeof(DROPFILES) + size);
    if (!data)
      return E_OUTOFMEMORY;
    auto *drop = static_cast<DROPFILES *>(GlobalLock(data));
    if (!drop) {
      GlobalFree(data);
      return E_OUTOFMEMORY;
    }
    *drop = {};
    drop->pFiles = sizeof(DROPFILES);
    drop->fWide = TRUE;
    std::memcpy(drop + 1, paths.c_str(), size);
    GlobalUnlock(data);
    medium->tymed = TYMED_HGLOBAL;
    medium->hGlobal = data;
    medium->pUnkForRelease = nullptr;
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **formats) override
  {
    if (direction != DATADIR_GET)
      return E_NOTIMPL;
    const FORMATETC format{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    return SHCreateStdEnumFmtEtc(1, &format, formats);
  }

  HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *format) override
  {
    format->ptd = nullptr;
    return E_NOTIMPL;
  }
  HRESULT STDMETHODCALLTYPE SetData(FORMATETC *, STGMEDIUM *, BOOL) override { return E_NOTIMPL; }
  HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override
  {
    return OLE_E_ADVISENOTSUPPORTED;
  }
  HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
  HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override { return OLE_E_ADVISENOTSUPPORTED; }

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

class RemoteDropSource final : public RuntimeClass<RuntimeClassFlags<ClassicCom>, IDropSource>
{
public:
  explicit RemoteDropSource(std::shared_ptr<FileDragPromise> promise) : m_promise(std::move(promise))
  {
    m_preview.setWindowFlags(Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput);
    m_preview.setAttribute(Qt::WA_ShowWithoutActivating);
    m_preview.setPixmap(QApplication::style()->standardIcon(QStyle::SP_FileIcon).pixmap(24, 24));
  }

  HRESULT STDMETHODCALLTYPE QueryContinueDrag(BOOL escape, DWORD keys) override
  {
    QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
    if (escape || m_promise->state() == FileDragPromise::State::Failed)
      return DRAGDROP_S_CANCEL;
    // Physical input stays untouched. Only the remote release ends this drag,
    // after any queued movement carrying the held-button flag.
    if (!remoteReleased || (keys & MK_LBUTTON))
      return S_OK;
    m_preview.hide();
    return DRAGDROP_S_DROP;
  }

  HRESULT STDMETHODCALLTYPE GiveFeedback(DWORD) override
  {
    if (!remoteReleased) {
      m_preview.move(QCursor::pos() + QPoint(14, 16));
      m_preview.show();
    }
    return DRAGDROP_S_USEDEFAULTCURSORS;
  }

private:
  std::shared_ptr<FileDragPromise> m_promise;
  QLabel m_preview;
};

void postDragPointer(HWND window, UINT message, WPARAM keys)
{
  POINT pos{};
  GetCursorPos(&pos);
  // Windows takes MSG.pt from lParam for posted mouse messages. OLE
  // needs the screen point, not coordinates relative to the source window.
  PostMessageW(window, message, keys, MAKELPARAM(pos.x, pos.y));
}

bool runPromiseDrag(const std::shared_ptr<FileDragPromise> &promise)
{
  if (FAILED(OleInitialize(nullptr)))
    return false;
  const auto ole = qScopeGuard([] { OleUninitialize(); });
  const auto files = Make<PromisedFiles>(promise);
  const auto source = Make<RemoteDropSource>(promise);
  DWORD effect = DROPEFFECT_NONE;
  NativeFileDragPlatform::moveReceivedDrag();
  const auto result = DoDragDrop(files.Get(), source.Get(), DROPEFFECT_COPY, &effect);
  const bool dropped = result == DRAGDROP_S_DROP && (effect & DROPEFFECT_COPY);
  LOG_INFO("edge drag: Windows drop ended result=0x%08lx copied=%d left=%d",
           static_cast<unsigned long>(result), dropped, NativeFileDragPlatform::leftButtonDown());
  return dropped;
}
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

void NativeFileDragPlatform::moveReceivedDrag()
{
  if (const auto window = receiveWindow.load(); window && !remoteReleased)
    postDragPointer(window, WM_MOUSEMOVE, MK_LBUTTON);
}

uint64_t NativeFileDragPlatform::dragInputSequence()
{
  return inputSequence.load();
}

bool NativeFileDragPlatform::releaseReceivedDrag()
{
  ++inputSequence;
  if (const auto window = receiveWindow.load(); window && !remoteReleased.exchange(true)) {
    postDragPointer(window, WM_LBUTTONUP, 0);
    return true;
  }
  return false;
}

void NativeFileDragPlatform::cancelReceivedDrag()
{
  ++inputSequence;
  if (const auto window = receiveWindow.load())
    PostMessageW(window, WM_KEYDOWN, VK_ESCAPE, 0);
}

void NativeFileDragPlatform::startPromiseDrag(
    std::shared_ptr<FileDragPromise> promise, std::function<void(bool)> finished, uint64_t startSequence
)
{
  if (receiveWindow.load()) {
    finished(false);
    return;
  }
  QWidget source;
  remoteReleased = false;
  receiveWindow = reinterpret_cast<HWND>(source.winId());
  const bool dropped = dragInputSequence() == startSequence && runPromiseDrag(promise);
  receiveWindow = nullptr;
  finished(dropped);
}
