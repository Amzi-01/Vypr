#!/usr/bin/env bash
#
# Vypr - Linux host installer.
#
# Sets up everything downstream of the hardware. What it cannot do, it checks
# for and tells you about rather than failing halfway through: a second GPU
# bound to vfio-pci, IOMMU enabled in firmware and kernel, and a Windows VM
# that already exists. Those are prerequisites, not install steps.
set -uo pipefail

VERSION="0.1.0"
PREFIX="${PREFIX:-$HOME/.local}"
SHM_NAME="vypr"
SHM_SIZE_MB=512
PORT=47820
BRIDGE_IP="${BRIDGE_IP:-192.168.122.1}"
KEY="$HOME/.ssh/vypr-guest"
DOMAIN="${DOMAIN:-}"
export LIBVIRT_DEFAULT_URI="${LIBVIRT_DEFAULT_URI:-qemu:///system}"

bold=$(tput bold 2>/dev/null || true); dim=$(tput dim 2>/dev/null || true)
red=$(tput setaf 1 2>/dev/null || true); grn=$(tput setaf 2 2>/dev/null || true)
ylw=$(tput setaf 3 2>/dev/null || true); rst=$(tput sgr0 2>/dev/null || true)

ok()   { printf '  %s✓%s %s\n' "$grn" "$rst" "$*"; }
warn() { printf '  %s!%s %s\n' "$ylw" "$rst" "$*"; }
bad()  { printf '  %s✗%s %s\n' "$red" "$rst" "$*"; }
info() { printf '  %s·%s %s\n' "$dim" "$rst" "$*"; }
head2(){ printf '\n%s%s%s\n' "$bold" "$*" "$rst"; }

FAIL=0
MANUAL=()

# ---------------------------------------------------------------- prerequisites
head2 "Checking what Vypr needs"

if [ -d /sys/kernel/iommu_groups ] && [ "$(ls -1 /sys/kernel/iommu_groups 2>/dev/null | wc -l)" -gt 1 ]; then
    ok "IOMMU is on ($(ls -1 /sys/kernel/iommu_groups | wc -l) groups)"
else
    bad "IOMMU is off. Enable VT-d/AMD-Vi in firmware and add intel_iommu=on or amd_iommu=on to the kernel command line."
    FAIL=1
fi

vfio_count=$(lspci -nnk 2>/dev/null | grep -c 'Kernel driver in use: vfio-pci')
if [ "$vfio_count" -gt 0 ]; then
    ok "a GPU is bound to vfio-pci and available to pass through"
else
    bad "no device is bound to vfio-pci. Vypr streams from a VM with a passed-through GPU; without one there is nothing to capture."
    FAIL=1
fi

for cmd in virsh cmake ninja gcc g++ ssh ssh-keygen; do
    command -v "$cmd" >/dev/null || { bad "$cmd is not installed"; FAIL=1; }
done
command -v virsh >/dev/null && ok "libvirt tools present"

if pkg-config --exists sdl3 2>/dev/null; then
    ok "SDL3 $(pkg-config --modversion sdl3)"
else
    bad "SDL3 development files are missing (package: sdl3 / libsdl3-dev)"
    FAIL=1
fi

if [ "$FAIL" -ne 0 ]; then
    printf '\n%sCannot continue.%s The items marked ✗ are prerequisites rather than\nthings an installer can arrange. Fix those and run this again.\n\n' "$red" "$rst"
    exit 1
fi

# ------------------------------------------------------------------- the domain
head2 "Choosing the VM"

mapfile -t domains < <(virsh list --all --name 2>/dev/null | grep -v '^$')
if [ "${#domains[@]}" -eq 0 ]; then
    bad "no libvirt domains exist. Create your Windows VM first - Vypr configures an existing one, it does not build it."
    exit 1
fi

if [ -z "$DOMAIN" ]; then
    if [ "${#domains[@]}" -eq 1 ]; then
        DOMAIN="${domains[0]}"
        info "only one domain exists, using it"
    else
        printf '  Which VM runs Windows?\n'
        select d in "${domains[@]}"; do DOMAIN="$d"; break; done
    fi
