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
