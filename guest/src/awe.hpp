// Ring memory: locked physical pages the host can find in QEMU's memory file.
//
// Windows' AWE calls are the one documented way for a user-mode process to
// hold physical pages and learn their page numbers - which is exactly what the
// host needs, since QEMU keeps guest RAM in a file indexed by guest physical
// address. The pages are locked by construction: Windows never pages them out
// or moves them, so the numbers stay true for as long as the ring exists.
//
// Needs the "Lock pages in memory" right (SeLockMemoryPrivilege) on the
// account the agent runs as; vypr-setup grants it.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vypr {

// Turn on "Lock pages in memory" for this process. False, with the reason in
// `why`, if the account does not hold the right - which until it signs out and
// back in after being granted it, it does not.
bool enable_locked_pages(std::string* why);

// A random 64-bit value, for telling one ring's pages from another's.
std::uint64_t ring_nonce();

class GuestRing {
public:
    GuestRing() = default;
    ~GuestRing() { release(); }
    GuestRing(const GuestRing&) = delete;
    GuestRing& operator=(const GuestRing&) = delete;

    // Lock enough pages for `bytes`, mapped contiguously. False if Windows
    // could not spare them.
    bool allocate(std::size_t bytes);

    // Hand the pages back to Windows. Whatever the host still has mapped of
    // them is from then on someone else's memory, which is why the host only
    // ever reads it - see vypr_shm.h.
    void release();

    void*       base()  const { return base_; }
    std::size_t bytes() const { return pages_ * 4096u; }
    std::size_t pages() const { return pages_; }

    // Where each page of the ring is, in ring order.
    const std::uint64_t* pfns() const { return pfns_.data(); }

private:
    void*                      base_  = nullptr;
    std::size_t                pages_ = 0;
    std::vector<std::uint64_t> pfns_;
};

}  // namespace vypr
