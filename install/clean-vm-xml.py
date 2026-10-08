#!/usr/bin/env python3
"""Retired. Do not use.

This removed the whole qemu:commandline from the domain. That section now
carries the virtual IOMMU and the ACPI OEM ids, so stripping it would silently
undo both. Nothing needs it removed; apply-vanguard.sh edits it in place.
"""
import sys

sys.exit("clean-vm-xml.py is retired: it would strip the virtual IOMMU and the ACPI ids "
         "out of the domain. Use apply-vanguard.sh.")
