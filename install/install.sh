#!/usr/bin/env bash
#
# Vypr - Linux host installer.
#
# Sets up everything downstream of the hardware. What it cannot do, it checks
# for and tells you about rather than failing halfway through: a second GPU
# bound to vfio-pci, IOMMU enabled in firmware and kernel, and a Windows VM
# that already exists. Those are prerequisites, not install steps.
set -uo pipefail

VERSION="0.5.0"
PREFIX="${PREFIX:-$HOME/.local}"
SHM_NAME="vypr"   # the shared-memory device older versions added
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

# ------------------------------------------------------- GPUs and their groups
#
# Handing a card to a VM hands over its whole IOMMU group: vfio takes the group
# or nothing at all. A group is fit for that when everything in it is either
# another function of the same card - the display half and its HDMI audio half
# - or a PCI bridge, which is not an endpoint and stays behind. Anything else
# in there would have to go to the guest as well, and on consumer boards that
# is usually the SATA controller or the wired NIC.
#
# These are used twice: once to judge whether this machine can pass a card
# through at all, and once to say which card that should be.

gpu_devices() {
    # 0300 VGA and 0302 3D both count. A card with no display outputs
    # enumerates as the latter, and is exactly the kind people buy to pass
    # through. Two passes, not two -d flags: lspci keeps only the last one and
    # says so on stderr, which is silenced here and would have gone unnoticed.
    { lspci -Dn -d ::0300 2>/dev/null; lspci -Dn -d ::0302 2>/dev/null; } |
        awk '{print $1}' | sort -u
}

gpu_name() { lspci -Dmm -s "$1" 2>/dev/null | awk -F'"' '{printf "%s %s", $4, $6}'; }

gpu_driver() {
    local p; p=$(readlink -f "/sys/bus/pci/devices/$1/driver" 2>/dev/null) || return 0
    [ -n "$p" ] && basename "$p"
}

iommu_group_of() {
    local g
    g=$(readlink -f "/sys/bus/pci/devices/$1/iommu_group" 2>/dev/null) || return 1
    [ -n "$g" ] && basename "$g"
}

# Everything in this card's group that is neither part of the card nor a
# bridge, one per line. No output means the card can go on its own.
group_strays() {
    local dev="$1" grp slot m cls
    grp=$(iommu_group_of "$dev") || return 0
    slot="${dev%.*}"
    for m in /sys/kernel/iommu_groups/"$grp"/devices/*; do
        [ -e "$m" ] || continue
        m=$(basename "$m")
        [ "${m%.*}" = "$slot" ] && continue
        cls=$(cat "/sys/bus/pci/devices/$m/class" 2>/dev/null)
        case "$cls" in 0x0600*|0x0604*) continue ;; esac   # host and PCI bridges
        printf '%s  %s\n' "$m" "$(lspci -mm -s "$m" 2>/dev/null | awk -F'"' '{print $2}')"
    done
}

# How many processes are holding the card, according to nvidia-smi - the only
# thing that knows. The obvious alternative does not work: the sysfs "enabled"
# flag on a connector reads "disabled" for outputs the compositor is actively
# driving under the proprietary driver, so it cannot tell the card running your
# desktop from the spare sitting next to it.
gpu_clients() {
    local idx
    command -v nvidia-smi >/dev/null || return 0
    idx=$(nvidia-smi --query-gpu=index,pci.bus_id --format=csv,noheader 2>/dev/null |
          awk -F', *' -v w="$1" 'tolower($2) ~ tolower(w)"$" { print $1; exit }')
    [ -n "$idx" ] || return 0
    nvidia-smi 2>/dev/null | sed -n '/Processes:/,$p' | awk -v g="$idx" '
        /^\| +[0-9]+ / { line = $0; gsub(/^\| */, "", line); split(line, f, / +/)
                         if (f[1] == g) n++ }
        END { print n + 0 }'
}

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
    # Not necessarily a broken machine. With the GPU swap the card stays on the
    # host driver until the VM starts, so a box set up for it correctly shows
    # nothing on vfio-pci while the VM is off. The real prerequisite is a
    # second card that is alone in its IOMMU group; only when there is no such
    # card is there genuinely nothing to pass through.
    spare=""
    mapfile -t all_gpus < <(gpu_devices)
    if [ "${#all_gpus[@]}" -gt 1 ]; then
        for g in "${all_gpus[@]}"; do
            [ -n "$(group_strays "$g")" ] || spare="$g"
        done
    fi
    if [ -n "$spare" ]; then
        warn "nothing is bound to vfio-pci, but $spare is alone in its IOMMU group"
        info "bind it at boot, or turn on the GPU swap further down and let"
        info "Vypr hand it over only while the VM is running"
    else
        bad "no device is bound to vfio-pci. Vypr streams from a VM with a passed-through GPU; without one there is nothing to capture."
        FAIL=1
    fi
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

