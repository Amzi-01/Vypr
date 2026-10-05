/*
 * SDL_GPU present path.
 *
 * The frame arrives in host RAM already - the guest wrote it there over PCIe -
 * so the only work left is getting it into VRAM. A transfer buffer is memory
 * the GPU can DMA from directly, so mapping it and copying the frame in is the
 * whole upload: one memcpy, no intermediate staging.
 *
 * Two details do most of the work here:
 *
 *  - `pixels_per_row` is set from the ring's stride, not the frame width. The
 *    slot's rows are padded for alignment, and telling the GPU about the pitch
 *    means the padding can be copied along with everything else as one
 *    contiguous block rather than row by row.
 *
 *  - `cycle` is true on both the map and the upload. That asks SDL for a fresh
 *    internal buffer when the previous one is still in flight, which is what
 *    stops the upload blocking on the frame the GPU is currently reading.
 */
#include "present_internal.h"

#include <stdio.h>
#include <string.h>

struct gpu_state {
    SDL_Window            *win;
    SDL_GPUDevice         *dev;
    bool                   owns_device;   /* false when borrowed from a sibling */
    SDL_GPUTexture        *tex;
    SDL_GPUTransferBuffer *xfer;

    uint32_t tex_w, tex_h;
    uint32_t xfer_bytes;
    uint32_t src_w, src_h, src_stride;
    bool     have_frame;

    /*
     * Set the first time a damage frame arrives, and then latched.
     *
     * The texture is persistent either way, but a whole-frame upload cycles it
     * - takes a fresh backing and rewrites every pixel - which is faster and
     * cannot stall. A damage frame paints only its rectangles, so the texture
     * must keep what it already holds, which means not cycling it and wearing
     * the occasional stall against the present reading it. Switching the whole
     * window to the no-cycle path on the first damage frame keeps the fast
     * path exactly as it was whenever damage is off.
     */
    bool     accumulate;

    uint64_t ns_upload, ns_present;
};

static const char *gpu_driver(void *impl)
{
    const struct gpu_state *p = impl;
    const char *drv = p->dev ? SDL_GetGPUDeviceDriver(p->dev) : NULL;
    return drv ? drv : "gpu";
}

static void *gpu_create(SDL_Window *win, void *share_impl)
{
    struct gpu_state *p = SDL_calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->win = win;

    if (share_impl) {
        p->dev = ((struct gpu_state *)share_impl)->dev;
        p->owns_device = false;
    } else {
        p->dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV |
                                     SDL_GPU_SHADERFORMAT_DXIL  |
                                     SDL_GPU_SHADERFORMAT_MSL,
                                     false, NULL);
        p->owns_device = true;
    }
    if (!p->dev) {
        fprintf(stderr, "vypr: SDL_CreateGPUDevice: %s\n", SDL_GetError());
        SDL_free(p);
        return NULL;
    }
    if (!SDL_ClaimWindowForGPUDevice(p->dev, win)) {
        fprintf(stderr, "vypr: ClaimWindowForGPUDevice: %s\n", SDL_GetError());
        if (p->owns_device) SDL_DestroyGPUDevice(p->dev);
        SDL_free(p);
        return NULL;
    }

    /*
     * MAILBOX where the driver offers it, VSYNC where it does not.
     *
     * This used to be VSYNC to avoid tearing, which is the right goal and the
     * wrong mode to rule out: MAILBOX does not tear. It still waits for the
     * vertical blank; it differs only in that a newer image replaces one still
     * waiting, rather than queueing behind it. IMMEDIATE is the mode that
     * tears. With VSYNC every frame queued behind whatever was already
     * pending, so a frame arriving mid-refresh was shown one or two
     * refreshes later than it needed to be. The guest is the clock here, as
     * the old comment said - which is exactly why the newest frame should win.
     *
     * One frame in flight rather than SDL's default of two, for the same
     * reason: a second slot is a second frame of queue.
     */
    SDL_GPUPresentMode mode = SDL_GPU_PRESENTMODE_VSYNC;
    if (SDL_WindowSupportsGPUPresentMode(p->dev, win, SDL_GPU_PRESENTMODE_MAILBOX))
        mode = SDL_GPU_PRESENTMODE_MAILBOX;
    SDL_SetGPUSwapchainParameters(p->dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode);
    SDL_SetGPUAllowedFramesInFlight(p->dev, 1);
    fprintf(stderr, "vypr: presenting with %s, 1 frame in flight\n",
            mode == SDL_GPU_PRESENTMODE_MAILBOX ? "mailbox" : "vsync");
    return p;
}

