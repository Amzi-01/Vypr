<p align="center">
  <img src="launcher/vypr.png" width="96" height="96" alt="">
</p>

<h1 align="center">Vypr</h1>

<p align="center">
  Per-application native windows from a Windows VM, over shared memory.
</p>

> [!NOTE]
> **Vypr is in the public domain**, under [the Unlicense](LICENSE). No rights
> reserved — copy it, change it, redistribute it, sell it, with or without
> credit. It comes with no warranty of any kind, express or implied.

An app running in the guest appears as an ordinary window on the Linux desktop —
its own entry in the taskbar, its own place in the stacking order — with no RDP
and no video codec in the path.

## What it is

A Windows application running inside a virtual machine, appearing on your Linux
desktop as an ordinary window. Its own taskbar entry. Its own place in the
stacking order. You alt-tab to it like anything else, and it is a Windows app
the whole time.

No remote desktop, no video encoder, no streaming client to connect to. The
picture goes from the guest's memory to your screen uncompressed.

## Where it came from

This started as a passion project with one goal: **play FiveM on Linux.**

FiveM does not run under Wine, and the usual answers were all unsatisfying. A
whole-desktop stream means alt-tabbing inside someone else's desktop. RDP hands
over individual windows properly, but its video path was built for documents and
falls apart on anything that moves. Running the game on a second machine means
owning a second machine.

What was actually wanted was narrower than any of those: **one window, from the
VM, behaving like a native one.** Vypr is that, and it turned out to work for
anything else in the VM too.

## What it looks like

<details>
<summary><b>FiveM</b> — the application this was built for, running in the VM with its own taskbar entry</summary>

<br>

![FiveM running through Vypr](docs/images/fivem.png)

</details>

<details>
<summary><b>Call of Duty: Modern Warfare II</b> — launched from Steam inside the guest</summary>

<br>

![Call of Duty running through Vypr](docs/images/cod.png)

</details>

## How it compares

