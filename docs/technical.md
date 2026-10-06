# Vypr — how it works

The [README](../README.md) covers what Vypr is and how to install it. This is
the long version: why it is built the way it is, what was measured, and what
went wrong on the way. Most of it is written as findings rather than
description, because nearly every design decision here was forced by something
that did not work.

---

## Why not the obvious approaches

**RDP RemoteApp** (what WinApps uses) hands over each window as a separate
object, which is exactly the right shape. Its video path is built for documents:
it is fine for a text editor and poor for anything that moves.

**Sunshine/Moonlight** has an excellent encoder, but streams a whole display.
Everything inside it arrives as one window with one identity, so it cannot make
two guest apps into two host windows.

**Looking Glass** proved the shared-memory approach this project started from,
but captures with DXGI Desktop Duplication, which returns the *composited
desktop*. A window behind
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
│ WGC capture per HWND      │─ guest RAM ─▶│ vyprd, then one vypr-window │
│ publish under a seqlock   │   (pixels)   │ per top-level and its popups│
│ SendInput injection       │◀── TCP ──────│ input, window lifecycle     │
│ WASAPI loopback           │── TCP ──────▶│ playback                    │
└───────────────────────────┘              └─────────────────────────────┘
            two TCP connections on one port: control, and audio on its own
```

**Pixels go through the guest's own RAM, uncompressed.** The agent locks each
window's ring as physical pages and the host reads those pages straight out of
QEMU's memory file, so a frame costs one copy in the guest and none on the host
— no encode, no decode, no network stack, no codec latency, and no driver on
either side. See *Frames travel through guest RAM* below.

**Control goes over TCP** on the virtual bridge, where a round trip is tens of
microseconds. One connection per session, not per window: window identity is an
explicit `window_id` in every message, so a re-attaching stream can say which
`HWND` it used to be.

**The guest owns ring memory; the host never writes it.** The host decides
slots and ring geometry and keeps them in a host-only table; the guest allocates
each ring itself and is the only writer of it. Neither side needs a shared
allocator, and a host bug cannot corrupt guest memory.

**One host process per window**, so a wedged stream costs one window rather than
the session, and the compositor sees the separate top-levels it needs to.

### Frames travel through guest RAM

Until 2026-10-06 frames went through an IVSHMEM device: a PCI BAR backed by a
host file. That needed a kernel driver in the guest to map the BAR into user
space, and the only signed one ships with Looking Glass. Windows will not load
an unsigned kernel driver without test-signing mode, which means turning Secure
Boot off - and which a lot of anti-cheat software refuses to run under - so
"write our own driver" was never an option. The transport was redesigned so that
no driver is needed at all.

**The guest allocates.** On `ATTACH` the agent allocates the ring - one header
page and three frame buffers - with Windows' AWE calls
(`AllocateUserPhysicalPages`, `MapUserPhysicalPages`). They are the documented
way for a user-mode process to hold physical pages and be told which pages they
are, and the pages are locked: never paged out, never moved. The only
requirement is the "Lock pages in memory" right, which `vypr-setup` grants.

**The host reads them where they are.** QEMU keeps guest RAM in a shared memfd
(`<memoryBacking>` with `memfd` and `shared`, which virtiofs already needed),
reachable as `/proc/<qemu>/fd/N`. Linux only opens that for a process whose user
*and* group match QEMU's, so `qemu.conf` has `group =` set to the user's own
group, next to the `user =` line the microphone already needed.

**Nothing is shown until every page checks out.** The agent stamps each page
with a magic, a per-ring nonce, its index and its physical page number, and
sends the page list (`RING_PAGES`, several messages for a big ring). vyprd maps
guest RAM read-only, translates each page through QEMU's memory layout - it
learns where the 4 GiB hole is from the first ring that reaches past it, by
finding the one layout under which every stamp reads back - and only then
writes the ring into its slot table and sends `RING_READY`. Until then the agent
writes nothing, so the stamps are still there to check. A wrong translation, a
stale list or a page shared between rings fails here rather than as a garbled
window.

**The host never writes guest memory.** When the agent exits, Windows takes its
locked pages back at once and may hand them to anything, so a host store into
one would corrupt some unrelated guest process. So vyprd maps guest RAM
read-only, every host-side decision lives in a separate host-only region
(`/dev/shm/vypr-host`), and the guest hears about them over TCP. A presenter
maps the ring read-write only because Vulkan will not import a read-only
mapping; nothing writes through it, and the GPU only ever copies out of it.

**Scattered pages, still zero-copy.** A guest that has been running a while
rarely has two free pages side by side: a 4K ring is ~24,000 pages in ~20,000
pieces. The presenter reserves address space for the whole ring and maps each
run into place - about 60 ms, once per window - which makes it one contiguous
range again, so the Vulkan presenter still hands the whole ring to the GPU in
one import. Measured on the RTX 5050 with `vypr-testsrc` at 3840x2160, a ring
in 24,481 pieces: 60 fps, upload 0.0-0.1 ms, frame age 0.5 ms average, 1.0 ms
worst - the same as the IVSHMEM path at its best.

Rings cost guest RAM now (about 100 MiB for a 4K window, only while it is
streamed) where the IVSHMEM device cost a fixed 512 MiB of host RAM whether
anything was streamed or not. How scattered a ring is depends on how long the
guest has been up: the 4K desktop's ring came back in 1,562 pieces just after a
boot and in 23,755 after a day of use. Both map in well under a frame.

### Reading frames back in bands (2026-10-06)

Measured honestly (see the next section), most of the guest's cost per frame
is not Vypr's copy at all but the GPU reading the frame back to system memory:
about 6 of the 7.5 ms for a 2560x1440 frame on this VM, whose passthrough GPU
sits on a PCIe x4 link. In one piece, the copy into the ring cannot start until
the last row has arrived.

So a frame of 8 MB or more (1080p and up) is read back as eight horizontal
bands, each into its own staging texture with its own fence. The reader copies
band 0 into the ring while the GPU is still reading bands 1-7, and so on down,
which leaves only the last band's copy after the readback ends. The capture
surface goes back to WGC's pool the moment the GPU is done with it - holding it
until the reader got round to the last band cost frames, because the pool has
three surfaces and DWM ran out. Smaller frames are read back whole: banding a
720p frame bought 6-10% on average and cost a millisecond or two at the worst.
Damage mode diffs the frame as a whole and is never banded.

The copy itself uses non-temporal stores (`stream_copy` in `capture.cpp`).
Rings are write-back RAM now, and an ordinary store reads each cache line before
overwriting it and evicts something the game wanted; the old write-combined BAR
got this for free.

Measured with `bench.exe` - a D3D11 window redrawing every vsync - on this VM,
old pipeline (IVSHMEM, Looking Glass's driver) against new, same session, same
metric, several runs each:

| | v2 capture→publish | v3 | v2 capture→host | v3 |
|---|---|---|---|---|
| 1280x720 window | 4.55-4.66 ms | 4.55-4.71 ms | | |
| 1920x1080 window | 5.98 ms | **5.02 ms** | | |
| 2560x1440 window | 7.42 ms | **6.29 ms** | 8.1 ms | **7.0 ms** |
| 3200x1800 window | 10.80 ms (worst ~22.7) | **8.72 ms** (worst ~12.9) | | |
| whole 4K desktop | 13.82 ms | **11.74 ms** | 14.5 ms | **12.6 ms** |

Frame rate was the same throughout (58-60 fps, run-to-run noise ±0.5), with
nothing dropped. The biggest remaining cost is the PCIe x4 link: the same GPU in
an x16 slot would read a frame back four times faster, which no change on
either side of the stream can match.

### The capture timestamp was in the future

Every latency figure before 2026-10-06 was too low. Frames were stamped with
WGC's `SystemRelativeTime`, which for a window presenting on vsync is the
vblank the composition is *for* - up to a frame after the capture. The agent's
own capture-to-publish time came out negative (about -7 ms) and the host's
frame age mostly read 0, because negative samples were discarded. A frame cannot
have been captured after it arrived, so the stamp is now bounded by the
arrival time, and both figures are positive and honest. Older numbers in this
document were measured with the old stamp.

### Frame handoff

Each slot is a ring of three buffers. The guest writes pixels into the next
buffer, then publishes `{index, serial, width, height, stride}` under a seqlock:
counter to odd, write the record, counter to even. A host that reads an odd
counter, or a different one before and after, saw a torn record and retries.
Two stores per frame, and no lock spanning the VM boundary.

`tools/vypr-testsrc.c` is the reference implementation of that publish path. If
it and the guest agent ever disagree about ordering, the tool is right — the
host is verified against it.

## Measured

Host presenting `vypr-testsrc` on an RTX 3060, SDL3 `gpu` backend:

| Source | Presented | Dropped | Upload | Present |
|---|---|---|---|---|
| 1920x1080 @ 60 | 60 fps | 0 | 3.0 ms | 13.7 ms (mostly vsync wait) |
| 3840x2160 @ 60 | 55 fps | 5-6 | 15.9 ms | 2.5 ms |

Renderer backend is worth roughly 2x at 4K and was the single largest factor
found so far: SDL's default `opengl` backend presents synchronously at 21-24 ms,
which misses vsync on its own and pins the stream to half refresh. `gpu` presents
in ~2 ms and lets the upload overlap, so it is now the default.

The 16 ms upload above runs at ~2 GB/s against 14.5 GB/s of measured memory
bandwidth. That ceiling was `SDL_Renderer`'s `LockTexture`/`UnlockTexture`
staging, which copies through a CPU buffer — not the transport. `present_gpu.c`
avoids it by writing into a mapped `SDL_GPUTransferBuffer` instead, and is the
default for that reason.

**Zero-copy, 2026-10-05.** `present_vk.c` removes the CPU copy altogether. The
ring is ordinary host RAM, so it is imported into Vulkan once with
`VK_EXT_external_memory_host` and every frame is a single DMA out of it, on the
GPU's dedicated copy engine rather than the graphics queue. Measured headless
under gamescope on the RTX 5050, 3840x2160 @ 60 from `vypr-testsrc`, with a
game running on the same GPU:

| Backend | Upload (CPU) | Shown, avg | Shown, worst | GPU copy |
|---|---|---|---|---|
| `gpu` (SDL_GPU) | 10.7 ms | 12.2 ms | ~22 ms | ~8.5 ms, graphics queue |
| `vulkan` (zero-copy) | 0.1 ms | 1.2 ms | ~2.5 ms | ~3.5 ms, copy engine |

"Shown" stops when the present is submitted, so the GPU copy - which runs after
it, asynchronously - is in neither column; the last column is that copy measured
on its own. End to end the host side went from roughly 19 ms to roughly 4 ms. It
is now the default, falling back to `gpu` and then `render`.

**The table further up predates both and wants re-measuring.** They
are left as recorded rather than adjusted by hand; run `vypr-window --stats`
against `vypr-testsrc` to replace them. In live use a 4K guest window publishes
a steady 60 fps, but that is the guest's publish rate, which is a different
measurement from the host's present cost.

### The guest side of a frame (2026-10-05)

Three faults in the agent were costing more than the transport:

- **Each frame waited for the next.** The readback used two staging textures
  and always read the one written a frame earlier, so a frame was published only
  when the following one arrived - a full frame interval late, and on a still
  screen, where WGC only calls back on change, indefinitely. A frame is now
  published the moment its own GPU copy lands: the capture callback starts the
  copy, and a reader thread waits for it on a D3D11 fence.
- **The agent ran below normal priority.** A task registered by `schtasks`
  without a priority runs at Task Scheduler's default of 7, so capture and input
  injection both queued behind any guest game. The agent raises itself to high
  priority at start, and the capture threads join MMCSS's "Capture" class.
- **Publishing lacked write barriers.** The region was mapped write-combined,
  which x86's ordinary store ordering does not cover, so the record could become
  visible before the pixels it describes. An `sfence` now precedes the publish.
  Rings are ordinary write-back memory since moving into guest RAM, but a large
  `memcpy` may still use non-temporal stores, so the fence stays.

Live on the 4K guest desktop, frame age while things are moving went from 10-37
ms average (60-180 ms worst) to 2-4 ms average (5-30 ms worst). These figures
used the old capture timestamp, which read low - see *The capture timestamp was
in the future*.

### Damage: sending only what changed (opt-in, 2026-10-05)

Everything above moves a whole frame per update. For a game that is the right
thing to do - most of the screen changes every frame. For a desktop app it is
mostly waste: a typed character changes a few hundred pixels and Vypr was
copying 33 MB to show it.

With damage on, the guest diffs each frame against the last in 64-pixel tiles,
writes only the changed rectangles into the ring, and names them in the publish
record (`VYPR_PUB_DAMAGE_RECTS`, up to `VYPR_MAX_DAMAGE_RECTS` of them). The host
keeps a persistent copy of the window and paints just those rectangles over it -
`present_vk.c` into a single accumulator texture, and the two fallback backends
likewise. A frame that changed nothing is not sent at all. The first frame, a
resize, or damage too scattered to fit the rectangle budget falls back to a
whole frame, so the picture is never wrong, only sent more cheaply.

It is **off by default** and turned on per session with `VYPR_DAMAGE=1` (which
passes `--damage` to `vyprd`, which sets `VYPR_ATTACH_DAMAGE` on each attach).
The reason it is opt-in rather than automatic: the guest pays a per-frame CPU
diff of the frame, which is cheap against the transfer it saves for a still
screen and pure overhead for full-screen motion. The workload decides, so the
user does.

Measured with `vypr-testsrc --damage` (a static background, one moving block)
at 1280x720, the host upload fell from the whole-frame figure to **0.0 ms** -
only the block's rectangle crosses - and all three backends reconstruct the
frame pixel-for-pixel. On the guest the ring write shrinks to the changed tiles
too; the readback from the guest GPU does not, as below.

Live on the 4K guest desktop, read straight out of the publish records over
ten seconds of an idle desktop: 20 partial frames and no full ones, each a single
64x64 rectangle - 16 KB per update where a whole frame is 33 MB.

What it does not fix is idle latency. A change on a still desktop still arrives
~45 ms after it was composed, with or without damage, because the guest reads
the whole frame back from its GPU before it can diff it. Damage removes the
transfer cost, not that readback. Reading back only the changed rectangles needs
the dirty regions before the copy, which Windows' capture API can report on
recent builds; that is the next step.

The wire format changed to carry the rectangles, so the region and protocol
versions both went to 2; a host and guest built from different trees refuse each
other rather than misread the longer records. Deploying it therefore means
replacing the agent and the host together.

## It works

A Windows Notepad window from the guest, presented as an ordinary window on the
Linux desktop, at 60 fps with no codec anywhere in the path:

```
vyprd: guest window 'Untitled - Notepad' 2858x1460
vyprd: window 'Untitled - Notepad' -> slot 0, pid 190725
vypr:  streaming HWND 00000000000801FC into slot 0

