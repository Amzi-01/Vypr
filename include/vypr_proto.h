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

    /*
     * A toast the guest raised, on its way to this desktop's own notifications.
     *
     * A notification is the one thing an application says that is not attached
     * to a window: it can arrive while the app is minimised, behind something
     * else, or not being streamed at all. Left in the guest it is invisible
     * unless you happen to be looking at the whole screen, which rather defeats
     * running one application as if it were native.
     */
    VYPR_MSG_NOTIFY           = 12,  /* vypr_msg_notify + utf8 app/title/body */

    /*
     * An image copied in the guest, on its way to this desktop's clipboard.
     *
     * Chunked, because a clipboard image is nothing like a clipboard string: a
     * screenshot of a 4K desktop is tens of megabytes and a single message
     * holds 64 KB. Same shape as a dragged file - one BEGIN, as many DATA as
     * it takes, one END - and for the same reason.
     *
     * The wire format is a BMP, which sounds old-fashioned and is exactly
     * right here: Windows already keeps clipboard images as a DIB, and a DIB
     * plus a fourteen-byte header is a BMP file. No encoder on either side, no
     * quality decision to make, nothing to get wrong.
     */
    VYPR_MSG_CLIP_IMAGE_BEGIN = 13,  /* vypr_msg_clip_image_begin */
    VYPR_MSG_CLIP_IMAGE_DATA  = 14,  /* vypr_msg_clip_image_data + raw bytes */
    VYPR_MSG_CLIP_IMAGE_END   = 15,  /* no payload */

    /*
     * The same three, going the other way: an image copied here, on its way to
     * the guest's clipboard. Separate ids rather than reusing the ones above,
     * because a message travelling in both directions on one link is a bounce
     * waiting to be written by accident.
     */
    VYPR_MSG_SET_CLIP_IMAGE_BEGIN = 84,  /* vypr_msg_clip_image_begin */
    VYPR_MSG_SET_CLIP_IMAGE_DATA  = 85,  /* vypr_msg_clip_image_data + bytes */
    VYPR_MSG_SET_CLIP_IMAGE_END   = 86,  /* no payload */

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

    /*
     * Clipboard text, UTF-8, no trailing NUL, sent whichever way it changed.
     *
     * Only text. Images and file lists are the two other things people expect
     * from a clipboard, and both want a negotiation this protocol does not
     * have - the far side has to say what formats it can take before megabytes
     * of bitmap are sent across on the chance it is wanted.
     */
    VYPR_MSG_CLIPBOARD        = 75,  /* utf8 text */
    VYPR_MSG_GAMEPAD          = 76,  /* vypr_msg_gamepad */

    /*
     * A file dragged from the Linux desktop onto a streamed window.
     *
     * The file is copied into the guest rather than pointed at. A host path
     * means nothing over there unless the optional shared folder happens to be
     * mounted and the file happens to be inside it; copying always works, and
     * what the Windows application opens is an ordinary local file.
     *
     * BEGIN names one file and gives its size, DATA carries it in chunks, and
     * END performs the drop once everything has arrived. Several BEGIN/DATA
     * runs may precede a single END - that is a drag holding several files.
     *
     * The chunks share the control channel with input, so they are sized to
     * pass quickly rather than to be efficient: a keystroke waits behind at
     * most one of them.
     */
    VYPR_MSG_DROP_BEGIN       = 77,  /* vypr_msg_drop_begin + utf8 file name */
    VYPR_MSG_DROP_DATA        = 78,  /* vypr_msg_drop_data + raw file bytes */
    VYPR_MSG_DROP_END         = 79,  /* vypr_msg_drop_end */

    /*
     * Forget which windows you have already told us about.
     *
     * The agent only announces a window once, which is right until the host
     * changes its mind about what it wants. A session told to watch for a new
     * title needs the windows that are already open re-offered against it,
     * and the alternative - restarting the agent to make it forget - throws
     * away every stream in flight to learn one name.
     */
    VYPR_MSG_RESCAN           = 80,  /* no payload */


    /* 128 and up are host-internal: they travel between vyprd and the per-window
     * clients over a unix socket and are never sent to the guest. Sharing the
     * framing means one reader implementation rather than two. */
    VYPR_MSG_CLIENT_HELLO     = 128, /* vypr_msg_window_id */
    VYPR_MSG_CLIENT_POPUP     = 129, /* vypr_msg_client_popup */
    VYPR_MSG_CLIENT_POPUP_END = 130, /* vypr_msg_window_id */
    VYPR_MSG_CLIENT_LOCK      = 131, /* vypr_msg_pointer_lock */
    VYPR_MSG_CLIENT_GEOM      = 132, /* vypr_msg_client_geom */
    VYPR_MSG_CLIENT_STATE     = 133, /* vypr_msg_window_state */
    VYPR_MSG_CLIENT_AUDIO     = 134, /* vypr_msg_audio + interleaved float */
    VYPR_MSG_CLIENT_CLIPBOARD = 135, /* utf8 text */

    /*
     * Another window title to watch for, sent to a running session.
     *
     * Launching a second application used to restart the daemon so it could be
     * given the new title on its command line - which killed the agent, and
     * with it every window already on screen, to learn one string.
     */
    VYPR_MSG_CLIENT_MATCH     = 136, /* utf8 title fragment */

    /*
     * Where the daemon put a clipboard image, as a path.
     *
     * A path rather than the bytes: the daemon has already assembled the whole
     * image to write it, and sending it on would mean a second chunked
     * protocol to say what the first one just said. Only one client is told -
     * the clipboard belongs to the desktop, not to a window, and every client
     * setting it would be the same work several times over.
     */
    VYPR_MSG_CLIENT_CLIPBOARD_IMAGE = 137  /* utf8 path to a BMP */
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

