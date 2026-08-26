/*
 * sash-host - presents one shared-memory slot as one native window.
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
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct options {
    const char *shm_path;
    const char *title;
    const char *sock_path;   /* unix socket back to sashd; NULL = no input path */
    const char *backend;     /* "gpu" or "render" */
    int         capture;     /* start with the pointer captured */
    uint64_t    window_id;
    uint32_t    slot;
    int         stats;
};

static void usage(void)
{
    fputs("usage: sash-host --shm PATH --slot N [--title NAME] [--stats]\n"
          "                 [--sock PATH --window-id ID] [--present gpu|render]\n"
          "\nRun standalone it presents a slot. sashd additionally passes --sock\n"
          "and --window-id, which is what turns input back on.\n", stderr);
}

/* Input goes back to sashd rather than straight to the guest: one process owns
 * the link to the agent, so window identity and reconnect are decided in one
 * place. Returns -1 when there is no daemon, which is the standalone case. */
static int connect_daemon(const char *path, uint64_t window_id)
{
    if (!path) return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "sash: cannot reach sashd at %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }

    struct sash_msg_window_id hello = { .window_id = window_id };
    if (msg_send(fd, SASH_MSG_CLIENT_HELLO, &hello, sizeof(hello)) < 0) {
        close(fd);
        return -1;
    }

    /* Non-blocking: this socket is polled from inside the render loop, and a
     * blocking read would stall presentation waiting for a popup message that
     * may never arrive. */
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    return fd;
}