[WinApps](https://github.com/winapps-org/winapps) and
[WinBoat](https://winboat.app/) solve the same shape of problem — Windows
applications as native Linux windows — and both do it over RDP RemoteApp. That
choice is what most of this table comes down to.

| | WinApps | WinBoat | Vypr |
|---|:---:|:---:|:---:|
| Windows apps as native Linux windows | ✅ | ✅ | ✅ |
| How the picture travels | RDP codec | RDP codec | **shared memory, uncompressed** |
| Frame latency | encode + decode | encode + decode | **no codec in the path** |
| Audio | ✅ | ✅ | ✅ *pinned to the app's own device* |
| Microphone into the guest | ✅ | ✅ | ✅ *no extra software* |
| Clipboard sharing | ✅ | ✅ | ✅ *text only* |
| A folder from Linux, inside Windows | ✅ | ✅ | ✅ *virtiofs, opt-in* |
| Finds your apps for you | ✅ | ✅ | ✅ *desktop shortcuts, `vypr detect`* |
| Fullscreen, minimise and window buttons | ✅ | ✅ | ✅ |
| Seeing the guest's whole screen when something is wrong | ✅ | ✅ | ✅ *`vypr --debug desktop`* |
| Setup effort | guided | guided | two installers, plus passthrough |
| GPU acceleration inside the guest | ❌ | ❌ *(planned)* | ✅ **passed-through GPU** |
| Demanding games | ❌ | ❌ | ✅ **4K at 60 fps, measured** |
| Games that read raw mouse input | ❌ | ❌ | ✅ **kernel-level HID injection** |
| Controllers | ❌ | ❌ | ✅ **as real XInput devices** |
| Opening a Linux file in a Windows app — `.psd`, `.exe`, anything | ❌ | ❌ | ✅ **double-click it, and saves come back** |
| Dragging a file onto a running Windows app | ❌ | ❌ | ✅ **copied in and dropped where you point** |

**Where Vypr wins, it wins on what it was built for.** RDP's video path was
designed for documents: fine for a text editor, and it falls apart on anything
that moves. Neither project has GPU passthrough — WinBoat lists it as planned —
so a demanding game is out of reach for both. Vypr puts a real GPU in the VM and
moves frames through shared memory with no encoder anywhere in the path.

**Where it loses, it loses on reach and convenience.** It needs a second GPU,
which most machines do not have and cannot be worked around. There is no
graphical manager. Its clipboard is text only, and it finds applications from the
guest's desktop rather than everything installed.

So: if you want Office or Photoshop occasionally on an ordinary laptop, WinApps
or WinBoat will cost you far less effort and serve you better. Vypr is for the
case they cannot reach — when the thing you are running has to be fast, and you
have a second GPU to give it.

*Compared against WinApps and WinBoat documentation as of August 2026.*

## What you need

Vypr does not create your VM — it makes an existing one useful. Before
installing, you need:

- a **second GPU** passed through to a Windows VM under libvirt, with IOMMU on
- a Linux desktop with SDL3, CMake, Ninja and a C++ compiler
- Windows 10 1903 or newer in the guest

Both installers check for these and say plainly what is missing rather than
failing halfway through.

## Installing

Two installers, one per side.

**On Linux**, from a clone of this repository:

```bash
./install/install.sh
```

It builds and installs the binaries, configures your VM, generates a key for the
guest, and prints the two commands that need root rather than asking for your
password. It only changes the VM where something is actually missing, and saves
the original settings before it does.

**In Windows**, run `vypr-setup.exe` from the
[latest release](https://github.com/Amzi-01/Vypr/releases/latest) inside the VM.
One file, nothing to unpack: it carries the agent, installs the two drivers Vypr
needs, and authorises the key the Linux installer printed. Windows will ask you
to accept the drivers, which only a person can click.

It can also turn on automatic login, because Vypr needs a logged-in desktop to
capture — a VM sitting at the lock screen has nothing to show. That asks for your
Windows password and uses it once, to write Windows' own auto-login setting.
The installer says so on screen and links to its own source.

## Using it

The quickest start is to let it look for you:

```bash
vypr detect
```

That reads the guest's desktop — shortcuts and Steam entries alike — lists what
it found, marks anything already registered, and adds the ones you pick. Or
register them one at a time:

```bash
vypr add fivem 'C:\Users\You\Desktop\FiveM.lnk' --name FiveM
```

That pulls the app's real icon out of the guest executable and puts it in your
application menu and on your desktop, so you launch it like anything else.
Steam games are registered by their URL instead of a path:

```bash
vypr add cod 'steam://rungameid/3595230' --name 'Call of Duty Modern Warfare II'
```

To open your own files in the guest's applications, register Vypr as their
handler once:

```bash
vypr associate
```

After that, double-clicking a `.psd`, `.exe`, `.msi` or similar opens it in the
VM: the file is copied in, opened with whatever Windows associates with it, and
each time the application saves, the new version is written back over the file
you double-clicked. `vypr associate --remove` undoes it, and `vypr open <file>`
does the same thing for one file without changing any defaults.

| | |
|---|---|
| `vypr run <app>` | start it, bringing up the VM and session if needed |
| `vypr add <app> <path>` | register a Windows application |
| `vypr detect` | find apps on the guest desktop and pick which to add |
| `vypr open <file>` | open a Linux file in the Windows app that handles it |
| `vypr associate` | make double-clicking `.psd`, `.exe` and friends do that |
| `vypr --debug desktop` | stream the guest's whole screen, for when something is wrong |
| `vypr remove <app>` | undo that — task, profile, menu entry and icons |
| `vypr apps` | list what is registered |
| `vypr status` | what is currently running |
| `vypr doctor` | check the setup for the things that fail quietly |

Close the last window and the VM shuts itself down a minute later. Launching
something during that minute cancels it.

## What works

- Any Windows application, as its own native window
- Games, including ones that capture the mouse and read raw input
- Audio, pinned to whatever the app is actually playing to
- Menus, popups and dialogs, positioned against the window they belong to
- Controllers, presented to Windows as real XInput devices
- Your microphone and speakers, as ordinary Windows devices
- Clipboard text, shared both ways
- Double-clicking a Linux file to open it in the Windows application that
  handles it — a `.psd` in Photoshop, a `.exe` or an installer, a document in
  whatever opens it over there. **Saving writes back to the file you opened**,
  not to a copy stranded in the VM
- Dragging a file from the Linux desktop straight onto a running Windows
  application, for when you want it opened by that application rather than by
  whatever Windows would choose
- A folder from this machine, mounted in Windows as a drive
- Finding what is on the guest's desktop and offering to add it
- Streaming the guest's whole screen, for debugging
- Minimise, maximise, close and dragging, all acting on the guest window
- Fullscreen, mirrored from the guest

## What does not

- **Exclusive fullscreen.** There is nothing to capture — the game bypasses the
  compositor entirely. Set the game to borderless or windowed. Vypr says so when
  it happens rather than showing a black window.
- **Games that read raw mouse input need Parsec running** in the guest, for its
  driver only — nothing in userspace can produce the mouse deltas they expect.
  The installer sets it up for you, and `USE_PARSEC=0` turns it off if you do
  not play those. Everything else works without it.
- **The passed-through GPU needs a display** — a monitor or a dummy plug. The
  compositor has nothing to draw on otherwise, and there is nothing to capture.
  `vypr doctor` checks this.
- **Two apps at once** works, but a second app restarts the session briefly.
- **Clipboard is text only.** Images and file lists need a format negotiation
  the protocol does not have.
- **Dragging goes one way, and files only.** A file dragged from Linux lands in
  the Windows application; dragging back out is not implemented, and neither is
  dragging a folder.
- **An opened file is copied, not shared.** The guest cannot see this
  filesystem, so `vypr open` copies the file in and copies each saved version
  back out, usually within a few seconds. That is invisible in ordinary use and
  wrong in two cases: editing the same file on both sides at once, and an
  application that expects the file to change underneath it while it is open.

## Digging deeper

**[docs/technical.md](docs/technical.md)** is the long version: how frames get
out of the VM, what the latency actually is and where it goes, why the audio
path looks the way it does, and a list of the things that went wrong and what
they turned out to be. Most of the design was forced by something breaking, and
that document says which.

**[docs/vm-setup.md](docs/vm-setup.md)** covers the VM itself.

## Building it yourself

```bash
cd host
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Needs SDL3. The guest agent is built with MSVC inside the VM; see
`tools/dev/README.md` for the loop used to develop it.

---

*Documentation and commit descriptions in this repository were written with
Claude. (It just helps speed things up.)*