static void gpu_destroy(void *impl)
{
    struct gpu_state *p = impl;
    if (!p) return;
    if (p->xfer) SDL_ReleaseGPUTransferBuffer(p->dev, p->xfer);
    if (p->tex)  SDL_ReleaseGPUTexture(p->dev, p->tex);
    if (p->dev) {
        SDL_ReleaseWindowFromGPUDevice(p->dev, p->win);
        if (p->owns_device) SDL_DestroyGPUDevice(p->dev);
    }
    SDL_free(p);
}

static bool ensure_resources(struct gpu_state *p, const struct vypr_frame_view *f)
{
    const uint32_t need = f->stride * f->height;

    if (p->tex && p->tex_w == f->width && p->tex_h == f->height &&
        p->xfer && p->xfer_bytes >= need)
        return true;

    if (p->tex) { SDL_ReleaseGPUTexture(p->dev, p->tex); p->tex = NULL; }
    if (p->xfer) { SDL_ReleaseGPUTransferBuffer(p->dev, p->xfer); p->xfer = NULL; }

    SDL_GPUTextureCreateInfo ti = {
        .type                 = SDL_GPU_TEXTURETYPE_2D,
        .format               = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM,
        .usage                = SDL_GPU_TEXTUREUSAGE_SAMPLER,
        .width                = f->width,
        .height               = f->height,
        .layer_count_or_depth = 1,
        .num_levels           = 1,
        .sample_count         = SDL_GPU_SAMPLECOUNT_1,
    };
    p->tex = SDL_CreateGPUTexture(p->dev, &ti);
    if (!p->tex) {
        fprintf(stderr, "vypr: CreateGPUTexture %ux%u: %s\n",
                f->width, f->height, SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo bi = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = need,
    };
    p->xfer = SDL_CreateGPUTransferBuffer(p->dev, &bi);
    if (!p->xfer) {
        fprintf(stderr, "vypr: CreateGPUTransferBuffer %u bytes: %s\n",
                need, SDL_GetError());
        return false;
    }

    p->tex_w = f->width;
    p->tex_h = f->height;
    p->xfer_bytes = need;
    return true;
}

static bool gpu_upload(void *impl, const struct vypr_frame_view *f)
{
    struct gpu_state *p = impl;
    const uint64_t t0 = SDL_GetTicksNS();

    if (f->stride % 4 != 0) return false;   /* pixels_per_row is in pixels */
    if (!ensure_resources(p, f)) return false;

    /* A damage frame can only be painted over a frame already held. If one has
     * not arrived yet - the very first frame should never be damage, but a
     * guest bug must not scribble - skip it. */
    const bool partial = (f->flags & VYPR_PUB_DAMAGE_RECTS) && f->damage_count > 0;
    if (partial && !p->have_frame) return true;
    if (partial) p->accumulate = true;

    void *dst = SDL_MapGPUTransferBuffer(p->dev, p->xfer, true);
    if (!dst) {
        fprintf(stderr, "vypr: MapGPUTransferBuffer: %s\n", SDL_GetError());
        return false;
    }
    if (partial) {
        /* Only the changed rows, each at the same byte offset it has in the
         * ring, so one pitch describes both sides below. */
        for (uint32_t r = 0; r < f->damage_count; r++) {
            const struct vypr_rect d = f->damage[r];
            for (uint32_t y = d.y; y < d.y + d.h; y++) {
                const size_t o = (size_t)y * f->stride + (size_t)d.x * 4;
                memcpy((uint8_t *)dst + o, f->pixels + o, (size_t)d.w * 4);
            }
        }
    } else {
        /* One contiguous copy, padding included - cheaper than skipping it. */
        memcpy(dst, f->pixels, (size_t)f->stride * f->height);
    }
    SDL_UnmapGPUTransferBuffer(p->dev, p->xfer);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(p->dev);
    if (!cmd) return false;

    /* Once a window is accumulating, never cycle: cycling hands back a fresh
     * texture and the previous frame with it. */
    const bool cycle = !p->accumulate;
    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
    if (partial) {
        for (uint32_t r = 0; r < f->damage_count; r++) {
            const struct vypr_rect d = f->damage[r];
            SDL_GPUTextureTransferInfo src = {
                .transfer_buffer = p->xfer,
                .offset          = (Uint32)((size_t)d.y * f->stride + (size_t)d.x * 4),
                .pixels_per_row  = f->stride / 4,
                .rows_per_layer  = d.h,
            };
            SDL_GPUTextureRegion dstr = {
                .texture = p->tex,
                .x = d.x, .y = d.y, .w = d.w, .h = d.h, .d = 1,
            };
            SDL_UploadToGPUTexture(copy, &src, &dstr, false);
        }
    } else {
        SDL_GPUTextureTransferInfo src = {
            .transfer_buffer = p->xfer,
            .offset          = 0,
            .pixels_per_row  = f->stride / 4,
            .rows_per_layer  = f->height,
        };
        SDL_GPUTextureRegion dstr = {
            .texture = p->tex,
            .w = f->width, .h = f->height, .d = 1,
        };
        SDL_UploadToGPUTexture(copy, &src, &dstr, cycle);
    }
    SDL_EndGPUCopyPass(copy);
    SDL_SubmitGPUCommandBuffer(cmd);

    p->src_w = f->width;
    p->src_h = f->height;
    p->src_stride = f->stride;
    p->have_frame = true;

    p->ns_upload += SDL_GetTicksNS() - t0;
    return true;
}

static void gpu_present(void *impl)
{
    struct gpu_state *p = impl;
    const uint64_t t0 = SDL_GetTicksNS();

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(p->dev);
    if (!cmd) return;

    SDL_GPUTexture *swap = NULL;
    uint32_t sw = 0, sh = 0;
    if (!SDL_WaitAndAcquireGPUSwapchainTexture(cmd, p->win, &swap, &sw, &sh) || !swap) {
        /* Window is minimised or the swapchain is being rebuilt. The command
         * buffer still has to be submitted or it leaks. */
        SDL_SubmitGPUCommandBuffer(cmd);
        p->ns_present += SDL_GetTicksNS() - t0;
        return;
    }

    if (p->have_frame && p->tex) {
        int dx, dy, dw, dh;
        vypr_fit_rect((int)sw, (int)sh, p->src_w, p->src_h, &dx, &dy, &dw, &dh);

        SDL_GPUBlitInfo blit = {
            .source      = { .texture = p->tex, .w = p->src_w, .h = p->src_h },
            .destination = { .texture = swap,
                             .x = (Uint32)dx, .y = (Uint32)dy,
                             .w = (Uint32)dw, .h = (Uint32)dh },
            /* CLEAR, not DONT_CARE: the margin either side of a fitted picture
             * is never written by the blit, and whatever the swapchain last
             * held would show through it. */
            .load_op     = SDL_GPU_LOADOP_CLEAR,
            .clear_color = (SDL_FColor){ 0.0f, 0.0f, 0.0f, 1.0f },
            .filter      = SDL_GPU_FILTER_LINEAR,
        };
        SDL_BlitGPUTexture(cmd, &blit);
    }

    SDL_SubmitGPUCommandBuffer(cmd);
    p->ns_present += SDL_GetTicksNS() - t0;
}

static void gpu_take_timings(void *impl, uint64_t *upload_ns, uint64_t *present_ns)
{
    struct gpu_state *p = impl;
    if (upload_ns)  *upload_ns  = p->ns_upload;
    if (present_ns) *present_ns = p->ns_present;
    p->ns_upload = p->ns_present = 0;
}

const struct present_ops present_gpu_ops = {
    .name         = "gpu",
    .create       = gpu_create,
    .destroy      = gpu_destroy,
    .driver       = gpu_driver,
    .upload       = gpu_upload,
    .present      = gpu_present,
    .take_timings = gpu_take_timings,
};
