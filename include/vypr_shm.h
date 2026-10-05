/*
 * vypr_shm.h - layout of the shared-memory region between the Windows guest
 *              and the Linux host. Included verbatim by both sides, so it must
 *              stay free of platform headers and free of anything C++-only.
 *
 * The region is an IVSHMEM BAR: the same physical pages appear as a PCI device
 * to the guest and as a file under /dev/shm to the host. There is no kernel
 * mediating access, which is the point - a frame costs one memcpy and no
 * encode, no decode, and no network stack.
 *
 * Ownership rules, which the whole design leans on:
 *
 *   - The HOST owns allocation. It carves slots and rings out of the region and
 *     tells the guest the offsets over the TCP control channel. Neither side
 *     needs a shared allocator, and cross-OS atomic allocation is avoided
 *     entirely.
 *   - The GUEST owns frame contents. It is the only writer of pixel data and of
 *     each slot's publish record.
 *   - Nothing here is a security boundary. A hostile guest can scribble over
 *     the whole region; it already has a passthrough GPU.
 */
#ifndef VYPR_SHM_H
#define VYPR_SHM_H

#include <stdint.h>

#define VYPR_SHM_MAGIC      0x52505956u  /* 'VYPR' little-endian */

/*
 * Bumped to 2 when the publish record grew its damage rectangles. The whole
 * record is a plain struct at a fixed offset read on both sides, so a size
 * change is a wire break: a host and guest that disagree must not talk, and the
 * version check in vypr_shm_open / Publisher::bind is what stops them. An old
 * guest paired with a new host, or the reverse, fails that check and refuses,
 * rather than reading the damage array at the wrong offset.
 */
#define VYPR_SHM_VERSION    2u

/* Slots are windows. Sixteen is far past what a person keeps open from one VM,
 * and keeping it fixed lets the header be a plain struct at a known offset. */
#define VYPR_MAX_SLOTS      16u

/* Ring depth per window. Three is the smallest depth where the producer can
 * write while the consumer reads and still have a spare to publish into. */
#define VYPR_RING_FRAMES    3u

/* Pixel formats. BGRA is what Windows.Graphics.Capture hands back, so it is
 * the only one the first version speaks. */
#define VYPR_FMT_BGRA8      1u

enum vypr_slot_state {
    VYPR_SLOT_FREE     = 0,  /* host may allocate it */
    VYPR_SLOT_ARMED    = 1,  /* host allocated, guest has not published yet */
    VYPR_SLOT_LIVE     = 2,  /* guest is publishing frames */
    VYPR_SLOT_CLOSED   = 3,  /* guest window is gone; host reclaims */
    VYPR_SLOT_RETIRING = 4   /* host dropped the window, waiting for the guest */
};

/*
 * RETIRING is the handshake that lets shared memory be reused.
 *
 * A publisher writes a whole frame into the ring *before* it publishes it, so
 * "the host stopped asking for this window" is not the same as "nothing is
 * writing to it": a 4K copy in flight lands wherever the ring used to be, long
 * after the host decided the window was gone. Handing that range to a new
 * window right then is how one window's pixels turn up in another's.
 *
 * So the host stores RETIRING instead of FREE and waits. The guest tears the
 * capture down, which drains any copy in flight, and only then stores CLOSED -
 * that store is the acknowledgement, and the range is not reusable until it
 * lands. A slot never goes ARMED again straight from RETIRING.
 *
 * An old guest never writes 4 and never reads a state it did not expect, so
 * adding this state does not break one; the layout is unchanged.
 */

/*
 * Publish record. The guest writes pixels into ring buffer `index`, then
 * publishes here.
 *
 * `seq` is a seqlock counter, not a frame number: the guest increments it to an
 * odd value before touching the record, and to the next even value after. A
 * host that reads an odd seq, or a different seq before and after, saw a torn
 * record and drops that read. It costs two stores per frame and removes the
 * need for any lock across the VM boundary.
 */
/*
 * How many separate changed rectangles a partial frame can name.
 *
 * Eight is a deliberate ceiling, not a guess at how many regions change: it
 * keeps the publish record a small fixed struct, and the guest coalesces its
 * real damage down to at most this many before publishing. When the true
 * damage is more scattered than eight rectangles, the guest sends their
 * bounding box as a single rectangle instead - still far less than the whole
 * frame for the cases this exists for (a blinking caret, a moving cursor, one
 * updating panel), and never wrong, only less selective.
 */
