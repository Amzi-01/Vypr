# VM changes sash needs

The VM already passes through the RTX 5050 and has a 128 MB IVSHMEM device named
`looking-glass`. sash needs its own, larger region, added alongside rather than
replacing it so Looking Glass keeps working.

All of this requires the guest powered off.

## 1. A second IVSHMEM device

`virsh edit RDPWindows`, then add inside `<devices>`:

```xml
<shmem name='sash'>
  <model type='ivshmem-plain'/>
  <size unit='M'>512</size>
</shmem>
```

512 MB holds three 4K buffers for one window plus several 1080p windows.
Uncompressed rings are large: a 4K slot is 96 MB, a 1080p slot 24 MB.

The region appears on the host as `/dev/shm/sash`, owned by the qemu user, so
the host tools need it group-readable — the same arrangement the existing
`looking-glass` region already uses.

## 2. IVSHMEM driver in the guest

Not currently installed. It ships with Looking Glass, not with virtio-win: the
signed `ivshmem` driver from the Looking Glass release matching the host tools.
Without it the guest sees an unknown PCI device and the BAR is unreachable.

## 3. Build environment in the guest

The agent is C++ against D3D11, WGC and WASAPI, which needs MSVC Build Tools and
the Windows SDK — neither is installed. The guest already has the `viofs`
driver, so a virtiofs share is the tidy way to move sources and binaries in and
out without copying through the network:

```xml
<filesystem type='mount' accessmode='passthrough'>
  <driver type='virtiofs'/>
  <source dir='/home/lucy/sash'/>
  <target dir='sash'/>
</filesystem>
```

virtiofs also needs `<memoryBacking><access mode='shared'/></memoryBacking>` on
the domain, and WinFsp installed in the guest.

## 4. A display must be attached

`<video model='none'/>` means the guest's only display comes from the passthrough
GPU's own outputs. DWM needs a live display to composite, and WGC captures from
DWM's surfaces — with no display attached there is nothing to capture. Either a
monitor input on the 5050 or a dummy plug has to be present.

## 5. Remove the USB tablet

```xml
<input type='tablet' bus='usb'/>
```

If the domain has one, take it out. It is an **absolute** pointing device, and a
game reading raw input in the guest treats its absolute coordinates as relative
motion and throws the view into a corner - classically the top left, which is
(0,0) in absolute space. Red Hat bug 852841 is this exact symptom: "Mouse jumps
to edges / corners when using an absolute input device (ie virtual machine usb
tablet)".

No amount of care on the host side fixes this, because the device is present in
the guest regardless of who is sending input. It is why the same VM misbehaves
under other streaming solutions too.

It can be removed without stopping the guest:

```bash
virsh detach-device RDPWindows tablet.xml --live --config
```

Leaving only `<input type='mouse' bus='ps2'/>`, a relative device. The cost is
that the SPICE console pointer now needs to be grabbed rather than tracking the
host pointer, which does not matter when the guest is driven through sash or
Looking Glass.

## 6. Parsec, running in the tray - for the mouse

Not for streaming. For its driver.

sash injects mouse motion with `SendInput`, which always goes through the Win32
cursor pipeline. A game that reads **raw input** for its camera - FiveM and
GTA V both do - wants the `lLastX`/`lLastY` deltas a physical mouse produces.
`SendInput` gives it `MOUSE_MOVE_ABSOLUTE` packets or zeroes, so the game reads
the cursor as (0,0), computes an enormous negative delta every frame, and whips
the camera into a corner.

**No userspace API can produce a real HID delta**, so this cannot be fixed in
sash as it currently injects input. Sunshine and Apollo have the same bug for
the same reason. Parsec's `parsecvusba` driver injects at the kernel HID level,
where the deltas are genuine, and with Parsec running the camera behaves.

```
https://builds.parsec.app/package/parsec-windows.exe     # the app
https://builds.parsec.app/vud/parsec-vud-0.3.10.0.exe    # the driver alone
```

Installed *and running in the tray* - installing alone is not enough. The
launcher starts it if it is not running.

### The real fix, not yet done

Replace `SendInput` with injection through a signed virtual relative HID
device, as ViGEmBus and HidHide do - both use free Microsoft attestation
signing. That removes the dependency on Parsec and the whole class of
raw-input problems with it. See Apollo issue #1479.

## 7. VB-Audio Virtual Cable, for the microphone

```
https://download.vb-audio.com/Download_CABLE/VBCABLE_Driver_Pack45.zip
```

Windows applications can only choose a microphone that exists as a device, and
this guest had no capture endpoint whatsoever. The cable supplies one: sash
writes the host's microphone into `CABLE Input` and applications record from
`CABLE Output`.

Installed with `VBCABLE_Setup_x64.exe -i`, elevated. The driver installation
raises a Windows Security prompt on the guest's own screen which has to be
accepted there.

Since it is the only recording device present, it is also the default, so
applications find it without being pointed at it.
