#define _GNU_SOURCE
#include "shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define ACQUIRE(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define RELEASE(p, v)   __atomic_store_n((p), (v), __ATOMIC_RELEASE)

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static uint64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/*
 * Return a range to the pool, joined to whatever it touches.
 *
 * Merging matters more than it looks: windows are closed and reopened at the
 * same size all day, and without it the region degrades into a scatter of
 * pieces that are each individually too small to carry a 4K ring, with plenty
 * of total space free and nothing able to use it.
 */
static void freed_insert(struct vypr_shm *s, uint64_t offset, uint64_t bytes)
{
    if (bytes == 0) return;

    /* Merge repeatedly rather than once: a range that closes the gap between
     * two others has to join both of them, not just the first one found. */
    int merged;
    do {
        merged = 0;
        for (int f = 0; f < s->freed_count; f++) {
            if (s->freed[f].offset + s->freed[f].bytes == offset) {
                offset = s->freed[f].offset;
                bytes += s->freed[f].bytes;
            } else if (offset + bytes == s->freed[f].offset) {
                bytes += s->freed[f].bytes;
            } else {
                continue;
            }
            s->freed[f] = s->freed[--s->freed_count];
            merged = 1;
            break;
        }
    } while (merged);

    /* Adjoins ground never handed out: rewind the cursor instead of keeping an
     * entry, so the region really is back to where it started. */
    if (offset + bytes == s->alloc_cursor) {
        s->alloc_cursor = offset;
        for (int f = 0; f < s->freed_count; f++) {
            if (s->freed[f].offset + s->freed[f].bytes == s->alloc_cursor) {
                s->alloc_cursor = s->freed[f].offset;
                s->freed[f] = s->freed[--s->freed_count];
                f = -1;   /* start again: the rewind may reach another range */
            }
        }
        return;
    }

    if (s->freed_count < VYPR_MAX_FREE_RANGES) {
        s->freed[s->freed_count].offset = offset;
        s->freed[s->freed_count].bytes  = bytes;
        s->freed_count++;
        return;
    }

    /* Nowhere left to record it. Losing the region is bad; losing track of it
     * while a window is using it would be worse, so the range is simply gone. */
    s->lost_bytes += bytes;
    fprintf(stderr, "vypr: free list full, %.1f MiB of region dropped\n",
            bytes / 1048576.0);
}

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

    /* Reuse a range a closed window gave back before taking new ground. A 4K
     * ring is about 114 MiB, so without reuse a 512 MiB region is spent after a
     * handful of windows and nothing can be streamed at all. Only ranges the
     * guest has finished with are on this list - see vypr_shm_free. */
    uint64_t offset = 0;
    int reused = -1;
    for (int f = 0; f < s->freed_count; f++) {
        if (s->freed[f].bytes < need) continue;
        /* Smallest range that fits, so a big one is left whole for a window
         * that needs all of it. */
        if (reused < 0 || s->freed[f].bytes < s->freed[reused].bytes) reused = f;
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
        uint64_t held = 0;
        for (int f = 0; f < s->pending_count; f++) held += s->pending[f].bytes;
        fprintf(stderr,
                "vypr: need %.1f MiB for %ux%u but only %.1f MiB unused, %d "
                "freed range(s), none big enough\n",
                need / 1048576.0, max_w, max_h,
                (s->bytes - s->alloc_cursor) / 1048576.0, s->freed_count);
        if (held)
            fprintf(stderr, "vypr: %.1f MiB still waiting on the guest to let "
                            "go of %d closed window(s)\n",
                    held / 1048576.0, s->pending_count);
        if (s->lost_bytes)
            fprintf(stderr, "vypr: %.1f MiB was never recovered\n",
                    s->lost_bytes / 1048576.0);
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

/* Hand a slot back for good and, if the range is safe, the range with it. */
static void slot_retire(struct vypr_shm *s, uint32_t slot)
{
    struct vypr_slot *sl = &s->hdr->slots[slot];

    /*
     * Bump the epoch last, and only here.
     *
     * A publisher binds to the epoch it was given and stops for good the moment
     * it no longer matches, so this store is what makes the slot safe to hand
     * to another window. It cannot be done when the window is first dropped:
     * the publisher's own "I have stopped" store is guarded by the same check,
     * so bumping early would silence the acknowledgement being waited for.
     */
    RELEASE(&sl->epoch, sl->epoch + 1);
    RELEASE(&sl->state, (uint32_t)VYPR_SLOT_FREE);
}

void vypr_shm_free(struct vypr_shm *s, uint32_t slot, enum vypr_writer writer)
{
    if (slot >= VYPR_MAX_SLOTS) return;
    struct vypr_slot *sl = &s->hdr->slots[slot];

    const uint64_t offset = sl->ring_offset;
    const uint64_t bytes  = sl->frame_bytes * VYPR_RING_FRAMES;

    /* Nothing carved means nothing to protect. */
    if (bytes == 0) writer = VYPR_WRITER_NONE;

    /*
     * A slot already back in the pool has already given its range up. Freeing
     * it twice would put the same range on the list twice and hand it to two
     * windows at once - the very thing all of this exists to prevent - so stop
     * here. Only the host ever writes FREE, so this is unambiguous.
     */
    const uint32_t state = ACQUIRE(&sl->state);
    if (state == VYPR_SLOT_FREE) return;

    /*
     * Take it all back at once when nothing in the guest can be mid-frame: the
     * ATTACH never went out, the guest refused it, or the guest has already
     * stored CLOSED. That last case is the common one - the guest closes its
     * own publisher the moment a window disappears, so by the time the host
     * hears about it the acknowledgement is usually already sitting there.
     */
    if (writer == VYPR_WRITER_NONE || state == VYPR_SLOT_CLOSED) {
        slot_retire(s, slot);
        freed_insert(s, offset, bytes);
        return;
    }

    /*
     * Otherwise the guest may be part-way through a frame right now. A frame is
     * written into the ring before it is published, so a copy that started
     * before the detach is still landing in this range for as long as it takes
     * to move 33 MiB - handing the range straight to a new window is precisely
     * how one window's pixels end up in another's.
     *
     * So park the slot in RETIRING and wait. Nothing reads this window any
     * more, so a late frame landing in its own ring harms nothing; what matters
     * is that the ring stays its own until the guest says it is done with it.
     */
    RELEASE(&sl->state, (uint32_t)VYPR_SLOT_RETIRING);

    if (s->pending_count >= VYPR_MAX_PENDING_RANGES) {
        /* No room to remember the wait. Give the slot back and write the range
         * off rather than reuse ground a live writer may still own. */
        s->lost_bytes += bytes;
        fprintf(stderr, "vypr: no room to retire slot %u; %.1f MiB written off\n",
                slot, bytes / 1048576.0);
        slot_retire(s, slot);
        return;
    }

    struct vypr_pending_range *p = &s->pending[s->pending_count++];
    p->offset    = offset;
    p->bytes     = bytes;
    p->slot      = slot;
    p->since_ns  = mono_ns();
    p->forfeited = 0;
}

int vypr_shm_reap(struct vypr_shm *s)
{
    const uint64_t now = mono_ns();
    int reclaimed = 0;

    for (int i = 0; i < s->pending_count; ) {
        struct vypr_pending_range *p = &s->pending[i];

        /* Already written off; only a departing agent brings these back. */
        if (p->forfeited) { i++; continue; }

        if (ACQUIRE(&s->hdr->slots[p->slot].state) == VYPR_SLOT_CLOSED) {
            /* The guest tears the capture down before storing this, which is
             * what drains any copy in flight. The range is ours again. */
            slot_retire(s, p->slot);
            freed_insert(s, p->offset, p->bytes);
            *p = s->pending[--s->pending_count];
            reclaimed++;
            continue;   /* the entry moved into this position needs looking at */
        }

        if (now - p->since_ns >= VYPR_RETIRE_TIMEOUT_NS) {
            /*
             * The guest never answered. Give the slot back so windows can still
             * be opened - the epoch bump stops the old publisher dead - but not
             * the range: a guest wedged mid-copy is exactly the one that would
             * scribble into whoever got it next. It comes back if the agent
             * ever goes away.
             */
            fprintf(stderr,
                    "vypr: guest never let go of slot %u; holding %.1f MiB back\n",
                    p->slot, p->bytes / 1048576.0);
            slot_retire(s, p->slot);
            p->forfeited = 1;
            s->lost_bytes += p->bytes;
            reclaimed++;
        }
        i++;
    }
    return reclaimed;
}

void vypr_shm_reap_all(struct vypr_shm *s)
{
    /* The agent is gone, so every publisher went with it and no store can land
     * in the region any more. Even ranges given up on above are safe now. */
    for (int i = 0; i < s->pending_count; i++) {
        struct vypr_pending_range *p = &s->pending[i];
        if (p->forfeited) s->lost_bytes -= p->bytes;
        else              slot_retire(s, p->slot);
        freed_insert(s, p->offset, p->bytes);
    }
    s->pending_count = 0;
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
