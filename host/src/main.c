/*
 * vypr-window - presents one shared-memory slot as one native window.
 *
 * One process per window. A crashed or wedged stream then takes down a single
 * window instead of the whole session, and the compositor treats each app as
 * the separate top-level it is meant to look like.
 */
#define _GNU_SOURCE
#include <SDL3/SDL.h>

#include <errno.h>
#include <time.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msg.h"
#include "present.h"
#include "shm.h"

#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct options {
    const char *shm_path;
    const char *title;
    const char *sock_path;   /* unix socket back to vyprd; NULL = no input path */
    const char *backend;     /* "gpu" or "render" */
    int         capture;     /* start with the pointer captured */
    uint32_t    chrome_top;  /* guest title-bar height, in guest pixels */
    uint64_t    window_id;
    uint32_t    slot;
    int         stats;
};

static void usage(void)
{
    fputs("usage: vypr-window --shm PATH --slot N [--title NAME] [--stats]\n"
          "                 [--sock PATH --window-id ID] [--present gpu|render]\n"
          "\nRun standalone it presents a slot. vyprd additionally passes --sock\n"
          "and --window-id, which is what turns input back on.\n", stderr);
}

/* Input goes back to vyprd rather than straight to the guest: one process owns
 * the link to the agent, so window identity and reconnect are decided in one
 * place. Returns -1 when there is no daemon, which is the standalone case. */
/*
 * Controllers, forwarded to the guest.
 *
 * SDL normalises every pad it recognises to the Xbox layout, which is the same
 * layout XInput reports and therefore the one the guest has to produce - so the
 * mapping is a table rather than a guess, and it happens here because SDL knows
 * about far more controllers than the agent ever would.
 */
#define VYPR_MAX_PADS 4

struct pad {
    SDL_Gamepad *pad;
    SDL_JoystickID id;
    struct vypr_msg_gamepad last;
    bool open;
};

static const struct { SDL_GamepadButton sdl; uint16_t bit; } pad_buttons[] = {
    { SDL_GAMEPAD_BUTTON_DPAD_UP,        VYPR_PAD_DPAD_UP        },
    { SDL_GAMEPAD_BUTTON_DPAD_DOWN,      VYPR_PAD_DPAD_DOWN      },
    { SDL_GAMEPAD_BUTTON_DPAD_LEFT,      VYPR_PAD_DPAD_LEFT      },
    { SDL_GAMEPAD_BUTTON_DPAD_RIGHT,     VYPR_PAD_DPAD_RIGHT     },
    { SDL_GAMEPAD_BUTTON_START,          VYPR_PAD_START          },
    { SDL_GAMEPAD_BUTTON_BACK,           VYPR_PAD_BACK           },
    { SDL_GAMEPAD_BUTTON_LEFT_STICK,     VYPR_PAD_LEFT_THUMB     },
    { SDL_GAMEPAD_BUTTON_RIGHT_STICK,    VYPR_PAD_RIGHT_THUMB    },
    { SDL_GAMEPAD_BUTTON_LEFT_SHOULDER,  VYPR_PAD_LEFT_SHOULDER  },
    { SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, VYPR_PAD_RIGHT_SHOULDER },
    { SDL_GAMEPAD_BUTTON_GUIDE,          VYPR_PAD_GUIDE          },
    { SDL_GAMEPAD_BUTTON_SOUTH,          VYPR_PAD_A              },
    { SDL_GAMEPAD_BUTTON_EAST,           VYPR_PAD_B              },
    { SDL_GAMEPAD_BUTTON_WEST,           VYPR_PAD_X              },
    { SDL_GAMEPAD_BUTTON_NORTH,          VYPR_PAD_Y              },
};

static void pads_open(struct pad *pads)
{
    int count = 0;
    SDL_JoystickID *ids = SDL_GetGamepads(&count);
    if (!ids) return;

    for (int i = 0; i < count && i < VYPR_MAX_PADS; i++) {
        if (pads[i].open) continue;
        SDL_Gamepad *g = SDL_OpenGamepad(ids[i]);
        if (!g) continue;
        pads[i].pad  = g;
        pads[i].id   = ids[i];
        pads[i].open = true;
        printf("vypr: controller %d: %s\n", i, SDL_GetGamepadName(g));
        fflush(stdout);
    }
    SDL_free(ids);
}

static void pads_close(struct pad *pads)
{
    for (int i = 0; i < VYPR_MAX_PADS; i++) {
        if (!pads[i].open) continue;
        SDL_CloseGamepad(pads[i].pad);
        pads[i].open = false;
    }
}

/*
 * Sent only when something changed. A controller reports continuously whether
 * or not it is being touched, and a packet per pad per frame is a packet per
 * pad per frame the guest has to wake up for and the link has to carry.
 */
static void pads_poll(struct pad *pads, int daemon_fd, bool focused)
{
    for (int i = 0; i < VYPR_MAX_PADS; i++) {
        if (!pads[i].open) continue;

        struct vypr_msg_gamepad m = {0};
        m.index = (uint32_t)i;

        /* Only the focused window drives the guest: two host windows both
         * forwarding the same physical pad would fight over it. */
        if (focused) {
            m.flags = VYPR_PAD_CONNECTED;
            for (size_t b = 0; b < sizeof(pad_buttons) / sizeof(pad_buttons[0]); b++)
                if (SDL_GetGamepadButton(pads[i].pad, pad_buttons[b].sdl))
                    m.buttons |= pad_buttons[b].bit;

            /* SDL gives triggers 0..32767; XInput wants 0..255. */
            m.left_trigger  = (uint8_t)(SDL_GetGamepadAxis(pads[i].pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) >> 7);
            m.right_trigger = (uint8_t)(SDL_GetGamepadAxis(pads[i].pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) >> 7);

            /* SDL's Y grows downwards, XInput's grows up. Negating -32768
             * would overflow, so it is clamped before it is flipped. */
            const Sint16 ly = SDL_GetGamepadAxis(pads[i].pad, SDL_GAMEPAD_AXIS_LEFTY);
            const Sint16 ry = SDL_GetGamepadAxis(pads[i].pad, SDL_GAMEPAD_AXIS_RIGHTY);
            m.lx = SDL_GetGamepadAxis(pads[i].pad, SDL_GAMEPAD_AXIS_LEFTX);
            m.rx = SDL_GetGamepadAxis(pads[i].pad, SDL_GAMEPAD_AXIS_RIGHTX);
            m.ly = (int16_t)-(ly == -32768 ? -32767 : ly);
            m.ry = (int16_t)-(ry == -32768 ? -32767 : ry);
        }

        if (memcmp(&m, &pads[i].last, sizeof m) == 0) continue;
        pads[i].last = m;
        if (daemon_fd >= 0) msg_send(daemon_fd, VYPR_MSG_GAMEPAD, &m, sizeof m);
    }
}

