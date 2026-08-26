# sash-agent

The guest half. Runs on Windows, captures individual windows, and publishes them
into the shared region the host carved.

## Requirements

- **MSVC Build Tools + Windows SDK.** Not optional: C++/WinRT and the WGC interop
  headers are not usable from mingw, so this cannot be cross-compiled from Linux.
- **The IVSHMEM driver**, which ships with Looking Glass rather than virtio-win.
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
sash-agent.exe --host 192.168.122.1
```

Must run **as the interactive desktop user**, never as a service. Session 0 has
no DWM to capture from, and `SetForegroundWindow` does not work from it.

## Two more things that will bite

**Two IVSHMEM devices now exist** on this VM - Looking Glass's and sash's - and
the same driver binds both. The agent does not select by device index, which
would silently stream into Looking Glass's region the first time PCI order
changed. It maps each device and keeps the one containing the sash magic the
host wrote. If it reports finding devices but no sash region, the host session
is not running yet.

**A display must be attached to the passthrough GPU.** `<video model='none'/>`
means the guest's only display is the 5050's physical outputs. WGC captures from
DWM's composited per-window surfaces; with no monitor or dummy plug there is
nothing composited and `GraphicsCaptureSession::IsSupported()` is the least of
the problems.

## Verified working

Built with MSVC 14.44 against Windows SDK 10.0.22621 and run against `sashd`:
Notepad from the guest presented as a native Linux window at **60 fps**, with
WGC capture, the IVSHMEM mapping and the control channel all live.

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
schtasks /create /tn sash-agent /tr "cmd /c C:\sash\guest\build\sash-agent.exe --host 192.168.122.1 > C:\sash\agent.log 2>&1" /sc once /st 00:00 /it /f
schtasks /run /tn sash-agent
```

Set `SASH_TRACE=1` for step-by-step tracing of capture startup.

Input injection is verified too - typing into the host window arrives in the
guest application, so the PS/2 set 1 scancode table and the AttachThreadInput
focus handling both work.

## Still unproven

Cursor shapes, popups and menus, reconnect, DPI scaling, mouse buttons and
wheel, and anything that moves fast enough to expose latency.

The IVSHMEM IOCTL numbers in `ivshmem.cpp` were checked against the installed
driver (2025-03-06) by reading its PDB and image rather than by compiling: the
symbol order gives function codes 0x800-0x803 and the interface GUID is present
in the `.sys`. Re-check if the driver is updated; a changed IOCTL number fails
as `ERROR_INVALID_FUNCTION`.
