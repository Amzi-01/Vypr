#include "gamepad.hpp"

#include <windows.h>
#include <setupapi.h>
#include <winioctl.h>

#include <cstdio>
#include <vector>

#include "vypr_proto.h"

namespace vypr {

namespace {

/*
 * ViGEmBus' interface, transcribed from its BusShared.h rather than linked.
 *
 * Only four of its requests are needed: say hello with a version, plug a pad
 * in, wait for Windows to finish enumerating it, and push reports at it.
 */
// {96E42B22-F5E9-42F8-B043-ED0F932F014F}
const GUID GUID_DEVINTERFACE_VIGEM = {
    0x96E42B22, 0xF5E9, 0x42F8, {0xB0, 0x43, 0xED, 0x0F, 0x93, 0x2F, 0x01, 0x4F}
};

#define VIGEM_W_IOCTL(i) CTL_CODE(FILE_DEVICE_BUS_EXTENDER, (i), METHOD_BUFFERED, FILE_WRITE_DATA)

constexpr DWORD VIGEM_BASE                = 0x801;
constexpr DWORD IOCTL_VIGEM_PLUGIN_TARGET = VIGEM_W_IOCTL(VIGEM_BASE + 0x000);
constexpr DWORD IOCTL_VIGEM_UNPLUG_TARGET = VIGEM_W_IOCTL(VIGEM_BASE + 0x001);
constexpr DWORD IOCTL_VIGEM_CHECK_VERSION = VIGEM_W_IOCTL(VIGEM_BASE + 0x002);
constexpr DWORD IOCTL_VIGEM_WAIT_READY    = VIGEM_W_IOCTL(VIGEM_BASE + 0x003);
constexpr DWORD IOCTL_XUSB_SUBMIT_REPORT  = VIGEM_W_IOCTL(VIGEM_BASE + 0x201);

constexpr ULONG VIGEM_COMMON_VERSION = 0x0001;
constexpr ULONG TARGET_XBOX360_WIRED = 0;

#pragma pack(push, 1)
struct CheckVersion { ULONG Size; ULONG Version; };
struct PluginTarget { ULONG Size; ULONG SerialNo; ULONG TargetType; USHORT VendorId; USHORT ProductId; };
struct UnplugTarget { ULONG Size; ULONG SerialNo; };
struct WaitReady    { ULONG Size; ULONG SerialNo; ULONG Timeout; };
struct XusbReport   { USHORT wButtons; BYTE bLeftTrigger; BYTE bRightTrigger;
                      SHORT sThumbLX; SHORT sThumbLY; SHORT sThumbRX; SHORT sThumbRY; };
struct XusbSubmit   { ULONG Size; ULONG SerialNo; XusbReport Report; };
#pragma pack(pop)

/* The bus exposes one device interface; find its path and open it. */
HANDLE open_bus()
{
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_VIGEM, nullptr, nullptr,
                                        DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) return INVALID_HANDLE_VALUE;

    HANDLE bus = INVALID_HANDLE_VALUE;
    SP_DEVICE_INTERFACE_DATA ifd{};
    ifd.cbSize = sizeof ifd;

    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &GUID_DEVINTERFACE_VIGEM, i, &ifd); i++) {
        DWORD needed = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &needed, nullptr);
        if (!needed) continue;

        std::vector<BYTE> buf(needed);
        auto* detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W*>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, detail, needed, nullptr, nullptr))
            continue;

        bus = CreateFileW(detail->DevicePath, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr);
        if (bus != INVALID_HANDLE_VALUE) break;
    }

    SetupDiDestroyDeviceInfoList(set);
    return bus;
}

/* The bus is opened overlapped, so every request carries its own event and
 * waits on it - otherwise concurrent requests would collide. */
bool ioctl_sync(HANDLE bus, DWORD code, void* in, DWORD in_len, DWORD timeout_ms = 5000)
{
    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!ov.hEvent) return false;

    DWORD returned = 0;
    BOOL ok = DeviceIoControl(bus, code, in, in_len, nullptr, 0, &returned, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING)
        ok = (WaitForSingleObject(ov.hEvent, timeout_ms) == WAIT_OBJECT_0) &&
             GetOverlappedResult(bus, &ov, &returned, FALSE);

    CloseHandle(ov.hEvent);
    return ok != FALSE;
}

}  // namespace