#define VYPR_MAX_DAMAGE_RECTS 8u

/* A changed region, in pixels from the top-left of the frame. */
struct vypr_rect {
    uint32_t x, y, w, h;
};

struct vypr_publish {
    volatile uint32_t seq;       /* even = stable, odd = being written */
    uint32_t index;              /* which ring buffer holds the frame */
    uint32_t serial;             /* increments once per published frame */
    uint32_t width;              /* may change mid-stream when the window resizes */
    uint32_t height;
    uint32_t stride;             /* bytes per row; >= width * 4 */
    uint64_t capture_qpc;        /* guest QueryPerformanceCounter at capture */
    uint64_t capture_qpc_freq;   /* so the host can convert without asking */
    uint32_t flags;

    /*
     * Partial frames.
     *
     * With VYPR_PUB_DAMAGE_RECTS set, only the pixels inside damage[0 ..
     * damage_count) were written into this ring buffer, at their own
     * coordinates; everything else in the buffer is stale and must not be
     * read. The host keeps its own full copy of the window and paints just
     * these rectangles over it. This is the whole point of damage: a typed
     * character crosses the VM boundary as a few kilobytes instead of a whole
     * frame.
     *
     * With VYPR_PUB_DAMAGE_FULL set (the default, and always the first frame of
     * a session or after a resize), the entire buffer is valid and damage_count
     * is zero. A host that does not understand damage simply never sees the
     * rects flag, because the guest only sets it once the host has asked for it
     * over the control channel.
     */
    uint32_t damage_count;
    struct vypr_rect damage[VYPR_MAX_DAMAGE_RECTS];
};

#define VYPR_PUB_CURSOR_VISIBLE  (1u << 0)
#define VYPR_PUB_DAMAGE_FULL     (1u << 1)
#define VYPR_PUB_DAMAGE_RECTS    (1u << 2)

struct vypr_slot {
    volatile uint32_t state;     /* enum vypr_slot_state */
    uint32_t format;             /* VYPR_FMT_* */
    uint64_t window_id;          /* guest HWND, as an opaque identity */

    /* Ring geometry, written by the host at allocation and read-only to the
     * guest. `frame_stride` is the allocation pitch, which does not shrink when
     * a window is resized smaller - that would mean reallocating mid-stream. */
    uint64_t ring_offset;        /* byte offset from region base */
    uint64_t frame_bytes;        /* bytes reserved per ring buffer */
    uint32_t max_width;
    uint32_t max_height;
    uint32_t frame_stride;

    /*
     * Incremented by the host every time this slot index is handed out.
     *
     * Slot indices get recycled when a window closes, and the guest's publisher
     * for the previous occupant may still be running - its next publish would
     * otherwise promote the freshly armed slot to LIVE and scribble the old
     * window's pixels into the new one's ring. The publisher records the epoch
     * it bound at and stops the moment it stops matching.
     */
    uint32_t epoch;

    struct vypr_publish pub;
};

struct vypr_shm_header {
    uint32_t magic;
    uint32_t version;
    uint64_t region_bytes;
    uint32_t slot_count;
    uint32_t _pad;

    /* Bumped by the host every time it re-carves the region, so a guest that
     * reconnects can tell its cached offsets are stale. */
    volatile uint32_t generation;
    uint32_t _pad2;

    /*
     * Nanoseconds to add to a guest QPC reading, converted to ns, to express it
     * in the host's monotonic clock. Written by the daemon once the control
     * channel has measured it; `offset_valid` stays zero until then.
     *
     * It lives here rather than in the control protocol because the process
     * that needs it - the one presenting frames - is not the one that owns the
     * control channel.
     */
    int64_t  guest_offset_ns;
    uint32_t offset_valid;

    /* Round trip of the exchange this offset came from. The offset is only as
     * trustworthy as that number is small, so consumers can state their own
     * uncertainty instead of implying precision they do not have. */
    uint32_t offset_rtt_us;

    struct vypr_slot slots[VYPR_MAX_SLOTS];
};

/* Pixel data starts after the header, rounded up to a page so that ring buffers
 * are page-aligned and a memcpy out of one does not straddle needlessly. */
#define VYPR_DATA_ALIGN     4096u
#define VYPR_HEADER_BYTES   ((sizeof(struct vypr_shm_header) + VYPR_DATA_ALIGN - 1) \
                             & ~(uint64_t)(VYPR_DATA_ALIGN - 1))

#endif /* VYPR_SHM_H */