# Not a hard requirement - without the headers the window still builds, and
# presents through SDL_GPU - but the zero-copy presenter needs them, and it is
# several times faster at 4K.
if [ -f /usr/include/vulkan/vulkan.h ]; then
    ok "Vulkan headers present (zero-copy presenter)"
else
    warn "Vulkan headers missing (package: vulkan-headers / libvulkan-dev) - building without the zero-copy presenter, which is the fast path"
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
# Experimental, and inert until `vypr --debug gpu-swap --enable` puts the hook
# in place - installed here only so that command has something to point at.
install -Dm755 "$here/launcher/vypr-gpu-swap"  "$PREFIX/bin/vypr-gpu-swap" 2>/dev/null || true
install -Dm755 "$here/install/hooks/vypr-gpu"  "$PREFIX/share/vypr/hooks/vypr-gpu" 2>/dev/null || true
install -Dm644 "$here/install/xorg/20-vypr-gpu-swap.conf" \
               "$PREFIX/share/vypr/xorg/20-vypr-gpu-swap.conf" 2>/dev/null || true
install -Dm755 "$here/install/anti-detect.py" "$PREFIX/share/vypr/anti-detect.py" 2>/dev/null || true
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

changes=$(python3 - "$tmp/domain.xml" "$SHM_NAME" <<'PY'
import sys, xml.etree.ElementTree as ET
path, name = sys.argv[1], sys.argv[2]

# Passthrough domains routinely carry a <qemu:commandline> block. Without
# registering the prefix, ElementTree would rewrite it as ns0: and libvirt
# would no longer recognise it, so register libvirt's namespaces first.
for prefix, uri in (
    ("qemu", "http://libvirt.org/schemas/domain/qemu/1.0"),
    ("lxc",  "http://libvirt.org/schemas/domain/lxc/1.0"),
    ("libosinfo", "http://libosinfo.org/xmlns/libvirt/domain/1.0"),
):
    ET.register_namespace(prefix, uri)

tree = ET.parse(path); root = tree.getroot(); dev = root.find("devices")
changed = []

# Frames used to travel through an IVSHMEM device, which needed a third-party
# driver in the guest. They are read out of guest RAM now (below), so an older
# install's device is only 512 MiB of host memory held for nothing.
for sh in list(dev.findall("shmem")):
    if sh.get("name") == name:
        dev.remove(sh)
        changed.append("removed the old %s shared-memory device, no longer needed" % name)

# Frames are read straight out of guest RAM, which QEMU only exposes when the
# RAM is a shared memory file. virtiofs needs the same thing, so a domain with a
# home share already has it.
mb = root.find("memoryBacking")
if mb is None:
    mb = ET.Element("memoryBacking")
    # libvirt wants it early in the domain; after <currentMemory> is right.
    anchor = root.find("currentMemory") if root.find("currentMemory") is not None else root.find("memory")
    root.insert(list(root).index(anchor) + 1 if anchor is not None else 0, mb)
if mb.find("source") is None and mb.find("hugepages") is None:
    ET.SubElement(mb, "source").set("type", "memfd")
    changed.append("gave the VM memfd memory, so the host can read its frames")
if mb.find("access") is None or mb.find("access").get("mode") != "shared":
    acc = mb.find("access")
    if acc is None: acc = ET.SubElement(mb, "access")
    acc.set("mode", "shared")
    changed.append("shared the VM's memory with the host")

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

# ------------------------------------------------------ GPU swap (experimental)
head2 "Graphics cards"

# Ordinarily the card the VM uses is taken by vfio-pci at boot and Linux never
# sees it again: a whole GPU idle whenever the VM is off. The swap moves it on
# demand instead - to the guest when the VM starts, back to the host when it
# stops. Whether that is possible is a question about IOMMU groups, so the
# answer is worked out here and offered rather than left to be discovered.
WANT_GPU_SWAP=0

# The host devices this domain already passes through. That settles which card
# would move, so it is worth more than any guess made from the topology.
#
# Parsed rather than grepped. A domain's XML is full of <address type='pci'>
# elements - every emulated controller has one, and each <hostdev> carries a
# second one for where the device lands inside the guest - so matching address
# lines returns mostly guest slots that happen to read like host addresses. The
# only ones that mean anything here are <hostdev><source><address>.
passed=$(virsh dumpxml --inactive "$DOMAIN" 2>/dev/null | python3 -c '
import sys, xml.etree.ElementTree as ET
try:
    root = ET.fromstring(sys.stdin.read())
except Exception:
    sys.exit(0)
for hd in root.iter("hostdev"):
    if hd.get("type") != "pci":
        continue
    a = hd.find("./source/address")
    if a is None:
        continue
    f = lambda k, w: format(int(a.get(k, "0"), 16), "0%dx" % w)
    print("%s:%s:%s.%s" % (f("domain", 4), f("bus", 2), f("slot", 2), f("function", 1)))
')

mapfile -t gpus < <(gpu_devices)
free_gpus=()        # cards that are alone in their group
free_clients=()     # and how many processes are on each

for g in "${gpus[@]}"; do
    strays=$(group_strays "$g")
    clients=$(gpu_clients "$g")
    printf '\n  %s  %s\n' "$g" "$(gpu_name "$g")"
    printf '    IOMMU group %s, driver %s%s\n' \
        "$(iommu_group_of "$g" || echo '?')" "$(gpu_driver "$g" || echo none)" \
        "${clients:+, $clients process(es) on it}"
    grep -qx "$g" <<<"$passed" && printf '    passed through to %s\n' "$DOMAIN"

    if [ -n "$strays" ]; then
        printf '    cannot be passed through on its own - its group also holds:\n'
        printf '%s\n' "$strays" | sed 's/^/      /'
        continue
    fi

    printf '    alone in its group, so it can be handed over\n'
    free_gpus+=("$g"); free_clients+=("${clients:-0}")
done

# Which of those to recommend.
#
# The card the domain already passes through, if one of them is: that is not a
# guess, it is what the VM is configured to take. Otherwise the one with the
# fewest processes on it, which on a two-card machine is the one not drawing
# the desktop. Either way it stays a recommendation - the helper checks the
# card again at swap time and refuses if this was wrong.
candidate=""
for i in "${!free_gpus[@]}"; do
    g="${free_gpus[$i]}"
    if grep -qx "$g" <<<"$passed"; then candidate="$g"; break; fi
done
if [ -z "$candidate" ]; then
    best=-1
    for i in "${!free_gpus[@]}"; do
        if [ "$best" -lt 0 ] || [ "${free_clients[$i]}" -lt "$best" ]; then
            best="${free_clients[$i]}"; candidate="${free_gpus[$i]}"
        fi
    done
fi

printf '\n'
if [ "${#gpus[@]}" -lt 2 ]; then
    info "only one graphics card, so there is nothing to swap"
elif [ -z "$candidate" ]; then
    warn "no card is alone in its IOMMU group, so none can be swapped"
    info "the groups above are set by the board's PCIe layout; moving the card"
    info "to a different slot sometimes separates it"
else
    printf '  The card to enable the swap on is %s%s%s (%s).\n\n' \
        "$bold" "$candidate" "$rst" "$(gpu_name "$candidate")"
    cat <<EOF
  Enabling it means: vfio-pci stops claiming the card at boot, a libvirt hook
  hands it to '$DOMAIN' when it starts and takes it back when it
  stops, and the card is yours to use the rest of the time.

  It is experimental. The card must not be the one drawing your desktop - the
  helper checks that every time and refuses rather than taking your screens
  with it - and turning this on needs a few commands run as root, which are
  printed at the end rather than run for you.

EOF
    read -rp "  Enable the GPU swap? [y/N] " reply
    case "${reply:-n}" in
        [Yy]*) WANT_GPU_SWAP=1; ok "the commands to turn it on are printed below" ;;
        *)     info "not enabled - 'vypr --debug gpu-swap --enable' does it later" ;;
    esac
