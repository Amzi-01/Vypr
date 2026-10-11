# Immersive desktop mode — design

**Goal.** One Vypr window per host monitor, each fullscreen on that monitor,
each showing a *different* guest monitor at that monitor's native resolution.
The VM is given one virtual display per host monitor (via the Virtual Display
Driver), so Windows composites natively across them and the result looks like
Windows running directly on the hardware.

## Pieces

1. **Guest displays — Virtual Display Driver (VDD).**
   Signed MIT IddCx driver (`MttVDD`, root-enumerated via nefcon). The Windows
   installer bundles it (`install/windows/vdd/`) and installs it silently by
   trusting the SignPath leaf cert. Monitor count lives in
   `C:\VirtualDisplayDriver\vdd_settings.xml`. Proven 2026-10-11: count=2 gave
   two extra displays, adapter advertises 3840x2160.

2. **Guest agent — one stream per monitor.**
   Today the whole-screen stream is a single sentinel (`VYPR_DESKTOP_WINDOW_ID`)
   that always captures the primary monitor. Immersive mode needs each monitor
   offered as its own stream:
   - `VYPR_DESKTOP_MONITOR_0 + i` (i = monitor index, sorted by x then y).
   - `vypr_is_desktop_id()` / `vypr_desktop_monitor_index()` in the proto.
   - Monitor enumeration (sorted, stable) shared by capture/input/windows_list
     via `geometry.hpp`.
   - `windows_list` emits one entry per monitor ("Vypr Monitor N"); the legacy
     primary entry stays for `--debug desktop`.
   - `capture` uses `CreateForMonitor(the i-th monitor)`.
   - `input` maps pointer offsets against the i-th monitor's rect.

3. **Host — fullscreen per output.**
   `vypr-window` gains `--fullscreen-on <output>`: create the window, move it to
   that output, go SDL fullscreen. One process per output, each attached to the
   matching guest-monitor stream.

4. **Launcher — `vypr immersive`.**
   Enumerate host outputs, set VDD monitor count to match, set each guest
   monitor to the host output's resolution and lay them out edge to edge, then
   launch one fullscreen `vypr-window` per output mapped to the guest monitor in
   the same sorted order. A native helper (`vypr-vdd`, built in the VM beside the
   agent) sets count/resolution/layout, avoiding fragile PowerShell.

## Ordering / mapping
Both ends sort monitors by (x, y). Host output k ↔ guest monitor k. The launcher
sizes guest monitor k to host output k's resolution, so the pairing is 1:1 and
geometry matches.
