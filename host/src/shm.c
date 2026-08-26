#define _GNU_SOURCE
#include "shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACQUIRE(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define RELEASE(p, v)   __atomic_store_n((p), (v), __ATOMIC_RELEASE)

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

int sash_shm_open(struct sash_shm *s, const char *path, int format)
{
    memset(s, 0, sizeof(*s));

    s->fd = open(path, O_RDWR);
    if (s->fd < 0) {
        fprintf(stderr, "sash: open %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(s->fd, &st) < 0) { close(s->fd); return -1; }
    s->bytes = (size_t)st.st_size;

    if (s->bytes < SASH_HEADER_BYTES + SASH_DATA_ALIGN) {
        fprintf(stderr, "sash: %s is only %zu bytes; too small to be useful\n",
                path, s->bytes);
        close(s->fd);
        return -1;
    }

    s->base = mmap(NULL, s->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
    if (s->base == MAP_FAILED) {
        fprintf(stderr, "sash: mmap %s: %s\n", path, strerror(errno));
        close(s->fd);
        return -1;
    }

    s->hdr = (struct sash_shm_header *)s->base;
    s->alloc_cursor = SASH_HEADER_BYTES;

    if (format) {
        uint32_t generation = 1;
        /* Preserve the generation across a restart if the region already held a
         * session, so a guest still running notices its offsets went stale. */
        if (s->hdr->magic == SASH_SHM_MAGIC)
            generation = s->hdr->generation + 1;

        memset(s->hdr, 0, sizeof(*s->hdr));
        s->hdr->region_bytes = s->bytes;
        s->hdr->slot_count   = SASH_MAX_SLOTS;
        s->hdr->version      = SASH_SHM_VERSION;
        RELEASE(&s->hdr->generation, generation);
        /* Magic last: a guest polling for a formatted region must not see a
         * half-written header. */
        __atomic_store_n(&s->hdr->magic, SASH_SHM_MAGIC, __ATOMIC_RELEASE);
    } else {
        if (ACQUIRE(&s->hdr->magic) != SASH_SHM_MAGIC) {
            fprintf(stderr, "sash: %s holds no sash region (magic 0x%08x)\n",
                    path, s->hdr->magic);
            sash_shm_close(s);
            return -1;
        }
        if (s->hdr->version != SASH_SHM_VERSION) {
            fprintf(stderr, "sash: region is version %u, this build speaks %u\n",
                    s->hdr->version, SASH_SHM_VERSION);
            sash_shm_close(s);
            return -1;
        }
    }
    return 0;
}

void sash_shm_close(struct sash_shm *s)
{
    if (s->base && s->base != MAP_FAILED) munmap(s->base, s->bytes);
    if (s->fd > 0) close(s->fd);
    memset(s, 0, sizeof(*s));
}

int sash_shm_alloc(struct sash_shm *s, uint64_t window_id,
                   uint32_t max_w, uint32_t max_h, struct sash_msg_attach *out)
{
    uint32_t i;
    for (i = 0; i < SASH_MAX_SLOTS; i++)
        if (s->hdr->slots[i].state == SASH_SLOT_FREE) break;
    if (i == SASH_MAX_SLOTS) {
        fprintf(stderr, "sash: all %u slots in use\n", SASH_MAX_SLOTS);
        return -1;
    }

    /* Round the ring generously: a window resized a little should not force a
     * re-attach, which costs a visible stall. */
    max_w = (uint32_t)align_up(max_w, 64);
    max_h = (uint32_t)align_up(max_h, 64);

    uint64_t stride      = align_up((uint64_t)max_w * 4, 256);
    uint64_t frame_bytes = align_up(stride * max_h, SASH_DATA_ALIGN);
    uint64_t need        = frame_bytes * SASH_RING_FRAMES;

    if (s->alloc_cursor + need > s->bytes) {
        fprintf(stderr,
                "sash: need %.1f MiB for %ux%u but only %.1f MiB of region left\n",
                need / 1048576.0, max_w, max_h,
                (s->bytes - s->alloc_cursor) / 1048576.0);
        return -1;
    }

    struct sash_slot *slot = &s->hdr->slots[i];
    memset(slot, 0, sizeof(*slot));
    slot->format       = SASH_FMT_BGRA8;
    slot->window_id    = window_id;
    slot->ring_offset  = s->alloc_cursor;
    slot->frame_bytes  = frame_bytes;
    slot->max_width    = max_w;
    slot->max_height   = max_h;
    slot->frame_stride = (uint32_t)stride;
    RELEASE(&slot->state, (uint32_t)SASH_SLOT_ARMED);

    s->alloc_cursor += need;

    memset(out, 0, sizeof(*out));
    out->window_id    = window_id;
    out->slot         = i;
    out->format       = SASH_FMT_BGRA8;
    out->ring_offset  = slot->ring_offset;
    out->frame_bytes  = frame_bytes;
    out->max_width    = max_w;
    out->max_height   = max_h;
    out->frame_stride = (uint32_t)stride;
    out->generation   = s->hdr->generation;
    return 0;
}

void sash_shm_free(struct sash_shm *s, uint32_t slot)
{
    if (slot >= SASH_MAX_SLOTS) return;
    /* The bump cursor is deliberately not rewound. Reusing a freed range while
     * a guest thread might still be mid-write into it is the one way to get
     * pixels from one window appearing in another. Fragmentation is the
     * cheaper problem; a session re-format reclaims everything. */
    RELEASE(&s->hdr->slots[slot].state, (uint32_t)SASH_SLOT_FREE);
}

uint32_t sash_slot_state(struct sash_shm *s, uint32_t slot)
{
    if (slot >= SASH_MAX_SLOTS) return (uint32_t)SASH_SLOT_FREE;
    return ACQUIRE(&s->hdr->slots[slot].state);
}

int sash_shm_acquire(struct sash_shm *s, uint32_t slot_index, uint32_t since,
                     struct sash_frame_view *out)
{
    if (slot_index >= SASH_MAX_SLOTS) return -1;
    struct sash_slot *slot = &s->hdr->slots[slot_index];
    if (ACQUIRE(&slot->state) != SASH_SLOT_LIVE) return -1;

    /* Seqlock read. A torn record means the guest published mid-read, so retry;
     * a handful of attempts is plenty, since the guest's write window is a few
     * stores wide. */
    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t seq0 = ACQUIRE(&slot->pub.seq);
        if (seq0 & 1u) continue;

        struct sash_publish p = slot->pub;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        if (ACQUIRE(&slot->pub.seq) != seq0) continue;

        if (p.serial == since) return -2;

        /* Everything below is guest-supplied. A guest bug should drop a frame,
         * not walk the host off the end of the mapping. */
        if (p.index >= SASH_RING_FRAMES) return -1;
        if (p.width == 0 || p.height == 0) return -1;
        if (p.width > slot->max_width || p.height > slot->max_height) return -1;
        if (p.stride < (uint64_t)p.width * 4) return -1;
        if ((uint64_t)p.stride * p.height > slot->frame_bytes) return -1;

        uint64_t off = slot->ring_offset + (uint64_t)p.index * slot->frame_bytes;
        if (off + slot->frame_bytes > s->bytes) return -1;

        out->pixels           = (const uint8_t *)s->base + off;
        out->width            = p.width;
        out->height           = p.height;
        out->stride           = p.stride;
        out->serial           = p.serial;
        out->capture_qpc      = p.capture_qpc;
        out->capture_qpc_freq = p.capture_qpc_freq;
        out->flags            = p.flags;
        return 0;
    }
    return -2;
}
