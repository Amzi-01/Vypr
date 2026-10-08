#!/bin/bash
# Turn Secure Boot on inside the VM, by enrolling Microsoft's keys into the
# VM's NVRAM - in place.
#
# The edk2 package ships no enrolled-keys NVRAM template, and libvirt refuses
# <feature name='enrolled-keys' enabled='yes'/> without one, so the keys are
# written into the VM's existing NVRAM file instead. Doing it in place is what
# keeps the Windows boot entry: an earlier version of this script deleted the
# NVRAM to force regeneration, which throws the boot entries away with it and
# leaves the VM at the firmware shell. Nothing here deletes anything; the
# original is backed up first and can be copied straight back.
#
# Needs the virt-firmware tool (virt-fw-vars). It is installed into a private
# venv under ~/.cache if it is not on the PATH, so no system package changes.
set -euo pipefail

VM_NAME="${1:-Microslop Win BOOBS 11}"
NVRAM="/var/lib/libvirt/qemu/nvram/${VM_NAME}_VARS.fd"
BACKUPS="$HOME/vm-backups"
export LIBVIRT_DEFAULT_URI="${LIBVIRT_DEFAULT_URI:-qemu:///system}"

die() { echo "fix-secureboot: $*" >&2; exit 1; }

virsh dominfo "$VM_NAME" >/dev/null 2>&1 || die "no VM called '$VM_NAME'"
[ -f "$NVRAM" ] || die "no NVRAM file at $NVRAM - has the VM ever been started?"
[ -r "$NVRAM" ] && [ -w "$NVRAM" ] || die "$NVRAM is not writable by $USER; run as the user QEMU runs as"

# The firmware image has to be the Secure Boot one, or the keys do nothing.
if ! virsh dumpxml --inactive "$VM_NAME" | grep -q "OVMF_CODE.secboot"; then
    die "the VM does not use the Secure Boot firmware image; run anti-detect.py --vanguard first"
fi

# Never with the VM running: QEMU owns the file then and would overwrite it.
state=$(virsh domstate "$VM_NAME")
if [ "$state" != "shut off" ]; then
    echo "==> Shutting the VM down (it is $state)..."
    virsh shutdown "$VM_NAME" >/dev/null
    for _ in $(seq 1 90); do
        [ "$(virsh domstate "$VM_NAME")" = "shut off" ] && break
        sleep 2
    done
    [ "$(virsh domstate "$VM_NAME")" = "shut off" ] || die "the VM did not shut down; try again when it has"
fi

# The tool. On the PATH, or in a venv of its own.
if command -v virt-fw-vars >/dev/null 2>&1; then
    VFW=virt-fw-vars
else
    VENV="$HOME/.cache/vypr-virt-firmware"
    if [ ! -x "$VENV/bin/virt-fw-vars" ]; then
        echo "==> Installing virt-firmware into $VENV..."
        python3 -m venv "$VENV"
        "$VENV/bin/pip" install -q virt-firmware
    fi
    VFW="$VENV/bin/virt-fw-vars"
fi

# Already done? Then there is nothing to do, and no reason to touch the file.
if "$VFW" -i "$NVRAM" -p 2>/dev/null | grep -qE "^SecureBootEnable\s*:\s*bool:\s*ON"; then
    echo "Secure Boot keys are already enrolled in $NVRAM. Nothing changed."
    exit 0
fi

mkdir -p "$BACKUPS"
backup="$BACKUPS/$(basename "$NVRAM" .fd).backup-$(date +%Y%m%d-%H%M)-pre-secureboot.fd"
cp -p "$NVRAM" "$backup"
echo "==> Backed up the NVRAM to $backup"

# Enrol into a copy, check the copy, and only then replace the original.
tmp=$(mktemp "${NVRAM}.XXXXXX")
trap 'rm -f "$tmp"' EXIT
"$VFW" -i "$NVRAM" --enroll-microsoft --secure-boot -o "$tmp" >/dev/null

"$VFW" -i "$tmp" -p | grep -qE "^SecureBootEnable\s*:\s*bool:\s*ON" || die "enrolment did not take; the original is untouched"
before=$("$VFW" -i "$NVRAM" -p | grep -cE "^Boot[0-9A-F]{4}" || true)
after=$("$VFW" -i "$tmp" -p | grep -cE "^Boot[0-9A-F]{4}" || true)
[ "$after" -ge "$before" ] || die "boot entries went from $before to $after; the original is untouched"

chmod --reference="$NVRAM" "$tmp"
mv "$tmp" "$NVRAM"
trap - EXIT

echo "==> Done. Secure Boot is enrolled, with all $after boot entries kept."
echo "    Start the VM and check with:  Confirm-SecureBootUEFI  (in PowerShell)"
echo "    To undo:  cp '$backup' '$NVRAM'  (with the VM shut off)"