static int parse_args(int argc, char **argv, struct options *o)
{
    o->shm_path  = "/dev/shm/sash";
    o->title     = "sash";
    o->sock_path = NULL;
    o->backend   = NULL;
    o->capture   = 0;
    o->window_id = 0;
    o->slot      = 0;
    o->stats     = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shm") && i + 1 < argc)        o->shm_path = argv[++i];
        else if (!strcmp(argv[i], "--slot") && i + 1 < argc)  o->slot = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) o->title = argv[++i];
        else if (!strcmp(argv[i], "--sock") && i + 1 < argc)  o->sock_path = argv[++i];
        else if (!strcmp(argv[i], "--present") && i + 1 < argc) o->backend = argv[++i];
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
        fprintf(stderr, "sash: relative mouse mode %s refused: %s\n",
                want ? "on" : "off", SDL_GetError());
        return;
    }
    if (announce) {
        printf("sash: mouse capture %s\n",
               want ? "ON - relative motion" : "OFF - absolute");
        fflush(stdout);
    }
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
static bool view_open_popup(struct view *views, int *count, struct sash_shm *shm,
                            const struct sash_msg_client_popup *msg,
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
        fprintf(stderr, "sash: SDL_CreatePopupWindow: %s\n", SDL_GetError());
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

int main(int argc, char **argv)
{
    struct options opt;
    if (parse_args(argc, argv, &opt) < 0) return 2;

    struct sash_shm shm;
    if (sash_shm_open(&shm, opt.shm_path, 0) < 0) return 1;

    const int daemon_fd = connect_daemon(opt.sock_path, opt.window_id);

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        fprintf(stderr, "sash: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    /* Start at the slot's maximum and resize down once a frame says otherwise;
     * opening at the wrong size and snapping is uglier than one late resize. */
    const struct sash_slot *slot = &shm.hdr->slots[opt.slot];
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
        fprintf(stderr, "sash: SDL_CreateWindow: %s\n", SDL_GetError());
        return 1;
    }

    views[0].pres = presenter_create(views[0].win, opt.backend, NULL);
    if (!views[0].pres) { fprintf(stderr, "sash: no usable present backend\n"); return 1; }

    if (opt.stats)
        printf("sash: present backend '%s'\n", presenter_name(views[0].pres));

    if (capture_forced_initial(&opt))
        set_capture(views[0].win, true, true);

    struct msg_reader daemon_rx = {0};

    /* Relative pointer mode, driven by the guest telling us an app has taken
     * the pointer. `suspended` is the user's override: a captured pointer must
     * always be escapable from the host side, whatever the guest thinks. */
    bool pointer_locked = false;
    /* Forced by the user with Ctrl+Alt. The guest's own detection - cursor
     * hidden or clipped - misses cases like a fullscreen game that leaves the
     * cursor nominally visible, so there has to be a way to say "capture"
     * that does not depend on guessing. */
    bool capture_forced = opt.capture != 0;

    uint64_t presented = 0, dropped = 0;
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
            case SDL_EVENT_QUIT:
                if (daemon_fd >= 0) {
                    struct sash_msg_window_id msg = { .window_id = opt.window_id };
                    msg_send(daemon_fd, SASH_MSG_CLOSE, &msg, sizeof(msg));
                }
                running = 0;
                break;
            case SDL_EVENT_MOUSE_MOTION:
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
            case SDL_EVENT_MOUSE_WHEEL: {
                if (daemon_fd < 0) break;

                float hx, hy;
                const SDL_MouseButtonFlags held = SDL_GetMouseState(&hx, &hy);

                SDL_GetWindowSize(v->win, &win_w, &win_h);

                struct sash_msg_pointer msg = {0};
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
                    msg.flags |= SASH_PTR_RELATIVE;
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

                msg_send(daemon_fd, SASH_MSG_POINTER, &msg, sizeof(msg));
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
                if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_M &&
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
                            struct sash_msg_key up = {0};
                            up.window_id = v->window_id;
                            up.scancode  = mods[k];
                            up.down      = 0;
                            msg_send(daemon_fd, SASH_MSG_KEY, &up, sizeof(up));
                        }
                    }

                    break;
                }
                if (daemon_fd < 0) break;

                struct sash_msg_key msg = {0};
                msg.window_id = v->window_id;
                /* SDL scancodes are USB HID usage ids; the guest wants PS/2
                 * set 1, which is what SendInput takes. */
                msg.scancode  = sdl_scancode_to_ps2(ev.key.scancode);
                msg.down      = (ev.type == SDL_EVENT_KEY_DOWN);
                msg.modifiers = ev.key.mod;
                if (msg.scancode)
                    msg_send(daemon_fd, SASH_MSG_KEY, &msg, sizeof(msg));
                break;
            }

            case SDL_EVENT_WINDOW_FOCUS_GAINED: {
                if (daemon_fd < 0 || v->is_popup) break;
                struct sash_msg_window_id msg = { .window_id = v->window_id };
                msg_send(daemon_fd, SASH_MSG_FOCUS, &msg, sizeof(msg));
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
                /* Ask the guest window to match, so the stream is pixel-exact
                 * rather than scaled. The guest may refuse - a fixed-size app
                 * simply will not resize - and then scaling stands in. */
                struct sash_msg_resize msg = {0};
                msg.window_id = v->window_id;
                msg.width  = (uint32_t)ev.window.data1;
                msg.height = (uint32_t)ev.window.data2;
                msg_send(daemon_fd, SASH_MSG_RESIZE, &msg, sizeof(msg));
                break;
            }
            default:
                break;
            }
        }

        /* Popups arrive and vanish while we run, so the daemon link is read
         * every frame rather than only at startup. */
        if (daemon_fd >= 0) {
            int rc = msg_reader_fill(&daemon_rx, daemon_fd);
            if (rc > 0) {
                struct sash_msg_head head;
                const uint8_t *payload;
                while (msg_reader_next(&daemon_rx, &head, &payload) == 1) {
                    if (head.type == SASH_MSG_CLIENT_POPUP &&
                        head.bytes >= sizeof(struct sash_msg_client_popup)) {
                        view_open_popup(views, &view_count, &shm,
                                        (const struct sash_msg_client_popup *)payload,
                                        opt.backend);
                    } else if (head.type == SASH_MSG_CLIENT_LOCK &&
                               head.bytes >= sizeof(struct sash_msg_pointer_lock)) {
                        const struct sash_msg_pointer_lock *m = (const void *)payload;
                        pointer_locked = m->locked != 0;
                        set_capture(views[0].win, pointer_locked || capture_forced, false);
                        printf("sash: guest %s the pointer%s\n",
                               pointer_locked ? "captured" : "released",
                               capture_forced ? " (manual capture still on)" : "");
                        fflush(stdout);
                    } else if (head.type == SASH_MSG_CLIENT_POPUP_END &&
                               head.bytes >= sizeof(struct sash_msg_window_id)) {
                        const struct sash_msg_window_id *m = (const void *)payload;
                        view_close(views, &view_count, m->window_id);
                    }
                }
            }
        }

        for (int i = 0; i < view_count; i++) {
            struct view *v = &views[i];
            struct sash_frame_view f;

            int rc = sash_shm_acquire(&shm, v->slot, v->last_serial, &f);
            if (rc == 0) {
                if (f.width != v->src_w || f.height != v->src_h) {
                    v->src_w = f.width;
                    v->src_h = f.height;
                    /* A popup is sized by the guest; only a top-level adopts
                     * the frame size, since the user may have resized it. */
                    if (!v->is_popup)
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
                if (i == 0) presented++;
            } else if (rc == -1 && !v->is_popup &&
                       sash_slot_state(&shm, v->slot) == SASH_SLOT_CLOSED) {
                running = 0;
            }

            presenter_present(v->pres);
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
    msg_reader_free(&daemon_rx);
    SDL_Quit();
    if (daemon_fd >= 0) close(daemon_fd);
    sash_shm_close(&shm);
    return 0;
}
