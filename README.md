# sash

Per-application native windows from a Windows VM, over shared memory.

An app running in the guest appears as an ordinary window on the Linux desktop —
its own entry in the taskbar, its own place in the stacking order — with no RDP
and no video codec in the path.

## Why not the obvious approaches

**RDP RemoteApp** (what WinApps uses) hands over each window as a separate
object, which is exactly the right shape. Its video path is built for documents:
it is fine for a text editor and poor for anything that moves.

**Sunshine/Moonlight** has an excellent encoder, but streams a whole display.
Everything inside it arrives as one window with one identity, so it cannot make
two guest apps into two host windows.

**Looking Glass** proves the transport this project uses, but captures with DXGI
Desktop Duplication, which returns the *composited desktop*. A window behind
another simply is not present in that data, so windows cannot be cropped out of
it afterwards.

`Windows.Graphics.Capture` is the piece that makes per-window work: it captures a
specific `HWND` from DWM's own per-window surfaces, and keeps producing frames
when the window is occluded, minimised, or offscreen. The whole design rests on
that property.

## Design

```
guest (Windows, C++)                       host (Linux, C)
┌───────────────────────────┐              ┌─────────────────────────────┐
│ WGC capture per HWND      │── IVSHMEM ──▶│ sash-host, one per window   │
│ publish under a seqlock   │   (pixels)   │ SDL3 present, native window │
│ SendInput injection       │◀── TCP ──────│ input, window lifecycle     │
└───────────────────────────┘  (control)   └─────────────────────────────┘
```

**Pixels go through IVSHMEM uncompressed.** The guest has a passthrough GPU, and
the shared BAR is host RAM, so a frame costs one write over PCIe and one read —
no encode, no decode, no network stack, and no codec latency at all.

**Control goes over TCP** on the virtual bridge, where a round trip is tens of
microseconds. One connection per session, not per window: window identity is an
explicit `window_id` in every message, so a re-attaching stream can say which
`HWND` it used to be.

**The host owns allocation.** It carves slots and rings out of the region and
tells the guest the offsets. Neither side needs a shared allocator and there are
no cross-OS atomics in the allocation path.

**One host process per window**, so a wedged stream costs one window rather than
the session, and the compositor sees the separate top-levels it needs to.

### Frame handoff

Each slot is a ring of three buffers. The guest writes pixels into the next
buffer, then publishes `{index, serial, width, height, stride}` under a seqlock:
counter to odd, write the record, counter to even. A host that reads an odd
counter, or a different one before and after, saw a torn record and retries.
Two stores per frame, and no lock spanning the VM boundary.

`tools/sash-testsrc.c` is the reference implementation of that publish path. If
it and the guest agent ever disagree about ordering, the tool is right — the
host is verified against it.

## Measured

Host presenting `sash-testsrc` on an RTX 3060, SDL3 `gpu` backend:

| Source | Presented | Dropped | Upload | Present |
|---|---|---|---|---|
| 1920x1080 @ 60 | 60 fps | 0 | 3.0 ms | 13.7 ms (mostly vsync wait) |
| 3840x2160 @ 60 | 55 fps | 5-6 | 15.9 ms | 2.5 ms |

Renderer backend is worth roughly 2x at 4K and was the single largest factor
found so far: SDL's default `opengl` backend presents synchronously at 21-24 ms,
which misses vsync on its own and pins the stream to half refresh. `gpu` presents
in ~2 ms and lets the upload overlap, so it is now the default.

4K60 is not yet clean. The remaining cost is the 16 ms upload, which runs at
~2 GB/s against 14.5 GB/s of measured memory bandwidth — the ceiling is SDL's
`LockTexture`/`UnlockTexture` staging, not the transport. Closing it means
uploading from the mapped region directly via a persistently mapped PBO, which
requires dropping `SDL_Renderer` for direct EGL or Vulkan.

## It works

A Windows Notepad window from the guest, presented as an ordinary window on the
Linux desktop, at 60 fps with no codec anywhere in the path:

```
sashd: guest window 'Untitled - Notepad' 2858x1460
sashd: window 'Untitled - Notepad' -> slot 0, pid 190725
sash:  streaming HWND 00000000000801FC into slot 0

slot 0 state   : LIVE      frame size : 2860x1536  stride 12544
serial         : 1947 -> 2067   (60.0 fps)
```

## Latency

