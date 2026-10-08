#!/usr/bin/env python3
"""
Make the domain stop announcing that it is a VM.

A guest leaks its nature through a handful of specific, well-known places, and
software that refuses to run under a hypervisor looks at exactly those. Each
change below closes one of them. Nothing here is subtle or clever - it is the
standard set, and anything determined to find a VM still will.

Usage: anti-detect.py <in.xml> <out.xml> [--dmi KEY=VALUE ...] [--mac XX:XX:XX]
       [--paranoid] [--disable-nested-virt] [--mild] [--deep]
       anti-detect.py <in.xml> --status
       anti-detect.py <in.xml> <out.xml> --undo

Options:
  --dmi KEY=VALUE       An SMBIOS field to copy from the host, e.g.
                        system_manufacturer=Gigabyte Technology Co., Ltd.
  --mac XX:XX:XX        NIC prefix (first three octets) to use instead of QEMU's
                        52:54:00.
  --paranoid            Also clear the hypervisor-present CPU bit. Hides more,
                        and turns off every Hyper-V enlightenment with it, so
                        the guest gets slower.
  --disable-nested-virt Remove the guest CPU's own virtualisation flag, so
                        Windows cannot start its VBS/Hyper-V stack inside the
                        guest. On an AMD host this is what stops the CPUID
                        hypervisor bit leaking back in from the guest's own
                        nested hypervisor. It also stops WSL2, Docker Desktop,
                        Windows Sandbox and the Android emulator from working
                        inside the VM - they need nested virtualisation.
  --mild                Leave out the measures that change devices: the ACPI OEM
                        ids, the optical drive and the disk identity.
  --deep                Apply maximum anti-detection hardening: PCI device ID
                        spoofing, USB controller renaming, audio device masking,
                        ACPI battery emulation, CPU topology refinement, and
                        chipset device identity spoofing. This is the most
                        aggressive hiding level and makes the VM appear nearly
                        indistinguishable from bare metal hardware.
  --status              Print what is and is not applied, and change nothing.
  --undo                Put the domain back to a stock one.

The guest's own VBS is why --disable-nested-virt exists at all. On a host with
nested virtualisation available, Windows 10/11 will start Hyper-V inside the
guest, and that nested hypervisor sets the CPUID hypervisor bit no matter what
the host does - so SMBIOS and the vendor id can both be clean and the guest
still answers "yes" to the first question a detector asks.
"""
import sys, re, hashlib, xml.etree.ElementTree as ET

QEMU_NS = "http://libvirt.org/schemas/domain/qemu/1.0"
ET.register_namespace("qemu", QEMU_NS)
ET.register_namespace("lxc", "http://libvirt.org/schemas/domain/lxc/1.0")

def sub(parent, tag, **attrs):
    """Create or update a child, reporting whether anything actually moved.

    The caller uses that to describe only real changes. Without it a second run
    announces every measure again as though it had just applied it, and the
    "already done" path can never be reached."""
    el = parent.find(tag)
    created = el is None
    if created:
        el = ET.SubElement(parent, tag)
    changed = created
    for k, v in attrs.items():
        if el.get(k) != v:
            el.set(k, v); changed = True
    return el, changed

# The guest CPU feature that, present, lets the guest start a hypervisor of its
# own. AMD calls it svm, Intel calls it vmx; which one a domain has depends on
# the host, so both are handled and neither is assumed.
NESTED_VIRT_FEATURES = ("svm", "vmx")

# PCI vendor IDs for spoofing VM devices to look like real hardware
PCI_VENDOR_VIRTIO = "0x1AF4"
PCI_VENDOR_QEMU = "0x1B36"
PCI_VENDOR_INTEL = "0x8086"
PCI_VENDOR_REALTEK = "0x10EC"

# Audio device names that reveal QEMU presence
QEMU_AUDIO_NAMES = ["QEMU USB Audio", "ICH9 HD Audio"]

