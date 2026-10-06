#define _GNU_SOURCE
#include "shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define ACQUIRE(p)      __atomic_load_n((p), __ATOMIC_ACQUIRE)
#define RELEASE(p, v)   __atomic_store_n((p), (v), __ATOMIC_RELEASE)

static uint64_t align_up(uint64_t v, uint64_t a) { return (v + a - 1) & ~(a - 1); }

static struct vypr_ring_run *slot_runs(struct vypr_shm *s, uint32_t slot)
{
    return (struct vypr_ring_run *)((uint8_t *)s->base + s->hdr->slots[slot].runs_offset);
}

int vypr_shm_open(struct vypr_shm *s, const char *path, int format)
{
    memset(s, 0, sizeof(*s));
    s->ram_fd = -1;

    /*
     * The daemon creates the region itself: it is host-only now, so there is
     * no device that has to have it open first. 0600, because nothing but this
     * user's own processes has any business reading which windows are open.
     */
    s->fd = format ? open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600)
                   : open(path, O_RDONLY | O_CLOEXEC);
    if (s->fd < 0) {
        fprintf(stderr, "vypr: open %s: %s\n", path, strerror(errno));
        return -1;
    }

    struct stat st;
    if (fstat(s->fd, &st) < 0) { close(s->fd); return -1; }

    /* Grown, never shrunk: a file somebody else still maps would fault for
     * them at the first touch past the new end. */
    if (format && (uint64_t)st.st_size < VYPR_HOST_REGION_BYTES) {
        if (ftruncate(s->fd, (off_t)VYPR_HOST_REGION_BYTES) < 0) {
            fprintf(stderr, "vypr: sizing %s: %s\n", path, strerror(errno));
            close(s->fd);
            return -1;
        }
        st.st_size = (off_t)VYPR_HOST_REGION_BYTES;
    }
    s->bytes = (size_t)st.st_size;

    if (s->bytes < VYPR_HOST_REGION_BYTES) {
        fprintf(stderr, "vypr: %s is only %zu bytes; not a vypr region\n", path, s->bytes);
        close(s->fd);
        return -1;
    }

    /* Presenters only ever read it, so they map it that way. */
    s->base = mmap(NULL, s->bytes, format ? PROT_READ | PROT_WRITE : PROT_READ,
                   MAP_SHARED, s->fd, 0);
    if (s->base == MAP_FAILED) {
        fprintf(stderr, "vypr: mmap %s: %s\n", path, strerror(errno));
        s->base = NULL;
        close(s->fd);
        return -1;
    }

    s->hdr = (struct vypr_shm_header *)s->base;

    if (format) {
        uint32_t generation = 1, ram_seq = 1;
        /* Carried across a restart, so a presenter still running from the last
         * session notices everything it knew went stale. */
        if (s->hdr->magic == VYPR_SHM_MAGIC) {
            generation = s->hdr->generation + 1;
            ram_seq    = (s->hdr->guest_ram_seq | 1u) + 1u;   /* even, and newer */
        }
        uint32_t epochs[VYPR_MAX_SLOTS], ring_seqs[VYPR_MAX_SLOTS];
        for (uint32_t i = 0; i < VYPR_MAX_SLOTS; i++) {
            epochs[i]    = s->hdr->slots[i].epoch;
            ring_seqs[i] = s->hdr->slots[i].ring_seq & ~1u;
        }

        memset(s->hdr, 0, sizeof(*s->hdr));
        s->hdr->region_bytes  = s->bytes;
        s->hdr->slot_count    = VYPR_MAX_SLOTS;
        s->hdr->version       = VYPR_SHM_VERSION;
        s->hdr->guest_ram_seq = ram_seq;
        for (uint32_t i = 0; i < VYPR_MAX_SLOTS; i++) {
            s->hdr->slots[i].epoch       = epochs[i];
            s->hdr->slots[i].ring_seq    = ring_seqs[i];
            s->hdr->slots[i].runs_offset = VYPR_HEADER_BYTES +
                (uint64_t)i * VYPR_RUNS_PER_SLOT * sizeof(struct vypr_ring_run);
        }
        RELEASE(&s->hdr->generation, generation);
        /* Magic last: a presenter must not see a half-written header. */
        RELEASE(&s->hdr->magic, VYPR_SHM_MAGIC);
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

static void ring_unmap(struct vypr_ring_map *m)
{
    if (m->base) munmap(m->base, m->bytes);
    memset(m, 0, sizeof(*m));
}

void vypr_shm_close(struct vypr_shm *s)
{
    for (uint32_t i = 0; i < VYPR_MAX_SLOTS; i++) ring_unmap(&s->rings[i]);
    if (s->ram_fd >= 0) close(s->ram_fd);
    if (s->base) munmap(s->base, s->bytes);
    if (s->fd >= 0) close(s->fd);
    memset(s, 0, sizeof(*s));
    s->fd = s->ram_fd = -1;
}

/* ------------------------------------------------------------- the daemon */

int vypr_shm_alloc(struct vypr_shm *s, uint64_t window_id,
                   uint32_t max_w, uint32_t max_h, struct vypr_msg_attach *out)
{
    uint32_t i;
    for (i = 0; i < VYPR_MAX_SLOTS; i++)
        if (ACQUIRE(&s->hdr->slots[i].state) == VYPR_SLOT_FREE) break;
    if (i == VYPR_MAX_SLOTS) {
        fprintf(stderr, "vypr: all %u slots in use\n", VYPR_MAX_SLOTS);
        return -1;
    }

    /* Round the ring generously: a window resized a little should not force a
     * re-attach, which costs a visible stall. */
    max_w = (uint32_t)align_up(max_w, 64);
    max_h = (uint32_t)align_up(max_h, 64);

    const uint64_t stride      = align_up((uint64_t)max_w * 4, 256);
    const uint64_t frame_bytes = align_up(stride * max_h, VYPR_PAGE_BYTES);
    if (vypr_ring_bytes(frame_bytes) / VYPR_PAGE_BYTES > VYPR_MAX_RING_PAGES) {
        fprintf(stderr, "vypr: a %ux%u ring would be %.0f MiB, past the %u MiB limit\n",
                max_w, max_h, vypr_ring_bytes(frame_bytes) / 1048576.0,
                VYPR_MAX_RING_PAGES / 256u);
        return -1;
    }

    struct vypr_slot *slot = &s->hdr->slots[i];
    /* These survive the wipe: the whole point of each is that it never
     * repeats for this slot index. */
    const uint32_t epoch    = slot->epoch + 1;
    const uint32_t ring_seq = slot->ring_seq & ~1u;
    const uint64_t runs_off = slot->runs_offset;
    memset(slot, 0, sizeof(*slot));
    slot->epoch        = epoch;
    slot->ring_seq     = ring_seq;
    slot->runs_offset  = runs_off;
    slot->format       = VYPR_FMT_BGRA8;
    slot->window_id    = window_id;
    slot->frame_bytes  = frame_bytes;
    slot->max_width    = max_w;
    slot->max_height   = max_h;
    slot->frame_stride = (uint32_t)stride;
    RELEASE(&slot->state, (uint32_t)VYPR_SLOT_ARMED);

    memset(out, 0, sizeof(*out));
    out->window_id    = window_id;
    out->slot         = i;
    out->format       = VYPR_FMT_BGRA8;
    out->frame_bytes  = frame_bytes;
    out->max_width    = max_w;
    out->max_height   = max_h;
    out->frame_stride = (uint32_t)stride;
    out->generation   = epoch;   /* the guest echoes this back in its ring */
    return 0;
}

void vypr_shm_free(struct vypr_shm *s, uint32_t slot)
{
    if (slot >= VYPR_MAX_SLOTS) return;
    /*
     * Straight back to FREE. There is nothing to wait for: the ring was the
     * guest's own memory and goes back to the guest, and the next window to
     * get this slot gets a ring of its own. A presenter still reading this one
     * stops because the slot is no longer LIVE for its window id.
     */
    RELEASE(&s->hdr->slots[slot].state, (uint32_t)VYPR_SLOT_FREE);
}

int vypr_shm_set_ring(struct vypr_shm *s, uint32_t slot,
                      const struct vypr_ring_run *runs, uint32_t run_count,
                      uint64_t nonce, uint64_t pages)
{
    if (slot >= VYPR_MAX_SLOTS || run_count == 0 || run_count > VYPR_RUNS_PER_SLOT)
        return -1;
    struct vypr_slot *sl = &s->hdr->slots[slot];

    /* Seqlock write, so a presenter copying the table never takes half of an
     * old one and half of a new one. */
    const uint32_t seq = sl->ring_seq | 1u;
    RELEASE(&sl->ring_seq, seq);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    memcpy(slot_runs(s, slot), runs, (size_t)run_count * sizeof(*runs));
    sl->run_count  = run_count;
    sl->ring_nonce = nonce;
    sl->ring_pages = pages;

    RELEASE(&sl->ring_seq, seq + 1);
    RELEASE(&sl->state, (uint32_t)VYPR_SLOT_LIVE);
    return 0;
}

void vypr_shm_set_guest_ram(struct vypr_shm *s, const char *path, uint64_t bytes)
{
    if (strcmp(s->hdr->guest_ram_path, path) == 0 && s->hdr->guest_ram_bytes == bytes)
        return;
    const uint32_t seq = s->hdr->guest_ram_seq | 1u;
    RELEASE(&s->hdr->guest_ram_seq, seq);
    __atomic_thread_fence(__ATOMIC_RELEASE);
    snprintf(s->hdr->guest_ram_path, sizeof(s->hdr->guest_ram_path), "%s", path);
    s->hdr->guest_ram_bytes = bytes;
    RELEASE(&s->hdr->guest_ram_seq, seq + 1);
}

/* ---------------------------------------------------------- the presenter */

/* Open (or reopen) the guest RAM file the daemon named. 0, or -1 if there is
 * none yet or it cannot be opened. */
static int ram_open(struct vypr_shm *s)
{
    const uint32_t seq = ACQUIRE(&s->hdr->guest_ram_seq);
    if (seq & 1u) return -1;
    if (s->ram_fd >= 0 && seq == s->ram_seq) return 0;

    /* A different file: everything mapped out of the old one belongs to a VM
     * that is not running any more. */
    for (uint32_t i = 0; i < VYPR_MAX_SLOTS; i++) ring_unmap(&s->rings[i]);
    if (s->ram_fd >= 0) { close(s->ram_fd); s->ram_fd = -1; }

    char path[sizeof(s->hdr->guest_ram_path)];
    memcpy(path, s->hdr->guest_ram_path, sizeof(path));
    path[sizeof(path) - 1] = 0;
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (ACQUIRE(&s->hdr->guest_ram_seq) != seq || !path[0]) return -1;

    /*
     * Read-write, though nothing here ever writes through it. The Vulkan
     * presenter hands rings to the GPU by importing their host pointer, and
     * the driver refuses a read-only mapping. Read-only still works - frames
     * then go through a staging copy - so it is the fallback, not an error.
     */
    s->ram_fd = open(path, O_RDWR | O_CLOEXEC);
    if (s->ram_fd < 0) s->ram_fd = open(path, O_RDONLY | O_CLOEXEC);
    if (s->ram_fd < 0) {
        fprintf(stderr, "vypr: cannot open guest RAM at %s: %s\n", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(s->ram_fd, &st) < 0) { close(s->ram_fd); s->ram_fd = -1; return -1; }
    s->ram_bytes = (uint64_t)st.st_size;
    s->ram_seq   = seq;
    return 0;
}

/*
 * Map `slot`'s ring, or keep the mapping already there if it is still the same
 * ring. -2 means try again later (the daemon is mid-update, or there is no
 * guest RAM to map yet); -1 means the description is unusable.
 */
static int ring_map(struct vypr_shm *s, uint32_t slot, uint64_t window_id)
{
    struct vypr_slot *sl = &s->hdr->slots[slot];
    struct vypr_ring_map *m = &s->rings[slot];

    if (ram_open(s) < 0) return -2;

    const uint32_t seq0 = ACQUIRE(&sl->ring_seq);
    if (seq0 & 1u) return -2;
    if (m->base && m->ring_seq == seq0 && m->window_id == window_id) return 0;
    if (s->map_failed[slot] == seq0 + 1) return -1;

    const uint32_t run_count = sl->run_count;
    const uint64_t nonce     = sl->ring_nonce;
    const uint64_t pages     = sl->ring_pages;
    if (run_count == 0 || run_count > VYPR_RUNS_PER_SLOT ||
        pages == 0 || pages > VYPR_MAX_RING_PAGES)
        return ACQUIRE(&sl->ring_seq) == seq0 ? -1 : -2;

    struct vypr_ring_run *runs = malloc((size_t)run_count * sizeof(*runs));
    if (!runs) return -2;
    memcpy(runs, slot_runs(s, slot), (size_t)run_count * sizeof(*runs));
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
    if (ACQUIRE(&sl->ring_seq) != seq0) { free(runs); return -2; }

    ring_unmap(m);

    const uint64_t bytes = pages * VYPR_PAGE_BYTES;
    uint8_t *base = mmap(NULL, bytes, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) { free(runs); return -2; }

    const int prot = (fcntl(s->ram_fd, F_GETFL) & O_ACCMODE) == O_RDWR
                   ? PROT_READ | PROT_WRITE : PROT_READ;
    uint64_t at = 0;
    int bad = 0;
    for (uint32_t r = 0; r < run_count && !bad; r++) {
        const uint64_t off = runs[r].offset, len = runs[r].pages * VYPR_PAGE_BYTES;
        if (runs[r].pages == 0 || (off % VYPR_PAGE_BYTES) || off > s->ram_bytes ||
            len > s->ram_bytes - off || len > bytes - at) {
            bad = 1;
            break;
        }
        if (mmap(base + at, len, prot, MAP_SHARED | MAP_FIXED, s->ram_fd, (off_t)off)
                == MAP_FAILED) {
            const int e = errno;
            fprintf(stderr, "vypr: cannot map this window's ring (piece %u of %u): %s\n",
                    r + 1, run_count, strerror(e));
            if (e == ENOMEM)
                fprintf(stderr, "vypr: the guest's memory is scattered past vm.max_map_count; "
                                "raise it (sysctl vm.max_map_count=1048576)\n");
            bad = 1;
            break;
        }
        at += len;
    }
    free(runs);

    if (bad || at != bytes) {
        munmap(base, bytes);
        s->map_failed[slot] = seq0 + 1;
        return -1;
    }

    m->base      = base;
    m->bytes     = bytes;
    m->ring_seq  = seq0;
    m->nonce     = nonce;
    m->window_id = window_id;
    return 0;
}

uint32_t vypr_slot_state(struct vypr_shm *s, uint32_t slot, uint64_t window_id)
{
    if (slot >= VYPR_MAX_SLOTS) return (uint32_t)VYPR_SLOT_CLOSED;
    const struct vypr_slot *sl = &s->hdr->slots[slot];
    const uint32_t st = ACQUIRE(&sl->state);
    /* Freed, or already handed to some other window: either way, as far as
     * this window is concerned it is over. */
    if (st == VYPR_SLOT_FREE || sl->window_id != window_id) return (uint32_t)VYPR_SLOT_CLOSED;
    return st;
}

int vypr_shm_acquire(struct vypr_shm *s, uint32_t slot_index, uint64_t window_id,
                     uint32_t since, struct vypr_frame_view *out)
{
    if (slot_index >= VYPR_MAX_SLOTS) return -1;
    struct vypr_slot *slot = &s->hdr->slots[slot_index];
    if (ACQUIRE(&slot->state) != VYPR_SLOT_LIVE || slot->window_id != window_id) return -1;

    const int mapped = ring_map(s, slot_index, window_id);
    if (mapped < 0) return mapped;
    const struct vypr_ring_map *m = &s->rings[slot_index];

    /*
     * The guest writes this header once it has been told the ring checked out,
     * so for a moment after the slot goes live there is nothing here yet. Every
     * field is the guest's word, so each is held against what the daemon
     * decided before any of it is used for addressing.
     */
    const struct vypr_ring_header *rh = (const struct vypr_ring_header *)m->base;
    if (ACQUIRE(&rh->magic) != VYPR_RING_MAGIC) return -2;
    if (rh->version != VYPR_SHM_VERSION || rh->nonce != m->nonce ||
        rh->window_id != window_id || rh->slot != slot_index ||
        rh->epoch != slot->epoch ||
        rh->frame_offset != VYPR_RING_HEADER_BYTES ||
        rh->frame_bytes != slot->frame_bytes ||
        rh->max_width != slot->max_width || rh->max_height != slot->max_height ||
        rh->frame_stride != slot->frame_stride)
        return -2;
    if (vypr_ring_bytes(slot->frame_bytes) > m->bytes) return -1;

    /* Seqlock read. A torn record means the guest published mid-read, so retry;
     * a handful of attempts is plenty, since the guest's write window is a few
     * stores wide. */
    for (int attempt = 0; attempt < 8; attempt++) {
        uint32_t seq0 = ACQUIRE(&rh->pub.seq);
        if (seq0 & 1u) continue;

        struct vypr_publish p = rh->pub;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);

        if (ACQUIRE(&rh->pub.seq) != seq0) continue;

        if (p.serial == 0 || p.serial == since) return -2;

        /* Everything below is guest-supplied. A guest bug should drop a frame,
         * not walk the host off the end of the mapping. */
        if (p.index >= VYPR_RING_FRAMES) return -1;
        if (p.width == 0 || p.height == 0) return -1;
        if (p.width > slot->max_width || p.height > slot->max_height) return -1;
        if (p.stride < (uint64_t)p.width * 4) return -1;
        if ((uint64_t)p.stride * p.height > slot->frame_bytes) return -1;

        const uint64_t off = VYPR_RING_HEADER_BYTES + (uint64_t)p.index * slot->frame_bytes;

        out->pixels           = m->base + off;
        out->ring             = m->base;
        out->ring_bytes       = m->bytes;
        out->ring_id          = m->nonce;
        out->width            = p.width;
        out->height           = p.height;
        out->stride           = p.stride;
        out->serial           = p.serial;
        out->capture_qpc      = p.capture_qpc;
        out->capture_qpc_freq = p.capture_qpc_freq;
        out->flags            = p.flags;

        /*
         * Damage rectangles, each clamped to the frame.
         *
         * Everything here is guest-supplied, so a rectangle that ran off the
         * edge of the frame would walk a presenter past the end of the ring
         * buffer. A rect that does not fit is dropped rather than clamped: a
         * wrong rectangle paints the wrong pixels, which is worse than a frame
         * that is a touch less up to date. If that leaves the flag set with no
         * usable rects, the frame carries nothing and the presenter keeps what
         * it had - correct, just not advanced.
         */
        out->damage_count = 0;
        if (p.flags & VYPR_PUB_DAMAGE_RECTS) {
            uint32_t n = p.damage_count;
            if (n > VYPR_MAX_DAMAGE_RECTS) n = VYPR_MAX_DAMAGE_RECTS;
            for (uint32_t r = 0; r < n; r++) {
                struct vypr_rect d = p.damage[r];
                if (d.w == 0 || d.h == 0) continue;
                if (d.x >= p.width || d.y >= p.height) continue;
                if (d.w > p.width - d.x || d.h > p.height - d.y) continue;
                out->damage[out->damage_count++] = d;
            }
        }
        return 0;
    }
    return -2;
}
