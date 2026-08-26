#ifndef SASH_HOST_SHM_H
#define SASH_HOST_SHM_H

#include <stddef.h>
#include <stdint.h>
#include "sash_shm.h"
#include "sash_proto.h"

/* A range returned to the pool when a window went away. */
struct sash_free_range {
    uint64_t offset;
    uint64_t bytes;
};

#define SASH_MAX_FREE_RANGES 64

struct sash_shm {
    int       fd;
    void     *base;
    size_t    bytes;
    struct sash_shm_header *hdr;
    uint64_t  alloc_cursor;   /* bump allocator position, host-private */

    struct sash_free_range freed[SASH_MAX_FREE_RANGES];
    int                    freed_count;
};

/* A borrowed view of the newest frame. Valid until the next acquire on the same
 * slot - the guest may reuse the buffer once we stop looking at it. */
struct sash_frame_view {
    const uint8_t *pixels;
    uint32_t width, height, stride;
    uint32_t serial;
    uint64_t capture_qpc, capture_qpc_freq;
    uint32_t flags;
};

/* `format` on open: 1 to write a fresh header (the host is starting a session),
 * 0 to attach to a region a running session already carved. */
int  sash_shm_open(struct sash_shm *s, const char *path, int format);
void sash_shm_close(struct sash_shm *s);

/* Carve a slot and a ring big enough for max_w x max_h, and fill `out` with the
 * attach message to hand the guest. Returns 0, or -1 if the region is full. */
int  sash_shm_alloc(struct sash_shm *s, uint64_t window_id,
                    uint32_t max_w, uint32_t max_h, struct sash_msg_attach *out);
void sash_shm_free(struct sash_shm *s, uint32_t slot);

/* 0 on success, -1 if the slot is not live, -2 if no new frame since `since`. */
int  sash_shm_acquire(struct sash_shm *s, uint32_t slot, uint32_t since,
                      struct sash_frame_view *out);

/* Current state of a slot, read with acquire ordering. Returns SASH_SLOT_FREE
 * for an out-of-range index. */
uint32_t sash_slot_state(struct sash_shm *s, uint32_t slot);

#endif
