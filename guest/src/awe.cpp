#include "awe.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cstdio>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace vypr {

static_assert(sizeof(ULONG_PTR) == sizeof(std::uint64_t), "the agent is 64-bit only");

bool enable_locked_pages(std::string* why) {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token)) {
        if (why) *why = "cannot open the process token";
        return false;
    }
    TOKEN_PRIVILEGES tp{};
    tp.PrivilegeCount = 1;
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    if (!LookupPrivilegeValueW(nullptr, L"SeLockMemoryPrivilege", &tp.Privileges[0].Luid)) {
        CloseHandle(token);
        if (why) *why = "Windows does not know the Lock pages in memory privilege";
        return false;
    }
    // Succeeds even when nothing was granted; the error code is what says so.
    AdjustTokenPrivileges(token, FALSE, &tp, 0, nullptr, nullptr);
    const DWORD err = GetLastError();
    CloseHandle(token);
    if (err == ERROR_SUCCESS) return true;
    if (why) {
        *why = err == ERROR_NOT_ALL_ASSIGNED
            ? "this account does not have the 'Lock pages in memory' right. Run "
              "vypr-setup again, then sign out and back in (or restart the VM)"
            : "could not enable 'Lock pages in memory'";
    }
    return false;
}

std::uint64_t ring_nonce() {
    std::uint64_t v = 0;
    if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&v), sizeof(v),
                        BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        LARGE_INTEGER q;
        QueryPerformanceCounter(&q);
        v = static_cast<std::uint64_t>(q.QuadPart) * 0x9E3779B97F4A7C15ull ^ GetCurrentProcessId();
    }
    return v ? v : 1;
}

bool GuestRing::allocate(std::size_t bytes) {
    release();
    const std::size_t want = (bytes + 4095u) / 4096u;
    if (want == 0) return false;

    std::vector<ULONG_PTR> pfns(want);
    std::size_t have = 0;
    // Windows may hand out fewer pages than asked and expects to be asked
    // again for the rest, so ask until there are enough or it gives nothing.
    while (have < want) {
        ULONG_PTR n = want - have;
        if (!AllocateUserPhysicalPages(GetCurrentProcess(), &n, pfns.data() + have) || n == 0) {
            std::fprintf(stderr, "vypr: could only lock %zu of %zu pages (error %lu)\n",
                         have, want, GetLastError());
            if (have) {
                ULONG_PTR h = have;
                FreeUserPhysicalPages(GetCurrentProcess(), &h, pfns.data());
            }
            return false;
        }
        have += n;
    }

    // In address order, so that whatever pages do sit side by side reach the
    // host as one run rather than several.
    std::sort(pfns.begin(), pfns.end());

    void* base = VirtualAlloc(nullptr, want * 4096u, MEM_RESERVE | MEM_PHYSICAL, PAGE_READWRITE);
    if (!base || !MapUserPhysicalPages(base, want, pfns.data())) {
        std::fprintf(stderr, "vypr: could not map a ring (error %lu)\n", GetLastError());
        if (base) VirtualFree(base, 0, MEM_RELEASE);
        ULONG_PTR h = want;
        FreeUserPhysicalPages(GetCurrentProcess(), &h, pfns.data());
        return false;
    }

    base_  = base;
    pages_ = want;
    pfns_.assign(pfns.begin(), pfns.end());
    return true;
}

void GuestRing::release() {
    if (!base_) return;
    MapUserPhysicalPages(base_, pages_, nullptr);
    VirtualFree(base_, 0, MEM_RELEASE);
    std::vector<ULONG_PTR> pfns(pfns_.begin(), pfns_.end());
    ULONG_PTR n = pages_;
    FreeUserPhysicalPages(GetCurrentProcess(), &n, pfns.data());
    base_  = nullptr;
    pages_ = 0;
    pfns_.clear();
}

}  // namespace vypr
