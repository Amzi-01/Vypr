# vypr-agent

The guest half. Runs on Windows, captures individual windows, and publishes them
into rings of its own locked memory that the host reads straight out of the VM's
RAM - see "Frames travel through guest RAM" in `docs/technical.md`.

## Requirements

- **MSVC Build Tools + Windows SDK.** Not optional: C++/WinRT and the WGC interop
  headers are not usable from mingw, so this cannot be cross-compiled from Linux.
- **The "Lock pages in memory" right** for the account it runs as, which
  `vypr-setup` grants. No driver: rings are allocated with Windows' AWE calls.
- **Windows 10 1903 or newer** for `Windows.Graphics.Capture` per-window capture.
  1903 also removes the need for a message loop via `CreateFreeThreaded`.

## Building

Inside the guest, from a Developer Command Prompt:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
```

## Running

```
vypr-agent.exe --host 192.168.122.1
```

Must run **as the interactive desktop user**, never as a service. Session 0 has
no DWM to capture from, and `SetForegroundWindow` does not work from it.

## Two more things that will bite

**The lock-pages right only applies from the next sign-in.** Granting it does
nothing for a session already running, so an agent started straight after
`vypr-setup` says it cannot lock pages and streams nothing until Windows has been
signed out or restarted. It says so in its log and in vyprd's.

**A display must be attached to the passthrough GPU.** `<video model='none'/>`
means the guest's only display is the 5050's physical outputs. WGC captures from
DWM's composited per-window surfaces; with no monitor or dummy plug there is
nothing composited and `GraphicsCaptureSession::IsSupported()` is the least of
the problems.

## Verified working

Built with MSVC 14.44 against Windows SDK 10.0.22621 and run against `vyprd`:
Notepad from the guest presented as a native Linux window at **60 fps**, with
WGC capture, the shared-memory transport and the control channel all live.

Two things that cost real time, both worth knowing:

**Optional WinRT interfaces must be reached through `try_as`, never called
directly.** `GraphicsCaptureSession.IsBorderRequired` is `IGraphicsCaptureSession3`
- Windows 11 22000+ - and this guest is Windows 10 19045. C++/WinRT's property
shim reinterpret-casts the object to the interface rather than doing a
QueryInterface, so calling a method the runtime class does not implement
dispatches through a vtable that is not there. That is an access violation, and
**no try/catch will catch it**. `IsCursorCaptureEnabled` has the same shape
(`IGraphicsCaptureSession2`, Windows 10 2004+) and got the same treatment.

**The agent cannot run from an SSH session.** `GraphicsCaptureSession::IsSupported()`
returns false there because the session has no desktop. Launch it in the console
session instead:

```
schtasks /create /tn vypr-agent /tr "cmd /c C:\vypr\guest\build\vypr-agent.exe --host 192.168.122.1 > C:\vypr\agent.log 2>&1" /sc once /st 00:00 /it /f
schtasks /run /tn vypr-agent
```

Set `VYPR_TRACE=1` for step-by-step tracing of capture startup.

Input injection is verified too - typing into the host window arrives in the
guest application, so the PS/2 set 1 scancode table and the AttachThreadInput
focus handling both work.

**All input coordinates are in captured-surface space**, which is the DWM
extended frame - not the client rect, and not GetWindowRect. See
`geometry.hpp`; getting this wrong silently offsets every click by the height
of the title bar.

## Still unproven

Cursor shapes, reconnect, and anything that moves fast enough to expose
latency.

Rings are scattered across guest RAM - a 4K ring is ~24,000 pages in ~20,000
pieces - and the host puts them back together with one mapping per piece. That
was measured on Linux with fake guest RAM; how scattered a real guest is after a
long uptime is worth watching in vyprd's log ("ring: N MiB in M pieces").