fi
[ -n "$DOMAIN" ] || { bad "no VM chosen"; exit 1; }
ok "configuring '$DOMAIN'"

state=$(virsh domstate "$DOMAIN" 2>/dev/null || echo missing)
if [ "$state" = "running" ]; then
    warn "'$DOMAIN' is running. Device changes need it shut down; they will be written to the persistent config and take effect at the next boot."
fi

# --------------------------------------------------------------------- building
head2 "Building"

here=$(cd "$(dirname "$0")/.." && pwd)
if cmake -S "$here/host" -B "$here/host/build" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null 2>&1 \
   && cmake --build "$here/host/build" >/dev/null 2>&1; then
    ok "built vyprd and vypr-window"
else
    bad "build failed - run cmake by hand in host/ to see why"
    exit 1
fi

install -Dm755 "$here/host/build/vyprd"        "$PREFIX/bin/vyprd"
install -Dm755 "$here/host/build/vypr-window"  "$PREFIX/bin/vypr-window"
install -Dm755 "$here/launcher/vypr"           "$PREFIX/bin/vypr" 2>/dev/null || true
ok "installed to $PREFIX/bin"

case ":$PATH:" in
    *":$PREFIX/bin:"*) ;;
    *) warn "$PREFIX/bin is not on your PATH" ;;
esac

# ------------------------------------------------------------- domain devices
head2 "Configuring $DOMAIN"

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
virsh dumpxml --inactive "$DOMAIN" > "$tmp/domain.xml"
cp "$tmp/domain.xml" "$tmp/domain.bak.xml"

changes=$(python3 - "$tmp/domain.xml" "$SHM_NAME" "$SHM_SIZE_MB" <<'PY'
import sys, xml.etree.ElementTree as ET
path, name, size = sys.argv[1], sys.argv[2], sys.argv[3]

# Passthrough domains routinely carry a <qemu:commandline> block. Without
# registering the prefix, ElementTree would rewrite it as ns0: and libvirt
# would no longer recognise it, so register libvirt's namespaces first.
for prefix, uri in (
    ("qemu", "http://libvirt.org/schemas/domain/qemu/1.0"),
    ("lxc",  "http://libvirt.org/schemas/domain/lxc/1.0"),
):
    ET.register_namespace(prefix, uri)

tree = ET.parse(path); root = tree.getroot(); dev = root.find("devices")
changed = []

if not any(s.get("name") == name for s in dev.findall("shmem")):
    sh = ET.SubElement(dev, "shmem"); sh.set("name", name)
    ET.SubElement(sh, "model").set("type", "ivshmem-plain")
    sz = ET.SubElement(sh, "size"); sz.set("unit", "M"); sz.text = size
    changed.append("added the %s shared-memory device (%s MiB)" % (name, size))

# An absolute pointing device makes raw-input games throw the view into a
# corner, whoever is sending the input. It cannot be compensated for on the host.
for inp in list(dev.findall("input")):
    if inp.get("type") == "tablet":
        dev.remove(inp)
        changed.append("removed the USB tablet, which breaks the mouse in games")

cpu = root.find("cpu")
if cpu is not None and not any(f.get("name") == "topoext" for f in cpu.findall("feature")):
    f = ET.SubElement(cpu, "feature"); f.set("policy", "require"); f.set("name", "topoext")
    changed.append("required the topoext CPU feature, so the guest can see SMT")

ET.indent(tree, space="  "); tree.write(path, encoding="unicode")
print("\n".join(changed))
PY
)
while IFS= read -r line; do [ -n "$line" ] && ok "$line"; done <<< "$changes"

if [ -n "$changes" ]; then
    backup="$HOME/.local/share/vypr/domain-before-install.xml"
    install -Dm600 "$tmp/domain.bak.xml" "$backup" 2>/dev/null || true
    if virsh define "$tmp/domain.xml" >/dev/null 2>&1; then
        ok "domain updated (the original is saved at $backup)"
    else
        bad "could not redefine the domain; it is unchanged"
    fi