static int connect_daemon(const char *path, uint64_t window_id)
{
    if (!path) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "vypr: cannot reach vyprd at %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    struct vypr_msg_window_id hello = { .window_id = window_id };
    if (msg_send(fd, VYPR_MSG_CLIENT_HELLO, &hello, sizeof(hello)) < 0) {
        close(fd);
        return -1;
    }

    /*
     * Non-blocking, still. A reader thread owns receiving and waits in poll(),
     * so reads no longer need it - but the render loop sends pointer and key
     * events on this same socket, and a blocking send would stall presentation
     * whenever the daemon was slow to drain. That is the coupling the reader
     * thread exists to remove, so it must not be reintroduced in the other
     * direction.
     */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

/*
 * The link to vyprd is read by its own thread.
 *
 * It used to be read once per rendered frame, which quietly made audio a
 * function of presentation: a long frame - and fullscreen frames are the long
 * ones - left the audio device with nothing to play, so it ran dry and went
 * silent, and then the backlog that had gathered in the socket arrived all at
 * once, overshot the queue's high mark, and was dropped as a burst. The log
 * showed both halves of that oscillation at the same time: a queue reading zero
 * milliseconds next to thousands of dropped packets.
 *
 * Audio is fed to the device straight from this thread, which SDL permits -
 * audio streams are safe to use from any thread. Everything else is parked for
 * the main thread, because it opens windows and touches the compositor, and
 * neither is safe to do from here.
 */

struct byte_buf {
    uint8_t *p;
    size_t   len, cap;
};

static int bb_append(struct byte_buf *b, const void *data, size_t n)
{
    if (b->len + n > b->cap) {
        size_t want = b->cap ? b->cap * 2 : 8192;
        while (want < b->len + n) want *= 2;
        uint8_t *grown = realloc(b->p, want);
        if (!grown) return -1;
        b->p = grown;
        b->cap = want;
    }
    memcpy(b->p + b->len, data, n);
    b->len += n;
    return 0;
}

struct link {
    int fd;

    /* Audio, owned entirely by the reader thread. */
    SDL_AudioStream *audio;
    uint32_t         rate;
    uint16_t         channels;
    uint64_t         logged_at;
    uint64_t         dropped, taken;
    bool             draining;
    /* Temporary instrumentation: the reported queue depth is only the value at
     * the moment of reporting, which told us nothing about how it got there. */
    int              q_min, q_max;
    uint64_t         toggles;
    /* Arrival timing, to tell a bursty producer apart from a queue we are
     * mismanaging. The queue depth alone cannot distinguish them. */
    uint64_t         last_ns, max_gap_ns;
    uint64_t         run, max_run;

    /* Everything else, handed to the main thread. */
    pthread_mutex_t  lock;
    struct byte_buf  pending;

    atomic_int stop;
    atomic_int dead;
};

static void link_audio(struct link *l, const struct vypr_msg_head *head,
                       const uint8_t *payload)
{
    const struct vypr_msg_audio *a = (const void *)payload;
    const uint32_t want = a->frames * a->channels * sizeof(float);

    if (!a->channels || !a->sample_rate || head->bytes < sizeof(*a) + want)
        return;

    if (!l->audio || l->rate != a->sample_rate || l->channels != a->channels) {
        if (l->audio) SDL_DestroyAudioStream(l->audio);
        SDL_AudioSpec spec = {
            .format   = SDL_AUDIO_F32,
            .channels = (int)a->channels,
            .freq     = (int)a->sample_rate,
        };
        l->audio = SDL_OpenAudioDeviceStream(
            SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
        if (l->audio) {
            SDL_ResumeAudioStreamDevice(l->audio);
            l->rate = a->sample_rate;
            l->channels = a->channels;
            printf("vypr: audio %u Hz, %u channels\n", a->sample_rate, a->channels);
            fflush(stdout);
        } else {
            fprintf(stderr, "vypr: audio device: %s\n", SDL_GetError());
            return;
        }
    }

    /*
     * Hold the queue down by dropping what arrives while it is too deep, rather
     * than emptying it.
     *
     * Clearing is a hard silence of however much was queued - a quarter of a
     * second of nothing, which is what "the audio cuts out" sounds like.
     * Dropping instead loses the same audio in ten-millisecond pieces spread
     * over the time it takes to drain, which is close to inaudible.
     *
     * Drain to a low mark, then stop - do not simply drop whatever is above a
     * threshold. A bare threshold becomes the steady state: the queue settles
     * just beneath it and packets are shed continuously to hold it there.
     * Measured at 163-187ms queued with 7-25% of packets dropped, heard as
     * sound cutting out at random.
     */
    {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        const uint64_t now = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
        if (l->last_ns) {
            const uint64_t gap = now - l->last_ns;
            if (gap > l->max_gap_ns) l->max_gap_ns = gap;
            /* Back-to-back arrivals are a backlog being flushed, not a live
             * stream: at 48 kHz a packet is worth milliseconds of sound. */
            if (gap < 1000000ull) {
                l->run++;
                if (l->run > l->max_run) l->max_run = l->run;
            } else {
                l->run = 0;
            }
        }
        l->last_ns = now;
    }

    const int per_second = (int)(l->rate * l->channels * sizeof(float));
    const int queued = SDL_GetAudioStreamQueued(l->audio);
    const int high = per_second / 8;    /* 125ms */
    const int low  = per_second / 25;   /*  40ms */

    if (!l->q_max && !l->q_min) { l->q_min = queued; l->q_max = queued; }
    if (queued > l->q_max) l->q_max = queued;
    if (queued < l->q_min) l->q_min = queued;

    const bool was = l->draining;
    if (queued > high) l->draining = true;
    else if (queued <= low) l->draining = false;
    if (was != l->draining) l->toggles++;

    if (!l->draining) {
        SDL_PutAudioStreamData(l->audio, payload + sizeof(*a), (int)want);
        l->taken++;
    } else {
        l->dropped++;
    }

    /*
     * Reported unconditionally, not behind --stats. Dropping is the only thing
     * here that can be heard as audio cutting out, and it is otherwise
     * invisible: everything else in the path looks healthy while it happens.
     */
    if (SDL_GetTicks() - l->logged_at > 10000) {
        l->logged_at = SDL_GetTicks();
        printf("vypr: audio queue %.0f ms (min %.0f, max %.0f), "
               "%llu of %llu dropped, %llu cycles | arrivals: gap max %.0f ms, "
               "longest burst %llu packets\n",
               queued  * 1000.0 / per_second,
               l->q_min * 1000.0 / per_second,
               l->q_max * 1000.0 / per_second,
               (unsigned long long)l->dropped,
               (unsigned long long)(l->dropped + l->taken),
               (unsigned long long)(l->toggles / 2),
               l->max_gap_ns / 1e6,
               (unsigned long long)l->max_run);
        fflush(stdout);
        l->dropped = l->taken = 0;
        l->q_min = l->q_max = 0;
        l->toggles = 0;
        l->max_gap_ns = 0;
        l->max_run = 0;
    }
}

static void *link_thread(void *arg)
{
    struct link *l = arg;
    struct msg_reader rx = {0};

    while (!atomic_load(&l->stop)) {
        struct pollfd pfd = { .fd = l->fd, .events = POLLIN };
        int pr = poll(&pfd, 1, 100);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pr == 0) continue;

        if (msg_reader_fill(&rx, l->fd) < 0) {
            atomic_store(&l->dead, 1);
            break;
        }

        struct vypr_msg_head head;
        const uint8_t *payload;
        while (msg_reader_next(&rx, &head, &payload) == 1) {
            if (head.type == VYPR_MSG_CLIENT_AUDIO &&
                head.bytes >= sizeof(struct vypr_msg_audio)) {
                link_audio(l, &head, payload);
            } else {
                /* Parked whole - header and payload together - so the main
                 * thread can walk it with the same parser. */
                pthread_mutex_lock(&l->lock);
                if (bb_append(&l->pending, &head, sizeof(head)) == 0)
                    bb_append(&l->pending, payload, head.bytes);
                pthread_mutex_unlock(&l->lock);
            }
        }
    }

    msg_reader_free(&rx);
    return NULL;
}

static int parse_args(int argc, char **argv, struct options *o)
{
    o->shm_path  = "/dev/shm/vypr";
    o->title     = "vypr";
    o->sock_path = NULL;
    o->backend   = NULL;
    o->capture   = 0;
    o->chrome_top = 0;
    o->window_id = 0;
    o->slot      = 0;
    o->stats     = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shm") && i + 1 < argc)        o->shm_path = argv[++i];
        else if (!strcmp(argv[i], "--slot") && i + 1 < argc)  o->slot = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) o->title = argv[++i];
        else if (!strcmp(argv[i], "--sock") && i + 1 < argc)  o->sock_path = argv[++i];
        else if (!strcmp(argv[i], "--present") && i + 1 < argc) o->backend = argv[++i];
        else if (!strcmp(argv[i], "--chrome-top") && i + 1 < argc)
            o->chrome_top = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--capture"))    o->capture = 1;
        else if (!strcmp(argv[i], "--no-capture")) o->capture = 0;
        else if (!strcmp(argv[i], "--window-id") && i + 1 < argc)
            o->window_id = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--stats"))                 o->stats = 1;
        else { usage(); return -1; }
    }
    return 0;
}


