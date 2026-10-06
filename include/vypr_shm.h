/*
 * vypr_shm.h - the memory frames travel through between the Windows guest and
 *              the Linux host. Included verbatim by both sides, so it must stay
 *              free of platform headers and free of anything C++-only.
 *
 * Frames live in the guest's own RAM. The agent allocates each window's ring
 * as locked physical pages (Windows' AWE API, which reports the physical page
 * numbers it hands out) and tells the host which pages they are. QEMU keeps
 * guest RAM in a shared memory file, so the host maps exactly those pages out
 * of it. Nothing mediates access and no driver is involved on either side - a
 * frame still costs one memcpy in the guest, and no encode, no decode, no
 * network stack.
 *
 * This replaced an IVSHMEM device, which needed a third-party kernel driver in
 * the guest just to map a PCI BAR into user space.
 *
 * Ownership rules, which the whole design leans on:
 *
 *   - The GUEST owns ring memory. It allocates it, it frees it, and it is the
 *     only writer of pixel data and of each ring's header and publish record.
 *   - The HOST never writes guest memory, at all. Pages a dead agent left
 *     behind go straight back to Windows, which may hand them to anything, so
 *     a host store into one would corrupt some unrelated guest process. Every
 *     host-side decision - which slots exist, where each ring lives - is kept
 *     in a second, host-only region (struct vypr_shm_header below) and reaches
 *     the guest over the TCP control channel.
 *   - Nothing here is a security boundary. A hostile guest can publish
 *     whatever it likes into its own rings; it already has a passthrough GPU.
 *     The host checks what it reads so that a confused guest drops a frame
 *     rather than walking a presenter off the end of a mapping.
 */
#ifndef VYPR_SHM_H
#define VYPR_SHM_H

#include <stdint.h>

#define VYPR_SHM_MAGIC      0x52505956u  /* 'VYPR' little-endian: host region */
#define VYPR_RING_MAGIC     0x47525956u  /* 'VYRG': a guest ring's header page */

/*
 * Bumped to 3 when rings moved out of an IVSHMEM BAR and into guest RAM. The
 * slot table, the ring header and the attach handshake all changed shape, so a
 * host and guest that disagree must not talk; the version checks on both ends
 * are what stop them.
 */
#define VYPR_SHM_VERSION    3u

/* Slots are windows. Sixteen is far past what a person keeps open from one VM,
 * and keeping it fixed lets the header be a plain struct at a known offset. */
#define VYPR_MAX_SLOTS      16u

/* Ring depth per window. Three is the smallest depth where the producer can
 * write while the consumer reads and still have a spare to publish into. */
#define VYPR_RING_FRAMES    3u

/* Pixel formats. BGRA is what Windows.Graphics.Capture hands back, so it is
 * the only one the first version speaks. */
#define VYPR_FMT_BGRA8      1u

/* The page size both sides count in. x86 guests only ever hand out 4 KiB
 * pages through AWE, so this is a fact rather than a tunable. */
#define VYPR_PAGE_BYTES     4096u

/*
 * The largest ring, in pages: 512 MiB, three frames of a 7680x4320 window with
 * headroom. The host keeps one run per contiguous stretch of guest pages and
 * sizes its table for the worst case, where no two pages are adjacent - which
 * on a guest that has been running a while is close to what really happens.
 */
#define VYPR_MAX_RING_PAGES (512u * 256u)

enum vypr_slot_state {
    VYPR_SLOT_FREE     = 0,  /* host may allocate it */
    VYPR_SLOT_ARMED    = 1,  /* ATTACH sent; the guest's ring is not verified yet */
    VYPR_SLOT_LIVE     = 2,  /* ring verified and described; presenters may map it */
    VYPR_SLOT_CLOSED   = 3   /* window gone; any presenter still reading should stop */
};

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

/* ------------------------------------------------------------ guest-owned */

/*
 * What the agent writes into the first 32 bytes of every ring page, before it
 * tells the host where the pages are.
 *
 * The host knows a page by the guest's physical address and has to turn that
 * into an offset in QEMU's RAM file, which depends on how QEMU laid memory out
 * around the 4 GiB hole. Rather than guess, the host reads every page back
 * through its own translation and checks that each one says what it should: a
 * wrong translation, a stale page list or a page shared between two rings all
 * fail here, before anything is shown. The stamps are overwritten by the first
 * frames, which is fine - the agent does not write a frame until the host has
 * said it checked them.
 */
#define VYPR_PAGE_MAGIC 0x4547415052505956ull  /* 'VYPRPAGE' */

struct vypr_page_stamp {
    uint64_t magic;
    uint64_t nonce;    /* this ring's, so a page from an earlier ring fails */
    uint64_t index;    /* which page of the ring this is */
    uint64_t pfn;      /* the guest's own idea of where the page is */
};

