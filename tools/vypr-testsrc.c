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
 * powered off.
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

#include "shm.h"

static volatile sig_atomic_t stop = 0;
static void on_signal(int sig) { (void)sig; stop = 1; }

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* Create the backing file when it does not exist, so the host side can be
 * exercised without the VM's IVSHMEM device present. */
static int ensure_region(const char *path, size_t bytes)
{
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) { perror("open"); return -1; }
    struct stat st;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size < bytes) {
        if (ftruncate(fd, (off_t)bytes) < 0) { perror("ftruncate"); close(fd); return -1; }
    }
    close(fd);
    return 0;
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

int main(int argc, char **argv)
{
    const char *path = "/dev/shm/vypr-test";
    uint32_t w = 1280, h = 720, fps = 60;
    size_t region = 256u * 1024u * 1024u;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shm") && i + 1 < argc)        path = argv[++i];
        else if (!strcmp(argv[i], "--size") && i + 1 < argc)  { sscanf(argv[++i], "%ux%u", &w, &h); }
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc)   fps = (uint32_t)atoi(argv[++i]);
        else {
            fputs("usage: vypr-testsrc [--shm PATH] [--size WxH] [--fps N]\n", stderr);
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (ensure_region(path, region) < 0) return 1;

    struct vypr_shm shm;
    if (vypr_shm_open(&shm, path, 1) < 0) return 1;

    struct vypr_msg_attach at;
    if (vypr_shm_alloc(&shm, 0xdeadbeef, w, h, &at) < 0) return 1;

    struct vypr_slot *slot = &shm.hdr->slots[at.slot];
    printf("vypr-testsrc: slot %u, %ux%u, ring at +%" PRIu64 ", %.1f MiB/frame\n",
           at.slot, w, h, at.ring_offset, at.frame_bytes / 1048576.0);
    printf("present it with:  ./build/vypr-window --shm %s --slot %u --stats\n",
           path, at.slot);

    __atomic_store_n(&slot->state, (uint32_t)VYPR_SLOT_LIVE, __ATOMIC_RELEASE);

    uint8_t *ring = (uint8_t *)shm.base + slot->ring_offset;
    uint32_t serial = 0, index = 0;
    uint64_t period = 1000000000ull / (fps ? fps : 60);
    uint64_t next = now_ns();

    while (!stop) {
        uint8_t *buf = ring + (size_t)index * slot->frame_bytes;
        draw(buf, w, h, slot->frame_stride, serial);

        /* Publish. Odd seq marks the record unstable, the fields are written
         * inside that window, and the even store releases it. The host retries
         * on a torn read rather than locking. */
        uint32_t seq = slot->pub.seq;
        __atomic_store_n(&slot->pub.seq, seq + 1, __ATOMIC_RELAXED);
        __atomic_thread_fence(__ATOMIC_RELEASE);

        slot->pub.index            = index;
        slot->pub.serial           = ++serial;
        slot->pub.width            = w;
        slot->pub.height           = h;
        slot->pub.stride           = slot->frame_stride;
        slot->pub.capture_qpc      = now_ns();
        slot->pub.capture_qpc_freq = 1000000000ull;
        slot->pub.flags            = VYPR_PUB_DAMAGE_FULL;

        __atomic_thread_fence(__ATOMIC_RELEASE);
        __atomic_store_n(&slot->pub.seq, seq + 2, __ATOMIC_RELEASE);

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

    __atomic_store_n(&slot->state, (uint32_t)VYPR_SLOT_CLOSED, __ATOMIC_RELEASE);
    printf("\nvypr-testsrc: published %u frames\n", serial);
    vypr_shm_close(&shm);
    return 0;
}