/*
 * SDL scancodes are USB HID usage ids; SendInput wants PS/2 set 1. The two
 * disagree about more than numbering, so this is a table rather than a formula.
 *
 * Extended keys carry 0xE000 in the high byte. The agent turns that into
 * KEYEVENTF_EXTENDEDKEY, without which arrow keys arrive as their numpad twins
 * and right-alt behaves as left-alt.
 */
static uint32_t sdl_scancode_to_ps2(SDL_Scancode sc)
{
    static const uint32_t map[SDL_SCANCODE_COUNT] = {
        [SDL_SCANCODE_A] = 0x1E, [SDL_SCANCODE_B] = 0x30, [SDL_SCANCODE_C] = 0x2E,
        [SDL_SCANCODE_D] = 0x20, [SDL_SCANCODE_E] = 0x12, [SDL_SCANCODE_F] = 0x21,
        [SDL_SCANCODE_G] = 0x22, [SDL_SCANCODE_H] = 0x23, [SDL_SCANCODE_I] = 0x17,
        [SDL_SCANCODE_J] = 0x24, [SDL_SCANCODE_K] = 0x25, [SDL_SCANCODE_L] = 0x26,
        [SDL_SCANCODE_M] = 0x32, [SDL_SCANCODE_N] = 0x31, [SDL_SCANCODE_O] = 0x18,
        [SDL_SCANCODE_P] = 0x19, [SDL_SCANCODE_Q] = 0x10, [SDL_SCANCODE_R] = 0x13,
        [SDL_SCANCODE_S] = 0x1F, [SDL_SCANCODE_T] = 0x14, [SDL_SCANCODE_U] = 0x16,
        [SDL_SCANCODE_V] = 0x2F, [SDL_SCANCODE_W] = 0x11, [SDL_SCANCODE_X] = 0x2D,
        [SDL_SCANCODE_Y] = 0x15, [SDL_SCANCODE_Z] = 0x2C,

        [SDL_SCANCODE_1] = 0x02, [SDL_SCANCODE_2] = 0x03, [SDL_SCANCODE_3] = 0x04,
        [SDL_SCANCODE_4] = 0x05, [SDL_SCANCODE_5] = 0x06, [SDL_SCANCODE_6] = 0x07,
        [SDL_SCANCODE_7] = 0x08, [SDL_SCANCODE_8] = 0x09, [SDL_SCANCODE_9] = 0x0A,
        [SDL_SCANCODE_0] = 0x0B,

        [SDL_SCANCODE_RETURN] = 0x1C, [SDL_SCANCODE_ESCAPE]    = 0x01,
        [SDL_SCANCODE_BACKSPACE] = 0x0E, [SDL_SCANCODE_TAB]    = 0x0F,
        [SDL_SCANCODE_SPACE]  = 0x39, [SDL_SCANCODE_MINUS]     = 0x0C,
        [SDL_SCANCODE_EQUALS] = 0x0D, [SDL_SCANCODE_LEFTBRACKET]  = 0x1A,
        [SDL_SCANCODE_RIGHTBRACKET] = 0x1B, [SDL_SCANCODE_BACKSLASH] = 0x2B,
        [SDL_SCANCODE_SEMICOLON] = 0x27, [SDL_SCANCODE_APOSTROPHE] = 0x28,
        [SDL_SCANCODE_GRAVE]  = 0x29, [SDL_SCANCODE_COMMA]     = 0x33,
        [SDL_SCANCODE_PERIOD] = 0x34, [SDL_SCANCODE_SLASH]     = 0x35,
        [SDL_SCANCODE_CAPSLOCK] = 0x3A,

        [SDL_SCANCODE_F1] = 0x3B, [SDL_SCANCODE_F2] = 0x3C, [SDL_SCANCODE_F3] = 0x3D,
        [SDL_SCANCODE_F4] = 0x3E, [SDL_SCANCODE_F5] = 0x3F, [SDL_SCANCODE_F6] = 0x40,
        [SDL_SCANCODE_F7] = 0x41, [SDL_SCANCODE_F8] = 0x42, [SDL_SCANCODE_F9] = 0x43,
        [SDL_SCANCODE_F10] = 0x44, [SDL_SCANCODE_F11] = 0x57, [SDL_SCANCODE_F12] = 0x58,

        [SDL_SCANCODE_PRINTSCREEN] = 0xE037, [SDL_SCANCODE_SCROLLLOCK] = 0x46,
        [SDL_SCANCODE_PAUSE]  = 0x45,
        [SDL_SCANCODE_INSERT] = 0xE052, [SDL_SCANCODE_HOME]     = 0xE047,
        [SDL_SCANCODE_PAGEUP] = 0xE049, [SDL_SCANCODE_DELETE]   = 0xE053,
        [SDL_SCANCODE_END]    = 0xE04F, [SDL_SCANCODE_PAGEDOWN] = 0xE051,
        [SDL_SCANCODE_RIGHT]  = 0xE04D, [SDL_SCANCODE_LEFT]     = 0xE04B,
        [SDL_SCANCODE_DOWN]   = 0xE050, [SDL_SCANCODE_UP]       = 0xE048,

        [SDL_SCANCODE_NUMLOCKCLEAR] = 0x45, [SDL_SCANCODE_KP_DIVIDE] = 0xE035,
        [SDL_SCANCODE_KP_MULTIPLY]  = 0x37, [SDL_SCANCODE_KP_MINUS]  = 0x4A,
        [SDL_SCANCODE_KP_PLUS]      = 0x4E, [SDL_SCANCODE_KP_ENTER]  = 0xE01C,
        [SDL_SCANCODE_KP_1] = 0x4F, [SDL_SCANCODE_KP_2] = 0x50, [SDL_SCANCODE_KP_3] = 0x51,
        [SDL_SCANCODE_KP_4] = 0x4B, [SDL_SCANCODE_KP_5] = 0x4C, [SDL_SCANCODE_KP_6] = 0x4D,
        [SDL_SCANCODE_KP_7] = 0x47, [SDL_SCANCODE_KP_8] = 0x48, [SDL_SCANCODE_KP_9] = 0x49,
        [SDL_SCANCODE_KP_0] = 0x52, [SDL_SCANCODE_KP_PERIOD] = 0x53,

        [SDL_SCANCODE_LCTRL]  = 0x1D,   [SDL_SCANCODE_LSHIFT] = 0x2A,
        [SDL_SCANCODE_LALT]   = 0x38,   [SDL_SCANCODE_LGUI]   = 0xE05B,
        [SDL_SCANCODE_RCTRL]  = 0xE01D, [SDL_SCANCODE_RSHIFT] = 0x36,
        [SDL_SCANCODE_RALT]   = 0xE038, [SDL_SCANCODE_RGUI]   = 0xE05C,
    };

    if (sc < 0 || sc >= SDL_SCANCODE_COUNT) return 0;
    return map[sc];
}