/*
 * One controller's state.
 *
 * The button bits and axis ranges are XInput's, not SDL's, because that is
 * what the guest has to produce and the mapping is better done once on the
 * host than in the agent: SDL already normalises every pad it knows about to
 * this layout, and it knows about far more of them than we would.
 */
enum {
    VYPR_PAD_DPAD_UP        = 0x0001,
    VYPR_PAD_DPAD_DOWN      = 0x0002,
    VYPR_PAD_DPAD_LEFT      = 0x0004,
    VYPR_PAD_DPAD_RIGHT     = 0x0008,
    VYPR_PAD_START          = 0x0010,
    VYPR_PAD_BACK           = 0x0020,
    VYPR_PAD_LEFT_THUMB     = 0x0040,
    VYPR_PAD_RIGHT_THUMB    = 0x0080,
    VYPR_PAD_LEFT_SHOULDER  = 0x0100,
    VYPR_PAD_RIGHT_SHOULDER = 0x0200,
    VYPR_PAD_GUIDE          = 0x0400,
    VYPR_PAD_A              = 0x1000,
    VYPR_PAD_B              = 0x2000,
    VYPR_PAD_X              = 0x4000,
    VYPR_PAD_Y              = 0x8000,
};

enum {
    VYPR_PAD_CONNECTED    = 1u << 0,
};

struct vypr_msg_gamepad {
    uint32_t index;          /* which pad; the guest plugs one target per index */
    uint32_t flags;          /* VYPR_PAD_CONNECTED, clear when it goes away */
    uint16_t buttons;
    uint8_t  left_trigger;   /* 0..255 */
    uint8_t  right_trigger;
    int16_t  lx, ly;         /* -32768..32767, y positive up, as XInput has it */
    int16_t  rx, ry;
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
/*
 * The primary monitor, offered as though it were a window.
 *
 * Chosen well outside the range Windows hands out for real HWNDs, so it cannot
 * collide with one. Everything downstream - matching, attaching, slots, input -
 * treats it exactly like any other window; only the agent's capture knows the
 * difference.
 */
#define VYPR_DESKTOP_WINDOW_ID  0xD35C709Full

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

/*
 * Dragging a file in.
 *
 * The chunk size is what is left of a message once the header is paid for,
 * rounded down to something tidy. The per-file ceiling is a guard against a
 * mis-sized length, not a policy: a drag is a deliberate act and the file is
 * whatever the user picked.
 */
#define VYPR_DROP_CHUNK      (48u * 1024u)
#define VYPR_DROP_MAX_BYTES  (8ull * 1024ull * 1024ull * 1024ull)
#define VYPR_DROP_MAX_FILES  256u

struct vypr_msg_drop_begin {
    uint64_t window_id;
    uint64_t bytes;              /* size of this one file */
    uint32_t name_bytes;         /* UTF-8 name follows; no directory part */
    uint32_t _pad;
};

struct vypr_msg_drop_data {
    uint64_t window_id;
    uint32_t bytes;              /* raw file bytes follow */
    uint32_t _pad;
};

struct vypr_msg_drop_end {
    uint64_t window_id;
    int32_t  x, y;               /* drop point, in captured-surface pixels */
    uint32_t cancelled;          /* non-zero: throw away what was staged */
    uint32_t _pad;
};

/*
 * Three UTF-8 strings follow the header, in this order and with no separators
 * or terminators: the application's name, the notification's title, its body.
 * Any of them may be empty - a toast is not obliged to fill them all in.
 */
#define VYPR_CLIP_IMAGE_MAX  (64u * 1024u * 1024u)

struct vypr_msg_clip_image_begin {
    uint64_t bytes;              /* total image bytes to follow */
    uint32_t _pad0;
    uint32_t _pad1;
};

struct vypr_msg_clip_image_data {
    uint32_t bytes;              /* raw image bytes follow */
    uint32_t _pad;
};

struct vypr_msg_notify {
    uint32_t app_bytes;
    uint32_t title_bytes;
    uint32_t body_bytes;
    uint32_t _pad;
};

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