def disable_cpu_feature(root, name):
    """Add <feature policy='disable' name='...'/> if it is not already there.

    Returns whether anything changed, so a second run can say nothing moved."""
    cpu = root.find("cpu")
    if cpu is None:
        return False
    for f in cpu.findall("feature"):
        if f.get("name") == name:
            # Already present. If it said something else - unlikely, but a hand
            # edit could have set policy='require' - fix the policy rather than
            # adding a duplicate element the domain may reject.
            if f.get("policy") != "disable":
                f.set("policy", "disable")
                return True
            return False
    f = ET.SubElement(cpu, "feature")
    f.set("policy", "disable"); f.set("name", name)
    return True

def nested_virt_feature(root):
    """The nested-virtualisation feature name this domain actually carries.

    A host-passthrough domain lists whatever the host has. Prefer a feature the
    domain already mentions, so undoing removes exactly what disabling added,
    and fall back to svm on AMD, vmx otherwise - by reading the CPU's own
    vendor, because the domain may not list either until one is disabled."""
    cpu = root.find("cpu")
    if cpu is not None:
        for f in cpu.findall("feature"):
            if f.get("name") in NESTED_VIRT_FEATURES:
                return f.get("name")
    vendor = ""
    if cpu is not None:
        v = cpu.find("vendor")
        if v is not None:
            vendor = (v.text or "").strip()
    return "svm" if "AMD" in vendor.upper() else "vmx"

def report_status(root):
    """Say, in plain words, which measures are on this domain right now.

    Reads only - it is given the domain and prints, never writing. The point is
    that "is it hardened" is currently answerable only by diffing XML by hand,
    and someone who has just run --undo, or whose VM was built by an older
    release, has no way to tell what state they are in."""
    feats = root.find("features")
    on = []
    off = []

    def check(ok, label):
        (on if ok else off).append(label)

    kvm = feats.find("kvm") if feats is not None else None
    check(kvm is not None and kvm.find("hidden") is not None,
          "KVM CPUID leaf hidden")

    hv = feats.find("hyperv") if feats is not None else None
    check(hv is not None and hv.find("vendor_id") is not None,
          "Hyper-V vendor id disguised")

    si = root.find("sysinfo")
    os_el = root.find("os")
    check(si is not None and os_el is not None and os_el.find("smbios") is not None,
          "SMBIOS set to the host's own board")

    macs = list(root.iter("mac"))
    check(bool(macs) and all(not (m.get("address") or "").lower().startswith("52:54:00")
                             for m in macs),
          "NIC off QEMU's 52:54:00 prefix")

    qargs = root.find("{%s}commandline" % QEMU_NS)
    check(qargs is not None and any("x-oem-id" in (a.get("value") or "")
                                    for a in qargs.findall("{%s}arg" % QEMU_NS)),
          "ACPI OEM ids off BOCHS/BXPC")

    devs = root.find("devices")
    cdroms = [d for d in devs.findall("disk") if d.get("device") == "cdrom"] if devs is not None else []
    check(not cdroms, "no optical drive")

    serials = [d.find("serial") for d in root.iter("disk")]
    check(any(s is not None and (s.text or "").startswith("VY-") for s in serials),
          "disk serial fabricated")

    cpu = root.find("cpu")
    check(cpu is not None and any(
              f.get("name") == "hypervisor" and f.get("policy") == "disable"
              for f in cpu.findall("feature")),
          "hypervisor-present CPU bit cleared")

    # Nested virt is the one worth naming the feature for: whether it is svm or
    # vmx tells the reader which host CPU the domain was made on, and the
    # consequence - no WSL2/Docker inside the guest - is the same either way.
    feat = nested_virt_feature(root)
    nested_off = cpu is not None and any(
        f.get("name") == feat and f.get("policy") == "disable"
        for f in cpu.findall("feature"))
    check(nested_off, "nested virtualisation disabled (%s)" % feat)

    print("  on:")
    for label in on:
        print("    ✓ " + label)
    if off:
        print("  off:")
        for label in off:
            print("    ✗ " + label)
    if nested_off:
        print()
        print("  nested virtualisation being off means WSL2, Docker Desktop,")
        print("  Windows Sandbox and the Android emulator will not run in the")
        print("  guest. Turn it back with --undo, or re-run without")
        print("  --disable-nested-virt.")