/*
 * One presented window. The process owns a top-level plus whatever popups its
 * guest window spawns: a popup surface must be parented to its owner, and only
 * the process holding the owner's surface can do that.
 */
enum { MAX_VIEWS = 12 };

struct view {
    uint64_t          window_id;
    uint32_t          slot;
    SDL_Window       *win;
    struct presenter *pres;
    uint32_t          src_w, src_h;
    uint32_t          last_serial;
    bool              is_popup;
};

/* Relative capture is a request to the compositor, not a guarantee - it can be
 * refused, and silently sending deltas afterwards looks like broken input. */
static void set_capture(SDL_Window *win, bool want, bool announce)
{
    if (!SDL_SetWindowRelativeMouseMode(win, want)) {
        fprintf(stderr, "vypr: relative mouse mode %s refused: %s\n",
                want ? "on" : "off", SDL_GetError());
        return;
    }
    if (announce) {
        printf("vypr: mouse capture %s\n",
               want ? "ON - relative motion" : "OFF - absolute");
        fflush(stdout);
    }
}

/*
 * Which parts of the window behave like a title bar.
 *
 * A client cannot place its own window on Wayland - the compositor owns
 * position, xdg_toplevel has no request to set one, and SDL_SetWindowPosition
 * is simply ignored. Moving a window has to be an *interactive move* the
 * compositor performs, which is what a hit test asks for.
 *
 * The guest's caption buttons live at the top right, so that corner stays
 * ordinary and its clicks are forwarded - close, minimise and maximise keep
 * working. The rest of the strip is draggable and the compositor moves the
 * window, with snapping and everything else it normally does.
 */
struct hit_ctx {
    uint32_t chrome_top;   /* guest pixels */
    uint32_t src_w, src_h; /* guest pixels, to scale into window pixels */
    bool     captured;     /* a captured game has no title bar to grab */
};

static SDL_HitTestResult SDLCALL title_hit_test(SDL_Window *win,
                                                const SDL_Point *pt, void *data)
{
    const struct hit_ctx *ctx = data;
    if (!ctx) return SDL_HITTEST_NORMAL;

    /*
     * A window that draws its own chrome reports no title bar - FiveM's
     * launcher and splash both do - and would otherwise have nothing to drag
     * by at all. Give it a strip the size of an ordinary title bar, but only
     * while the pointer is free: a captured game must not lose a band of
     * clicks across its top.
     */
    uint32_t chrome = ctx->chrome_top;
    if (chrome == 0) {
        if (ctx->captured) return SDL_HITTEST_NORMAL;
        chrome = 32;
    }

    int w = 0, h = 0;
    SDL_GetWindowSize(win, &w, &h);
    if (h <= 0 || ctx->src_h == 0) return SDL_HITTEST_NORMAL;

    const float scale_y = (float)h / (float)ctx->src_h;
    const float strip = chrome * scale_y;
    if (pt->y >= strip) return SDL_HITTEST_NORMAL;

    /*
     * Leave the caption buttons alone so their clicks are forwarded.
     *
     * Scaled horizontally, which is not the same thing as vertically: a window
     * whose aspect differs from the guest's - which is any window the user has
     * resized - would otherwise get a button zone of the wrong width, and
     * clicks meant for close or minimise would be swallowed as a drag.
     *
     * Windows draws three buttons at roughly 1.5x the caption height each, and
     * a little margin beyond costs nothing: past the buttons there is only
     * title bar, which can still be dragged from the left.
     */
    if (ctx->src_w == 0) return SDL_HITTEST_DRAGGABLE;
    const float scale_x = (float)w / (float)ctx->src_w;
    const float buttons = chrome * 5.0f * scale_x;
    if (pt->x > w - buttons) return SDL_HITTEST_NORMAL;

    return SDL_HITTEST_DRAGGABLE;
}

/*
 * Pointer motion, gathered up and sent once a frame.
 *
 * A high-polling-rate mouse reports around a thousand times a second. Sending
 * each one meant a thousand messages a second across the link and a thousand
 * SendInput calls in the guest, on the same CPU that is capturing frames and
 * running the game - which is why the stream turned choppy the moment the
 * window took focus and went smooth again as soon as it lost it.
 *
 * Nothing is lost by combining them: relative deltas sum exactly, and for
 * absolute positioning only the latest one was ever going to matter. Buttons
 * and the wheel are sent the moment they happen, since a click that waits for
 * the next frame is a click that feels late.
 */
struct pointer_accum {
    bool     pending;
    int32_t  x, y;
    uint32_t buttons;
    int32_t  wheel, hwheel;
    uint32_t flags;
};

static void pointer_flush(int fd, uint64_t window_id, struct pointer_accum *a)
{
    if (!a->pending || fd < 0) return;

    struct vypr_msg_pointer msg = {0};
    msg.window_id = window_id;
    msg.x       = a->x;
    msg.y       = a->y;
    msg.buttons = a->buttons;
    msg.wheel   = a->wheel;
    msg.hwheel  = a->hwheel;
    msg.flags   = a->flags;
    msg_send(fd, VYPR_MSG_POINTER, &msg, sizeof(msg));

    a->pending = false;
    a->wheel = a->hwheel = 0;
    if (a->flags & VYPR_PTR_RELATIVE) { a->x = 0; a->y = 0; }
}

static bool capture_forced_initial(const struct options *o) { return o->capture != 0; }

static struct view *view_for_sdl_id(struct view *views, int count, SDL_WindowID id)
{
    for (int i = 0; i < count; i++)
        if (views[i].win && SDL_GetWindowID(views[i].win) == id)
            return &views[i];
    return NULL;
}