slot 0 state   : LIVE      frame size : 2860x1536  stride 12544
serial         : 1947 -> 2067   (60.0 fps)
```

## Latency

The guest stamps each frame with QueryPerformanceCounter, which means nothing
in host time on its own. The daemon aligns the two clocks over the control
channel - ping, guest counter, pong - and assumes the guest read its counter
halfway through the round trip. Measured round trip across the virtual bridge
is **0.30 ms**, so that assumption is wrong by at most ~0.15 ms, far below a
frame. The offset lands in vyprd's slot table because the process presenting
frames is not the one that owns the control channel.

`--stats` then reports frame age: guest capture to host acquire, in host time.

```
60 fps presented, 0 dropped | upload 0.3 ms, present 16.3 ms | age avg 6.2 ms worst 14.3 ms
```

**The offset comes from the lowest-latency exchange, not the most recent one.**
The estimate assumes the guest read its counter halfway through the round trip,
so the error it hides is up to half that trip. Round trips reach *seconds* when
the guest is saturated by a game - measured 1039 ms and 2814 ms - which is
exactly when a latency figure is wanted. A slow exchange is evidence of
queueing, not of a changed offset, so the best sample from the last 60 seconds
wins. That holds the error at ~0.15 ms under full game load, where taking the
latest sample produced a 470 ms error and nonsense readings.

### Where the time actually goes

Frames are stamped with WGC's `SystemRelativeTime` - when DWM composed the
frame - not with a counter read just before publishing. The latter excludes the
guest's whole capture pipeline, which is where the time is, and flattered the
figure to 0.7 ms.

Measured on FiveM at 3840x2160, guest side only, needing no clock alignment:

```
capture->publish avg  0.65 ms  worst  23.21 ms
capture->publish avg 45.53 ms  worst  96.96 ms
```

That is the finding: **the transport is not the bottleneck and never was.**
Publish-to-host-acquire is under a millisecond. The cost is WGC capture plus the
GPU-to-CPU readback inside the guest, and it swings by two orders of magnitude
with GPU contention. Optimising the shared-memory path further would buy
nothing; the readback is the thing to attack.

## Launching

`vypr run <app>` brings up whatever is not already running - the VM, the session
daemon, the guest agent, Parsec for its driver - and then starts the app,
skipping any step that is already done. Each registered app gets a desktop entry,
so it is in the application menu like anything else.

An app can match every window it puts up - for FiveM that is the splash, the
Rockstar launcher and the sign-in dialog, not only the main game window - so the
whole startup sequence is visible rather than a blank wait followed by a game.
Each appears and closes in turn as its own host window.

**A window can freeze while everything around it is healthy.** WGC reads DWM's
per-window surfaces, so anything that takes a window's swapchain past the
compositor takes the surface with it: true exclusive fullscreen does, and so
does a *borderless* window whose swapchain is promoted to independent flip.
Nothing fails when it happens — the process runs, the window reports its size,
audio keeps playing, no error is raised anywhere, and WGC simply stops calling
back. Measured on Call of Duty: `wgc callbacks 33, dropped 0`, unchanged across
every report while the game burned four cores.

Two things follow. Exclusive fullscreen genuinely cannot be captured, so a
window that produces nothing for ten seconds says so and suggests borderless.
And because borderless can stall too, the agent restarts the capture session
against the same `HWND` after five seconds without a callback, up to five
attempts — recreating the session picks up the new surface. The counters that
made this diagnosable are reported every five seconds and stay in: a capture
that has stopped and one whose frames are being discarded look identical from
the host, and both look like a frozen window.

**Some games hide their window title from matching.** Call of Duty's title
carries fifty-two `U+200B` ZERO WIDTH SPACE characters, one between every
visible character, so the title that reads `Call of Duty: Modern Warfare II`
contains the substring `Call` nowhere in it — and it is invisible in any log you
print it to. Titles are therefore reduced to letters and digits, lowercased,
before matching, on both sides. The same pass absorbs the ordinary reasons a
title does not match what someone typed: `®`, colons and spacing.

**Steam games** are registered by their URL, and the app id is not a window
title, so Steam's own record of the game's name is used instead:

```bash
vypr add cod 'steam://rungameid/3595230' --name 'Call of Duty Modern Warfare II'
```

**Window titles to match are derived from the app** — its display name, its
short name and the executable's basename — so `vypr add notepad ...` finds
'Untitled - Notepad' without being told. Anything beyond that is app-specific:
FiveM also puts up Cfx, Rockstar and Social Club windows, and those belong in
that app's `--match` flags rather than in every app's defaults.

A daemon already running for a *different* app is watching for that app's
titles and will ignore the new one — the window is offered and logged as `no
title match`, which looks exactly like the app failing to start. Launching a
second app therefore restarts the daemon with both sets of titles, so neither
app goes dark.

**The VM shuts down when the last window closes.** Nothing else was watching:
`vypr run` returns as soon as the app is on screen and the daemon stays up
waiting for more windows, so a supervisor runs alongside the session and, once
the last streamed window has been gone for `SHUTDOWN_GRACE` seconds, tears the
session down and asks the VM to shut down. The grace period is what makes it
recoverable — a window closed by accident is one relaunch away from cancelling
the countdown. The shutdown is graceful, so a guest that refuses to go down is
left running rather than pulled out from under.

It waits for the guest to *answer*, not merely for the domain to report
'running': a booting Windows cannot do anything useful yet. It also refuses
early with a clear message if nobody is logged in, since without a desktop
there is no DWM to capture and interactive scheduled tasks will not run.

## Window decoration

The host window is **borderless**, and the captured image includes the guest
window's own title bar and buttons. That is the whole point: what you see and
click is Windows' own chrome, and close, minimise and maximise work because the
input goes straight through to Windows. Decorating it with the compositor as
well would mean two sets of chrome for one window, with the outer set operating
on a picture of the inner one.

The whole-screen view is the exception and is decorated normally. A screen has
no title bar inside it to grab, so undecorated its only handle was the strip the
hit test invents for windows that report no chrome of their own - 32 guest pixels
across the top, taken out of the picture and turned into a drag handle. That band
is the top of the guest's desktop, which is somewhere one actually clicks. Given
a real title bar it is dragged by that instead, and it gets no hit test at all,
so the whole picture stays the guest's.

## Audio

The guest captures its default playback endpoint with WASAPI loopback and sends
it as interleaved 32-bit float over a TCP connection of its own. The host opens a
playback stream on the first block that arrives, matching whatever rate and
channel count the guest is actually producing rather than asking for a format
and making somebody resample.

Capture is pinned to the endpoint the streamed process is actually playing to,
found by walking each render endpoint's audio sessions for that process id -
not to whatever Windows currently calls the default. This guest has three
playback devices and the default moves between them; when it moves to one the
game is not using, loopback captures silence while both ends still look
perfectly healthy. If the process has no session yet - the launcher, before the
game starts - it falls back to the default and re-pins when the game window
attaches.

Multichannel is folded to stereo **in the guest**. This endpoint is 7.1, so the
stream was 48 kHz by 8 channels of float - about 1.5 MB/s, four times what
stereo needs, down the same control channel as input. Audio the link cannot keep
up with backs up in socket buffers and arrives late. Centre and the surrounds
are attenuated into both sides rather than dropped, since dialogue lives in the
centre channel.

An earlier attempt bundled this with host-side queue trimming and logging, and
collapsed the *video* stream to about a frame every twenty seconds. Reintroduced
on its own, with the frame rate measured after: 60 and 51 fps, unaffected. So
the collapse was one of the host-side changes, and those stay out until they can
be tried the same way.

When the queue runs deep the host drains it to a low mark and then stops, rather
than shedding whatever sits above a threshold. A bare threshold becomes the
steady state: the queue settles just beneath it and packets are dropped
continuously to hold it there. Measured at 163-187 ms queued with 7-25% of
packets dropped, heard as sound cutting out at random. With a low mark - drain
below 40 ms once a backlog passes 125 ms - the same backlog costs one short
burst and then nothing: 49 ms queued and 92 drops, then 53 ms and 16, then 46 ms
and none.

Clearing is a hard silence of however much was queued - a
quarter of a second of nothing - and it fired on busy scenes, when the guest
falls behind and a backlog builds. Busy scenes are loud ones, so the symptom
was audio cutting out exactly when something loud happened. Dropping loses the
same audio in ten-millisecond pieces spread across the drain, which is close to
inaudible.

Audio has **its own TCP connection**, not a share of the control channel. A
queued input event or window update sitting in front of an audio packet delays
it however promptly TCP is configured to send - `TCP_NODELAY` says nothing about
a message already ahead of yours in the same stream. It also gave the audio
thread a socket to itself, where three threads had been writing one.

### One slow window stalled the whole daemon

The symptom was audio cutting out, worst in fullscreen. The cause was not in the
audio path at all.

Client sockets were accepted **blocking**, and the send loop writes until every
byte is gone. A window slow to drain its socket therefore stalled `vyprd` inside
that write — including the guest's audio connection, which has nothing to do
with that window. Fullscreen made it routine: a 4K present slows the render
loop, the window drains more slowly, its socket fills, and the daemon blocks.
The guest keeps sending, the kernel buffers megabytes, and when the client
catches up the whole backlog lands at once. Measured at twenty seconds of
silence followed by 1745 packets arriving inside a millisecond of each other,
nearly all then discarded as a burst too deep to queue.

Client sockets are now non-blocking and nothing is written to them directly:
messages are queued per client and flushed when the socket says it can take
them. Control messages are always queued, since dropping one desynchronises the
far end and they are small and rare; audio is dropped once a client is far
enough behind, since a sound that cannot be delivered now is worth less than the
ones behind it. The window process also receives on its own thread rather than
once per rendered frame, so audio no longer depends on how long a frame takes.

Measured after: bursts of 1–22 packets, gaps of 20–130 ms, and nothing dropped
at all in most ten-second windows, against 60–83% before.

**The instrumentation is why this was found.** The log line reported a single
queue depth, which was consistent with several different faults and identified
none — the same reading appeared whether the queue was overflowing or starving.
Reporting the queue's range, the number of drain cycles, the largest gap between
arrivals and the longest unbroken burst named the real fault on first read: a
1745-packet burst is a backlog being flushed, not a queue being mismanaged. It
stays in for that reason.

Both connections go to the same port, so there is only ever one firewall rule:
a new connection is parked until its first message says whether it is the
control channel or the audio one.

Audio does not go through the shared region. It is tiny beside video - a tenth
of a second of 48 kHz stereo is under 40 KB - and it wants ordering and
reliability far more than it wants the last microsecond of latency, which is
what TCP already gives.

It is the whole guest's output, not one app's. WASAPI can capture a single
process tree (`AUDCLNT_ACTIVATION_TYPE_PROCESS_LOOPBACK`, Windows 10 20H1+),
which would suit a per-window model better, and the interface is shaped so that
can be swapped in - but this VM runs one app at a time and endpoint loopback is
considerably simpler and harder to get wrong.

### Fullscreen

An app that goes fullscreen in the guest is now drawing a window that covers the
guest's whole desktop, so the host window goes fullscreen with it. Showing that
inside a small window is not what the user asked the app to do.

### Closing

Closing the host window closes the app in the guest. The compositor sends a
close request for the window itself - from its close button, the taskbar's
context menu, or a shortcut - and that arrives as
`SDL_EVENT_WINDOW_CLOSE_REQUESTED`, not `SDL_EVENT_QUIT`. Handling only the
latter meant Close on the taskbar shut the host window while the app carried on
running in the VM, invisible.

The daemon also remembers which windows were closed. The guest re-offers any
window nothing is streaming, so an app that ignores `WM_CLOSE` - which games
routinely do - would otherwise have its window reappear a few seconds after
being closed. The dismissal is forgotten once the guest window genuinely goes
away, so the same app can be started again.

### Minimise

The guest's minimise button is part of the captured image, so clicking it
minimises the window inside the VM - and a minimised window stops producing
frames, which would leave a live host window showing a picture that never
changes. Minimising only on the host is no better: the guest window stays up
and renders for nothing.

So the state travels both ways. The guest reporting itself minimised minimises
the host window to the taskbar; restoring it from the taskbar restores the
guest window. Each side applies a state only when it differs from the one it
already has, which is what stops the two bouncing it back and forth.

### Dragging and the window buttons

The guest's title bar is the only handle an undecorated window has, so a press
there is held rather than forwarded. If the pointer moves it becomes a drag and
the *host* window moves on the Linux desktop; if it is released without moving
it was a click on close, minimise or maximise, and is forwarded then - press and
release together - so those buttons still act on the guest window.

Forwarding the drag instead would move the window inside the guest, which is
invisible from here: the captured image *is* the window, so it looks like
nothing happened. The guest reports the height of its own title bar
(`chrome_top`), kept current as the window changes - sent only once it goes
stale the moment a window is maximised or switches to fullscreen, and a stale
value means drags leak through again.

A window with custom-drawn chrome reports no title bar at all - FiveM's launcher
is one - so a window without a Win32 title bar still gets a strip of its own,
sized like an ordinary one. Otherwise those windows can only ever be dragged
inside the VM.

A window that really has a Win32 title bar keeps it as a drag handle **even
while the pointer is captured**. Suppressing it there was the reason dragging
kept moving the window inside the VM: the guest reports the pointer captured
almost constantly during a game, and that skipped the handling entirely so
every drag was forwarded. A fullscreen game reports no title bar, so it is
unaffected either way.

The fallback strip - for windows that draw their own chrome and report no title
bar - applies only when the pointer is *not* captured, so a captured game never
has a dead band across the top of it.

## Controllers

A pad is forwarded as a **real XInput device**, not as injected events. Games
read controllers through XInput and XInput only reports genuine hardware, so
there is nothing to inject into - the guest has to present a device. ViGEmBus is
a signed bus driver that does that, and the agent speaks its ioctl interface
directly rather than linking its client library.

The host reads pads through SDL and maps them to the XInput layout before
sending, because SDL already normalises every controller it recognises to that
layout and recognises far more of them than the agent would. Two details that
would otherwise be bugs: SDL's stick Y grows downwards where XInput's grows up,
and negating -32768 overflows a signed 16-bit value, so it is clamped first.

**The first bug report against this was not in this code at all.** A DualShock 4
enumerated on USB, produced no input device, and was invisible to SDL; over
Bluetooth it reported `Connected: yes` with `Paired: no`. Both were one cause:
the machine was running a kernel whose module tree had been removed by an
upgrade it had not yet rebooted into, so `hid_playstation` and `hidp` could not
load. Nothing that was not already in memory could. `vypr doctor` checks for
that now, and `vypr-window --list-pads` says what SDL can see, because "my
controller does not work" otherwise has three indistinguishable answers.

## Raw-input games need a kernel HID device

`SendInput` cannot drive a game that reads raw input for its camera. It always
goes through the Win32 cursor pipeline, so the game receives
`MOUSE_MOVE_ABSOLUTE` packets or zeroes rather than the `lLastX`/`lLastY`
deltas a physical mouse produces - it reads the cursor as (0,0), computes a
huge negative delta every frame, and throws the camera into a corner. FiveM and
GTA V both do this; so do Sunshine and Apollo, for the same reason.

Until vypr injects through a virtual HID device of its own, **Parsec running in
the tray** supplies one: its `parsecvusba` driver injects at the kernel level
where the deltas are real. See `docs/vm-setup.md`.

**It is not needed for anything else, which took a while to establish.** An
agent started without Parsec once reported `Windows.Graphics.Capture is
unavailable`, and the conclusion drawn — that Parsec's virtual display was what
gave DWM something to compose — was wrong. Measured directly afterwards, with
Parsec killed: the passed-through adapter still drove 3840x2160, its virtual
display adapter reported no resolution at all, and the agent started clean.

The real requirement is a display on the passed-through GPU, which a monitor or
a dummy plug satisfies. `vypr doctor` reports it, because "capture is
unavailable" says nothing about what to do and the answer is not to install
more software.

This is worth knowing before debugging anything else about mouse behaviour in a
game - it is not fixable at the level vypr currently operates, and every
plausible-looking fix above it (relative mode, acceleration, re-centring) is
treating a symptom.

### Input costs frames if you let it

A high-polling-rate mouse reports about a thousand times a second. Sent one
message per report, that was a thousand messages a second across the link and a
thousand `SendInput` calls in the guest - each one also looking up the window
geometry through `DwmGetWindowAttribute`, a cross-process call - on the same CPU
that is capturing frames and running the game. The stream turned choppy the
moment the window took focus and went smooth again as soon as it lost it, which
is the tell.

Motion is now gathered up and sent once a frame. Nothing is lost: relative
deltas sum exactly, and for absolute positioning only the latest report was ever
going to matter. Buttons and wheel notches still go immediately, since a click
that waits for the next frame is a click that feels late. The captured rectangle
is cached for a tenth of a second rather than fetched per event.

Measured 22-29 fps published before, 60 after - the guest display's refresh rate
and therefore the ceiling.

## Mouse capture

A game that takes the pointer needs relative motion, not positions. It warps the
cursor to a fixed point every frame and reads the deltas, so an absolute
position from the host lands as a large bogus delta on top of its own warp -
the view spins and the pointer ends up in a corner.

The guest reports when an app takes the pointer (cursor hidden, or clipped
smaller than the virtual desktop) and the host switches to relative motion.
**Ctrl+Shift+M** toggles it by hand, the same chord Moonlight uses, because the
detection can miss a fullscreen app that leaves the cursor nominally visible -
and because a game that grabs the mouse must always be escapable from the host
side.

Three things had to be right before capture felt correct, and each was
independently capable of ruining it:

- **Debounce the lock.** A game toggles cursor visibility constantly, so
  reporting every flicker flipped the pointer mode several times a second.
  Only a state that survives three consecutive polls counts.
- **Scale the deltas.** They arrive in host window pixels; the guest surface is
  usually a different size. A 4K stream in a 1080p window moved the guest
  pointer at half speed. Rounding is floored at one pixel so a small real
  movement is never rounded away to nothing.
- **Suspend pointer acceleration.** Windows applies a ballistics curve to
  injected relative motion, so the guest does not receive the deltas the host
  sent - small movements compressed, fast ones amplified, on top of the game's
  own sensitivity. The agent suspends it while an app holds the pointer and
  restores it afterwards, including on exit; it is the user's setting, not
  ours to keep.

**Keep the guest cursor in the captured image.** Excluding it assumed the host
would draw a cursor of its own in the right place; the result was a window with
no pointer in it at all. Capturing it means what the user sees is where the
guest actually believes the pointer is, which is the only version that can be
trusted for clicking on things.

**Do not warp the cursor.** An earlier version re-centred it when it strayed,
to stop relative deltas jamming it against a screen edge. What the user sees is
the pointer snapping to the middle of the window while they are using it, which
is worse than the edge case it guarded against - and an app that genuinely needs
the cursor centred does that itself.

Capture is also a request the compositor may refuse, so the result is checked -
silently sending deltas after a refused grab looks exactly like broken input.

**The VM must not present a USB tablet.** An absolute pointing device makes a
raw-input game read absolute coordinates as motion and slam the view into a
corner, whoever is sending the input - see `docs/vm-setup.md`. This is worth
checking first when a game's mouse misbehaves, because nothing on the host side
can compensate for it.

Absolute positioning is also simply wrong in that situation: FiveM changed the
guest display mode to 2560x1440 while its window stayed 3840x2160, and SendInput
normalises absolute coordinates against the *virtual desktop*, so every point
past 2560x1440 mapped out of range.

## Status

| Component | State |
|---|---|
| `include/vypr_shm.h` — ring and slot-table layout | done (v3: rings in guest RAM) |
| `include/vypr_proto.h` — control protocol | spoken by both ends |
| `host/src/shm.c` — slot table, ring mapping, seqlock reader | done, verified |
| `host/src/guest_ram.c` — find guest RAM, verify rings | done, verified on Linux (scattered pages, 4 GiB hole) |
| `host/src/main.c` — present a slot as a native window | working |
| `host/src/present_vk.c` — zero-copy Vulkan path | working, default; ~5x faster at 4K than `gpu` |
| `host/src/present_gpu.c` — SDL_GPU upload path | fallback |
| Damage (changed-region) streaming | working, opt-in (`VYPR_DAMAGE=1`); verified live on the 4K guest |
| `host/src/present_render.c` — SDL_Renderer path | kept for comparison |
| `tools/vypr-testsrc.c` — reference producer | working |
| Guest agent — publish path (`guest/src/publisher.cpp`) | verified on Linux, 1080p60, 0 drops |
| Guest agent — WGC capture | **working** — Notepad at 60 fps |
| Guest agent — ring memory (`guest/src/awe.cpp`) | **working** on the real guest; replaces the IVSHMEM mapping |
| Banded readback, streaming copy (`guest/src/capture.cpp`) | **working** — 13-19% lower latency from 1080p up |
| Guest agent — input injection | **working** — typed into the host window, arrived in the guest |
| Popups and menus | **working** — real popup surfaces, GDI fallback for menus |
| VM memory shared with the host | memfd + shared, `qemu.conf` group = the user's |
| Third-party drivers needed in the guest | none for frames (IVSHMEM and Looking Glass are gone); Parsec stays optional, for raw-input games |
| `host/src/vyprd.c` — session daemon | working, verified end to end |
| `host/src/msg.c` — framing, shared by both host processes | done |
| Input path — pointer, keys, focus, resize, close | working, verified |
| Launcher / `.desktop` integration | done — `vypr add` registers any app |
| `vypr doctor` — health check | done |
| Linux installer | done — `install/install.sh` |
| Windows installer | done — one `vypr-setup.exe` |

## Running

`vypr run <app>` is the normal way in. Driving the daemon directly is still the
way to see what the guest is offering:

```bash
./build/vyprd --match Notepad --launch 'C:\\Windows\\System32\\notepad.exe'
```

`vyprd` formats its slot table, waits for the agent, and spawns one `vypr-window` per
matching guest window. `--all` streams every window, which is the way to see what
the guest is actually offering.

## Running without the VM

The whole system runs with the guest powered off, daemon included:

```bash
./build/vyprd --shm /dev/shm/vypr-test-host --guest-ram /dev/shm/vypr-test-ram \
              --bind 127.0.0.1 --port 47899 --match "test window" &