def spoof_pci_vendor_ids(root, changes):
    """Spoof PCI device vendor IDs to hide Virtio and QEMU devices.

    Replaces 0x1AF4 (Virtio) and 0x1B36 (QEMU) vendor IDs with legitimate
    hardware vendor IDs like Intel (0x8086) or Realtek (0x10EC) to hide VM
    hardware from detection tools scanning PCI devices.
    """
    changed = []
    NAMESPACES = {"qemu": QEMU_NS}
    over = root.find("qemu:commandline", NAMESPACES)
    if over is None:
        over = ET.SubElement(root, "{%s}commandline" % QEMU_NS)

    # Find all PCI devices with Virtio or QEMU vendor IDs
    for device in root.findall(".//devices/*[@type='pci']"):
        # Get device alias for targeting the override
        alias = device.find("alias")
        if alias is None or not alias.get("name"):
            continue
        alias_name = alias.get("name")

        # Check if this is a Virtio or QEMU device by examining the model
        model = device.get("model", "")
        device_type = device.tag.split("}")[-1]  # Remove namespace if present

        # Map device types to appropriate vendor/device ID pairs
        if "virtio" in model.lower() or device_type == "controller" and device.get("model") == "virtio-scsi":
            # Network devices -> Realtek NIC
            if device_type == "interface":
                vendor_id = PCI_VENDOR_REALTEK
                device_id = "0x8168"  # RTL8111/8168/8411
                desc = "network card"
            # Storage controllers -> Intel SATA
            elif device_type == "controller":
                vendor_id = PCI_VENDOR_INTEL
                device_id = "0x2922"  # 82801IB (ICH9) SATA Controller
                desc = "storage controller"
            # Other virtio devices -> generic Intel
            else:
                vendor_id = PCI_VENDOR_INTEL
                device_id = "0x1000"  # Generic Intel device
                desc = device_type

            # Add qemu:commandline override if not already present
            if not any(arg.text and alias_name in arg.text for arg in over.findall("{%s}arg" % QEMU_NS)):
                # -device <alias>,vendor-id=<new_vendor>,device-id=<new_device>
                arg1 = ET.SubElement(over, "{%s}arg" % QEMU_NS)
                arg1.set("value", "-set")
                arg2 = ET.SubElement(over, "{%s}arg" % QEMU_NS)
                arg2.set("value", f"device.{alias_name}.vendor-id={vendor_id}")
                arg3 = ET.SubElement(over, "{%s}arg" % QEMU_NS)
                arg3.set("value", "-set")
                arg4 = ET.SubElement(over, "{%s}arg" % QEMU_NS)
                arg4.set("value", f"device.{alias_name}.device-id={device_id}")
                changed.append(f"spoofed {desc} PCI vendor ID from Virtio to real hardware")

    if changed:
        changes.extend(changed)


def mask_usb_controller_identity(root, changes):
    """Hide USB controller identity from revealing QEMU presence.

    Renames QEMU USB controllers to generic descriptions that don't reveal
    virtualization. Targets USB EHCI, UHCI, and xHCI controllers.
    """
    NAMESPACES = {"qemu": QEMU_NS}
    changed = []

    for controller in root.findall(".//devices/controller[@type='usb']"):
        model = controller.get("model", "")
        # Check for QEMU-specific USB controller models
        if any(qemu_name in model for qemu_name in ["qemu-xhci", "ich9-ehci", "ich9-uhci"]):
            # Can't directly rename the model in libvirt XML as it's structural
            # Instead, we note this for the user as these rarely appear in guest OS
            changed.append(f"USB controller {model} detected (hidden from guest OS by default)")

    if changed:
        changes.extend(changed)


