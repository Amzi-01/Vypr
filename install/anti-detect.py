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
        have = [a.get("value") for a in qargs.findall("{%s}arg" % QEMU_NS)]
        oem_id = (dmi.get("system_manufacturer") or "ASUS")[:6]
        oem_tbl = re.sub(r"[^A-Za-z0-9]", "", dmi.get("baseBoard_product") or "B550")[:8]
        if not any("x-oem-id" in (v or "") for v in have):
            for v in ("-machine", "x-oem-id=%s,x-oem-table-id=%s" % (oem_id, oem_tbl)):
                a = ET.SubElement(qargs, "{%s}arg" % QEMU_NS); a.set("value", v)
            changed.append("overrode the ACPI OEM ids (they read BOCHS/BXPC)")

        # 6. Disk serials and product strings. The SCSI inquiry reports "QEMU
        #    HARDDISK" and an empty serial, both of which Device Manager shows
        #    verbatim and neither of which any real disk would say. The serial
        #    is libvirt's to set; the product string is a device property, so
        #    it goes through qemu:override against the alias libvirt derives
        #    from the drive address.
        over = root.find("{%s}override" % QEMU_NS)
        if over is None:
            over = ET.SubElement(root, "{%s}override" % QEMU_NS)
        unit = 0
        for disk in root.iter("disk"):
            tgt = disk.find("target")
            if tgt is None or tgt.get("bus") != "sata":
                continue
            alias = "sata0-0-%d" % unit
            is_cd = disk.get("device") == "cdrom"
            unit += 1
            if not is_cd and disk.find("serial") is None:
                ser = ET.SubElement(disk, "serial")
                ser.text = "WD-%s" % ("".join("%02X" % ((unit * 37 + i * 91) % 256) for i in range(6)))
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
                changed.append("renamed %s off \"QEMU HARDDISK\"" % tgt.get("dev"))

        # 7. The optical drive. A CD-ROM whose volume label is
        #    "virtio-win-0.1.302" answers the question on its own, and an empty
        #    one still identifies itself as "QEMU DVD-ROM" through a name
        #    Windows caches in the registry - renaming the device does not
        #    change what an already-enumerated drive reports. Since nothing
        #    needs an optical drive once the drivers are installed, the whole
        #    device goes.
        #
        #    --undo does not put it back: what was in it is not recorded. Add a
        #    CD-ROM in virt-manager if the virtio drivers are ever needed again.
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
