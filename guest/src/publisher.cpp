#include "publisher.hpp"

#include <cstring>

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#endif

namespace vypr {

namespace {
// std::atomic_ref rather than atomics in the struct itself: the layout is a C
// struct shared with the host, so it must stay a plain struct. atomic_ref
// gives the ordering without changing that.
inline void store_release(volatile std::uint32_t& v, std::uint32_t x) {
    std::atomic_ref<std::uint32_t> r(const_cast<std::uint32_t&>(v));
    r.store(x, std::memory_order_release);
}
inline void store_relaxed(volatile std::uint32_t& v, std::uint32_t x) {
    std::atomic_ref<std::uint32_t> r(const_cast<std::uint32_t&>(v));
    r.store(x, std::memory_order_relaxed);
}

/*
 * Order streaming stores.
 *
 * Rings are ordinary write-back RAM now, where x86 keeps plain stores in
 * order by itself - but a large memcpy is free to use non-temporal stores, and
 * those are weakly ordered: the host could see the even sequence number before
 * the last rows of pixels. SFENCE is the instruction that puts them in order.
 * The C++ fences below compile to nothing on x86, so on their own they would
 * order the compiler and not the hardware.
 */
inline void store_fence() {
#if defined(_M_X64) || defined(__x86_64__)
    _mm_sfence();
#endif
}
}  // namespace

void stamp_pages(void* ring, std::size_t pages, const std::uint64_t* pfns,
                 std::uint64_t nonce) {
    auto* base = static_cast<std::uint8_t*>(ring);
    for (std::size_t i = 0; i < pages; i++) {
        vypr_page_stamp st{VYPR_PAGE_MAGIC, nonce, i, pfns[i]};
        std::memcpy(base + i * VYPR_PAGE_BYTES, &st, sizeof(st));
    }
    store_fence();
}

bool Publisher::bind(void* ring, std::size_t ring_bytes, const vypr_msg_attach& at,
                     std::uint64_t nonce) {
    hdr_ = nullptr;
    if (!ring || at.frame_bytes == 0 || at.frame_stride == 0) return false;

    // The host computed these, but a mismatch here writes pixels outside the
    // ring, so check rather than trust.
    const std::uint64_t need = VYPR_RING_HEADER_BYTES + at.frame_bytes * VYPR_RING_FRAMES;
    if (need > ring_bytes) return false;
    if (static_cast<std::uint64_t>(at.frame_stride) * at.max_height > at.frame_bytes) return false;
    static_assert(sizeof(vypr_ring_header) <= VYPR_RING_HEADER_BYTES, "ring header outgrew its page");

    auto* h = static_cast<vypr_ring_header*>(ring);
    // The page still holds the stamp the host checked. Clear it, so nothing of
    // it can be mistaken for a field.
    std::memset(h, 0, sizeof(*h));
    h->version      = VYPR_SHM_VERSION;
    h->nonce        = nonce;
    h->window_id    = at.window_id;
    h->slot         = at.slot;
    h->epoch        = at.generation;
    h->frame_offset = VYPR_RING_HEADER_BYTES;
    h->frame_bytes  = at.frame_bytes;
    h->max_width    = at.max_width;
    h->max_height   = at.max_height;
    h->frame_stride = at.frame_stride;
    h->format       = at.format;
    h->state        = VYPR_RING_READY;
    // Magic last: the host reads nothing until it sees it.
    std::atomic_thread_fence(std::memory_order_release);
    store_fence();
    store_release(*reinterpret_cast<volatile std::uint32_t*>(&h->magic), VYPR_RING_MAGIC);

    hdr_         = h;
    frames_      = static_cast<std::uint8_t*>(ring) + VYPR_RING_HEADER_BYTES;
    frame_bytes_ = at.frame_bytes;
    index_       = 0;
    serial_      = 0;
    return true;
}

std::uint8_t* Publisher::begin_frame(std::uint32_t* out_stride) const {
    if (!hdr_) return nullptr;
    if (out_stride) *out_stride = hdr_->frame_stride;
    return frames_ + static_cast<std::size_t>(index_) * frame_bytes_;
}

bool Publisher::publish(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                        std::uint64_t capture_ts, std::uint64_t ts_freq,
                        std::uint32_t flags,
                        const vypr_rect* damage, std::uint32_t damage_count) {
    if (!hdr_) return false;

    if (width == 0 || height == 0) return false;
    if (width > hdr_->max_width || height > hdr_->max_height) return false;
    if (static_cast<std::uint64_t>(stride) * height > frame_bytes_) return false;

    // A damage frame with no rectangles would tell the host the buffer holds
    // nothing valid, which is never what is meant: fall back to a whole frame.
    if ((flags & VYPR_PUB_DAMAGE_RECTS) && (!damage || damage_count == 0)) {
        flags = (flags & ~VYPR_PUB_DAMAGE_RECTS) | VYPR_PUB_DAMAGE_FULL;
    }
    if (damage_count > VYPR_MAX_DAMAGE_RECTS) damage_count = VYPR_MAX_DAMAGE_RECTS;

    vypr_publish& pub = hdr_->pub;

    // Seqlock write. Odd marks the record unstable; the release fences keep the
    // pixel writes and the field writes from being seen after the even store
    // that publishes them. Every row of the frame lands before the record says
    // it is there.
    store_fence();

    const std::uint32_t seq = pub.seq;
    store_relaxed(pub.seq, seq + 1);
    std::atomic_thread_fence(std::memory_order_release);

    pub.index            = index_;
    pub.serial           = ++serial_;
    pub.width            = width;
    pub.height           = height;
    pub.stride           = stride;
    pub.capture_qpc      = capture_ts;
    pub.capture_qpc_freq = ts_freq;
    pub.flags            = flags;

    pub.damage_count = (flags & VYPR_PUB_DAMAGE_RECTS) ? damage_count : 0;
    for (std::uint32_t r = 0; r < pub.damage_count; r++) pub.damage[r] = damage[r];

    std::atomic_thread_fence(std::memory_order_release);
    store_release(pub.seq, seq + 2);

    if (hdr_->state != VYPR_RING_LIVE) store_release(hdr_->state, VYPR_RING_LIVE);

    index_ = (index_ + 1) % VYPR_RING_FRAMES;
    return true;
}

void Publisher::close() {
    if (!hdr_) return;
    // Informational only: the host stops reading because the daemon dropped
    // the slot, not because of this. It makes a ring caught mid-teardown easy
    // to recognise in a memory dump, which is worth one store.
    store_release(hdr_->state, VYPR_RING_CLOSED);
    hdr_ = nullptr;
}

}  // namespace vypr