./build/vypr-testagent --connect 127.0.0.1 --port 47899 --guest-ram /dev/shm/vypr-test-ram
```

Guest RAM is a sparse 3 GiB file laid out like a q35 guest's, with the 4 GiB
hole (`tools/fake_ram.c`), and the test agent hands out pages scattered the way
a real guest does. `vypr-testagent` speaks the real control protocol and links
the real `publisher.cpp`, so this covers slot allocation, the ring handshake and
its page check, client spawning, the frame handoff and the input return path.
Verified: a 2560x1440 ring in 14,257 pieces checks out, the window appears and
streams at 60 fps, and pointer/focus events arrive at the agent. `--bad-ring`
spoils one page's stamp, to show the daemon refusing it and both ends
recovering.

For the presenter alone, without the daemon:

```bash
./build/vypr-testsrc --size 3840x2160 --fps 60 &
./build/vypr-window --shm /dev/shm/vypr-test-host --slot 0 --window-id 3735928559 --stats
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
- ~~**Popups and menus.**~~ Done. Each is its own `HWND` and arrives as its own
  stream, positioned against its owner: the popup message carries `owner_id`
  and a `dx, dy` offset, and the owner's process opens it, so a menu belongs to
  the window it came from rather than becoming a top-level of its own.
- **Z-order and focus.** The host WM owns stacking; the guest has its own idea.
  Unreconciled, the two fight.
- ~~**Reconnect.**~~ Partly done. The guest re-offers any window nothing is
  streaming every five seconds, so a host window that closes or a client that
  dies comes back by itself - verified by killing the client and watching it
  return in ~6s. Announcing only on first sight meant a window was offered
  exactly once, and a guest window that outlived its host end could never
  return without restarting the session. Re-attaching *the same* stream after a
  dropped agent connection is still not handled.
- **Cursor.** Whether to composite the guest cursor into the frame or hand the
  host a cursor shape. The protocol assumes the latter.
