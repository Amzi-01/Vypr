/*
 * vypr_proto.h - the control channel between host and guest agent.
 *
 * Control is a single TCP connection over the VM's virtual bridge. Pixels never
 * travel here; this carries window lifecycle, slot assignment, and input. On a
 * virtual bridge a round trip is tens of microseconds, which is well under the
 * frame budget input has to meet.
 *
 * One connection for the whole session, not one per window as the earlier
 * prototype did. Per-window connections meant window identity lived in the
 * socket, which made reconnect ambiguous: a re-attaching stream could not say
 * which HWND it used to be. Here identity is always an explicit window_id.
 *
 * Everything is little-endian. Both ends are x86-64 and one of them is Windows;
 * pretending otherwise would be ceremony.
 */
#ifndef VYPR_PROTO_H
#define VYPR_PROTO_H

#include <stdint.h>

#define VYPR_PROTO_VERSION   1u
#define VYPR_CONTROL_PORT    47820u
#define VYPR_MAX_MSG_BYTES   (64u * 1024u)

enum vypr_msg_type {
    /* guest -> host */
    VYPR_MSG_HELLO            = 1,   /* vypr_msg_hello */
    VYPR_MSG_WINDOW_ADDED     = 2,   /* vypr_msg_window + utf8 title */
    VYPR_MSG_WINDOW_REMOVED   = 3,   /* vypr_msg_window_id */
    VYPR_MSG_WINDOW_CHANGED   = 4,   /* vypr_msg_window + utf8 title */
    VYPR_MSG_ATTACH_RESULT    = 5,   /* vypr_msg_attach_result */
    VYPR_MSG_CURSOR           = 6,   /* vypr_msg_cursor */
    VYPR_MSG_LOG              = 7,   /* utf8 text */
    VYPR_MSG_PONG             = 8,   /* vypr_msg_pong */
    VYPR_MSG_AUDIO            = 10,  /* vypr_msg_audio + interleaved float */

    /*
     * First message on a second connection, saying it carries audio and
     * nothing else.
     *
     * Audio shared the control channel, which meant a queued input event or
     * window update sat in front of an audio packet and delayed it however
     * promptly TCP was configured to send - TCP_NODELAY does nothing about a
     * message already ahead of yours in the same stream. It also had three
     * threads writing one socket.
     *
     * Same port, so no second firewall rule: the connection announces itself
     * and the daemon sorts it from the control channel by what arrives first.
     */
    VYPR_MSG_AUDIO_HELLO      = 11,  /* no payload */
    VYPR_MSG_POINTER_LOCK     = 9,   /* vypr_msg_pointer_lock */

    /* host -> guest */
    VYPR_MSG_ATTACH           = 64,  /* vypr_msg_attach */
    VYPR_MSG_DETACH           = 65,  /* vypr_msg_window_id */
    VYPR_MSG_LAUNCH           = 66,  /* utf8 command line */
    VYPR_MSG_POINTER          = 67,  /* vypr_msg_pointer */
    VYPR_MSG_KEY              = 68,  /* vypr_msg_key */
    VYPR_MSG_TEXT             = 69,  /* vypr_msg_window_id + utf8 */
    VYPR_MSG_RESIZE           = 70,  /* vypr_msg_resize */
    VYPR_MSG_FOCUS            = 71,  /* vypr_msg_window_id */
    VYPR_MSG_CLOSE            = 72,  /* vypr_msg_window_id */
    VYPR_MSG_PING             = 73,  /* vypr_msg_ping */
    VYPR_MSG_WINDOW_STATE     = 74,  /* vypr_msg_window_state */

    /* 128 and up are host-internal: they travel between vyprd and the per-window
     * clients over a unix socket and are never sent to the guest. Sharing the
     * framing means one reader implementation rather than two. */
    VYPR_MSG_CLIENT_HELLO     = 128, /* vypr_msg_window_id */
    VYPR_MSG_CLIENT_POPUP     = 129, /* vypr_msg_client_popup */
    VYPR_MSG_CLIENT_POPUP_END = 130, /* vypr_msg_window_id */
    VYPR_MSG_CLIENT_LOCK      = 131, /* vypr_msg_pointer_lock */
    VYPR_MSG_CLIENT_GEOM      = 132, /* vypr_msg_client_geom */
    VYPR_MSG_CLIENT_STATE     = 133, /* vypr_msg_window_state */
    VYPR_MSG_CLIENT_AUDIO     = 134  /* vypr_msg_audio + interleaved float */
};