The guest stamps each frame with QueryPerformanceCounter, which means nothing
in host time on its own. The daemon aligns the two clocks over the control
channel - ping, guest counter, pong - and assumes the guest read its counter
halfway through the round trip. Measured round trip across the virtual bridge
is **0.30 ms**, so that assumption is wrong by at most ~0.15 ms, far below a
frame. The offset lands in the shared region because the process presenting
frames is not the one that owns the control channel.

`--stats` then reports frame age: guest capture to host acquire, in host time.

```
60 fps presented, 0 dropped | upload 0.3 ms, present 16.3 ms | age avg 6.2 ms worst 14.3 ms
```

That 6 ms is the whole transport - capture, PCIe, and the ring. It excludes the
guest's own render-to-capture delay and the host's scanout, so glass to glass is
that plus up to one refresh interval.

## Status

| Component | State |
|---|---|
| `include/sash_shm.h` — region layout | done |
| `include/sash_proto.h` — control protocol v1 | defined, not yet spoken |
| `host/src/shm.c` — mapping, allocation, seqlock reader | done, verified |
| `host/src/main.c` — present a slot as a native window | working |
| `host/src/present_gpu.c` — SDL_GPU upload path | working, 4x faster at 4K |
| `host/src/present_render.c` — SDL_Renderer path | kept for comparison |
| `tools/sash-testsrc.c` — reference producer | working |
| Guest agent — publish path (`guest/src/publisher.cpp`) | verified on Linux, 1080p60, 0 drops |
| Guest agent — WGC capture | **working** — Notepad at 60 fps |
| Guest agent — IVSHMEM mapping | **working** — self-identifies by magic |
| Guest agent — input injection | **working** — typed into the host window, arrived in the guest |
| Popups and menus | **working** — real popup surfaces, GDI fallback for menus |
| IVSHMEM device on the VM | added — 512 MB, PCI 08:02 |
| IVSHMEM driver in the guest | installed with Looking Glass |
| `host/src/sashd.c` — session daemon | working, verified end to end |
| `host/src/msg.c` — framing, shared by both host processes | done |
| Input path — pointer, keys, focus, resize, close | working, verified |
| Launcher / `.desktop` integration | not started |

## Building

```bash
cd host
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Needs SDL3.

## Running

```bash
./build/sashd --match Notepad --launch 'C:\\Windows\\System32\\notepad.exe'
```

`sashd` formats the region, waits for the agent, and spawns one `sash-host` per
matching guest window. `--all` streams every window, which is the way to see what
the guest is actually offering.

## Running without the VM

The whole system runs with the guest powered off, daemon included:

```bash
truncate -s 256M /dev/shm/sash-test
./build/sashd --shm /dev/shm/sash-test --match "test window" --port 47899 &
./build/sash-testagent --connect 127.0.0.1 --port 47899 --shm /dev/shm/sash-test
```

`sash-testagent` speaks the real control protocol and links the real
`publisher.cpp`, so this covers slot allocation, attach, client spawning, the
frame handoff and the input return path. Verified: window appears, streams at
60fps, and pointer/focus/resize events arrive at the agent in guest coordinates.

For the presenter alone, without the daemon:

```bash
./build/sash-testsrc --shm /dev/shm/sash-test --size 1920x1080 --fps 60 &
./build/sash-host --shm /dev/shm/sash-test --slot 0 --stats
```

## Known hard problems

Inherited from the earlier prototype's notes, none of them solved yet:

- ~~**DPI / coordinate space.**~~ Done, and it was the cause of menu bar clicks
  being ignored. Three rectangles describe a window and all three differ - at
  150% scaling Notepad measured window 2085x1053 at 152,152, client 2063x967 at
  163,227, and DWM extended frame 2065x1043 at 162,152. WGC captures the **DWM
  extended frame**; reporting the client rect put the origin 75px too low, the
  height of the title bar plus menu bar, so every click landed that far down and
  menu bar clicks reached the text area instead. `geometry.hpp` now defines the
  captured rectangle once and enumeration, input and the GDI fallback all use
  it.
- **Popups and menus.** Each is its own `HWND`, so each arrives as its own
  stream and has to be positioned against its owner — `owner_id` exists in the
  protocol for this, and nothing uses it yet.
- **Z-order and focus.** The host WM owns stacking; the guest has its own idea.
  Unreconciled, the two fight.
- **Reconnect.** A dropped stream must re-attach to the same `HWND` rather than
  opening a second window for it.
- **Cursor.** Whether to composite the guest cursor into the frame or hand the
  host a cursor shape. The protocol assumes the latter.