fi

# ------------------------------------------------------- hiding the hypervisor
head2 "Looking like a PC"

# Plenty of Windows software refuses to run under a hypervisor, and a stock
# libvirt domain tells it so immediately: CPUID returns KVMKVMKVM, SMBIOS reads
# QEMU, the NIC carries QEMU's registered 52:54:00 prefix. None of that is
# needed for the VM to work, so Vypr turns it off.
#
# The guest is given this machine's own board details, so it claims to be the
# computer it is running on rather than a generic PC that does not exist.
#
# Note on where this stops. The Hyper-V enlightenments stay enabled and the
# hypervisor-present CPU bit stays set, because every one of those
# enlightenments depends on Windows being able to see a hypervisor, and they
# are worth real performance in a guest that exists to run games. Hiding that
# bit as well is `anti-detect.py --paranoid`, and it costs them.
if [ -r /sys/class/dmi/id/board_name ]; then
    tmp4=$(mktemp -d)
    virsh dumpxml --inactive "$DOMAIN" > "$tmp4/domain.xml" 2>/dev/null

    dmi() { cat "/sys/class/dmi/id/$1" 2>/dev/null; }
    # The host's own NIC prefix is a real vendor's; QEMU's is not.
    host_oui=$(cat /sys/class/net/*/address 2>/dev/null |
               grep -vE '^(52:54:00|02:|a2:|7a:|42:)' | head -1 | cut -d: -f1-3)

    if out=$(python3 "$here/install/anti-detect.py" "$tmp4/domain.xml" "$tmp4/hard.xml" \
                --dmi "bios_vendor=$(dmi bios_vendor)" \
                --dmi "bios_version=$(dmi bios_version)" \
                --dmi "system_manufacturer=$(dmi sys_vendor)" \
                --dmi "system_product=$(dmi product_name)" \
                --dmi "system_version=$(dmi product_version)" \
                --dmi "baseBoard_manufacturer=$(dmi board_vendor)" \
                --dmi "baseBoard_product=$(dmi board_name)" \
                --dmi "baseBoard_version=$(dmi board_version)" \
                ${host_oui:+--mac "$host_oui"} 2>&1)
    then
        if grep -q . <<<"$out"; then
            while IFS= read -r line; do [ -n "$line" ] && ok "${line# }"; done <<<"$out"
            if virsh define "$tmp4/hard.xml" >/dev/null 2>&1; then
                ok "domain updated; it takes effect at the VM's next boot"
                warn "changing the NIC address means Windows sees a new adapter"
                warn "and takes a new DHCP lease - Vypr asks libvirt, so it copes"
                info ""
                info "This is on so that software which refuses to run under a"
                info "hypervisor will run. Some games forbid virtual machines in"
                info "their terms and treat hiding one as a bannable offence,"
                info "whatever it is being used for. Which games those are is"
                info "yours to know; Vypr does not and cannot check."
                info "Turn it off again with: vypr --debug anti-detect --undo"
            else
                bad "could not redefine the domain; it is unchanged"
            fi
        else
            info "already looks like a PC"
        fi
    else
        warn "could not rewrite the domain: $out"
    fi
    rm -rf "$tmp4"
else
    info "no DMI on this host, so there is nothing believable to copy"
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

# And under your group. Frames are read out of the VM's RAM through
# /proc/<qemu>/fd, which Linux only opens for a process whose user *and* group
# both match QEMU's - libvirt's default group shuts Vypr out.
my_group=$(id -gn)
qemu_group=$(grep -sE "^[[:space:]]*group[[:space:]]*=" /etc/libvirt/qemu.conf 2>/dev/null |
             tail -1 | sed 's/.*"\(.*\)".*/\1/')
if [ "$qemu_group" = "$my_group" ]; then
    ok "QEMU runs under your group, so Vypr can read frames out of the VM"
else
    warn "QEMU does not run under your group, so Vypr cannot read frames yet"
    MANUAL+=("sudo sed -i 's|^#\\?group = .*|group = \"$my_group\"|' /etc/libvirt/qemu.conf && sudo systemctl restart virtqemud   # let Vypr read frames; restart the VM after")
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

# ------------------------------------------------------- the old shared region
# Before frames moved into guest RAM, a tmpfiles rule kept /dev/shm/vypr around
# for the IVSHMEM device. Nothing uses it now, and once a VM has run with the
# device it holds 512 MiB of RAM until it is removed.
if [ -e /etc/tmpfiles.d/10-vypr.conf ] || [ -e /dev/shm/vypr ]; then
    warn "the old shared-memory region from an earlier Vypr is still set up"
    MANUAL+=("sudo rm -f /etc/tmpfiles.d/10-vypr.conf; rm -f /dev/shm/$SHM_NAME   # the old frame region, unused since frames moved into the VM's RAM")
fi

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
#
# Quoted, because a libvirt domain may be named anything a person can type.
# Unquoted, a name with a space in it sources as a command: "Microslop Win 11"
# set VM to "Microslop" and then tried to run "Win", and every later step failed
# on a VM that was never set.
VM="$DOMAIN"
GUEST="$guest_ip"
GUEST_USER="$guest_user"

# Parsec is started alongside the session because its driver is what makes the
# mouse work in games that read raw input. Set to 0 if you do not play those.
USE_PARSEC=1

# Let the VM grant administrator rights without a prompt. Windows draws that
# prompt where Vypr cannot show or answer it, so with this off an installer that
# asks for approval just hangs. It does lower the VM's security; set it to 0 to
# keep the prompt (you will have to answer it on the VM's own screen).
OPEN_ELEVATION=1

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

# ------------------------------------------------------------- gpu swap, on
if [ "${WANT_GPU_SWAP:-0}" = 1 ]; then
    head2 "Turning the GPU swap on"
    # Printed by the launcher rather than written out again here, so there is
    # one copy of these instructions and it is the same one you get from
    # `vypr --debug gpu-swap --enable` afterwards - including the extra file
    # X11 needs, which depends on the session you are sitting in.
    "$PREFIX/bin/vypr" --debug gpu-swap --enable || \
        warn "could not print the steps - run 'vypr --debug gpu-swap --enable'"
fi

# ------------------------------------------------------------------------- done
head2 "Next"

cat <<EOF
  Vypr $VERSION is installed on this side. The guest half is a single
  installer you run inside Windows:

    ${bold}vypr-setup.exe${rst}   (from the release page, or install/windows/)

  Copy it into the VM and run it. It installs the agent, gives it the one
  Windows right it needs to stream (no driver: frames are read straight out
  of the VM's memory), and registers it to start with the desktop. If you
  tick Parsec's mouse driver for raw-input games, Windows will ask you to
  accept it, which only a person can click. Restart Windows afterwards.

  It needs this public key, so the host can drive it:

$(sed 's/^/    /' "$KEY.pub")

EOF

if [ "${#MANUAL[@]}" -gt 0 ]; then
    printf '  %sThese need root, so they are yours to run:%s\n\n' "$bold" "$rst"
    for m in "${MANUAL[@]}"; do printf '    %s\n' "$m"; done
    printf '\n'
fi
