#include "ivshmem.hpp"

#include <windows.h>
// CTL_CODE and the METHOD_/FILE_ constants live here. WIN32_LEAN_AND_MEAN keeps
// windows.h from pulling it in, so it has to be asked for explicitly.
#include <winioctl.h>
#include <setupapi.h>
#include <initguid.h>

#include <cstdio>
#include <vector>

extern "C" {
#include "sash_shm.h"
}

#pragma comment(lib, "setupapi.lib")

namespace {

// ABI of the Looking Glass IVSHMEM driver. These constants are the driver's
// public contract, not ours.
//
// Verified against the installed driver (build dated 2025-03-06): its PDB names
// ioctl_request_peerid, ioctl_request_size, ioctl_request_mmap and
// ioctl_release_mmap in that order, giving function codes 0x800-0x803, and the
// interface GUID below appears in the .sys image. The struct fields match the
// PDB's peerID / size / vectors / cacheMode. Re-check if the driver is ever
// updated - a changed IOCTL number fails as ERROR_INVALID_FUNCTION.
DEFINE_GUID(GUID_DEVINTERFACE_IVSHMEM,
            0xdf576976, 0x569d, 0x4672, 0x95, 0xa0, 0xf5, 0x7e, 0x4e, 0xa0, 0xb2, 0x10);

#define IOCTL_IVSHMEM_REQUEST_SIZE \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x801, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_REQUEST_MMAP \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x802, METHOD_BUFFERED, FILE_ANY_ACCESS)
#define IOCTL_IVSHMEM_RELEASE_MMAP \
    CTL_CODE(FILE_DEVICE_UNKNOWN, 0x803, METHOD_BUFFERED, FILE_ANY_ACCESS)

enum IVSHMEM_CACHE_MODE {
    IVSHMEM_CACHE_NONCACHED      = 0,
    IVSHMEM_CACHE_CACHED         = 1,
    IVSHMEM_CACHE_WRITECOMBINED  = 2
};

struct IVSHMEM_MMAP_CONFIG { UINT8 cacheMode; };

struct IVSHMEM_MMAP {
    UINT16 peerID;
    UINT64 size;
    PVOID  ptr;
    UINT16 vectors;
};

}  // namespace

namespace sash {

Region::~Region() { close(); }

bool Region::try_device(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;

    IVSHMEM_MMAP_CONFIG cfg{};
    // Write-combined: the agent only ever writes pixels here, and WC turns
    // those into full cache-line bursts across PCIe. Reads from WC memory are
    // slow, which is fine - the only reads are the slot geometry, once.
    cfg.cacheMode = IVSHMEM_CACHE_WRITECOMBINED;

    IVSHMEM_MMAP map{};
    DWORD returned = 0;
    if (!DeviceIoControl(h, IOCTL_IVSHMEM_REQUEST_MMAP, &cfg, sizeof(cfg),
                         &map, sizeof(map), &returned, nullptr)) {
        // Expected for any device another process already holds - Looking
        // Glass's host keeps its own region mapped. Only interesting if no
        // device works at all, which open() reports.
        CloseHandle(h);
        return false;
    }

    if (!map.ptr || map.size < SASH_HEADER_BYTES) {
        DeviceIoControl(h, IOCTL_IVSHMEM_RELEASE_MMAP, nullptr, 0, nullptr, 0, &returned, nullptr);
        CloseHandle(h);
        return false;
    }

    const auto* hdr = static_cast<const sash_shm_header*>(map.ptr);
    if (hdr->magic != SASH_SHM_MAGIC) {
        // Somebody else's region - Looking Glass's, most likely. Let it go.
        DeviceIoControl(h, IOCTL_IVSHMEM_RELEASE_MMAP, nullptr, 0, nullptr, 0, &returned, nullptr);
        CloseHandle(h);
        return false;
    }
    if (hdr->version != SASH_SHM_VERSION) {
        std::fprintf(stderr, "sash: region is version %u, agent speaks %u\n",
                     hdr->version, SASH_SHM_VERSION);
        DeviceIoControl(h, IOCTL_IVSHMEM_RELEASE_MMAP, nullptr, 0, nullptr, 0, &returned, nullptr);
        CloseHandle(h);
        return false;
    }

    handle_ = h;
    base_   = map.ptr;
    bytes_  = static_cast<std::size_t>(map.size);
    std::fprintf(stderr, "sash: mapped %.0f MiB region, generation %u\n",
                 bytes_ / 1048576.0, hdr->generation);
    return true;
}

bool Region::open() {
    close();

    HDEVINFO info = SetupDiGetClassDevsW(&GUID_DEVINTERFACE_IVSHMEM, nullptr, nullptr,
                                         DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (info == INVALID_HANDLE_VALUE) {
        std::fprintf(stderr, "sash: no IVSHMEM class; is the driver installed?\n");
        return false;
    }

    int seen = 0;
    for (DWORD i = 0;; i++) {
        SP_DEVICE_INTERFACE_DATA iface{};
        iface.cbSize = sizeof(iface);
        if (!SetupDiEnumDeviceInterfaces(info, nullptr, &GUID_DEVINTERFACE_IVSHMEM, i, &iface))
            break;
        seen++;

        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(info, &iface, nullptr, 0, &need, nullptr);
        if (!need) continue;

        std::vector<std::uint8_t> buf(need);
        auto* detail = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        detail->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(info, &iface, detail, need, nullptr, nullptr))
            continue;

        if (try_device(detail->DevicePath)) {
            SetupDiDestroyDeviceInfoList(info);
            return true;
        }
    }

    SetupDiDestroyDeviceInfoList(info);
    if (seen == 0)
        std::fprintf(stderr, "sash: IVSHMEM driver present but no devices\n");
    else
        std::fprintf(stderr,
                     "sash: %d IVSHMEM device(s), none holding a sash region - "
                     "start the host session first\n", seen);
    return false;
}

void Region::close() {
    if (handle_) {
        DWORD returned = 0;
        DeviceIoControl(handle_, IOCTL_IVSHMEM_RELEASE_MMAP, nullptr, 0, nullptr, 0,
                        &returned, nullptr);
        CloseHandle(handle_);
    }
    handle_ = nullptr;
    base_   = nullptr;
    bytes_  = 0;
}

}  // namespace sash
