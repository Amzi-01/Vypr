/*
 * vypr-testsrc - stands in for the guest agent.
 *
 * Publishes an animated pattern into a slot exactly the way the Windows agent
 * will: write pixels into the next ring buffer, then publish under the seqlock.
 * This is the reference for the guest's publish path - if the C++ agent and
 * this file ever disagree about ordering, this file is right, because the host
 * is verified against it.
 *
 * It also means the entire host side can be developed and proven with the VM
 * powered off. Guest RAM is a sparse file laid out like a q35 guest's (see
 * fake_ram.h), and the ring goes through the same page check vyprd does, so a
 * presenter reading it takes exactly the path it takes against a real VM.
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <inttypes.h>
#include <time.h>
#include <unistd.h>

#include "fake_ram.h"
#include "guest_ram.h"
#include "shm.h"

static volatile sig_atomic_t stop = 0;
static void on_signal(int sig) { (void)sig; stop = 1; }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void draw(uint8_t *dst, uint32_t w, uint32_t h, uint32_t stride, uint32_t frame)
{
    /* Moving bars plus a per-frame marker block. The marker makes a stalled or
     * repeated frame obvious on the host without instrumenting anything. */
    for (uint32_t y = 0; y < h; y++) {
        uint8_t *row = dst + (size_t)y * stride;
        for (uint32_t x = 0; x < w; x++) {
            uint8_t r = (uint8_t)((x + frame * 3) & 0xff);
            uint8_t g = (uint8_t)((y + frame) & 0xff);
            uint8_t b = (uint8_t)(((x ^ y) + frame * 2) & 0xff);
            row[x * 4 + 0] = b;
            row[x * 4 + 1] = g;
            row[x * 4 + 2] = r;
            row[x * 4 + 3] = 0xff;
        }
    }
    uint32_t bx = (frame * 7) % (w > 64 ? w - 64 : 1);
    for (uint32_t y = 0; y < 64 && y < h; y++) {
        uint8_t *row = dst + (size_t)y * stride;
        for (uint32_t x = bx; x < bx + 64 && x < w; x++) {
            row[x * 4 + 0] = 0xff; row[x * 4 + 1] = 0xff;
            row[x * 4 + 2] = 0xff; row[x * 4 + 3] = 0xff;
        }
    }
}

/* A static gradient background with a white block over it, drawn into one
 * rectangle only. Used by the damage mode to prove partial updates: the
 * background a damage rect repaints is identical to the keyframe's, so a clean
 * block should move over an unchanging background on the host, with no trail -
 * which only holds if the host is accumulating partial frames correctly. */
static void draw_block_rect(uint8_t *dst, uint32_t stride,
                            uint32_t rx, uint32_t ry, uint32_t rw, uint32_t rh,
                            uint32_t bx, uint32_t by, uint32_t bs)
{
    for (uint32_t y = ry; y < ry + rh; y++) {
        uint8_t *row = dst + (size_t)y * stride;
        for (uint32_t x = rx; x < rx + rw; x++) {
            const int in_block = (x >= bx && x < bx + bs && y >= by && y < by + bs);
            row[x * 4 + 0] = in_block ? 0xff : (uint8_t)(x & 0xff);
            row[x * 4 + 1] = in_block ? 0xff : (uint8_t)(y & 0xff);
            row[x * 4 + 2] = in_block ? 0xff : (uint8_t)((x ^ y) & 0xff);
            row[x * 4 + 3] = 0xff;
        }
    }
}