else
    info "the domain already has everything it needs"
fi

# ------------------------------------------------- microphone and speakers
head2 "Audio devices in the VM"

# Windows can only use a microphone that exists as a device. Rather than have
# anyone install a virtual audio cable, the VM is given an emulated sound card:
# Windows drives it with its own inbox driver and QEMU carries this machine's
# real microphone into it. Nothing third-party on either side, which is the
# only version of this that can be packaged.
dom_xml=$(virsh dumpxml --inactive "$DOMAIN" 2>/dev/null)
if grep -q "<sound" <<<"$dom_xml" && grep -q "type='pulseaudio'" <<<"$dom_xml"; then
    ok "already configured"
elif [ ! -S "/run/user/$(id -u)/pulse/native" ]; then
    warn "no PulseAudio socket at /run/user/$(id -u)/pulse/native, so the VM has"
    warn "nowhere to take audio from; skipping"
else
    tmp3=$(mktemp -d)
    virsh dumpxml --inactive "$DOMAIN" > "$tmp3/domain.xml"
    if python3 - "$tmp3/domain.xml" "/run/user/$(id -u)/pulse/native" <<'PY'
import sys, xml.etree.ElementTree as ET
for prefix, uri in (("qemu", "http://libvirt.org/schemas/domain/qemu/1.0"),
                    ("lxc",  "http://libvirt.org/schemas/domain/lxc/1.0")):
    ET.register_namespace(prefix, uri)

path, socket = sys.argv[1], sys.argv[2]
tree = ET.parse(path); root = tree.getroot(); dev = root.find("devices")

if dev.find("sound") is None:
    ET.SubElement(dev, "sound").set("model", "ich9")

# PulseAudio, deliberately, and not PipeWire's own backend.
#
# QEMU runs under a seccomp sandbox with resourcecontrol=deny, which blocks
# sched_setscheduler. PipeWire's client library sets up a realtime thread when
# it connects, is refused, and stalls - taking the guest with it. Measured:
# twenty-four seconds of boot, then every vCPU idle and a black screen. The
# PulseAudio socket reaches exactly the same devices and needs no such thing.
for a in dev.findall("audio"):
    dev.remove(a)
a = ET.SubElement(dev, "audio")
a.set("id", "1"); a.set("type", "pulseaudio"); a.set("serverName", socket)

ET.indent(tree, space="  "); tree.write(path, encoding="unicode")
PY
    then
        if virsh define "$tmp3/domain.xml" >/dev/null 2>&1; then
            ok "sound card added; the VM will use this machine's mic and speakers"
            info "they appear in Windows as Vypr Microphone and Vypr Speakers"
            info "once the guest installer has run"
        else
            bad "could not add the sound card; the domain is unchanged"
        fi
    fi
    rm -rf "$tmp3"
fi

# QEMU has to run as you to reach your audio socket, which lives in a directory
# only you can enter. Without this the devices exist and carry silence.
qemu_user=$(grep -sE "^[[:space:]]*user[[:space:]]*=" /etc/libvirt/qemu.conf 2>/dev/null |
            tail -1 | sed 's/.*"\(.*\)".*/\1/')
if [ "$qemu_user" = "$USER" ]; then
    ok "QEMU runs as you, so it can reach your microphone"
else
    warn "QEMU does not run as you, so it cannot reach your audio"
    MANUAL+=("sudo sed -i 's|^#\\?user = .*|user = \"$USER\"|' /etc/libvirt/qemu.conf && sudo systemctl restart virtqemud   # let the VM use your mic")
fi

# ------------------------------------------------------------- home folder
head2 "Sharing your home folder"

# Asked here rather than assumed, because this is the one setting that decides
# what the VM can read. Windows gets whatever the share points at, and a guest
# that is compromised has it too - which is a different proposition from
# lending it a GPU.
if [ "$(virsh dumpxml --inactive "$DOMAIN" 2>/dev/null | grep -c "target dir='vyprhome'")" != "0" ]; then
    ok "already shared"
