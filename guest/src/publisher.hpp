// Publishing frames into a vypr slot.
//
// Deliberately free of Windows headers. The seqlock ordering and the ring
// arithmetic are the parts that are hardest to get right and worst to debug
// across a VM boundary, so they live here where they can be compiled and run
// against the real host client on Linux. Only the capture and transport around
// them are Windows-specific.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "vypr_shm.h"
}

namespace vypr {

// A slot the host has assigned to one window. Non-owning: the region belongs to
// the mapping, and the header belongs to the host.
class Publisher {
public:
    Publisher() = default;

    // `base` is the start of the mapped region, `slot_index` the slot the host
    // named in its ATTACH message. Returns false if the slot is not armed for
    // us, which means the host re-carved the region and our attach is stale.
    bool bind(void* base, std::size_t region_bytes, std::uint32_t slot_index);

    // Where to write the next frame's pixels, and the pitch to write at.
    // Returns nullptr if not bound. Never blocks: the ring always has a buffer
    // that is neither being displayed nor the one just published.
    std::uint8_t* begin_frame(std::uint32_t* out_stride) const;

    // Publish what begin_frame handed out. `width`/`height` may change between
    // frames when the window is resized, as long as they stay within the slot's
    // maximum - past that the host has to re-attach with a bigger ring.
    bool publish(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                 std::uint64_t capture_ts, std::uint64_t ts_freq,
                 std::uint32_t flags);

    // Guest window went away. The host reclaims the slot.
    void close();

    std::uint32_t max_width()  const { return slot_ ? slot_->max_width  : 0; }
    std::uint32_t max_height() const { return slot_ ? slot_->max_height : 0; }
    std::uint32_t stride()     const { return slot_ ? slot_->frame_stride : 0; }
    std::uint32_t serial()     const { return serial_; }
    bool bound()               const { return slot_ != nullptr; }

private:
    std::uint8_t*     base_   = nullptr;
    std::size_t       bytes_  = 0;
    struct vypr_slot* slot_   = nullptr;
    std::uint8_t*     ring_   = nullptr;
    std::uint32_t     index_  = 0;   // buffer begin_frame is currently lending
    std::uint32_t     serial_ = 0;
    std::uint32_t     epoch_  = 0;   // the allocation this binding belongs to
};

}  // namespace vypr