/*
 * A popup belonging to a window a client is already presenting.
 *
 * Popups are not their own host process, unlike top-level windows. A menu has
 * to be a real popup surface parented to its owner - Wayland's xdg_popup, via
 * SDL_CreatePopupWindow - and a surface can only be parented to one owned by
 * the same process. So the daemon hands the popup to the owner's client rather
 * than spawning another.
 *
 * `dx`/`dy` are relative to the owner's client-area origin, computed by the
 * daemon from the two windows' guest screen positions, because that is the one
 * place both are known.
 */
struct vypr_msg_client_popup {
    uint64_t window_id;
    uint64_t owner_id;
    uint32_t slot;
    uint32_t _pad;
    int32_t  dx, dy;
    uint32_t width, height;
};

/* Every message begins with this. `bytes` counts the payload only. */
struct vypr_msg_head {
    uint32_t bytes;
    uint16_t type;
    uint16_t flags;
};

struct vypr_msg_hello {
    uint32_t version;
    uint32_t _pad;
    uint64_t qpc_freq;           /* guest timer frequency, for latency maths */
    uint64_t shm_bytes;          /* size of the BAR the guest can see */
    uint32_t agent_pid;
    uint32_t capabilities;
};

#define VYPR_CAP_CURSOR_SHAPES   (1u << 0)
#define VYPR_CAP_AUDIO           (1u << 1)
#define VYPR_CAP_RESIZE          (1u << 2)

struct vypr_msg_window_id {
    uint64_t window_id;
};

/*
 * A window the guest is willing to stream. `owner_id` is what makes menus and
 * dialogs tractable: each is its own HWND and so its own stream, and the host
 * needs to know which window to position it against. Zero means top level.
 */
struct vypr_msg_window {
    uint64_t window_id;
    uint64_t owner_id;
    int32_t  x, y;               /* guest screen coords of the client area */
    uint32_t width, height;      /* client area size in physical pixels */
    uint32_t dpi;                /* 96 = 100%; the host scales against this */
    uint32_t pid;
    uint32_t flags;

    /* Height in captured pixels of the guest window's own title bar - the gap
     * between the top of the captured frame and the top of the client area.
     * The host presents undecorated, so this strip is the only thing the user
     * has to drag the window by, and it has to know where it is. */
    uint32_t chrome_top;

    uint32_t title_bytes;        /* utf8 title follows the struct */
};

#define VYPR_WIN_TOOL_WINDOW     (1u << 0)
#define VYPR_WIN_POPUP           (1u << 1)
#define VYPR_WIN_FULLSCREEN      (1u << 2)
#define VYPR_WIN_RESIZABLE       (1u << 3)
#define VYPR_WIN_MINIMIZED       (1u << 4)

/* Host assigns a slot and the ring geometry it carved for this window. The
 * guest may not publish a frame larger than max_width x max_height; if the
 * window grows past that it reports VYPR_MSG_WINDOW_CHANGED and waits for the
 * host to re-attach with a bigger ring. */
struct vypr_msg_attach {
    uint64_t window_id;
    uint32_t slot;
    uint32_t format;
    uint64_t ring_offset;
    uint64_t frame_bytes;
    uint32_t max_width;
    uint32_t max_height;
    uint32_t frame_stride;
    uint32_t generation;
};

struct vypr_msg_attach_result {
    uint64_t window_id;
    uint32_t slot;
    int32_t  status;             /* 0 = streaming, negative = errno-ish */
};

/*
 * Pointer state, sent absolute rather than as deltas. The host window is
 * resizable and the guest client area is not necessarily the same size, so the
 * host converts to guest client-area pixels before sending. Sending deltas
 * would push that conversion into the guest, where the host's window geometry
 * is not known.
 */
