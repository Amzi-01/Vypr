#ifndef VYPR_HOST_SHM_H
#define VYPR_HOST_SHM_H

#include <stddef.h>
#include <stdint.h>
#include "vypr_shm.h"
#include "vypr_proto.h"

/*
 * One slot's ring, mapped out of guest RAM by a presenter.
 *
 * The guest's pages are scattered, so the ring is put back together here: a
 * reserved stretch of address space with each run of pages mapped into its
 * place. That makes it one contiguous range again, which is what lets the
 * Vulkan presenter hand the whole ring to the GPU in one import.
 */
struct vypr_ring_map {
    uint8_t  *base;
    uint64_t  bytes;
    uint32_t  ring_seq;      /* the slot's ring_seq when this was mapped */
    uint64_t  nonce;
    uint64_t  window_id;
};

struct vypr_shm {
    int       fd;
    void     *base;
    size_t    bytes;
    struct vypr_shm_header *hdr;

    /* Presenter side: the guest RAM file and the rings mapped out of it. */
    int       ram_fd;
    uint32_t  ram_seq;
    uint64_t  ram_bytes;
    struct vypr_ring_map rings[VYPR_MAX_SLOTS];

    /* A ring that could not be mapped, by its ring_seq (+1, so zero means
     * none). Not retried until the daemon describes a different ring: each try
     * is tens of thousands of mmap calls, and the presenter asks every frame. */
    uint32_t  map_failed[VYPR_MAX_SLOTS];
};

/* A borrowed view of the newest frame. Valid until the next acquire on the same
 * slot - the guest may reuse the buffer once we stop looking at it. */
struct vypr_frame_view {
    const uint8_t *pixels;
    /* The whole ring `pixels` lies in. It stays put for as long as the slot is
     * attached, which is what lets a presenter hand it to the GPU once and
     * have the GPU read every later frame straight out of it. */
    const uint8_t *ring;
    uint64_t ring_bytes;
    /* Which ring that is. A new ring can be mapped at the address an old one
     * used, so the address alone does not say whether an import is current. */
    uint64_t ring_id;
    uint32_t width, height, stride;
    uint32_t serial;
    uint64_t capture_qpc, capture_qpc_freq;
    uint32_t flags;

    /*
     * Changed rectangles, when `flags` has VYPR_PUB_DAMAGE_RECTS.
     *
     * Then only the pixels inside these rectangles are valid in `pixels`, and a
     * presenter must paint them over the frame it already holds rather than
     * treating `pixels` as a whole frame. Already validated against the frame's
     * own width, height and stride, so a presenter can use them without
     * re-checking. Zero `damage_count` with the flag clear means the whole
     * frame is valid, as before.
     */
    uint32_t damage_count;
    struct vypr_rect damage[VYPR_MAX_DAMAGE_RECTS];
};

/* `format` on open: 1 to create and format the region (the daemon is starting
 * a session), 0 to attach to one a running session already formatted. */
int  vypr_shm_open(struct vypr_shm *s, const char *path, int format);
void vypr_shm_close(struct vypr_shm *s);

/* Daemon: pick a free slot and decide a ring geometry big enough for
 * max_w x max_h, and fill `out` with the ATTACH to send. Returns 0, or -1 if
 * every slot is in use. */
int  vypr_shm_alloc(struct vypr_shm *s, uint64_t window_id,
                    uint32_t max_w, uint32_t max_h, struct vypr_msg_attach *out);

/* Daemon: give a slot up. Presenters still reading it see CLOSED and stop. */
void vypr_shm_free(struct vypr_shm *s, uint32_t slot);

/* Daemon: describe a slot's ring, already verified, and make the slot live. */
int  vypr_shm_set_ring(struct vypr_shm *s, uint32_t slot,
                       const struct vypr_ring_run *runs, uint32_t run_count,
                       uint64_t nonce, uint64_t pages);

/* Daemon: say where guest RAM is, for presenters to open. */
void vypr_shm_set_guest_ram(struct vypr_shm *s, const char *path, uint64_t bytes);

/* Presenter: the newest frame of `window_id`'s ring in `slot`. 0 on success,
 * -1 if the slot is not live for that window, -2 if no new frame since
 * `since` (or none yet). */
int  vypr_shm_acquire(struct vypr_shm *s, uint32_t slot, uint64_t window_id,
                      uint32_t since, struct vypr_frame_view *out);

/* Current state of a slot as far as `window_id` is concerned, read with
 * acquire ordering. A slot that now belongs to another window reads CLOSED. */
uint32_t vypr_slot_state(struct vypr_shm *s, uint32_t slot, uint64_t window_id);

/* Bytes a ring for this geometry takes in the guest: the header page and the
 * buffers. Both ends compute it, and both must agree. */
static inline uint64_t vypr_ring_bytes(uint64_t frame_bytes)
{
    return VYPR_RING_HEADER_BYTES + frame_bytes * VYPR_RING_FRAMES;
}

#endif
