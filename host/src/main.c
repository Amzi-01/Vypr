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
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "msg.h"
#include "shm.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

struct options {
    const char *shm_path;
    const char *title;
    const char *sock_path;   /* unix socket back to sashd; NULL = no input path */
    uint64_t    window_id;
    uint32_t    slot;
    int         stats;
};

static void usage(void)
{
    fputs("usage: sash-host --shm PATH --slot N [--title NAME] [--stats]\n"
          "                 [--sock PATH --window-id ID]\n"
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
    return fd;
}

static int parse_args(int argc, char **argv, struct options *o)
{
    o->shm_path  = "/dev/shm/sash";
    o->title     = "sash";
    o->sock_path = NULL;
    o->window_id = 0;
    o->slot      = 0;
    o->stats     = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shm") && i + 1 < argc)        o->shm_path = argv[++i];
        else if (!strcmp(argv[i], "--slot") && i + 1 < argc)  o->slot = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) o->title = argv[++i];
        else if (!strcmp(argv[i], "--sock") && i + 1 < argc)  o->sock_path = argv[++i];
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

    SDL_Window *win = SDL_CreateWindow(opt.title, win_w, win_h, SDL_WINDOW_RESIZABLE);
    if (!win) { fprintf(stderr, "sash: SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }

    /* Backend choice is worth 2x at 4K. The default 'opengl' backend presents
     * synchronously - measured 21-24 ms per present, which alone misses vsync
     * and pins the stream to half refresh. The 'gpu' backend presents in ~2 ms
     * and lets the upload overlap. Env still wins, for debugging. */
    if (!SDL_getenv("SDL_RENDER_DRIVER"))
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "gpu");

    SDL_Renderer *ren = SDL_CreateRenderer(win, NULL);
    if (!ren) { fprintf(stderr, "sash: SDL_CreateRenderer: %s\n", SDL_GetError()); return 1; }
    SDL_SetRenderVSync(ren, 1);

    if (opt.stats) {
        const char *name = SDL_GetRendererName(ren);
        printf("sash: renderer '%s'\n", name ? name : "?");
    }

    /* A pool, not a single texture. Uploading into the texture the GPU is still
     * reading from forces the driver to stall until that draw retires, which at
     * 4K costs more than a frame and shows up as exactly half refresh rate.
     * Rotating means the upload always targets a texture the GPU has finished
     * with. */
    enum { TEX_POOL = 3 };
    SDL_Texture *tex[TEX_POOL] = { NULL, NULL, NULL };
    int tex_at = 0;
    uint32_t tex_w = 0, tex_h = 0;

    /* Frame-time accounting, split at the two places the time can go: getting
     * the frame into a texture, and getting that texture on screen. Present
     * includes the vsync wait, so it is expected to dominate when healthy. */
    uint64_t ns_upload = 0, ns_present = 0;

    uint32_t last_serial = 0;
    uint64_t presented = 0, dropped = 0;
    uint64_t stats_at = SDL_GetTicks();

    int running = 1;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
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

                SDL_GetWindowSize(win, &win_w, &win_h);

                struct sash_msg_pointer msg = {0};
                msg.window_id = opt.window_id;
                to_guest_coords(win_w, win_h, tex_w, tex_h, hx, hy, &msg.x, &msg.y);

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
                if (daemon_fd < 0) break;

                struct sash_msg_key msg = {0};
                msg.window_id = opt.window_id;
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
                if (daemon_fd < 0) break;
                struct sash_msg_window_id msg = { .window_id = opt.window_id };
                msg_send(daemon_fd, SASH_MSG_FOCUS, &msg, sizeof(msg));
                break;
            }

            case SDL_EVENT_WINDOW_RESIZED: {
                if (daemon_fd < 0) break;
                /* Adopting the frame size raises this event too. Asking the
                 * guest to become the size it already is achieves nothing and
                 * oscillates against a guest that rounds differently. */
                if ((uint32_t)ev.window.data1 == tex_w &&
                    (uint32_t)ev.window.data2 == tex_h)
                    break;
                /* Ask the guest window to match, so the stream is pixel-exact
                 * rather than scaled. The guest may refuse - a fixed-size app
                 * simply will not resize - and then scaling stands in. */
                struct sash_msg_resize msg = {0};
                msg.window_id = opt.window_id;
                msg.width  = (uint32_t)ev.window.data1;
                msg.height = (uint32_t)ev.window.data2;
                msg_send(daemon_fd, SASH_MSG_RESIZE, &msg, sizeof(msg));
                break;
            }
            default:
                break;
            }
        }

        struct sash_frame_view f;
        int rc = sash_shm_acquire(&shm, opt.slot, last_serial, &f);

        if (rc == 0) {
            if (!tex[0] || f.width != tex_w || f.height != tex_h) {
                int failed = 0;
                for (int t = 0; t < TEX_POOL; t++) {
                    if (tex[t]) SDL_DestroyTexture(tex[t]);
                    tex[t] = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888,
                                               SDL_TEXTUREACCESS_STREAMING,
                                               (int)f.width, (int)f.height);
                    if (!tex[t]) {
                        fprintf(stderr, "sash: SDL_CreateTexture: %s\n", SDL_GetError());
                        failed = 1;
                        break;
                    }
                    SDL_SetTextureScaleMode(tex[t], SDL_SCALEMODE_LINEAR);
                }
                if (failed) break;
                tex_w = f.width;
                tex_h = f.height;
                tex_at = 0;
                SDL_SetWindowSize(win, (int)f.width, (int)f.height);
            }
            SDL_Texture *upload = tex[tex_at];
            tex_at = (tex_at + 1) % TEX_POOL;

            /* Count gaps in the serial: the guest published frames we never
             * showed, which is the number that says whether the host is
             * keeping up. */
            if (last_serial && f.serial > last_serial + 1)
                dropped += f.serial - last_serial - 1;
            last_serial = f.serial;

            uint64_t t_up = SDL_GetTicksNS();
            void *dst = NULL;
            int   dst_pitch = 0;
            if (SDL_LockTexture(upload, NULL, &dst, &dst_pitch)) {
                if ((uint32_t)dst_pitch == f.stride) {
                    memcpy(dst, f.pixels, (size_t)f.stride * f.height);
                } else {
                    size_t row = (size_t)f.width * 4;
                    for (uint32_t y = 0; y < f.height; y++)
                        memcpy((uint8_t *)dst + (size_t)y * dst_pitch,
                               f.pixels + (size_t)y * f.stride, row);
                }
                SDL_UnlockTexture(upload);
            }
            ns_upload += SDL_GetTicksNS() - t_up;
            presented++;
        } else if (rc == -1 && sash_slot_state(&shm, opt.slot) == SASH_SLOT_CLOSED) {
            running = 0;
        }

        uint64_t t_pr = SDL_GetTicksNS();
        SDL_RenderClear(ren);
        /* Draw the most recently uploaded texture, which is the one before the
         * cursor now points at. */
        SDL_Texture *show = tex[(tex_at + TEX_POOL - 1) % TEX_POOL];
        if (show) SDL_RenderTexture(ren, show, NULL, NULL);
        SDL_RenderPresent(ren);
        ns_present += SDL_GetTicksNS() - t_pr;

        if (opt.stats) {
            uint64_t now = SDL_GetTicks();
            if (now - stats_at >= 1000) {
                printf("%" PRIu64 " fps presented, %" PRIu64 " dropped | "
                       "upload %.1f ms/frame, present %.1f ms/frame\n",
                       presented, dropped,
                       presented ? ns_upload  / 1e6 / presented : 0.0,
                       presented ? ns_present / 1e6 / presented : 0.0);
                presented = dropped = 0;
                ns_upload = ns_present = 0;
                stats_at = now;
            }
        }
    }

    for (int t = 0; t < TEX_POOL; t++)
        if (tex[t]) SDL_DestroyTexture(tex[t]);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    if (daemon_fd >= 0) close(daemon_fd);
    sash_shm_close(&shm);
    return 0;
}
