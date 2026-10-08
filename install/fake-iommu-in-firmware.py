#!/usr/bin/env python3
"""Retired. Do not use.

This used to inject a hand-built DMAR ACPI table so that firmware would claim
an IOMMU was present without one existing. It cannot be made safe:

- DMAR describes an Intel VT-d IOMMU. On an AMD host the CPU and the firmware
  then contradict each other, and Windows' DMA-protection code trusts neither.
- A table that names a remapping unit at a register address nothing answers at
  is a lie to the kernel, not a hint; a driver that probes it reads garbage.
- The table was written to a temp file that is gone after a reboot, so the VM
  stopped starting the next day.

The working route is a real virtual IOMMU, which Windows then reports as
DMAProtection. QEMU 11.1 provides one, launched through qemu:commandline
because libvirt cannot set its dma-remap option:

  -device {"driver":"AMDVI-PCI","id":"vypriommu","bus":"pcie.0","addr":"0x4"}
  -device {"driver":"amd-iommu","pci-id":"vypriommu","intremap":"on","dma-remap":true}

with <ioapic driver='qemu'/> under <features>. That is what the VM runs now;
see the vm-virtual-iommu note in the maintainer's records.
"""
import sys

sys.exit("fake-iommu-in-firmware.py is retired: it injects a fake Intel IOMMU table "
         "that breaks DMA protection and does not survive a reboot. The VM has a "
         "real virtual IOMMU instead; see the comment at the top of this file.")
