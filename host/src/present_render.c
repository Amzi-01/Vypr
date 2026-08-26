/*
 * SDL_Renderer present path - the original, kept for comparison.
 *
 * LockTexture hands back a CPU staging buffer which UnlockTexture then copies
 * into the texture, so every frame is copied twice before the GPU sees it. At
 * 1080p that costs 3 ms and nobody notices; at 4K it was measured at 15 ms,
 * which misses vsync on its own.
 *
 * The texture pool is here because uploading into a texture the GPU is still
 * reading forces a stall. It did not help on its own - the copies dominate -
 * but removing it would reintroduce a second problem behind the first.
 */
#include "present_internal.h"

#include <stdio.h>
#include <string.h>

enum { TEX_POOL = 3 };

struct render_state {
    SDL_Renderer *ren;
    SDL_Texture  *tex[TEX_POOL];
    int           at;
    uint32_t      tex_w, tex_h;
    bool          have_frame;
    uint64_t      ns_upload, ns_present;
};

static const char *render_driver(void *impl)
{
    struct render_state *p = impl;
    const char *n = p->ren ? SDL_GetRendererName(p->ren) : NULL;
    return n ? n : "render";
}

static void *render_create(SDL_Window *win, void *share_impl)
{
    (void)share_impl;   /* SDL_Renderer has no shareable device */
    struct render_state *p = SDL_calloc(1, sizeof(*p));
    if (!p) return NULL;

    /* The default 'opengl' backend presents synchronously - measured 21-24 ms
     * at 4K. Anything else is better; 'gpu' was the best of them. */
    if (!SDL_getenv("SDL_RENDER_DRIVER"))
        SDL_SetHint(SDL_HINT_RENDER_DRIVER, "gpu");

    p->ren = SDL_CreateRenderer(win, NULL);
    if (!p->ren) {
        fprintf(stderr, "sash: SDL_CreateRenderer: %s\n", SDL_GetError());
        SDL_free(p);
        return NULL;
    }
    SDL_SetRenderVSync(p->ren, 1);
    return p;
}

static void render_destroy(void *impl)
{
    struct render_state *p = impl;
    if (!p) return;
    for (int i = 0; i < TEX_POOL; i++)
        if (p->tex[i]) SDL_DestroyTexture(p->tex[i]);
    if (p->ren) SDL_DestroyRenderer(p->ren);
    SDL_free(p);
}

static bool render_upload(void *impl, const struct sash_frame_view *f)
{
    struct render_state *p = impl;
    const uint64_t t0 = SDL_GetTicksNS();

    if (!p->tex[0] || f->width != p->tex_w || f->height != p->tex_h) {
        for (int i = 0; i < TEX_POOL; i++) {
            if (p->tex[i]) SDL_DestroyTexture(p->tex[i]);
            p->tex[i] = SDL_CreateTexture(p->ren, SDL_PIXELFORMAT_ARGB8888,
                                          SDL_TEXTUREACCESS_STREAMING,
                                          (int)f->width, (int)f->height);
            if (!p->tex[i]) {
                fprintf(stderr, "sash: SDL_CreateTexture: %s\n", SDL_GetError());
                return false;
            }
            SDL_SetTextureScaleMode(p->tex[i], SDL_SCALEMODE_LINEAR);
        }
        p->tex_w = f->width;
        p->tex_h = f->height;
        p->at = 0;
    }

    SDL_Texture *target = p->tex[p->at];
    p->at = (p->at + 1) % TEX_POOL;

    void *dst = NULL;
    int   pitch = 0;
    if (SDL_LockTexture(target, NULL, &dst, &pitch)) {
        if ((uint32_t)pitch == f->stride) {
            memcpy(dst, f->pixels, (size_t)f->stride * f->height);
        } else {
            const size_t row = (size_t)f->width * 4;
            for (uint32_t y = 0; y < f->height; y++)
                memcpy((uint8_t *)dst + (size_t)y * pitch,
                       f->pixels + (size_t)y * f->stride, row);
        }
        SDL_UnlockTexture(target);
        p->have_frame = true;
    }

    p->ns_upload += SDL_GetTicksNS() - t0;
    return true;
}

static void render_present(void *impl)
{
    struct render_state *p = impl;
    const uint64_t t0 = SDL_GetTicksNS();

    SDL_RenderClear(p->ren);
    if (p->have_frame) {
        SDL_Texture *show = p->tex[(p->at + TEX_POOL - 1) % TEX_POOL];
        if (show) SDL_RenderTexture(p->ren, show, NULL, NULL);
    }
    SDL_RenderPresent(p->ren);

    p->ns_present += SDL_GetTicksNS() - t0;
}

static void render_take_timings(void *impl, uint64_t *upload_ns, uint64_t *present_ns)
{
    struct render_state *p = impl;
    if (upload_ns)  *upload_ns  = p->ns_upload;
    if (present_ns) *present_ns = p->ns_present;
    p->ns_upload = p->ns_present = 0;
}

const struct present_ops present_render_ops = {
    .name         = "render",
    .create       = render_create,
    .destroy      = render_destroy,
    .driver       = render_driver,
    .upload       = render_upload,
    .present      = render_present,
    .take_timings = render_take_timings,
};
