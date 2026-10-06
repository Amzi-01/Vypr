# VM changes vypr needs

`install/install.sh` makes the domain changes below and prints the two lines of
`qemu.conf` it needs root for. All of this requires the guest powered off.

## 1. Guest memory shared with the host

Frames travel through the guest's own RAM: the agent locks each window's ring
as physical pages and the host reads those pages out of QEMU's memory file. That
file only exists when the domain's memory is a shared memfd:

```xml
<memoryBacking>
  <source type='memfd'/>
  <access mode='shared'/>
</memoryBacking>
```

virtiofs needs exactly the same, so a VM with a home share already has it.

Older versions of Vypr added an IVSHMEM device instead (`<shmem name='vypr'>`,
512 MB). It needed Looking Glass's kernel driver in the guest and is not used any
more; the installer removes it, which also gives the 512 MB of host RAM back.

## 2. QEMU under your group

The host finds guest RAM at `/proc/<qemu pid>/fd/N`, and Linux only lets one
process open another's files when both user *and* group match. libvirt runs QEMU
under its own group by default, so `/etc/libvirt/qemu.conf` needs, next to the
`user =` line the microphone already needed:

```
user = "lucy"
group = "lucy"
```

then `sudo systemctl restart virtqemud`, and a restart of the VM so it picks the
new group up. `vypr doctor` checks both halves.

No driver is needed in the guest for frames. The agent needs one Windows user
right, "Lock pages in memory", which `vypr-setup` grants.

## 3. Build environment in the guest

The agent is C++ against D3D11, WGC and WASAPI, which needs MSVC Build Tools and
the Windows SDK — neither is installed. The guest already has the `viofs`
driver, so a virtiofs share is the tidy way to move sources and binaries in and
out without copying through the network:

```xml
<filesystem type='mount' accessmode='passthrough'>
  <driver type='virtiofs'/>
  <source dir='/home/lucy/vypr'/>
  <target dir='vypr'/>
</filesystem>
```

virtiofs also needs the shared memory backing from step 1, and WinFsp installed
in the guest.

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
host pointer, which does not matter when the guest is driven through vypr.

## 6. Parsec, running in the tray - for the mouse

Not for streaming. For its driver.

vypr injects mouse motion with `SendInput`, which always goes through the Win32
cursor pipeline. A game that reads **raw input** for its camera - FiveM and
GTA V both do - wants the `lLastX`/`lLastY` deltas a physical mouse produces.
`SendInput` gives it `MOUSE_MOVE_ABSOLUTE` packets or zeroes, so the game reads
the cursor as (0,0), computes an enormous negative delta every frame, and whips
the camera into a corner.

**No userspace API can produce a real HID delta**, so this cannot be fixed in
vypr as it currently injects input. Sunshine and Apollo have the same bug for
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