/* Host window pixel -> guest client-area pixel. The host window is freely
 * resizable while the guest client area is whatever the guest app decided, so
 * every pointer event has to be converted before it means anything over there. */
static void to_guest_coords(int win_w, int win_h, uint32_t src_w, uint32_t src_h,
                            float hx, float hy, int32_t *gx, int32_t *gy)
{
    if (win_w <= 0 || win_h <= 0) { *gx = *gy = 0; return; }
    *gx = (int32_t)(hx * (float)src_w / (float)win_w);
    *gy = (int32_t)(hy * (float)src_h / (float)win_h);
    if (*gx < 0) *gx = 0;
    if (*gy < 0) *gy = 0;
    if (*gx >= (int32_t)src_w) *gx = (int32_t)src_w - 1;
    if (*gy >= (int32_t)src_h) *gy = (int32_t)src_h - 1;
}

/* A popup the daemon handed us: a real popup surface anchored to the owner's
 * window, so the compositor treats it as a menu rather than a floating
 * top-level - it stacks correctly and does not take focus. */
static bool view_open_popup(struct view *views, int *count, struct vypr_shm *shm,
                            const struct vypr_msg_client_popup *msg,
                            const char *backend)
{
    if (*count >= MAX_VIEWS) return false;
    for (int i = 0; i < *count; i++)
        if (views[i].window_id == msg->window_id) return true;   /* already open */

    struct view *owner = NULL;
    for (int i = 0; i < *count; i++)
        if (views[i].window_id == msg->owner_id) { owner = &views[i]; break; }
    if (!owner || !owner->win) return false;

    struct view *v = &views[*count];
    memset(v, 0, sizeof(*v));
    v->window_id = msg->window_id;
    v->slot      = msg->slot;
    v->is_popup  = true;

    v->win = SDL_CreatePopupWindow(owner->win, msg->dx, msg->dy,
                                   (int)msg->width, (int)msg->height,
                                   SDL_WINDOW_POPUP_MENU);
    if (!v->win) {
        fprintf(stderr, "vypr: SDL_CreatePopupWindow: %s\n", SDL_GetError());
        return false;
    }

    v->pres = presenter_create(v->win, backend, owner->pres);
    if (!v->pres) {
        SDL_DestroyWindow(v->win);
        v->win = NULL;
        return false;
    }

    (*count)++;
    return true;
}

static void view_close(struct view *views, int *count, uint64_t window_id)
{
    for (int i = 0; i < *count; i++) {
        if (views[i].window_id != window_id) continue;
        if (views[i].pres) presenter_destroy(views[i].pres);
        if (views[i].win)  SDL_DestroyWindow(views[i].win);
        /* Compact, so the main view stays at index 0. */
        for (int j = i; j + 1 < *count; j++) views[j] = views[j + 1];
        (*count)--;
        return;
    }
}

/*
 * Report what SDL can see and exit.
 *
 * Vypr can only forward a controller the host has. A pad that is not paired,
 * or is held exclusively by something else, is invisible here and no amount of
 * guest-side work changes that - so it is worth being able to ask directly
 * rather than inferring it from a game not responding.
 */
