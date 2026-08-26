/*
 * sash-host - presents one shared-memory slot as one native window.
 *
 * One process per window. A crashed or wedged stream then takes down a single
 * window instead of the whole session, and the compositor treats each app as
 * the separate top-level it is meant to look like.
 */
#define _GNU_SOURCE
#include <SDL3/SDL.h>

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "shm.h"

struct options {
    const char *shm_path;
    const char *title;
    uint32_t    slot;
    int         stats;
};

static void usage(void)
{
    fputs("usage: sash-host --shm PATH --slot N [--title NAME] [--stats]\n", stderr);
}

static int parse_args(int argc, char **argv, struct options *o)
{
    o->shm_path = "/dev/shm/sash";
    o->title    = "sash";
    o->slot     = 0;
    o->stats    = 0;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shm") && i + 1 < argc)        o->shm_path = argv[++i];
        else if (!strcmp(argv[i], "--slot") && i + 1 < argc)  o->slot = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "--title") && i + 1 < argc) o->title = argv[++i];
        else if (!strcmp(argv[i], "--stats"))                 o->stats = 1;
        else { usage(); return -1; }
    }
    return 0;
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
                running = 0;
                break;
            case SDL_EVENT_MOUSE_MOTION: {
                int32_t gx, gy;
                SDL_GetWindowSize(win, &win_w, &win_h);
                to_guest_coords(win_w, win_h, tex_w, tex_h, ev.motion.x, ev.motion.y, &gx, &gy);
                /* The control channel is not wired up yet, so events are
                 * reported rather than sent. This is the seam the agent
                 * connection plugs into. */
                if (opt.stats)
                    printf("pointer %" PRId32 ",%" PRId32 "\n", gx, gy);
                break;
            }
            case SDL_EVENT_KEY_DOWN:
                if (ev.key.key == SDLK_ESCAPE && (ev.key.mod & SDL_KMOD_CTRL))
                    running = 0;
                break;
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
    sash_shm_close(&shm);
    return 0;
}
