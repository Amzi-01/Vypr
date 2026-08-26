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

## Two things that will bite

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

## Unverified

Written but never compiled - there is no Windows toolchain in the guest yet. The
frame handoff underneath it *is* verified: `publisher.cpp` is compiled on Linux
and run against the real host client by `tools/sash-testagent.cpp`, at 1080p60
with zero drops. What that leaves unproven is capture, the IVSHMEM mapping, and
input - not the seqlock ordering or ring arithmetic.

The IVSHMEM IOCTL numbers in `ivshmem.cpp` were checked against the installed
driver (2025-03-06) by reading its PDB and image rather than by compiling: the
symbol order gives function codes 0x800-0x803 and the interface GUID is present
in the `.sys`. Re-check if the driver is updated; a changed IOCTL number fails
as `ERROR_INVALID_FUNCTION`.
