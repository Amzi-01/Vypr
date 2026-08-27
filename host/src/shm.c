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

int vypr_shm_open(struct vypr_shm *s, const char *path, int format)
{
    memset(s, 0, sizeof(*s));

    s->fd = open(path, O_RDWR);
    if (s->fd < 0) {
        fprintf(stderr, "vypr: open %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(s->fd, &st) < 0) { close(s->fd); return -1; }
    s->bytes = (size_t)st.st_size;

    if (s->bytes < VYPR_HEADER_BYTES + VYPR_DATA_ALIGN) {
        fprintf(stderr, "vypr: %s is only %zu bytes; too small to be useful\n",
                path, s->bytes);
        close(s->fd);
        return -1;
    }

    s->base = mmap(NULL, s->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, s->fd, 0);
    if (s->base == MAP_FAILED) {
        fprintf(stderr, "vypr: mmap %s: %s\n", path, strerror(errno));
        close(s->fd);
        return -1;
    }

    s->hdr = (struct vypr_shm_header *)s->base;
    s->alloc_cursor = VYPR_HEADER_BYTES;

    if (format) {
        uint32_t generation = 1;
        /* Preserve the generation across a restart if the region already held a
         * session, so a guest still running notices its offsets went stale. */
        if (s->hdr->magic == VYPR_SHM_MAGIC)
            generation = s->hdr->generation + 1;

        memset(s->hdr, 0, sizeof(*s->hdr));
        s->hdr->region_bytes = s->bytes;
        s->hdr->slot_count   = VYPR_MAX_SLOTS;
        s->hdr->version      = VYPR_SHM_VERSION;
        RELEASE(&s->hdr->generation, generation);
        /* Magic last: a guest polling for a formatted region must not see a
         * half-written header. */
        __atomic_store_n(&s->hdr->magic, VYPR_SHM_MAGIC, __ATOMIC_RELEASE);
    } else {
        if (ACQUIRE(&s->hdr->magic) != VYPR_SHM_MAGIC) {
            fprintf(stderr, "vypr: %s holds no vypr region (magic 0x%08x)\n",
                    path, s->hdr->magic);
            vypr_shm_close(s);
            return -1;
        }
        if (s->hdr->version != VYPR_SHM_VERSION) {
            fprintf(stderr, "vypr: region is version %u, this build speaks %u\n",
                    s->hdr->version, VYPR_SHM_VERSION);
            vypr_shm_close(s);
            return -1;
        }
    }
    return 0;
}

void vypr_shm_close(struct vypr_shm *s)
{
    if (s->base && s->base != MAP_FAILED) munmap(s->base, s->bytes);
    if (s->fd > 0) close(s->fd);
    memset(s, 0, sizeof(*s));
}

int vypr_shm_alloc(struct vypr_shm *s, uint64_t window_id,
                   uint32_t max_w, uint32_t max_h, struct vypr_msg_attach *out)
{
    uint32_t i;
    for (i = 0; i < VYPR_MAX_SLOTS; i++)
        if (s->hdr->slots[i].state == VYPR_SLOT_FREE) break;
    if (i == VYPR_MAX_SLOTS) {
        fprintf(stderr, "vypr: all %u slots in use\n", VYPR_MAX_SLOTS);
        return -1;
    }

    /* Round the ring generously: a window resized a little should not force a
     * re-attach, which costs a visible stall. */
    max_w = (uint32_t)align_up(max_w, 64);
    max_h = (uint32_t)align_up(max_h, 64);

    uint64_t stride      = align_up((uint64_t)max_w * 4, 256);
    uint64_t frame_bytes = align_up(stride * max_h, VYPR_DATA_ALIGN);
    uint64_t need        = frame_bytes * VYPR_RING_FRAMES;

    /* Reuse a range a closed window gave back before taking new ground.
     * Without this the region is consumed at roughly 114 MiB per 4K window and
     * a long session simply runs out - four windows opened and closed is enough
     * to exhaust 512 MiB, after which nothing can be streamed at all. */
    uint64_t offset = 0;
    int reused = -1;
    for (int f = 0; f < s->freed_count; f++) {
        if (s->freed[f].bytes >= need) { reused = f; break; }
    }

    if (reused >= 0) {
        offset = s->freed[reused].offset;
        if (s->freed[reused].bytes > need) {
            /* Keep the remainder rather than losing it. */
            s->freed[reused].offset += need;
            s->freed[reused].bytes  -= need;
        } else {
            s->freed[reused] = s->freed[--s->freed_count];
        }
    } else if (s->alloc_cursor + need <= s->bytes) {
        offset = s->alloc_cursor;
        s->alloc_cursor += need;
    } else {
        fprintf(stderr,
                "vypr: need %.1f MiB for %ux%u but only %.1f MiB unused and no "
                "freed range big enough (%d free)\n",
                need / 1048576.0, max_w, max_h,
                (s->bytes - s->alloc_cursor) / 1048576.0, s->freed_count);
        return -1;
    }

    struct vypr_slot *slot = &s->hdr->slots[i];
    /* Survives the wipe: the whole point is that it never repeats. */
    const uint32_t epoch = slot->epoch + 1;
    memset(slot, 0, sizeof(*slot));
    slot->epoch        = epoch;
    slot->format       = VYPR_FMT_BGRA8;
    slot->window_id    = window_id;
    slot->ring_offset  = offset;
    slot->frame_bytes  = frame_bytes;
    slot->max_width    = max_w;
    slot->max_height   = max_h;
    slot->frame_stride = (uint32_t)stride;
    RELEASE(&slot->state, (uint32_t)VYPR_SLOT_ARMED);

    memset(out, 0, sizeof(*out));
    out->window_id    = window_id;
    out->slot         = i;
    out->format       = VYPR_FMT_BGRA8;
    out->ring_offset  = slot->ring_offset;
    out->frame_bytes  = frame_bytes;
    out->max_width    = max_w;
    out->max_height   = max_h;
    out->frame_stride = (uint32_t)stride;
    out->generation   = epoch;   /* the guest echoes this back by binding to it */
    return 0;
}

void vypr_shm_free(struct vypr_shm *s, uint32_t slot)
{
    if (slot >= VYPR_MAX_SLOTS) return;
    struct vypr_slot *sl = &s->hdr->slots[slot];

    /*
     * Bump the epoch before anything else.
     *
     * This is what makes returning the memory safe. A publisher in the guest
     * binds to the epoch it saw and stops the moment it no longer matches, so
     * once this store lands nothing can still be writing into that range - and
     * the range can be handed to another window without one window's pixels
     * turning up in another's ring.
     */
    RELEASE(&sl->epoch, sl->epoch + 1);
    RELEASE(&sl->state, (uint32_t)VYPR_SLOT_FREE);

    const uint64_t bytes = sl->frame_bytes * VYPR_RING_FRAMES;
    if (bytes == 0) return;

    /* Give the range back. Adjacent ranges are merged where they meet, so a
     * window closed and reopened at the same size does not slowly shred the
     * region into pieces too small to use. */
    const uint64_t offset = sl->ring_offset;
    for (int f = 0; f < s->freed_count; f++) {
        if (s->freed[f].offset + s->freed[f].bytes == offset) {
            s->freed[f].bytes += bytes;
            return;
        }
        if (offset + bytes == s->freed[f].offset) {
            s->freed[f].offset = offset;
            s->freed[f].bytes += bytes;
            return;
        }
    }

    if (s->freed_count < VYPR_MAX_FREE_RANGES) {
        s->freed[s->freed_count].offset = offset;
        s->freed[s->freed_count].bytes  = bytes;
        s->freed_count++;
    }
}

uint32_t vypr_slot_state(struct vypr_shm *s, uint32_t slot)
{
    if (slot >= VYPR_MAX_SLOTS) return (uint32_t)VYPR_SLOT_FREE;
    return ACQUIRE(&s->hdr->slots[slot].state);
}

int vypr_shm_acquire(struct vypr_shm *s, uint32_t slot_index, uint32_t since,
                     struct vypr_frame_view *out)
{
    if (slot_index >= VYPR_MAX_SLOTS) return -1;
    struct vypr_slot *slot = &s->hdr->slots[slot_index];
    if (ACQUIRE(&slot->state) != VYPR_SLOT_LIVE) return -1;

    /* Seqlock read. A torn record means the guest published mid-read, so retry;
     * a handful of attempts is plenty, since the guest's write window is a few
     * stores wide. */
    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t seq0 = ACQUIRE(&slot->pub.seq);
        if (seq0 & 1u) continue;

        struct vypr_publish p = slot->pub;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        if (ACQUIRE(&slot->pub.seq) != seq0) continue;

        if (p.serial == since) return -2;

        /* Everything below is guest-supplied. A guest bug should drop a frame,
         * not walk the host off the end of the mapping. */
        if (p.index >= VYPR_RING_FRAMES) return -1;
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
