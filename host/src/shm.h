#ifndef VYPR_HOST_SHM_H
#define VYPR_HOST_SHM_H

#include <stddef.h>
#include <stdint.h>
#include "vypr_shm.h"
#include "vypr_proto.h"

/* A range returned to the pool when a window went away. */
struct vypr_free_range {
    uint64_t offset;
    uint64_t bytes;
};

/*
 * A range whose window is gone but whose writer might not be.
 *
 * It is not reusable yet - see VYPR_SLOT_RETIRING in vypr_shm.h for why the
 * guest has to say so first.
 */
struct vypr_pending_range {
    uint64_t offset;
    uint64_t bytes;
    uint32_t slot;
    uint64_t since_ns;    /* when the wait started, for the give-up deadline */
    int      forfeited;   /* waited too long: slot recycled, range held back */
};

#define VYPR_MAX_FREE_RANGES    64
#define VYPR_MAX_PENDING_RANGES 64

/* How long to wait for the guest's acknowledgement before giving up on a
 * range. A guest that is answering at all acknowledges within a round trip;
 * this is long enough that only a wedged one hits it. */
#define VYPR_RETIRE_TIMEOUT_NS (2000ull * 1000000ull)

/* Whether anything in the guest could still be writing into a slot's ring. */
enum vypr_writer {
    VYPR_WRITER_NONE  = 0,  /* the guest was never told to attach, or refused */
    VYPR_WRITER_MAYBE = 1   /* ATTACH went out; wait for the guest to finish */
};

struct vypr_shm {
    int       fd;
    void     *base;
    size_t    bytes;
    struct vypr_shm_header *hdr;
    uint64_t  alloc_cursor;   /* bump allocator position, host-private */

    struct vypr_free_range freed[VYPR_MAX_FREE_RANGES];
    int                    freed_count;

    struct vypr_pending_range pending[VYPR_MAX_PENDING_RANGES];
    int                       pending_count;

    uint64_t lost_bytes;      /* forfeited or spilled, for the failure message */
};

/* A borrowed view of the newest frame. Valid until the next acquire on the same
 * slot - the guest may reuse the buffer once we stop looking at it. */
struct vypr_frame_view {
    const uint8_t *pixels;
    uint32_t width, height, stride;
    uint32_t serial;
    uint64_t capture_qpc, capture_qpc_freq;
    uint32_t flags;
};

/* `format` on open: 1 to write a fresh header (the host is starting a session),
 * 0 to attach to a region a running session already carved. */
int  vypr_shm_open(struct vypr_shm *s, const char *path, int format);
void vypr_shm_close(struct vypr_shm *s);

/* Carve a slot and a ring big enough for max_w x max_h, and fill `out` with the
 * attach message to hand the guest. Returns 0, or -1 if the region is full. */
int  vypr_shm_alloc(struct vypr_shm *s, uint64_t window_id,
                    uint32_t max_w, uint32_t max_h, struct vypr_msg_attach *out);

/*
 * Give a slot up. `writer` says whether the guest could still be writing into
 * it: with VYPR_WRITER_MAYBE the slot goes to RETIRING and neither it nor its
 * range comes back until vypr_shm_reap() sees the guest acknowledge.
 */
void vypr_shm_free(struct vypr_shm *s, uint32_t slot, enum vypr_writer writer);

/* Collect ranges whose guest has acknowledged. Call it from the event loop;
 * returns how many slots came back. */
int  vypr_shm_reap(struct vypr_shm *s);

/* The agent is gone, so nothing in the guest can be writing: take everything
 * back at once, including ranges an earlier reap had given up on. */
void vypr_shm_reap_all(struct vypr_shm *s);

/* 0 on success, -1 if the slot is not live, -2 if no new frame since `since`. */
int  vypr_shm_acquire(struct vypr_shm *s, uint32_t slot, uint32_t since,
                      struct vypr_frame_view *out);

/* Current state of a slot, read with acquire ordering. Returns VYPR_SLOT_FREE
 * for an out-of-range index. */
uint32_t vypr_slot_state(struct vypr_shm *s, uint32_t slot);

#endif
