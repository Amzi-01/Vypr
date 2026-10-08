#!/usr/bin/env python3
"""Retired. Do not use.

This added x-no-mmap=on to every passed-through PCI device, to work around an
intel-iommu device that no longer exists in the configuration. x-no-mmap makes
every access to the device's registers and memory trap into QEMU instead of
being mapped directly - on a passed-through GPU that is every frame, and the
card becomes unusably slow. It was never the right fix: the real virtual IOMMU
the VM has now (an AMD one, see fake-iommu-in-firmware.py) coexists with VFIO
passthrough as it is, and the 3060 runs at full speed behind it.
"""
import sys

sys.exit("iommu-vfio-fix.py is retired: x-no-mmap=on makes a passed-through GPU "
         "unusably slow, and the IOMMU problem it worked around no longer exists.")