static int list_pads(void)
{
    if (!SDL_Init(SDL_INIT_GAMEPAD)) {
        printf("SDL could not start its gamepad subsystem: %s\n", SDL_GetError());
        return 1;
    }

    int nj = 0;
    SDL_JoystickID *js = SDL_GetJoysticks(&nj);
    if (nj == 0) printf("no controllers\n");

    for (int i = 0; i < nj; i++) {
        const char *name = SDL_GetJoystickNameForID(js[i]);
        if (!name) name = "(unnamed)";
        if (SDL_IsGamepad(js[i]))
            printf("%s\n", name);
        else
            /* SDL sees the device but has no button layout for it, so it
             * cannot be mapped onto the Xbox layout the guest expects. */
            printf("%s - no mapping, so it cannot be forwarded\n", name);
    }
    SDL_free(js);
    SDL_Quit();
    return 0;
}

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--list-pads")) return list_pads();

    struct options opt;
    if (parse_args(argc, argv, &opt) < 0) return 2;

    struct vypr_shm shm;
    if (vypr_shm_open(&shm, opt.shm_path, 0) < 0) return 1;

    const int daemon_fd = connect_daemon(opt.sock_path, opt.window_id);

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) {
        fprintf(stderr, "vypr: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* Start at the slot's maximum and resize down once a frame says otherwise;
     * opening at the wrong size and snapping is uglier than one late resize. */
    const struct vypr_slot *slot = &shm.hdr->slots[opt.slot];
    int win_w = (int)(slot->max_width  ? slot->max_width  : 1280);
    int win_h = (int)(slot->max_height ? slot->max_height : 720);

    struct view views[MAX_VIEWS] = {0};
    int view_count = 1;

    views[0].window_id = opt.window_id;
    views[0].slot      = opt.slot;
    /*
     * Borderless: the captured image already contains the guest window's own
     * title bar and buttons, and a compositor decoration around it would be a
     * second set of chrome for one window. Undecorated, what the user sees and
     * clicks is Windows' own title bar - close, minimise and maximise reach the
     * guest app because the input goes straight through.
     */
    views[0].win = SDL_CreateWindow(opt.title, win_w, win_h,
                                    SDL_WINDOW_RESIZABLE | SDL_WINDOW_BORDERLESS);
    if (!views[0].win) {
        fprintf(stderr, "vypr: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    views[0].pres = presenter_create(views[0].win, opt.backend, NULL);
    if (!views[0].pres) { fprintf(stderr, "vypr: no usable present backend\n"); return 1; }

    if (opt.stats)
        printf("vypr: present backend '%s'\n", presenter_name(views[0].pres));

    if (capture_forced_initial(&opt))
        set_capture(views[0].win, true, true);

    struct link link = {0};
    link.fd = daemon_fd;
    pthread_mutex_init(&link.lock, NULL);
    pthread_t link_tid = 0;
    bool link_running = false;
    if (daemon_fd >= 0) {
        if (pthread_create(&link_tid, NULL, link_thread, &link) == 0)
            link_running = true;
        else
            fprintf(stderr, "vypr: could not start the link reader thread\n");
    }
    struct pad pads[VYPR_MAX_PADS] = {0};
    pads_open(pads);

    uint8_t *parked = NULL;
    size_t   parked_cap = 0;
    /* The last clipboard text we set or sent, to tell an echo from a change. */
    char    *clip_last = NULL;

    /* Relative pointer mode, driven by the guest telling us an app has taken
     * the pointer. `suspended` is the user's override: a captured pointer must
     * always be escapable from the host side, whatever the guest thinks. */
    bool pointer_locked = false;
    /* Forced by the user with Ctrl+Alt. The guest's own detection - cursor
     * hidden or clipped - misses cases like a fullscreen game that leaves the
     * cursor nominally visible, so there has to be a way to say "capture"
     * that does not depend on guessing. */
    bool capture_forced = opt.capture != 0;

    /* Height of the guest window's own title bar, kept in step with the window
     * - it changes when a window is maximised, restored or goes fullscreen. */
    uint32_t chrome_reported = opt.chrome_top;

    /* Kept current for the hit test, which decides what is title bar. */
    struct hit_ctx hit = { opt.chrome_top, 0, 0, false };
    SDL_SetWindowHitTest(views[0].win, title_hit_test, &hit);

    /*
     * Audio, opened on the first block that arrives rather than up front: the
     * guest's mix format is whatever its endpoint happens to use, and asking
     * for a rate the guest is not producing would mean resampling for no
     * reason. SDL converts if the device disagrees.
     */

    struct pointer_accum pointer = {0};

    /*
     * Resizing is a negotiation, and both ends were talking at once.
     *
     * The host asked the guest to resize on every intermediate size as the
     * window was dragged, and adopted each frame size that came back - so the
     * user's drag, the guest's resize and the host's adoption all fought,
     * which looks like the window juddering and snapping about.
     *
     * The request is now sent once the size has settled, and the frame size is
     * not adopted while a resize is still in progress.
     */
    uint32_t resize_w = 0, resize_h = 0;
    uint64_t resize_at = 0;

    /* What the guest last told us, so a state we applied ourselves is not
     * reported straight back to it as though the user had done it. */
    bool guest_minimized = false;

    uint64_t presented = 0, dropped = 0;
    /* To notice a window that has stopped producing frames while still being
     * perfectly alive - which is what exclusive fullscreen looks like. */
    uint64_t last_frame_ms = SDL_GetTicks();
    bool     stall_reported = false;
    /* Frame age: guest capture to host acquire, in host time. Needs the clock
     * offset the daemon negotiates, so it stays zero until that lands. */
    uint64_t age_total_ns = 0, age_samples = 0, age_worst_ns = 0;
    uint64_t stats_at = SDL_GetTicks();

    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            /* Input is reported against the guest window the event landed on,
             * so a click on a menu reaches the menu rather than its owner. */
            struct view *v = view_for_sdl_id(views, view_count, ev.window.windowID);
            if (!v) v = &views[0];

            switch (ev.type) {
            case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            case SDL_EVENT_QUIT: {
                /*
                 * Closing the window here closes the app in the guest.
                 *
                 * The compositor sends a close request for the window itself -
                 * from its close button, the taskbar's context menu, or a
                 * keyboard shortcut - and it arrives as CLOSE_REQUESTED rather
                 * than QUIT. Handling only QUIT meant the taskbar's Close shut
                 * the host window while the app carried on running in the VM,
                 * invisible.
                 *
                 * A popup closes on its own; only a top-level ends the session.
                 */
                const uint64_t id = (ev.type == SDL_EVENT_QUIT)
                                  ? opt.window_id : v->window_id;
                if (daemon_fd >= 0) {
                    struct vypr_msg_window_id msg = { .window_id = id };
                    msg_send(daemon_fd, VYPR_MSG_CLOSE, &msg, sizeof(msg));
                }
                if (ev.type == SDL_EVENT_QUIT || !v->is_popup) running = 0;
                break;
            }
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_WHEEL: {
                if (daemon_fd < 0) break;

                /* The hit test handles the title bar; anything arriving here
                 * is either content or a caption button, and both are the
                 * guest's business. */
                float hx, hy;
                const SDL_MouseButtonFlags held = SDL_GetMouseState(&hx, &hy);

                SDL_GetWindowSize(v->win, &win_w, &win_h);

                struct vypr_msg_pointer msg = {0};
                msg.window_id = v->window_id;

                if (pointer_locked || capture_forced) {
                    /* Send motion, not position. The guest app is warping the
                     * cursor itself; telling it where our pointer is would add
                     * a bogus delta on top of its own warp every frame.
                     *
                     * Deltas are in host window pixels, and the window is
                     * rarely the size of the guest surface - a 4K stream shown
                     * in a 1080p window would otherwise move the guest pointer
                     * at half speed. */
                    if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                        float sx = 1.0f, sy = 1.0f;
                        if (win_w > 0 && win_h > 0 && v->src_w && v->src_h) {
                            sx = (float)v->src_w / (float)win_w;
                            sy = (float)v->src_h / (float)win_h;
                        }
                        msg.x = (int32_t)(ev.motion.xrel * sx);
                        msg.y = (int32_t)(ev.motion.yrel * sy);
                        /* Never round a real movement away to nothing. */
                        if (msg.x == 0 && ev.motion.xrel != 0.0f)
                            msg.x = ev.motion.xrel > 0 ? 1 : -1;
                        if (msg.y == 0 && ev.motion.yrel != 0.0f)
                            msg.y = ev.motion.yrel > 0 ? 1 : -1;
                    }
                    msg.flags |= VYPR_PTR_RELATIVE;
                } else {
                    to_guest_coords(win_w, win_h, v->src_w, v->src_h, hx, hy,
                                    &msg.x, &msg.y);
                }

                if (held & SDL_BUTTON_LMASK) msg.buttons |= 1u << 0;
                if (held & SDL_BUTTON_RMASK) msg.buttons |= 1u << 1;
                if (held & SDL_BUTTON_MMASK) msg.buttons |= 1u << 2;

                if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                    /* Windows counts a detent as 120; SDL reports fractional
                     * notches, so scale rather than truncate to zero. */
                    msg.wheel  = (int32_t)(ev.wheel.y * 120.0f);
                    msg.hwheel = (int32_t)(ev.wheel.x * 120.0f);
                }

                if (msg.flags & VYPR_PTR_RELATIVE) {
                    pointer.x += msg.x;          /* deltas sum exactly */
                    pointer.y += msg.y;
                } else {
                    pointer.x = msg.x;           /* only the latest matters */
                    pointer.y = msg.y;
                }
                pointer.buttons = msg.buttons;
                pointer.wheel  += msg.wheel;
                pointer.hwheel += msg.hwheel;
                pointer.flags   = msg.flags;
                pointer.pending = true;

                /* A click or a wheel notch goes now; motion can wait for the
                 * frame. */
                if (ev.type != SDL_EVENT_MOUSE_MOTION)
                    pointer_flush(daemon_fd, v->window_id, &pointer);
                break;
            }

            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP: {
                /* Ctrl+Escape releases the window without reaching the guest,
                 * so a guest app that grabs input cannot trap the user. */
                if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE &&
                    (ev.key.mod & SDL_KMOD_CTRL)) {
                    running = 0;
                    break;
                }
                /* Ctrl+Alt+Shift+M toggles between pointer capture and direct
                 * control - the chord Moonlight uses for the same thing, so the
                 * muscle memory carries over. Ctrl+Shift+M is accepted too. It is deliberately a host-side hotkey: a game that has
                 * grabbed the pointer must always be escapable, and the guest's
                 * own detection can miss a fullscreen app that leaves the
                 * cursor nominally visible. */
                /*
                 * Matched on scancode, not keycode. A keycode is what the key
                 * produces *after* the layout and modifiers are applied, so
                 * with Ctrl+Alt+Shift held it is not necessarily SDLK_M at all
                 * and the chord silently never fires. The scancode is the
                 * physical key regardless of what is held down with it.
                 */
                if (ev.type == SDL_EVENT_KEY_DOWN &&
                    ev.key.scancode == SDL_SCANCODE_M &&
                    (ev.key.mod & SDL_KMOD_CTRL) && (ev.key.mod & SDL_KMOD_SHIFT)) {
                    capture_forced = !capture_forced;
                    set_capture(views[0].win, pointer_locked || capture_forced, true);

                    /* The Ctrl and Shift presses already went to the guest, and
                     * the M never will - so release the modifiers explicitly or
                     * the guest is left holding them down. A stuck Ctrl in a
                     * game is its own kind of misery. */
                    if (daemon_fd >= 0) {
                        static const uint32_t mods[] = {
                            0x1D, 0xE01D,      /* ctrl */
                            0x2A, 0x36,        /* shift */
                            0x38, 0xE038       /* alt, for the Moonlight chord */
                        };
                        for (size_t k = 0; k < sizeof(mods) / sizeof(mods[0]); k++) {
                            struct vypr_msg_key up = {0};
                            up.window_id = v->window_id;
                            up.scancode  = mods[k];
                            up.down      = 0;
                            msg_send(daemon_fd, VYPR_MSG_KEY, &up, sizeof(up));
                        }
                    }

                    break;
                }
                if (daemon_fd < 0) break;

                struct vypr_msg_key msg = {0};
                msg.window_id = v->window_id;
                /* SDL scancodes are USB HID usage ids; the guest wants PS/2
                 * set 1, which is what SendInput takes. */
                msg.scancode  = sdl_scancode_to_ps2(ev.key.scancode);
                msg.down      = (ev.type == SDL_EVENT_KEY_DOWN);
                msg.modifiers = ev.key.mod;
                if (msg.scancode)
                    msg_send(daemon_fd, VYPR_MSG_KEY, &msg, sizeof(msg));
                break;
            }

            case SDL_EVENT_WINDOW_MINIMIZED:
            case SDL_EVENT_WINDOW_RESTORED: {
                if (daemon_fd < 0 || v->is_popup) break;
                const bool mini = (ev.type == SDL_EVENT_WINDOW_MINIMIZED);
                if (mini == guest_minimized) break;   /* we are only catching up */
                guest_minimized = mini;

                /* Minimising here should minimise the app in the VM too - it is
                 * otherwise left rendering a window nobody can see. Restoring
                 * has to bring it back, or the window returns showing whatever
                 * frame it had when it stopped. */
                struct vypr_msg_window_state st = {0};
                st.window_id = v->window_id;
                st.minimized = mini ? 1u : 0u;
                msg_send(daemon_fd, VYPR_MSG_WINDOW_STATE, &st, sizeof(st));
                break;
            }

            case SDL_EVENT_WINDOW_FOCUS_GAINED: {
                if (daemon_fd < 0 || v->is_popup) break;
                struct vypr_msg_window_id msg = { .window_id = v->window_id };
                msg_send(daemon_fd, VYPR_MSG_FOCUS, &msg, sizeof(msg));
                break;
            }

            case SDL_EVENT_GAMEPAD_ADDED:
            case SDL_EVENT_GAMEPAD_REMOVED:
                pads_close(pads);
                pads_open(pads);
                break;

            case SDL_EVENT_CLIPBOARD_UPDATE: {
                if (daemon_fd < 0) break;
                char *text = SDL_GetClipboardText();
                if (!text) break;
                const size_t len = strlen(text);
                if (len && len <= VYPR_MAX_MSG_BYTES &&
                    (!clip_last || strcmp(clip_last, text) != 0)) {
                    free(clip_last);
    pads_close(pads);
                    clip_last = strdup(text);
                    msg_send(daemon_fd, VYPR_MSG_CLIPBOARD, text, (uint32_t)len);
                }
                SDL_free(text);
                break;
            }

            case SDL_EVENT_WINDOW_RESIZED: {
                if (daemon_fd < 0) break;
                /* Adopting the frame size raises this event too. Asking the
                 * guest to become the size it already is achieves nothing and
                 * oscillates against a guest that rounds differently. */
                if (v->is_popup) break;   /* the guest owns a popup's size */
                if ((uint32_t)ev.window.data1 == v->src_w &&
                    (uint32_t)ev.window.data2 == v->src_h)
                    break;
                /* Note it and wait for the drag to finish. */
                resize_w  = (uint32_t)ev.window.data1;
                resize_h  = (uint32_t)ev.window.data2;
                resize_at = SDL_GetTicks();
                break;
            }
            default:
                break;
            }
        }

        /* Settled? Then ask the guest to match. */
        if (resize_at && SDL_GetTicks() - resize_at > 200) {
            struct vypr_msg_resize rs = {0};
            rs.window_id = views[0].window_id;
            rs.width  = resize_w;
            rs.height = resize_h;
            msg_send(daemon_fd, VYPR_MSG_RESIZE, &rs, sizeof(rs));
            resize_at = 0;
        }

        hit.chrome_top = chrome_reported;
        hit.src_w      = views[0].src_w;
        hit.src_h      = views[0].src_h;
        hit.captured   = pointer_locked || capture_forced;

        pointer_flush(daemon_fd, views[0].window_id, &pointer);
        pads_poll(pads, daemon_fd,
                  (SDL_GetWindowFlags(views[0].win) & SDL_WINDOW_INPUT_FOCUS) != 0);

        /* Messages the reader thread parked for us. Popups open windows and
         * the rest touches the compositor, so they are handled here rather
         * than on the thread that receives them. */
        if (daemon_fd >= 0) {
            if (atomic_load(&link.dead)) running = 0;

            pthread_mutex_lock(&link.lock);
            size_t plen = link.pending.len;
            if (plen) {
                if (parked_cap < plen) {
                    uint8_t *g = realloc(parked, plen);
                    if (g) { parked = g; parked_cap = plen; }
                    else plen = 0;
                }
                if (plen) memcpy(parked, link.pending.p, plen);
                link.pending.len = 0;
            }
            pthread_mutex_unlock(&link.lock);

            size_t off = 0;
            while (plen - off >= sizeof(struct vypr_msg_head)) {
                struct vypr_msg_head head;
                memcpy(&head, parked + off, sizeof(head));
                /* head.bytes counts the payload only - the message on the wire
                 * is the header plus that. Treating it as the total walked this
                 * buffer at the wrong stride, so every message after the first
                 * was read from the wrong offset: the client acted on a pointer
                 * release while the daemon had sent a capture. */
                const size_t total = sizeof(head) + head.bytes;
                if (head.bytes > VYPR_MAX_MSG_BYTES || off + total > plen) break;
                const uint8_t *payload = parked + off + sizeof(head);
                off += total;
                {
                    if (head.type == VYPR_MSG_CLIENT_POPUP &&
                        head.bytes >= sizeof(struct vypr_msg_client_popup)) {
                        view_open_popup(views, &view_count, &shm,
                                        (const struct vypr_msg_client_popup *)payload,
                                        opt.backend);
                    } else if (head.type == VYPR_MSG_CLIENT_CLIPBOARD) {
                        /* Remembered before setting it, so the update this
                         * causes is recognised as our own and not sent back. */
                        char *text = malloc(head.bytes + 1);
                        if (text) {
                            memcpy(text, payload, head.bytes);
                            text[head.bytes] = '\0';
                            free(clip_last);
                            clip_last = text;
                            SDL_SetClipboardText(text);
                        }
                    } else if (head.type == VYPR_MSG_CLIENT_STATE &&
                               head.bytes >= sizeof(struct vypr_msg_window_state)) {
                        const struct vypr_msg_window_state *m = (const void *)payload;
                        if (m->window_id == opt.window_id) {
                            const bool mini = m->minimized != 0;
                            const bool have =
                                (SDL_GetWindowFlags(views[0].win) & SDL_WINDOW_MINIMIZED) != 0;
                            guest_minimized = mini;
                            /* Only when it differs, so this does not echo back
                             * out as a user action. */
                            if (mini && !have)       SDL_MinimizeWindow(views[0].win);
                            else if (!mini && have)  SDL_RestoreWindow(views[0].win);

                            /* An app that goes fullscreen in the guest should
                             * go fullscreen here: the window it is drawing now
                             * covers the guest's whole desktop, and showing
                             * that inside a small window is not what the user
                             * asked the app to do. */
                            const bool want_fs = m->fullscreen != 0;
                            const bool is_fs =
                                (SDL_GetWindowFlags(views[0].win) & SDL_WINDOW_FULLSCREEN) != 0;
                            if (want_fs != is_fs)
                                SDL_SetWindowFullscreen(views[0].win, want_fs);
                        }
                    } else if (head.type == VYPR_MSG_CLIENT_GEOM &&
                               head.bytes >= sizeof(struct vypr_msg_client_geom)) {
                        const struct vypr_msg_client_geom *m = (const void *)payload;
                        if (m->window_id == opt.window_id)
                            chrome_reported = m->chrome_top;
                    } else if (head.type == VYPR_MSG_CLIENT_LOCK &&
                               head.bytes >= sizeof(struct vypr_msg_pointer_lock)) {
                        const struct vypr_msg_pointer_lock *m = (const void *)payload;
                        pointer_locked = m->locked != 0;
                        set_capture(views[0].win, pointer_locked || capture_forced, false);
                        printf("vypr: guest %s the pointer%s\n",
                               pointer_locked ? "captured" : "released",
                               capture_forced ? " (manual capture still on)" : "");
                        fflush(stdout);
                    } else if (head.type == VYPR_MSG_CLIENT_POPUP_END &&
                               head.bytes >= sizeof(struct vypr_msg_window_id)) {
                        const struct vypr_msg_window_id *m = (const void *)payload;
                        view_close(views, &view_count, m->window_id);
                    }
                }
            }
        }

        for (int i = 0; i < view_count; i++) {
            struct view *v = &views[i];
            struct vypr_frame_view f;

            int rc = vypr_shm_acquire(&shm, v->slot, v->last_serial, &f);
            if (rc == 0) {
                if (f.width != v->src_w || f.height != v->src_h) {
                    v->src_w = f.width;
                    v->src_h = f.height;
                    /* A popup is sized by the guest. A top-level adopts the
                     * frame size too, but not while the user is still dragging
                     * its edge - that is what made resizing judder. */
                    if (!v->is_popup && resize_at == 0)
                        SDL_SetWindowSize(v->win, (int)f.width, (int)f.height);
                }

                if (v->last_serial && f.serial > v->last_serial + 1)
                    dropped += f.serial - v->last_serial - 1;
                v->last_serial = f.serial;

                if (i == 0 && f.capture_qpc_freq &&
                    __atomic_load_n(&shm.hdr->offset_valid, __ATOMIC_ACQUIRE)) {
                    const uint64_t guest_ns =
                        (uint64_t)((__int128)f.capture_qpc * 1000000000 /
                                   f.capture_qpc_freq);
                    const int64_t captured_host_ns =
                        (int64_t)guest_ns + shm.hdr->guest_offset_ns;
                    struct timespec ts;
                    clock_gettime(CLOCK_MONOTONIC, &ts);
                    const int64_t now = (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
                    const int64_t age = now - captured_host_ns;
                    if (age >= 0 && age < 1000000000) {
                        age_total_ns += (uint64_t)age;
                        if ((uint64_t)age > age_worst_ns) age_worst_ns = (uint64_t)age;
                        age_samples++;
                    }
                }

                presenter_upload(v->pres, &f);
                if (i == 0) {
                    presented++;
                    last_frame_ms = SDL_GetTicks();
                    stall_reported = false;
                }
            } else if (rc == -1 && !v->is_popup &&
                       vypr_slot_state(&shm, v->slot) == VYPR_SLOT_CLOSED) {
                running = 0;
            }

            presenter_present(v->pres);
        }

        /*
         * A window that is alive and silent.
         *
         * Windows.Graphics.Capture reads DWM's per-window surfaces. A game in
         * *exclusive* fullscreen bypasses DWM entirely, so there is no surface
         * left to capture: frames simply stop, with no error anywhere - the
         * process is running, the window still reports its size, and audio
         * keeps playing. Said plainly here because it is otherwise a silent
         * black window, and the fix is in the game's own settings.
         */
        if (!stall_reported && SDL_GetTicks() - last_frame_ms > 10000) {
            stall_reported = true;
            printf("vypr: no frames for 10s from '%s' - if it just went "
                   "fullscreen, set it to borderless or windowed: exclusive "
                   "fullscreen bypasses the compositor and cannot be captured\n",
                   opt.title);
            fflush(stdout);
        }

        if (opt.stats) {
            uint64_t now = SDL_GetTicks();
            if (now - stats_at >= 1000) {
                uint64_t ns_upload = 0, ns_present = 0;
                presenter_take_timings(views[0].pres, &ns_upload, &ns_present);
                /* Half the round trip is the most the offset can be wrong by,
                 * so it is the honest error bar on every age below. */
                const double err_ms = shm.hdr->offset_rtt_us / 2000.0;
                printf("%" PRIu64 " fps presented, %" PRIu64 " dropped | "
                       "upload %.1f ms, present %.1f ms | age avg %.1f ms "
                       "worst %.1f ms (+/- %.2f)\n",
                       presented, dropped,
                       presented ? ns_upload  / 1e6 / presented : 0.0,
                       presented ? ns_present / 1e6 / presented : 0.0,
                       age_samples ? age_total_ns / 1e6 / age_samples : 0.0,
                       age_worst_ns / 1e6, err_ms);
                presented = dropped = 0;
                age_total_ns = age_samples = age_worst_ns = 0;
                stats_at = now;
            }
        }
    }

    for (int i = view_count - 1; i >= 0; i--) {
        if (views[i].pres) presenter_destroy(views[i].pres);
        if (views[i].win)  SDL_DestroyWindow(views[i].win);
    }
    if (link_running) {
        atomic_store(&link.stop, 1);
        pthread_join(link_tid, NULL);
    }
    if (link.audio) SDL_DestroyAudioStream(link.audio);
    pthread_mutex_destroy(&link.lock);
    free(link.pending.p);
    free(parked);
    free(clip_last);
    SDL_Quit();
    if (daemon_fd >= 0) close(daemon_fd);
    vypr_shm_close(&shm);
    return 0;
}
