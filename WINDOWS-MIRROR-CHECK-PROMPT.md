# Prompt: check the Windows side of Deskflow window mirroring

Copy everything below the line into an AI coding assistant running **on the Windows laptop**, opened in the Deskflow source folder (`Deskflow-super`, branch `main`, pulled to the latest commit).

---

You are working on a customized Deskflow fork ("LBM"). macOS is the **server** (physical keyboard and trackpad); this Windows laptop is the **client** in LittleBigMouse local-cursor mode. Build the latest `main`, then verify and, where needed, fix the **Windows side** of "window mirroring". Show evidence (log lines, command output) for each conclusion and do not guess. Do not change the packet formats below: the Mac side is already built against them.

## What the feature does

The user drags a Mac window over the screen edge to Windows. Windows shows a live copy (`MirrorView` in `src/lib/deskflow/WindowMirror.cpp`, plain Qt, runs in the core process). The real window stays on the Mac screen; the Mac replays the viewer's input as real input.

Packets are CBOR maps sent as `kMsgDNativeFileDrag`, marked `"mirror": true` (viewer ↔ owner) or `"drag": true` (edge-drag control).

1. Mac → Windows `{"drag":true,"type":"window","id","name","title","w","h","grabX","grabY"}`: Windows creates a hidden `MirrorView` titled with `name` and answers at once with `{"mirror":true,"type":"hello","id","v":2}`. Without that answer the Mac assumes an old viewer and sends complete pictures only. (`title` carries an "update Deskflow" notice that only old viewers display. **If the copy's title shows that notice, or the picture is a magnified, flickering fragment, this computer is running an old build: rebuild from the latest `main` first.**)
2. Mac → Windows `{"drag":true,"type":"start","id"}`: `MirrorView::start()` shows it under the cursor. It must **follow the cursor until the server sends the left-button release**. No button is down on Windows at that moment, so it watches `WindowMirrorRouting::remoteLeftUps`, which `ServerProxy::mouseUp()` increments.
3. Mac → Windows `frame` packets: `{"type":"frame","id","seq","x","y","fw","fh","jpeg"}`. `jpeg` is the picture of the area at (`x`,`y`) of a window `fw`×`fh` pixels large. The first one is complete; later ones are patches. Windows answers each with `{"type":"ack","id","seq"}`; the Mac sends the next picture only after the ack. If a patch does not fit, Windows sends `{"type":"refresh","id"}`.
4. Windows → Mac `{"type":"pointer","id","inside","fx","fy"}` (fractions of the window size; below 0 or above 1 while a held button has left the window), `focus`, `resize` (`w`,`h` logical pixels), `close`.
5. Mac → Windows `{"type":"size","id","w","h"}` when the real window changed size by itself, and `{"type":"closed","id","reason"}`.

## Things to verify on Windows

1. **Following**: after the window arrives it follows the cursor until the trackpad button is released on the Mac, then stays. Check `ServerProxy::mouseUp()` runs for that release.
2. **Pointer reports**: moving over the copy sends `pointer` packets with `inside: true`; leaving sends `inside: false`. While the left button is held and the pointer leaves the copy, `mouseMoveEvent` must keep firing (Qt mouse grab) with `inside: false`.
3. **Picture**: patches are composited correctly (no stale areas), the copy is sharp, and each `frame` is acknowledged promptly. Measure the time from packet arrival (`NativeFileDrag::receive`) to the `ack` being written; it should be a few milliseconds. If decoding or painting is slow, say where.
4. **Drag files out of a mirrored Finder window** (new): the user presses on a file inside the copy and drags it out of the copy's frame. The Mac then sends a normal edge-drag `offer` followed at once by `{"drag":true,"type":"start","id"}`. Unlike an edge drag, **the left button is really down on Windows** here (the server relayed the press). Check that:
   - `handleStart` → `NativeFileDragPlatform::startPromiseDrag` → `DoDragDrop` starts while the real button is down and the `MirrorView` has the mouse capture;
   - on release, `ServerProxy::mouseUp()` calls `releaseReceivedDrag()` **and also injects the real button-up** (the code now only skips the injection when `leftButtonDown()` is false), so the drop happens and no button stays stuck;
   - the files land in the Explorer folder or on the desktop.
5. After closing the copy, the Mac log should say `stopped showing "..." on the other computer (closed on the other computer)`.

## Where to look

- Viewer: `src/lib/deskflow/WindowMirror.cpp` (`MirrorView`, `WindowMirror::handlePacket`).
- Controller: `src/lib/deskflow/NativeFileDrag.cpp` (`handleDragPacket`, `handleOffer`, `handleStart`).
- Windows drag: `src/lib/deskflow/NativeFileDragWindows.cpp` (`startPromiseDrag`, `RemoteDropSource`, `releaseReceivedDrag`).
- Client input: `src/lib/client/ServerProxy.cpp` (`mouseUp`).
- Turn on Debug logging with "log to file" in Deskflow's settings and quote the relevant lines.

Commit fixes to `main` and push, and summarize what you changed and what you measured.