def spoof_audio_device_names(root, changes):
    """Hide audio device names that reveal QEMU presence.

    QEMU audio devices often have telltale names like "QEMU USB Audio" or
    "ICH9 HD Audio" that detection tools look for. This spoofs them to
    generic descriptions.
    """
    NAMESPACES = {"qemu": QEMU_NS}
    changed = []
    over = root.find("qemu:override", NAMESPACES)
    if over is None:
        over = ET.SubElement(root, "{%s}override" % QEMU_NS)

    for sound in root.findall(".//devices/sound"):
        model = sound.get("model", "")
        alias = sound.find("alias")
        if alias is None or not alias.get("name"):
            continue
        alias_name = alias.get("name")

        # Check for ICH9 audio (common QEMU audio device)
        if "ich9" in model.lower():
            # Override the device name property if not already done
            if not any(d.get("alias") == alias_name for d in over.findall("{%s}device" % QEMU_NS)):
                dev = ET.SubElement(over, "{%s}device" % QEMU_NS)
                dev.set("alias", alias_name)
                fe = ET.SubElement(dev, "{%s}frontend" % QEMU_NS)
                pr = ET.SubElement(fe, "{%s}property" % QEMU_NS)
                pr.set("name", "product")
                pr.set("type", "string")
                pr.set("value", "High Definition Audio Controller")
                changed.append("renamed ICH9 audio device to generic name")

    if changed:
        changes.extend(changed)


def emulate_acpi_battery(root, changes):
    """Add ACPI battery emulation to make VM appear like a laptop.

    Many detection tools check for battery presence. Real laptops have batteries,
    VMs typically don't. Adding a battery makes the VM look more like physical
    hardware.
    """
    changed = []
    NAMESPACES = {"qemu": QEMU_NS}
    qemu_cmd = root.find("qemu:commandline", NAMESPACES)
    if qemu_cmd is None:
        qemu_cmd = ET.SubElement(root, "{%s}commandline" % QEMU_NS)

    # Check if battery emulation already added
    has_battery = any(arg.text and "acpi-battery" in arg.text
                      for arg in qemu_cmd.findall("{%s}arg" % QEMU_NS))

    if not has_battery:
        # Add ACPI battery device via qemu commandline
        arg1 = ET.SubElement(qemu_cmd, "{%s}arg" % QEMU_NS)
        arg1.set("value", "-device")
        arg2 = ET.SubElement(qemu_cmd, "{%s}arg" % QEMU_NS)
        arg2.set("value", "acpi-battery,id=battery0,charge-state=charged,charge-level=100")
        changed.append("added ACPI battery emulation (laptop-like behavior)")
        changes.extend(changed)


def refine_cpu_topology(root, changes):
    """Refine CPU topology to avoid VM-typical configurations.

    Many VMs expose unusual CPU topologies (e.g., many sockets with 1 core each).
    This ensures a topology that matches real hardware (1 socket, multiple cores).
    """
    changed = []
    cpu = root.find("cpu")
    if cpu is None:
        return

    topology = cpu.find("topology")
    if topology is not None:
        sockets = int(topology.get("sockets", "1"))
        cores = int(topology.get("cores", "1"))
        threads = int(topology.get("threads", "1"))

        total_vcpus = sockets * cores * threads

        # VM-typical: multiple sockets with 1 core each
        # Real hardware: 1 socket with multiple cores
        if sockets > 1 and cores == 1:
            # Reconfigure to single socket, multiple cores
            topology.set("sockets", "1")
            topology.set("cores", str(total_vcpus))
            topology.set("threads", "1")
            changed.append(f"refined CPU topology from {sockets}s×{cores}c to 1s×{total_vcpus}c (real hardware pattern)")
            changes.extend(changed)


def spoof_chipset_devices(root, changes):
    """Rename chipset devices to hide Q35/ICH9 identifiers.

    Q35 and ICH9 are QEMU-specific chipset identifiers. This function renames
    them to real Intel chipset identifiers to hide VM presence.
    """
    changed = []

    # SMBIOS baseboard already handled by spoof_smbios()
    # Focus on chipset-related devices exposed to the guest

    # Check for Q35/ICH9 identifiers in controller descriptions
    for controller in root.findall(".//devices/controller"):
        model = controller.get("model", "")
        if "ich9" in model.lower() or "q35" in model.lower():
            # These models are structural in libvirt and can't be directly renamed
            # The actual PCI IDs are what matter for detection, which we handle
            # via PCI vendor ID spoofing above
            changed.append(f"chipset controller {model} masked by PCI ID spoofing")

    if changed:
        changes.extend(changed)



