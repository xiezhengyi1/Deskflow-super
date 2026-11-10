# Prompt: debug Deskflow edge file drag on the Windows client

Copy everything below the line into an AI coding assistant (for example Claude Code) running **on the Windows laptop**, opened in the Deskflow source folder that the Windows build was made from (`Deskflow-LBM-dragdrop`).

---

You are debugging a customized Deskflow fork ("LBM") on this Windows machine. Please investigate and fix the Windows side of a new "edge file drag" feature. Work step by step, show me evidence (log lines, command output) for each conclusion, and do not guess.

## Setup

- **macOS** is the Deskflow **server**: it has the physical keyboard and trackpad.
- **This Windows laptop** is the Deskflow **client**. It runs LittleBigMouse, and Deskflow uses "local cursor mode": the client moves its own cursor and reports positions back to the server.
- The two computers are connected over Deskflow's TLS connection. The older "File relay" window works and uses the same connection.

## How the feature should work: Mac → Windows (this is what fails)

1. The user drags a file in Finder with the Mac trackpad to the screen edge that leads to Windows.
2. The Mac server sends a `probe` to its own drag controller. The Mac reads Finder's drag pasteboard, ends the Finder drag (Escape), and sends a FileTransferSession `offer` packet to Windows. These packets are CBOR maps with `"drag": true`, sent as protocol message `kMsgDNativeFileDrag` (`"DNFM%s"`).
3. The server switches the cursor to Windows and sends a `start` packet: `{"drag": true, "type": "start", "id": ...}`.
4. On Windows the controller (`src/lib/deskflow/NativeFileDrag.cpp`, `handleDragPacket` → `handleOffer` / `handleStart`) should do two things:
   - Answer the offer with an `ack` packet straight away, then receive the file in 64 KiB chunks into a temp staging folder.
   - On `start`, call `NativeFileDragPlatform::startPromiseDrag` (`src/lib/deskflow/NativeFileDragWindows.cpp`). That injects a tagged LEFTDOWN and runs `QDrag::exec`, so the file icon follows the cursor. When the user releases, Explorer receives the files (`PromisedFiles::retrieveData` waits until they are complete).

## Evidence collected on the Mac (server log)

- The Windows client **does** advertise the new capability (file drag mode 3). So the Windows build contains the new code, or at least the new `ServerProxy.cpp`.
- On every Mac → Windows attempt the Mac logs:
  - `edge drag: handing 1 file(s) to the other computer`
  - `edge drag: moving dragged files from "<mac>" to "LAPTOP-9VCVUKCD"`
  - `switch from "<mac>" to "LAPTOP-9VCVUKCD"`
- After that, **Windows never sends anything back**: no `ack`, no `cancel`. Not even the automatic cancel that the Windows code sends 60 seconds after an offer that never got a `start`. The Mac never logs `files delivered` or `sending stopped`.
- On screen, the file snaps back in Finder (expected: the Mac ends its own drag), but **no drag icon appears on Windows**.
- On macOS, a unit test feeds an offer into the same controller code, and it replies with an `ack` correctly. So the failure is specific to Windows: the build, the process environment, or the Windows-only code.

## What to check, in this order

1. **Which binary is running.**
   - Find the installed Deskflow version and build time, and confirm the running `deskflow-core.exe` was built from this source tree (`git log -1` in this folder, if it is a git repo).
   - Confirm `NativeFileDragWindows.cpp` is compiled into the core (`src/lib/deskflow/CMakeLists.txt`, `if(WIN32)` block).
2. **How the core runs.** Deskflow can start the core as a Windows service (as SYSTEM, through the daemon) or in desktop mode (started by the GUI as the user).
   - The drag code needs desktop mode in the logged-in user's interactive session.
   - Check the Deskflow setting `core/processMode` (it must be Desktop), whether a Deskflow daemon/service is installed and still running, and which user, session ID and integrity level `deskflow-core.exe` runs at (Task Manager details or `Get-Process` / Process Explorer).
3. **Elevation (UIPI).** If `deskflow-core.exe` runs elevated (as administrator) while Explorer is not:
   - Explorer cannot drop onto the core's hidden catcher window.
   - The OLE drag/drop between the two processes may be blocked.
   - Report the integrity levels of `deskflow-core.exe`, `Deskflow.exe` and `explorer.exe`.
4. **Turn on detailed logging on Windows.** In Deskflow settings, enable log to file with level **Verbose** (or set `log/toFile=true`, `log/level=Verbose`, `log/file=<path>` in the Deskflow config). Restart Deskflow, then reproduce one Mac → Windows drag of a small file.
   - Find every line containing `edge drag`, `DNFM`, `WARNING` or `ERROR`.
   - Check whether the client logs receiving the `DNFM` message at all.
5. **If the packets arrive but nothing happens,** add temporary `LOG_INFO` lines in `NativeFileDrag.cpp`, rebuild and reproduce:
   - in `receive()`, inside the queued lambda: the peer, `type`, `id`, and whether `controller` is null or `peers` lacks the peer
   - in `handleOffer` and `handleStart`, plus the `ack` going out through `sendDrag`
   - in `NativeFileDragWindows.cpp`: `startPromiseDrag` (before and after `QDrag::exec`, plus whether the injected LEFTDOWN was observed) and `PromisedFiles::retrieveData`

   This will show whether the core's Qt main thread receives the queued calls, or is blocked (for example in a modal loop).
6. **Fix the cause** you find, rebuild with this machine's usual Windows build, and verify:
   - dragging a small file from the Mac onto the Windows desktop creates an identical copy, and the Windows log shows the offer, the start, the drop and the completed receive
   - the Mac log shows `edge drag: files delivered`

## Not a bug: dragging Windows → Mac with the laptop's own touchpad

- Deskflow only shares the **Mac's** input. Input from the laptop's own touchpad never reaches the Mac, and this fork deliberately ignores it at the screen edges, so the cursor stops at the edge.
- Windows → Mac edge drag only works when the drag is made with the **Mac trackpad** while the cursor is on Windows.
- Do not "fix" this by letting the native touchpad switch screens. Report it only if a drag made with the Mac trackpad also fails.

## Report back

Report:
- the root cause, with the log lines that prove it
- the code or setting you changed (a diff)
- the Windows log excerpt from one successful Mac → Windows drag