struct Gamepads::Impl {
    HANDLE bus = INVALID_HANDLE_VALUE;
    bool   plugged[4] = {false, false, false, false};
};

Gamepads::Gamepads() : impl_(std::make_unique<Impl>()) {}
Gamepads::~Gamepads() { close(); }

bool Gamepads::open()
{
    impl_->bus = open_bus();
    if (impl_->bus == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "vypr: ViGEmBus is not installed; controllers unavailable\n");
        return false;
    }

    CheckVersion cv{sizeof(CheckVersion), VIGEM_COMMON_VERSION};
    if (!ioctl_sync(impl_->bus, IOCTL_VIGEM_CHECK_VERSION, &cv, sizeof cv)) {
        std::fprintf(stderr, "vypr: ViGEmBus rejected our version\n");
        CloseHandle(impl_->bus);
        impl_->bus = INVALID_HANDLE_VALUE;
        return false;
    }

    std::fprintf(stderr, "vypr: controllers ready\n");
    return true;
}

void Gamepads::apply(const vypr_msg_gamepad& s)
{
    if (impl_->bus == INVALID_HANDLE_VALUE) return;
    if (s.index >= 4) return;
    const ULONG serial = s.index + 1;   /* the bus counts from one */

    const bool want = (s.flags & VYPR_PAD_CONNECTED) != 0;

    if (want && !impl_->plugged[s.index]) {
        PluginTarget plug{sizeof(PluginTarget), serial, TARGET_XBOX360_WIRED, 0, 0};
        if (!ioctl_sync(impl_->bus, IOCTL_VIGEM_PLUGIN_TARGET, &plug, sizeof plug)) {
            std::fprintf(stderr, "vypr: could not plug in pad %u\n", s.index);
            return;
        }
        /* Windows has to finish enumerating it before reports mean anything. */
        WaitReady wait{sizeof(WaitReady), serial, 3000};
        ioctl_sync(impl_->bus, IOCTL_VIGEM_WAIT_READY, &wait, sizeof wait, 4000);
        impl_->plugged[s.index] = true;
        std::fprintf(stderr, "vypr: controller %u plugged in\n", s.index);
    }

    if (!want) {
        if (impl_->plugged[s.index]) {
            UnplugTarget un{sizeof(UnplugTarget), serial};
            ioctl_sync(impl_->bus, IOCTL_VIGEM_UNPLUG_TARGET, &un, sizeof un);
            impl_->plugged[s.index] = false;
            std::fprintf(stderr, "vypr: controller %u unplugged\n", s.index);
        }
        return;
    }

    XusbSubmit rep{};
    rep.Size = sizeof(XusbSubmit);
    rep.SerialNo = serial;
    rep.Report.wButtons      = s.buttons;
    rep.Report.bLeftTrigger  = s.left_trigger;
    rep.Report.bRightTrigger = s.right_trigger;
    rep.Report.sThumbLX = s.lx;
    rep.Report.sThumbLY = s.ly;
    rep.Report.sThumbRX = s.rx;
    rep.Report.sThumbRY = s.ry;
    ioctl_sync(impl_->bus, IOCTL_XUSB_SUBMIT_REPORT, &rep, sizeof rep, 1000);
}

void Gamepads::close()
{
    if (impl_->bus == INVALID_HANDLE_VALUE) return;
    for (ULONG i = 0; i < 4; i++) {
        if (!impl_->plugged[i]) continue;
        UnplugTarget un{sizeof(UnplugTarget), i + 1};
        ioctl_sync(impl_->bus, IOCTL_VIGEM_UNPLUG_TARGET, &un, sizeof un);
        impl_->plugged[i] = false;
    }
    CloseHandle(impl_->bus);
    impl_->bus = INVALID_HANDLE_VALUE;
}

}  // namespace vypr
