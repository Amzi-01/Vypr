// Finding and mapping the vypr IVSHMEM region from inside the guest.
//
// There are now two IVSHMEM devices on this VM - Looking Glass's and ours - and
// the same driver binds both. Selecting by device index would silently stream
// frames into Looking Glass's region the first time the PCI order changed, so
// the region identifies itself instead: map each candidate, look for the vypr
// magic the host wrote, keep the one that has it.
#pragma once

#include <cstddef>
#include <cstdint>

namespace vypr {

class Region {
public:
    ~Region();
    Region() = default;
    Region(const Region&) = delete;
    Region& operator=(const Region&) = delete;

    // Scans every IVSHMEM device and keeps the one holding a formatted vypr
    // region. Returns false if none of them does, which normally means the host
    // session is not running yet.
    bool open();
    void close();

    void*       base()  const { return base_; }
    std::size_t bytes() const { return bytes_; }
    bool        valid() const { return base_ != nullptr; }

private:
    bool try_device(const wchar_t* path);

    void*       handle_ = nullptr;   // HANDLE, kept opaque to avoid windows.h here
    void*       base_   = nullptr;
    std::size_t bytes_  = 0;
};

}  // namespace vypr
