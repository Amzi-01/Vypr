#!/bin/bash
# Set up the Windows security features that some software checks for:
# Secure Boot (firmware and keys), VBS/HVCI, TPM 2.0, and a virtual IOMMU so
# that Windows reports Kernel DMA Protection.
#
# This replaces five earlier scripts (apply-vanguard-complete.sh,
# apply-vanguard-final.sh, run-vanguard-setup.sh, clean-and-apply.sh and
# clean-vm-xml.py) that between them could leave the VM unbootable: they
# deleted the NVRAM and its boot entries, pointed it at a template that does
# not exist, stripped the whole qemu:commandline (which now carries the real
# IOMMU and the ACPI ids), and injected a fake Intel IOMMU table that did not
# survive a reboot. None of that happens here. Every step keeps what is already
# in place, backs the domain up before changing it, and refuses rather than
# guesses when something is not as expected.
set -euo pipefail

VM_NAME="${VM_NAME:-Microslop Win BOOBS 11}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BACKUPS="$HOME/vm-backups"
export LIBVIRT_DEFAULT_URI="${LIBVIRT_DEFAULT_URI:-qemu:///system}"

die() { echo "apply-vanguard: $*" >&2; exit 1; }

virsh dominfo "$VM_NAME" >/dev/null 2>&1 || die "no VM called '$VM_NAME' (set VM_NAME=...)"

# Device changes only take effect on a cold boot, and the NVRAM must not be
# touched while QEMU holds it. So: shut down, cleanly, and wait.
if [ "$(virsh domstate "$VM_NAME")" != "shut off" ]; then
    if pgrep -x vypr-window >/dev/null; then
        die "a Vypr session is on screen; close it before changing the VM"
    fi
    echo "==> Shutting the VM down..."
    virsh shutdown "$VM_NAME" >/dev/null
    for _ in $(seq 1 90); do
        [ "$(virsh domstate "$VM_NAME")" = "shut off" ] && break
        sleep 2
    done
    [ "$(virsh domstate "$VM_NAME")" = "shut off" ] || die "the VM did not shut down"
fi

mkdir -p "$BACKUPS"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

# The *inactive* config: it is what the next boot uses, and the live one of a
# running domain carries runtime-only elements that must not be redefined.
virsh dumpxml --inactive "$VM_NAME" > "$tmp/current.xml"
stamp=$(date +%Y%m%d-%H%M)
install -Dm600 "$tmp/current.xml" "$BACKUPS/${VM_NAME// /}.backup-$stamp-pre-vanguard.xml"
echo "==> Backed up the domain to $BACKUPS/${VM_NAME// /}.backup-$stamp-pre-vanguard.xml"

echo "==> Secure Boot firmware, VBS/HVCI, TPM..."
"$HERE/anti-detect.py" "$tmp/current.xml" "$tmp/step1.xml" --vanguard

echo "==> Virtual IOMMU (what Windows reports as DMA protection)..."
python3 - "$tmp/step1.xml" "$tmp/final.xml" <<'PYEOF'
import sys, xml.etree.ElementTree as ET
src, dst = sys.argv[1], sys.argv[2]
for prefix, uri in (("qemu", "http://libvirt.org/schemas/domain/qemu/1.0"),
                    ("libosinfo", "http://libosinfo.org/xmlns/libvirt/domain/1.0")):
    ET.register_namespace(prefix, uri)
tree = ET.parse(src); root = tree.getroot()
Q = "{http://libvirt.org/schemas/domain/qemu/1.0}"
feat = root.find("features")
if feat.find("ioapic") is None:
    ET.SubElement(feat, "ioapic").set("driver", "qemu")
cl = root.find(Q + "commandline")
if cl is None:
    cl = ET.SubElement(root, Q + "commandline")
have = [a.get("value") or "" for a in cl.findall(Q + "arg")]
# Added once, and only if no IOMMU of any kind is already there: a second
# one would make QEMU refuse to start.
if (not any("amd-iommu" in v or "intel-iommu" in v or "virtio-iommu" in v for v in have)
        and root.find("devices/iommu") is None):
    for arg in ("-device", '{"driver":"AMDVI-PCI","id":"vypriommu","bus":"pcie.0","addr":"0x4"}',
                "-device", '{"driver":"amd-iommu","pci-id":"vypriommu","intremap":"on","dma-remap":true}'):
        ET.SubElement(cl, Q + "arg").set("value", arg)
    print("  added the virtual AMD IOMMU (pcie.0 slot 4)")
else:
    print("  an IOMMU is already configured; left as is")
ET.indent(tree, space="  "); tree.write(dst, encoding="unicode")
PYEOF

# Only accept what libvirt accepts; the backup is untouched either way.
virsh define "$tmp/final.xml" >/dev/null || die "libvirt rejected the new configuration; the VM is unchanged"
echo "==> Domain updated."

echo "==> Secure Boot keys..."
"$HERE/fix-secureboot.sh" "$VM_NAME"

echo
echo "Done. Start the VM and check in PowerShell:"
echo "  Confirm-SecureBootUEFI"
echo "  (Get-ComputerInfo -Property DeviceGuardAvailableSecurityProperties).DeviceGuardAvailableSecurityProperties"
echo "The second should list DMAProtection. HVCI (Memory integrity) is turned on"
echo "in Windows Security > Device security > Core isolation, if it is not already."
