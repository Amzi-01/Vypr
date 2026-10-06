// Publishing frames into a window's ring.
//
// Deliberately free of Windows headers. The seqlock ordering and the ring
// arithmetic are the parts that are hardest to get right and worst to debug
// across a VM boundary, so they live here where they can be compiled and run
// against the real host client on Linux. Only the capture and the memory that
// rings are made of are Windows-specific.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

extern "C" {
#include "vypr_shm.h"
#include "vypr_proto.h"
}

namespace vypr {

// Write the stamp the host checks into the first bytes of each page of a ring,
// before its page list is sent - see vypr_page_stamp. `pfns[i]` is where page i
// of the ring really is.
void stamp_pages(void* ring, std::size_t pages, const std::uint64_t* pfns,
                 std::uint64_t nonce);

// One window's ring. Non-owning: the memory belongs to whoever allocated it,
// and must outlive the publisher.
class Publisher {
public:
    Publisher() = default;

    // `ring` is the ring the host checked - header page first, `ring_bytes`
    // long - and `at` the ATTACH it answers. Writes the ring header, which is
    // what the host waits for before it reads anything. Returns false if the
    // ring cannot hold what the attach asks for.
    bool bind(void* ring, std::size_t ring_bytes, const vypr_msg_attach& at,
              std::uint64_t nonce);

    // Where to write the next frame's pixels, and the pitch to write at.
    // Returns nullptr if not bound. Never blocks: the ring always has a buffer
    // that is neither being displayed nor the one just published.
    std::uint8_t* begin_frame(std::uint32_t* out_stride) const;

    // Publish what begin_frame handed out. `width`/`height` may change between
    // frames when the window is resized, as long as they stay within the
    // ring's maximum - past that the host has to re-attach with a bigger ring.
    //
    // With `damage`/`damage_count`, only those rectangles of the ring buffer
    // were written and the host paints just them over the frame it holds - see
    // VYPR_PUB_DAMAGE_RECTS. The caller sets the matching flag; passing no
    // rectangles (the default) publishes a whole frame, as before. More than
    // VYPR_MAX_DAMAGE_RECTS are ignored past the limit, so the caller coalesces
    // first.
    bool publish(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                 std::uint64_t capture_ts, std::uint64_t ts_freq,
                 std::uint32_t flags,
                 const vypr_rect* damage = nullptr, std::uint32_t damage_count = 0);

    // The capture is torn down; say so in the header and stop.
    void close();

    std::uint32_t max_width()  const { return hdr_ ? hdr_->max_width  : 0; }
    std::uint32_t max_height() const { return hdr_ ? hdr_->max_height : 0; }
    std::uint32_t stride()     const { return hdr_ ? hdr_->frame_stride : 0; }
    std::uint32_t serial()     const { return serial_; }
    bool bound()               const { return hdr_ != nullptr; }

private:
    vypr_ring_header* hdr_    = nullptr;
    std::uint8_t*     frames_ = nullptr;
    std::uint64_t     frame_bytes_ = 0;
    std::uint32_t     index_  = 0;   // buffer begin_frame is currently lending
    std::uint32_t     serial_ = 0;
};

}  // namespace vypr