else
    echo "  Windows can be given a folder from this machine, appearing as a drive."
    echo "  Anything it can reach, the VM can read and write."
    echo
    read -rp "  Share a folder? [y/N]: " share_reply
    if [ "${share_reply:-n}" = "y" ] || [ "${share_reply:-n}" = "Y" ]; then
        read -rp "  Which folder [$HOME]: " share_dir
        share_dir="${share_dir:-$HOME}"
        if [ ! -d "$share_dir" ]; then
            bad "$share_dir is not a directory; skipping"
        elif ! command -v virtiofsd >/dev/null 2>&1 && [ ! -x /usr/lib/virtiofsd ]; then
            bad "virtiofsd is not installed, so the share cannot be served"
            MANUAL+=("install virtiofsd, then re-run this to add the share")
        else
            tmp2=$(mktemp -d)
            virsh dumpxml --inactive "$DOMAIN" > "$tmp2/domain.xml"
            if python3 - "$tmp2/domain.xml" "$share_dir" <<'PY'
import sys, xml.etree.ElementTree as ET
for prefix, uri in (("qemu", "http://libvirt.org/schemas/domain/qemu/1.0"),
                    ("lxc",  "http://libvirt.org/schemas/domain/lxc/1.0")):
    ET.register_namespace(prefix, uri)

path, share = sys.argv[1], sys.argv[2]
tree = ET.parse(path); root = tree.getroot()

# virtiofs reads guest memory directly, so the VM's memory has to be shareable.
# Without this the domain will not start with a virtiofs device attached.
mb = root.find("memoryBacking")
if mb is None:
    mb = ET.SubElement(root, "memoryBacking")
if mb.find("access") is None:
    if mb.find("source") is None:
        ET.SubElement(mb, "source").set("type", "memfd")
    ET.SubElement(mb, "access").set("mode", "shared")

dev = root.find("devices")
fs = ET.SubElement(dev, "filesystem")
fs.set("type", "mount"); fs.set("accessmode", "passthrough")
ET.SubElement(fs, "driver").set("type", "virtiofs")
ET.SubElement(fs, "source").set("dir", share)
ET.SubElement(fs, "target").set("dir", "vyprhome")

ET.indent(tree, space="  "); tree.write(path, encoding="unicode")
PY
            then
                if virsh define "$tmp2/domain.xml" >/dev/null 2>&1; then
                    ok "sharing $share_dir with the VM"
                    info "it appears in Windows once the guest installer's"
                    info "'home folder' box has been ticked, and after a VM restart"
                else
                    bad "could not add the share; the domain is unchanged"
                fi
            fi
            rm -rf "$tmp2"
        fi
    else
        info "not shared - re-run this installer to change that"
    fi
fi

# ---------------------------------------------------------------- shared region
head2 "Shared memory"

if [ ! -e "/dev/shm/$SHM_NAME" ]; then
    install -m 0660 /dev/null "/dev/shm/$SHM_NAME" 2>/dev/null && \
        chgrp kvm "/dev/shm/$SHM_NAME" 2>/dev/null && \
        truncate -s "${SHM_SIZE_MB}M" "/dev/shm/$SHM_NAME" && \
        ok "created /dev/shm/$SHM_NAME" || warn "could not create /dev/shm/$SHM_NAME"
else
    ok "/dev/shm/$SHM_NAME exists"
fi
# Printed for the user to paste, so it has to work in whatever shell they run.
# A here-string is bash syntax and fish rejects it outright; a pipe is universal.
MANUAL+=("echo 'f /dev/shm/$SHM_NAME 0660 $USER kvm -' | sudo install -Dm644 /dev/stdin /etc/tmpfiles.d/10-vypr.conf   # so it survives a reboot")

# --------------------------------------------------------------------- firewall
head2 "Firewall"