int main(int argc, char **argv)
{
    const char *path = "/dev/shm/vypr-test-host";
    const char *ram_path = "/dev/shm/vypr-test-ram";
    uint32_t w = 1280, h = 720, fps = 60;
    int damage = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shm") && i + 1 < argc)        path = argv[++i];
        else if (!strcmp(argv[i], "--guest-ram") && i + 1 < argc) ram_path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)  { sscanf(argv[++i], "%ux%u", &w, &h); }
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)   fps = (uint32_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--damage"))                damage = 1;
        else {
            fputs("usage: vypr-testsrc [--shm PATH] [--guest-ram PATH] [--size WxH] [--fps N] [--damage]\n", stderr);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    struct vypr_shm shm;
    if (vypr_shm_open(&shm, path, 1) < 0) return 1;

    const uint64_t window_id = 0xdeadbeef;
    struct vypr_msg_attach at;
    if (vypr_shm_alloc(&shm, window_id, w, h, &at) < 0) return 1;
    struct vypr_slot *slot = &shm.hdr->slots[at.slot];

    /* The guest's half: a ring of scattered pages, each stamped. */
    struct fake_ram ram;
    if (fake_ram_open(&ram, ram_path, 3ull << 30, 2ull << 30) < 0) return 1;
    const uint32_t pages = (uint32_t)(vypr_ring_bytes(at.frame_bytes) / VYPR_PAGE_BYTES);
    uint64_t *pfns = malloc((size_t)pages * sizeof(*pfns));
    uint8_t *ring_mem = pfns ? fake_ram_alloc(&ram, pages, pfns) : NULL;
    if (!ring_mem) { fputs("vypr-testsrc: out of fake guest RAM\n", stderr); return 1; }
    const uint64_t nonce = now_ns() ^ ((uint64_t)getpid() << 32);
    for (uint32_t i = 0; i < pages; i++) {
        struct vypr_page_stamp st = { VYPR_PAGE_MAGIC, nonce, i, pfns[i] };
        memcpy(ring_mem + (size_t)i * VYPR_PAGE_BYTES, &st, sizeof(st));
    }

    /* The daemon's half: check every page and describe the ring. */
    struct vypr_guest_ram gram;
    struct vypr_ring_run *runs = malloc((size_t)pages * sizeof(*runs));
    char why[256];
    if (!runs || vypr_guest_ram_open(&gram, ram_path, why, sizeof(why)) < 0) {
        fprintf(stderr, "vypr-testsrc: %s\n", why);
        return 1;
    }
    const int run_count = vypr_guest_ram_verify(&gram, pfns, pages, nonce, runs, pages,
                                                why, sizeof(why));
    if (run_count < 0) { fprintf(stderr, "vypr-testsrc: ring check failed: %s\n", why); return 1; }
    vypr_shm_set_guest_ram(&shm, ram_path, ram.bytes);
    vypr_shm_set_ring(&shm, at.slot, runs, (uint32_t)run_count, nonce, pages);

    printf("vypr-testsrc: slot %u, %ux%u, ring %.1f MiB in %d pieces, %.1f MiB/frame\n",
           at.slot, w, h, pages * (double)VYPR_PAGE_BYTES / 1048576.0, run_count,
           at.frame_bytes / 1048576.0);
    printf("present it with:  ./build/vypr-window --shm %s --slot %u --window-id %" PRIu64 " --stats\n",
           path, at.slot, window_id);

    /* This source stamps frames with the host's own monotonic clock, so the
     * guest-to-host offset the daemon normally measures is exactly zero.
     * Saying so lets the client compute frame age - without it every latency
     * figure in --stats reads 0.0, and the stand-in cannot be used to measure
     * the thing it most needs to measure. */
    shm.hdr->guest_offset_ns = 0;
    shm.hdr->offset_rtt_us   = 0;
    __atomic_store_n(&shm.hdr->offset_valid, 1u, __ATOMIC_RELEASE);

    /* The ring header, magic last - what a presenter waits for. */
    struct vypr_ring_header *rh = (struct vypr_ring_header *)ring_mem;
    memset(rh, 0, sizeof(*rh));
    rh->version      = VYPR_SHM_VERSION;
    rh->nonce        = nonce;
    rh->window_id    = window_id;
    rh->slot         = at.slot;
    rh->epoch        = at.generation;
    rh->frame_offset = VYPR_RING_HEADER_BYTES;
    rh->frame_bytes  = at.frame_bytes;
    rh->max_width    = at.max_width;
    rh->max_height   = at.max_height;
    rh->frame_stride = at.frame_stride;
    rh->format       = at.format;
    rh->state        = VYPR_RING_READY;
    __atomic_store_n(&rh->magic, VYPR_RING_MAGIC, __ATOMIC_RELEASE);

    uint8_t *ring = ring_mem + VYPR_RING_HEADER_BYTES;
    struct vypr_publish *pub = &rh->pub;
    uint32_t serial = 0, index = 0;
    uint64_t period = 1000000000ull / (fps ? fps : 60);
    uint64_t next = now_ns();

    const uint32_t bs = w > 128 ? 64 : 16;   /* block size */
    uint32_t prev_bx = 0, prev_by = 0;

    while (!stop) {
        uint8_t *buf = ring + (size_t)index * slot->frame_bytes;

        /* In damage mode, frame 0 (and every 120th) is a whole keyframe; the
         * rest write only the rectangle bounding the block's old and new
         * positions, and publish that one rectangle. Otherwise every frame is
         * whole, as before. */
        uint32_t dmg_x = 0, dmg_y = 0, dmg_w = 0, dmg_h = 0;
        int partial = damage && (serial % 120 != 0);

        if (partial) {
            const uint32_t span = w > bs ? w - bs : 1;
            uint32_t bx = (serial * 7) % span;
            uint32_t by = (h > bs) ? (serial * 3) % (h - bs) : 0;
            dmg_x = bx < prev_bx ? bx : prev_bx;
            dmg_y = by < prev_by ? by : prev_by;
            dmg_w = (bx > prev_bx ? bx - prev_bx : prev_bx - bx) + bs;
            dmg_h = (by > prev_by ? by - prev_by : prev_by - by) + bs;
            if (dmg_x + dmg_w > w) dmg_w = w - dmg_x;
            if (dmg_y + dmg_h > h) dmg_h = h - dmg_y;
            draw_block_rect(buf, slot->frame_stride, dmg_x, dmg_y, dmg_w, dmg_h, bx, by, bs);
            prev_bx = bx; prev_by = by;
        } else {
            draw(buf, w, h, slot->frame_stride, damage ? 0 : serial);
            if (damage) {
                /* The keyframe draws the whole static background; start the
                 * block at the origin so the first partial erases from there. */
                prev_bx = prev_by = 0;
            }
        }

        /* Publish. Odd seq marks the record unstable, the fields are written
         * inside that window, and the even store releases it. The host retries
         * on a torn read rather than locking. */
        uint32_t seq = pub->seq;
        __atomic_store_n(&pub->seq, seq + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);

        pub->index            = index;
        pub->serial           = ++serial;
        pub->width            = w;
        pub->height           = h;
        pub->stride           = slot->frame_stride;
        pub->capture_qpc      = now_ns();
        pub->capture_qpc_freq = 1000000000ull;
        if (partial) {
            pub->flags          = VYPR_PUB_DAMAGE_RECTS;
            pub->damage_count   = 1;
            pub->damage[0]      = (struct vypr_rect){ dmg_x, dmg_y, dmg_w, dmg_h };
        } else {
            pub->flags          = VYPR_PUB_DAMAGE_FULL;
            pub->damage_count   = 0;
        }

        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&pub->seq, seq + 2, __ATOMIC_RELEASE);

        index = (index + 1) % VYPR_RING_FRAMES;

        next += period;
        uint64_t t = now_ns();
        if (next > t) {
            struct timespec ts = { .tv_sec = (time_t)((next - t) / 1000000000ull),
                                   .tv_nsec = (long)((next - t) % 1000000000ull) };
            nanosleep(&ts, NULL);
        } else {
            next = t;  /* fell behind; do not try to catch up in a burst */
        }
    }

    vypr_shm_free(&shm, at.slot);
    rh->state = VYPR_RING_CLOSED;
    printf("\nvypr-testsrc: published %u frames\n", serial);
    fake_ram_free(&ram, ring_mem, pages, pfns);
    fake_ram_close(&ram);
    vypr_guest_ram_close(&gram);
    free(runs);
    free(pfns);
    vypr_shm_close(&shm);
    return 0;
}