struct vypr_msg_pointer {
    uint64_t window_id;
    int32_t  x, y;               /* guest client-area pixels */
    uint32_t buttons;            /* bitmask, bit 0 = left */
    int32_t  wheel;              /* 120 units per detent, as Windows counts */
    int32_t  hwheel;
    uint32_t flags;
};

#define VYPR_PTR_RELATIVE        (1u << 0)  /* pointer-locked; x,y are deltas */

struct vypr_msg_key {
    uint64_t window_id;
    uint32_t scancode;           /* PS/2 set 1, which is what SendInput wants */
    uint32_t down;
    uint32_t modifiers;
    uint32_t _pad;
};

struct vypr_msg_resize {
    uint64_t window_id;
    uint32_t width, height;
};

/*
 * Clock alignment, so a frame's guest capture timestamp can be compared against
 * host time and turned into a latency figure.
 *
 * The guest stamps frames with QueryPerformanceCounter, which has no defined
 * relationship to the host's clock. This is the usual round-trip estimate: the
 * host notes when it sent the ping and when the pong came back, and assumes the
 * guest's reading was taken halfway between. Across a virtual bridge the round
 * trip is tens of microseconds, so the error is far below a frame.
 */
/*
 * The guest app has taken the pointer - it hid the cursor, or confined it with
 * ClipCursor, or both. That is what a game does when it wants raw motion: it
 * warps the cursor back to a fixed point every frame and reads the deltas.
 *
 * Absolute positioning fights that. Every absolute move the host sends becomes
 * a large bogus delta on top of the game's own warping, which is why an
 * uncorrected stream sends the view spinning and the pointer into a corner.
 * While locked, the host switches to relative motion and stops saying where the
 * pointer *is* at all.
 */
struct vypr_msg_pointer_lock {
    uint64_t window_id;
    uint32_t locked;
    uint32_t _pad;
};

/*
 * Geometry a window client needs to keep current.
 *
 * chrome_top decides which part of the window is title bar, and therefore which
 * presses are held for a possible drag rather than forwarded to the guest. Sent
 * once at spawn it goes stale the moment the window is maximised, restored, or
 * switches between windowed and fullscreen - and a stale value means drags get
 * forwarded and move the window inside the VM instead.
 */
/*
 * Minimised or not, kept the same on both sides.
 *
 * The guest's own minimise button is part of the captured image, so clicking it
 * minimises the window in the VM - and a minimised window stops producing
 * frames, leaving a live host window showing a picture that never changes.
 * Equally, minimising on the host without telling the guest leaves the guest
 * window up and rendering for nothing.
 *
 * So the state travels in both directions, and each side applies it only when
 * it differs from what it already has, which is what stops the two of them
 * bouncing it back and forth forever.
 */
struct vypr_msg_window_state {
    uint64_t window_id;
    uint32_t minimized;
    uint32_t fullscreen;   /* the guest window covers the guest desktop */
};

struct vypr_msg_client_geom {
    uint64_t window_id;
    uint32_t chrome_top;
    uint32_t _pad;
};

/*
 * A block of what the guest is playing, as interleaved 32-bit float.
 *
 * Audio goes over the control channel rather than the shared region. It is
 * tiny next to video - a tenth of a second of 48 kHz stereo is under 40 KB -
 * and it needs ordering and reliability far more than it needs the last
 * microsecond of latency, which is what TCP already provides.
 */
struct vypr_msg_audio {
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t _pad;
    uint32_t frames;
    uint32_t _pad2;
};

struct vypr_msg_ping {
    uint64_t token;              /* host monotonic ns, echoed back */
};

struct vypr_msg_pong {
    uint64_t token;
    uint64_t guest_qpc;
    uint64_t guest_qpc_freq;
};

struct vypr_msg_cursor {
    uint64_t window_id;
    int32_t  hotspot_x, hotspot_y;
    uint32_t width, height;
    uint32_t visible;
    uint32_t bitmap_bytes;       /* BGRA cursor image follows, may be zero */
};

#endif /* VYPR_PROTO_H */