if systemctl is-active --quiet ufw 2>/dev/null; then
    rules=$(sudo -n ufw status 2>/dev/null)
    if [ -z "$rules" ]; then
        warn "ufw is active, but reading its rules needs root - so this is unchecked"
        MANUAL+=("sudo ufw allow in on virbr0 to any port $PORT proto tcp   # unless it is already allowed")
    elif grep -q "$PORT" <<<"$rules"; then
        ok "ufw already allows $PORT"
    else
        warn "ufw is active and will block the guest"
        MANUAL+=("sudo ufw allow in on virbr0 to any port $PORT proto tcp   # let the guest reach vyprd")
    fi
else
    ok "no ufw to get in the way"
fi

# -------------------------------------------------------------------- guest key
head2 "Guest access"

if [ ! -f "$KEY" ]; then
    ssh-keygen -t ed25519 -f "$KEY" -N "" -C "vypr (host -> guest)" >/dev/null 2>&1
    ok "created a keypair at $KEY"
else
    ok "keypair already exists"
fi

# ---------------------------------------------------------------- session config
head2 "Session configuration"

CONF_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/vypr"
mkdir -p "$CONF_DIR/apps"

# The guest's address, if libvirt already knows it. It only knows once the VM
# has booted at least once with the guest agent or a DHCP lease, so this is
# offered as a default rather than relied on.
guest_ip=$(virsh domifaddr "$DOMAIN" 2>/dev/null | awk '/ipv4/ {split($4,a,"/"); print a[1]; exit}')
[ -z "$guest_ip" ] && guest_ip=$(virsh net-dhcp-leases default 2>/dev/null | awk -v d="$DOMAIN" '$0 ~ d {split($5,a,"/"); print a[1]; exit}')

if [ -n "$guest_ip" ]; then
    read -rp "  Guest address [$guest_ip]: " reply; guest_ip="${reply:-$guest_ip}"
else
    read -rp "  Guest address (find it with ipconfig in the VM): " guest_ip
fi

read -rp "  Windows username [$USER]: " guest_user; guest_user="${guest_user:-$USER}"

cat > "$CONF_DIR/config" <<EOF
# Written by the Vypr installer on $(date +%Y-%m-%d).
VM=$DOMAIN
GUEST=$guest_ip
GUEST_USER=$guest_user

# Must match the <shmem> size in the domain. The launcher grows the region to
# this before starting anything, because the tmpfiles rule recreates it empty.
SHM_SIZE_MB=$SHM_SIZE_MB

# Parsec is started alongside the session because its driver is what makes the
# mouse work in games that read raw input. Set to 0 if you do not play those.
USE_PARSEC=1

# Shut the VM down once the last streamed window has been gone this long.
# Relaunching anything during the countdown cancels it. Set to 0 to leave the
# VM running after you close things.
SHUTDOWN_VM_ON_EXIT=1
SHUTDOWN_GRACE=60
EOF
ok "wrote $CONF_DIR/config"

install -Dm644 "$here/launcher/vypr.png" \
    "${XDG_DATA_HOME:-$HOME/.local/share}/icons/hicolor/256x256/apps/vypr.png" 2>/dev/null \
    && ok "installed the icon" || true

# ------------------------------------------------------------------------- done
head2 "Next"

cat <<EOF
  Vypr $VERSION is installed on this side. The guest half is a single
  installer you run inside Windows:

    ${bold}vypr-setup.exe${rst}   (from the release page, or install/windows/)

  Copy it into the VM and run it. It installs the agent, the IVSHMEM driver,
  and the drivers that make the mouse and audio behave, then registers the
  agent to start with the desktop. It will ask you to accept a driver prompt
  or two, which Windows requires a person to click.

  It needs this public key, so the host can drive it:

$(sed 's/^/    /' "$KEY.pub")

EOF

if [ "${#MANUAL[@]}" -gt 0 ]; then
    printf '  %sTwo of these need root, so they are yours to run:%s\n\n' "$bold" "$rst"
    for m in "${MANUAL[@]}"; do printf '    %s\n' "$m"; done
    printf '\n'
fi