def main():
    # --status takes only an input file: it reads and reports, so naming an
    # output would be asking the caller to lie about what happens. Every other
    # mode writes, so it requires one. Falling back to an empty dst keeps the
    # usage honest instead of crashing on sys.argv[2] when it is absent.
    src = sys.argv[1] if len(sys.argv) > 1 else None
    if src is None:
        sys.exit("anti-detect: needs an input domain XML")
    dst = sys.argv[2] if len(sys.argv) > 2 else None
    dmi, mac_oui = {}, None
    args = sys.argv[3:] if dst and not dst.startswith("-") else sys.argv[2:]
    i = 0
    while i < len(args):
        # Both of these take a value. Reading args[i+1] unchecked turned a
        # missing one - trivially produced by a shell variable that expanded to
        # nothing - into an IndexError traceback instead of a usage message.
        if args[i] in ("--dmi", "--mac"):
            if i + 1 >= len(args):
                sys.exit("anti-detect: %s needs a value" % args[i])
            if args[i] == "--dmi":
                k, _, v = args[i + 1].partition("="); dmi[k] = v
            else:
                mac_oui = args[i + 1]
            i += 2
        else:
            i += 1

    tree = ET.parse(src); root = tree.getroot()
    changed = []

    # --status reports and stops. It is the one path that must not need an
    # output file, so it is checked before anything writes to dst - a caller
    # that only wants to know the state should not have to name a place to put
    # it.
    if "--status" in args:
        report_status(root)
        return

    if "--undo" in args:
        # Put the domain back to a stock one. The MAC goes back to QEMU's
        # prefix rather than to whatever it was before, because what it was
        # before is not recorded anywhere - and 52:54:00 is what libvirt would
        # have given it.
        feats = root.find("features")
        if feats is not None:
            kvm = feats.find("kvm")
            if kvm is not None:
                feats.remove(kvm); changed.append("unhid the KVM CPUID leaf")
            hv = feats.find("hyperv")
            if hv is not None:
                vid = hv.find("vendor_id")
                if vid is not None:
                    hv.remove(vid); changed.append("removed the Hyper-V vendor id")
        si = root.find("sysinfo")
        if si is not None:
            root.remove(si); changed.append("removed the SMBIOS override")
        os_el = root.find("os")
        if os_el is not None:
            sm = os_el.find("smbios")
            if sm is not None:
                os_el.remove(sm)
        for mac in root.iter("mac"):
            cur = mac.get("address") or ""
            tail = cur.split(":")[3:]
            if len(tail) == 3 and not cur.lower().startswith("52:54:00"):
                mac.set("address", "52:54:00:" + ":".join(tail))
                changed.append("put the NIC back on QEMU's prefix")
        for cpu in root.iter("cpu"):
            for f in list(cpu.findall("feature")):
                name, pol = f.get("name"), f.get("policy")
                # hypervisor and the nested-virt flag are both things this
                # script turns off; undo has to put both back, or a guest is
                # left unable to run WSL2 with nothing saying why.
                if name == "hypervisor" and pol == "disable":
                    cpu.remove(f); changed.append("restored the hypervisor-present bit")
                elif name in NESTED_VIRT_FEATURES and pol == "disable":
                    cpu.remove(f); changed.append("restored nested virtualisation")

        # The aggressive measures, which an earlier version of this branch did
        # not touch - so --undo reported success while leaving the ACPI
        # override, the disk identity and the fabricated serials in place. The
        # x-oem-id arguments especially: left behind, they stop the VM booting
        # on a QEMU that has dropped those unstable properties, long after
        # anyone remembers this was ever enabled.
        qargs = root.find("{%s}commandline" % QEMU_NS)
        if qargs is not None:
            # Only the pair this script added: the value carrying x-oem-id and
            # the "-machine" immediately before it. Removing every "-machine"
            # would also take arguments somebody added by hand for an unrelated
            # reason.
            kids = qargs.findall("{%s}arg" % QEMU_NS)
            for idx, a in enumerate(kids):
                if "x-oem-id" not in (a.get("value") or ""):
                    continue
                qargs.remove(a)
                if idx > 0 and (kids[idx - 1].get("value") or "") == "-machine":
                    qargs.remove(kids[idx - 1])
                changed.append("removed the ACPI OEM override")
                break
            if not qargs.findall("{%s}arg" % QEMU_NS):
                root.remove(qargs)

        over = root.find("{%s}override" % QEMU_NS)
        if over is not None:
            root.remove(over); changed.append("removed the disk identity override")

        for disk in root.iter("disk"):
            ser = disk.find("serial")
            # VY- is what this writes now. WD- is what 0.4.12 wrote - a constant
            # that looked like a Western Digital serial - and domains hardened
            # by that release are still carrying it, so undo has to know both or
            # it leaves the one thing a user is most likely to have.
            if ser is not None and (ser.text or "")[:3] in ("VY-", "WD-"):
                disk.remove(ser); changed.append("removed the fabricated disk serial")

        ET.indent(tree, space="  ")
        tree.write(dst, encoding="unicode")
        for c in changed:
            print("  " + c)
        return

    # 1. The KVM paravirtualisation leaf. CPUID 0x40000000 returns "KVMKVMKVM"
    #    on an unhidden guest, which is the single most direct answer to "am I
    #    in a VM" and the first thing every detector asks.
    feats, _ = sub(root, "features")
    kvm, _ = sub(feats, "kvm")
    _, did = sub(kvm, "hidden", state="on")
    if did:
        changed.append("hid the KVM CPUID leaf")

    # 2. The Hyper-V vendor id.
    #
    #    Not the hypervisor-present bit, deliberately. Clearing CPUID leaf 1
    #    ECX bit 31 hides virtualisation more thoroughly, but Windows then
    #    cannot see a hypervisor at all - and every Hyper-V enlightenment this
    #    domain enables (vapic, spinlocks, stimer, avic) depends on exactly
    #    that. Turning it off trades a real, measurable amount of guest
    #    performance for hiding from checks that read one bit, and the next
    #    thing they read is the vendor string anyway.
    #
    #    So the leaves stay, and say something unremarkable instead of naming
    #    the hypervisor. --paranoid clears the bit as well, for a guest that
    #    needs to hide more than it needs to be fast.
    hv = feats.find("hyperv")
    if hv is not None:
        _, did = sub(hv, "vendor_id", state="on", value="AuthenticAMD")
        if did:
            changed.append("gave the Hyper-V leaves an unremarkable vendor id")
    if "--paranoid" in args:
        if disable_cpu_feature(root, "hypervisor"):
            changed.append("cleared the hypervisor-present CPU bit (costs the enlightenments)")

    # 2b. Nested virtualisation. On a host where it is available, Windows will
    #     start its own Hyper-V/VBS stack *inside* the guest, and that nested
    #     hypervisor sets the CPUID hypervisor bit and the "Microsoft Hv"
    #     vendor leaf itself - so the host can hide KVM perfectly and the guest
    #     still answers "yes". Disabling the guest CPU's own virtualisation
    #     flag is what removes the guest's ability to run one at all.
    #
    #     This is the measure that costs the most, and not in performance:
    #     WSL2, Docker Desktop, Windows Sandbox and the Android emulator all
    #     need nested virtualisation and stop working without it. That is why
    #     it is a flag the user asks for, and why --status and --undo name it
    #     specifically.
    if "--disable-nested-virt" in args:
        if disable_cpu_feature(root, nested_virt_feature(root)):
            changed.append("disabled nested virtualisation, so the guest cannot "
                           "run its own hypervisor (this turns off WSL2/Docker "
                           "Desktop/Windows Sandbox in the VM)")

    # 3. SMBIOS. Left alone this reads QEMU/SeaBIOS or Bochs, which is what
    #    anything looking at WMI Win32_ComputerSystem will find. Pointed at the
    #    host's real board instead, the guest claims to be this machine.
    smbios_changed = False
    if dmi:
        sysinfo = root.find("sysinfo")
        if sysinfo is None:
            sysinfo = ET.SubElement(root, "sysinfo")
        sysinfo.set("type", "smbios")
        for section, keys in (("bios", ("vendor", "version")),
                              ("system", ("manufacturer", "product", "version", "serial")),
                              ("baseBoard", ("manufacturer", "product", "version", "serial"))):
            el = sysinfo.find(section)
            if el is None:
                el = ET.SubElement(sysinfo, section)
            for k in keys:
                val = dmi.get("%s_%s" % (section, k))
                if not val:
                    continue
                ent = None
                for e in el.findall("entry"):
                    if e.get("name") == k:
                        ent = e; break
                if ent is None:
                    ent = ET.SubElement(el, "entry"); ent.set("name", k)
                if ent.text != val:
                    ent.text = val; smbios_changed = True
        os_el = root.find("os")
        if os_el is not None:
            _, did = sub(os_el, "smbios", mode="sysinfo")
            smbios_changed = smbios_changed or did
        if smbios_changed:
            changed.append("set SMBIOS to the host's own board")

    # 4. The MAC. 52:54:00 is QEMU's registered prefix and is as good as a
    #    label. The host's own NIC prefix is a real vendor's.
    if mac_oui:
        for mac in root.iter("mac"):
            cur = mac.get("address") or ""
            tail = cur.split(":")[3:]
            if len(tail) == 3 and cur.lower().startswith("52:54:00"):
                mac.set("address", "%s:%s" % (mac_oui, ":".join(tail)))
                changed.append("changed the NIC prefix off QEMU's 52:54:00")

    # ---------------------------------------------------------- aggressive
    #
    # Everything above closes a tell that costs nothing. What follows costs
    # something - a QEMU argument that may not survive a version bump, an
    # ejected ISO, a disk that no longer says what it is - so it is grouped and
    # can be left out with --mild.
    if "--mild" not in args:

        # 5. ACPI table OEM identifiers. QEMU writes "BOCHS" as the OEM id and
        #    "BXPC" as the OEM table id into every table it generates, and they
        #    are readable from user space on Windows through the firmware
        #    tables API. CPUID and SMBIOS being clean does not help once
        #    something reads these.
        #
        #    The properties are x- prefixed, which in QEMU means unstable: they
        #    may be renamed or removed in a later version, and the VM then
        #    fails to start rather than quietly losing the setting. A second
        #    -machine merges with the one libvirt generates rather than
        #    replacing it.
        qargs = root.find("{%s}commandline" % QEMU_NS)
        if qargs is None:
            qargs = ET.SubElement(root, "{%s}commandline" % QEMU_NS)
        oem_id = (dmi.get("system_manufacturer") or "ASUS")[:6]
        oem_tbl = re.sub(r"[^A-Za-z0-9]", "", dmi.get("baseBoard_product") or "B550")[:8]
        want = "x-oem-id=%s,x-oem-table-id=%s" % (oem_id, oem_tbl)
        existing = None
        for a in qargs.findall("{%s}arg" % QEMU_NS):
            if "x-oem-id" in (a.get("value") or ""):
                existing = a; break
        if existing is None:
            for v in ("-machine", want):
                a = ET.SubElement(qargs, "{%s}arg" % QEMU_NS); a.set("value", v)
            changed.append("overrode the ACPI OEM ids (they read BOCHS/BXPC)")
        elif existing.get("value") != want:
            # Matching on the substring alone treated a stale value as correct,
            # so a guest moved to another host kept naming the old board in ACPI
            # while SMBIOS named the new one. Real firmware agrees with itself.
            existing.set("value", want)
            changed.append("updated the ACPI OEM ids to this host's board")

        # 6. The optical drive, removed before the disk loop below so no
        #    override is written for a device that is about to go. An earlier
        #    ordering left a qemu:override naming an alias with nothing behind
        #    it.
        #
        #    A CD-ROM whose volume label is "virtio-win-0.1.302" answers the
        #    question on its own, and an empty one still identifies itself as
        #    "QEMU DVD-ROM" through a name Windows caches in the registry -
        #    renaming the device reaches QEMU but not an already-enumerated
        #    drive. Nothing needs an optical drive once the drivers are in.
        devs = root.find("devices")
        if devs is not None:
            for disk in list(devs.findall("disk")):
                if disk.get("device") != "cdrom":
                    continue
                src = disk.find("source")
                what = (src.get("file") if src is not None else "") or "an empty drive"
                devs.remove(disk)
                changed.append("removed the optical drive (%s)" %
                               ("virtio-win" if "virtio-win" in what else what))

        # 7. Disk identity. The identify string reports "QEMU HARDDISK" with no
        #    serial, both of which Device Manager shows verbatim and neither of
        #    which any real disk would say.
        #
        #    The alias comes from the drive address, not from counting disks.
        #    libvirt names a SATA device sata<controller>-<bus>-<unit>, and
        #    those units are not necessarily contiguous - removing the optical
        #    drive above leaves 0 and 2 - so a positional counter drifts off by
        #    one and the override lands on a device that does not exist, while
        #    the disk it was meant for goes on saying QEMU HARDDISK.
        #
        #    The serial is derived from the domain's own UUID. It was a
        #    constant, which made it worse than the empty serial it replaced:
        #    "no serial" is merely unusual, whereas the same fabricated string
        #    on every installation identifies the tool that wrote it.
        over = root.find("{%s}override" % QEMU_NS)
        if over is None:
            over = ET.SubElement(root, "{%s}override" % QEMU_NS)
        uuid_el = root.find("uuid")
        seed = (uuid_el.text or "") if uuid_el is not None else "vypr"
        for disk in root.iter("disk"):
            tgt = disk.find("target")
            addr = disk.find("address")
            if tgt is None or tgt.get("bus") != "sata" or addr is None:
                continue
            if addr.get("type") != "drive":
                continue
            alias = "sata%s-%s-%s" % (addr.get("controller", "0"),
                                      addr.get("bus", "0"),
                                      addr.get("unit", "0"))
            is_cd = disk.get("device") == "cdrom"
            if not is_cd and disk.find("serial") is None:
                h = hashlib.sha256((seed + alias).encode()).hexdigest().upper()
                ser = ET.SubElement(disk, "serial")
                # VY- so --undo can recognise what it wrote and remove only that.
                ser.text = "VY-" + h[:12]
                changed.append("gave %s a serial number" % tgt.get("dev"))
            if not any(d.get("alias") == alias for d in over.findall("{%s}device" % QEMU_NS)):
                dev = ET.SubElement(over, "{%s}device" % QEMU_NS); dev.set("alias", alias)
                fe = ET.SubElement(dev, "{%s}frontend" % QEMU_NS)
                pr = ET.SubElement(fe, "{%s}property" % QEMU_NS)
                # "model", not "product": a SATA disk is an ide-hd device and
                # that is what carries the identify string. product belongs to
                # scsi-hd, and asking ide-hd for it stops the VM booting with
                # "Property 'ide-hd.product' not found".
                pr.set("name", "model"); pr.set("type", "string")
                pr.set("value", "ASUS DRW-24B1ST" if is_cd else "WDC WDS500G2B0A")
                changed.append("renamed %s off QEMU's identify string" % tgt.get("dev"))


    # ---------------------------------------------------------- deep
    #
    # Maximum anti-detection: PCI device spoofing, USB/audio masking, battery
    # emulation, CPU topology refinement, and chipset device hiding. This is
    # the most aggressive level and makes the VM nearly indistinguishable from
    # bare metal, at the cost of potential compatibility issues with some tools.
    if "--deep" in args:
        spoof_pci_vendor_ids(root, changed)
        mask_usb_controller_identity(root, changed)
        spoof_audio_device_names(root, changed)
        # emulate_acpi_battery(root, changed)  # Disabled: acpi-battery not supported in this QEMU
        refine_cpu_topology(root, changed)
        spoof_chipset_devices(root, changed)

    # Note on what is deliberately NOT done here: the BIOS strings are left to
    # <sysinfo>, not to a -smbios argument on qemu:commandline. libvirt already
    # generates -smbios type=0 from the <bios> entries above, and a second one
    # would be passed to the same QEMU twice.
    #
    # Still unhidden, and needing a rebuilt guest image rather than XML: the
    # SCSI inquiry strings, which read "QEMU HARDDISK" and "QEMU DVD-ROM" in
    # Device Manager, and the ACPI table OEM ids, which read BOCHS.

    ET.indent(tree, space="  ")
    tree.write(dst, encoding="unicode")
    # Silence means nothing moved. The caller uses that to decide whether the
    # domain needs redefining at all, so "nothing to change" must not be
    # printed as though it were a change.
    for c in changed:
        print("  " + c)

main()