/*
 * Page 0 of every ring, written only by the guest.
 *
 * The frames follow at `frame_offset`. The host has its own copy of every
 * geometry field in the slot table and only trusts the two to agree: the slot
 * table is what the host decided, this is what the guest says it is writing.
 */
#define VYPR_RING_HEADER_BYTES  VYPR_PAGE_BYTES

enum vypr_ring_state {
    VYPR_RING_READY  = 1,   /* header written, no frame yet */
    VYPR_RING_LIVE   = 2,   /* at least one frame published */
    VYPR_RING_CLOSED = 3    /* the capture is torn down; nothing more will come */
};

struct vypr_ring_header {
    uint32_t magic;              /* VYPR_RING_MAGIC, stored last */
    uint32_t version;            /* VYPR_SHM_VERSION */
    uint64_t nonce;              /* matches the page stamps the host checked */
    uint64_t window_id;
    uint32_t slot;
    uint32_t epoch;              /* the attach this ring answers */
    uint64_t frame_offset;       /* bytes from ring start to buffer 0 */
    uint64_t frame_bytes;        /* bytes per ring buffer */
    uint32_t max_width;
    uint32_t max_height;
    uint32_t frame_stride;
    uint32_t format;
    volatile uint32_t state;     /* enum vypr_ring_state */
    uint32_t _pad;
    struct vypr_publish pub;
};

/* -------------------------------------------------------------- host-only */

/*
 * One contiguous stretch of a ring, as an offset into the guest RAM file.
 *
 * Runs are in ring order: the first run's pages are the ring's first pages,
 * and so on, so laying them end to end gives the ring exactly as the guest
 * sees it in its own address space.
 */
struct vypr_ring_run {
    uint64_t offset;   /* bytes into the guest RAM file */
    uint64_t pages;
};

struct vypr_slot {
    volatile uint32_t state;     /* enum vypr_slot_state */
    uint32_t format;             /* VYPR_FMT_* */
    uint64_t window_id;          /* guest HWND, as an opaque identity */

    /* Ring geometry, decided by the host at allocation. `frame_stride` is the
     * allocation pitch, which does not shrink when a window is resized smaller
     * - that would mean reallocating mid-stream. */
    uint64_t frame_bytes;        /* bytes reserved per ring buffer */
    uint32_t max_width;
    uint32_t max_height;
    uint32_t frame_stride;

    /*
     * Incremented every time this slot index is handed out. The guest echoes
     * it in its ring header, so a ring left over from the slot's previous
     * occupant is told apart from the one this window asked for.
     */
    uint32_t epoch;

    /*
     * Where the guest put the ring, once vyprd has checked it.
     *
     * `ring_seq` is a seqlock over the fields below it and the run table, odd
     * while vyprd is rewriting them. A presenter that maps a ring reads it
     * before and after copying the runs out and starts again if it moved.
     */
    volatile uint32_t ring_seq;
    uint32_t run_count;
    uint64_t ring_nonce;
    uint64_t ring_pages;
    uint64_t runs_offset;        /* bytes from region start to this slot's runs */
};

/* Room in the host region for every slot's worst-case run table. Most of it is
 * never touched, and an untouched page of a tmpfs file costs nothing. */
#define VYPR_RUNS_PER_SLOT  VYPR_MAX_RING_PAGES

struct vypr_shm_header {
    uint32_t magic;
    uint32_t version;
    uint64_t region_bytes;
    uint32_t slot_count;
    uint32_t _pad;

    /* Bumped by vyprd every time it formats the region, so a presenter left
     * over from an earlier session can tell. */
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

    /*
     * Where the guest's RAM can be opened, for presenters to map rings out of.
     *
     * A path under /proc/<qemu pid>/fd: QEMU keeps guest RAM in an anonymous
     * memfd, and that is the only name it has. `guest_ram_seq` changes whenever
     * vyprd finds a different one - the VM restarted under a running session -
     * so a presenter holding the old file knows to let go of it.
     */
    volatile uint32_t guest_ram_seq;
    uint32_t _pad3;
    char     guest_ram_path[96];
    uint64_t guest_ram_bytes;

    struct vypr_slot slots[VYPR_MAX_SLOTS];
};

/* The run tables start after the header, rounded up to a page. */
#define VYPR_DATA_ALIGN     4096u
#define VYPR_HEADER_BYTES   ((sizeof(struct vypr_shm_header) + VYPR_DATA_ALIGN - 1) \
                             & ~(uint64_t)(VYPR_DATA_ALIGN - 1))
#define VYPR_HOST_REGION_BYTES \
    (VYPR_HEADER_BYTES + (uint64_t)VYPR_MAX_SLOTS * VYPR_RUNS_PER_SLOT * \
                         sizeof(struct vypr_ring_run))

#endif /* VYPR_SHM_H */
