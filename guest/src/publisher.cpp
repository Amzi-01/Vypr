#include "publisher.hpp"

#include <cstring>

namespace vypr {

namespace {
// std::atomic_ref rather than atomics in the struct itself: the layout is a C
// struct shared with the host and with the kernel's view of the BAR, so it must
// stay a plain struct. atomic_ref gives the ordering without changing that.
inline void store_release(volatile std::uint32_t& v, std::uint32_t x) {
    std::atomic_ref<std::uint32_t> r(const_cast<std::uint32_t&>(v));
    r.store(x, std::memory_order_release);
}
inline std::uint32_t load_acquire(const volatile std::uint32_t& v) {
    std::atomic_ref<std::uint32_t> r(const_cast<std::uint32_t&>(v));
    return r.load(std::memory_order_acquire);
}
inline void store_relaxed(volatile std::uint32_t& v, std::uint32_t x) {
    std::atomic_ref<std::uint32_t> r(const_cast<std::uint32_t&>(v));
    r.store(x, std::memory_order_relaxed);
}
}  // namespace

bool Publisher::bind(void* base, std::size_t region_bytes, std::uint32_t slot_index) {
    slot_ = nullptr;
    if (!base || slot_index >= VYPR_MAX_SLOTS) return false;

    auto* hdr = static_cast<vypr_shm_header*>(base);
    if (load_acquire(hdr->magic) != VYPR_SHM_MAGIC) return false;
    if (hdr->version != VYPR_SHM_VERSION) return false;

    vypr_slot* slot = &hdr->slots[slot_index];
    if (load_acquire(slot->state) != VYPR_SLOT_ARMED) return false;

    // The host computes these, but a mismatch here writes pixels outside the
    // region, so check rather than trust.
    if (slot->frame_bytes == 0 || slot->frame_stride == 0) return false;
    const std::uint64_t span = slot->ring_offset +
                               slot->frame_bytes * VYPR_RING_FRAMES;
    if (span > region_bytes) return false;
    if (static_cast<std::uint64_t>(slot->frame_stride) * slot->max_height >
        slot->frame_bytes) return false;

    base_   = static_cast<std::uint8_t*>(base);
    bytes_  = region_bytes;
    slot_   = slot;
    epoch_  = slot->epoch;
    ring_   = base_ + slot->ring_offset;
    index_  = 0;
    serial_ = 0;
    return true;
}

std::uint8_t* Publisher::begin_frame(std::uint32_t* out_stride) const {
    if (!slot_) return nullptr;
    if (out_stride) *out_stride = slot_->frame_stride;
    return ring_ + static_cast<std::size_t>(index_) * slot_->frame_bytes;
}

bool Publisher::publish(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                        std::uint64_t capture_ts, std::uint64_t ts_freq,
                        std::uint32_t flags) {
    if (!slot_) return false;

    // The host handed this slot index to somebody else. Writing now would put
    // this window's pixels into theirs, so stop for good rather than race.
    if (load_acquire(slot_->epoch) != epoch_) {
        slot_ = nullptr;
        return false;
    }

    if (width == 0 || height == 0) return false;
    if (width > slot_->max_width || height > slot_->max_height) return false;
    if (static_cast<std::uint64_t>(stride) * height > slot_->frame_bytes) return false;

    vypr_publish& pub = slot_->pub;

    // Seqlock write. Odd marks the record unstable; the release fences keep the
    // pixel writes and the field writes from being seen after the even store
    // that publishes them.
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

    std::atomic_thread_fence(std::memory_order_release);
    store_release(pub.seq, seq + 2);

    // First publish takes the slot live, so the host starts reading only once
    // there is a whole frame to read.
    if (load_acquire(slot_->state) == VYPR_SLOT_ARMED)
        store_release(slot_->state, VYPR_SLOT_LIVE);

    index_ = (index_ + 1) % VYPR_RING_FRAMES;
    return true;
}

void Publisher::close() {
    if (!slot_) return;
    store_release(slot_->state, VYPR_SLOT_CLOSED);
    slot_ = nullptr;
}

}  // namespace vypr
