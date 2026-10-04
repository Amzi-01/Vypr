#!/usr/bin/env python3
"""
Make the domain stop announcing that it is a VM.

A guest leaks its nature through a handful of specific, well-known places, and
software that refuses to run under a hypervisor looks at exactly those. Each
change below closes one of them. Nothing here is subtle or clever - it is the
standard set, and anything determined to find a VM still will.

Usage: anti-detect.py <in.xml> <out.xml> [--dmi KEY=VALUE ...] [--mac XX:XX:XX]
       [--paranoid]
"""
import sys, re, xml.etree.ElementTree as ET

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

def main():
    src, dst = sys.argv[1], sys.argv[2]
    dmi, mac_oui = {}, None
    args = sys.argv[3:]
    i = 0
    while i < len(args):
        if args[i] == "--dmi":
            k, _, v = args[i + 1].partition("="); dmi[k] = v; i += 2
        elif args[i] == "--mac":
            mac_oui = args[i + 1]; i += 2
        else:
            i += 1

    tree = ET.parse(src); root = tree.getroot()
    changed = []

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
                if f.get("name") == "hypervisor" and f.get("policy") == "disable":
                    cpu.remove(f); changed.append("restored the hypervisor-present bit")
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
        cpu = root.find("cpu")
        if cpu is not None and not any(f.get("name") == "hypervisor" for f in cpu.findall("feature")):
            f = ET.SubElement(cpu, "feature")
            f.set("policy", "disable"); f.set("name", "hypervisor")
            changed.append("cleared the hypervisor-present CPU bit (costs the enlightenments)")

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
