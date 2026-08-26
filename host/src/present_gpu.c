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
    SDL_GPUTexture        *tex;
    SDL_GPUTransferBuffer *xfer;

    uint32_t tex_w, tex_h;
    uint32_t xfer_bytes;
    uint32_t src_w, src_h, src_stride;
    bool     have_frame;

    uint64_t ns_upload, ns_present;
};

static const char *gpu_driver(void *impl)
{
    const struct gpu_state *p = impl;
    const char *drv = p->dev ? SDL_GetGPUDeviceDriver(p->dev) : NULL;
    return drv ? drv : "gpu";
}

static void *gpu_create(SDL_Window *win)
{
    struct gpu_state *p = SDL_calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->win = win;
    p->dev = SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV |
                                 SDL_GPU_SHADERFORMAT_DXIL  |
                                 SDL_GPU_SHADERFORMAT_MSL,
                                 false, NULL);
    if (!p->dev) {
        fprintf(stderr, "sash: SDL_CreateGPUDevice: %s\n", SDL_GetError());
        SDL_free(p);
        return NULL;
    }
    if (!SDL_ClaimWindowForGPUDevice(p->dev, win)) {
        fprintf(stderr, "sash: ClaimWindowForGPUDevice: %s\n", SDL_GetError());
        SDL_DestroyGPUDevice(p->dev);
        SDL_free(p);
        return NULL;
    }

    /* VSYNC rather than MAILBOX: the guest is the clock here, and tearing a
     * frame that cost a PCIe crossing to deliver is a poor trade. */
    SDL_SetGPUSwapchainParameters(p->dev, win, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                  SDL_GPU_PRESENTMODE_VSYNC);
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
        SDL_DestroyGPUDevice(p->dev);
    }
    SDL_free(p);
}

static bool ensure_resources(struct gpu_state *p, const struct sash_frame_view *f)
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
        fprintf(stderr, "sash: CreateGPUTexture %ux%u: %s\n",
                f->width, f->height, SDL_GetError());
        return false;
    }

    SDL_GPUTransferBufferCreateInfo bi = {
        .usage = SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD,
        .size  = need,
    };
    p->xfer = SDL_CreateGPUTransferBuffer(p->dev, &bi);
    if (!p->xfer) {
        fprintf(stderr, "sash: CreateGPUTransferBuffer %u bytes: %s\n",
                need, SDL_GetError());
        return false;
    }

    p->tex_w = f->width;
    p->tex_h = f->height;
    p->xfer_bytes = need;
    return true;
}

static bool gpu_upload(void *impl, const struct sash_frame_view *f)
{
    struct gpu_state *p = impl;
    const uint64_t t0 = SDL_GetTicksNS();

    if (f->stride % 4 != 0) return false;   /* pixels_per_row is in pixels */
    if (!ensure_resources(p, f)) return false;

    void *dst = SDL_MapGPUTransferBuffer(p->dev, p->xfer, true);
    if (!dst) {
        fprintf(stderr, "sash: MapGPUTransferBuffer: %s\n", SDL_GetError());
        return false;
    }
    /* One contiguous copy, padding included - cheaper than skipping it. */
    memcpy(dst, f->pixels, (size_t)f->stride * f->height);
    SDL_UnmapGPUTransferBuffer(p->dev, p->xfer);

    SDL_GPUCommandBuffer *cmd = SDL_AcquireGPUCommandBuffer(p->dev);
    if (!cmd) return false;

    SDL_GPUCopyPass *copy = SDL_BeginGPUCopyPass(cmd);
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
    SDL_UploadToGPUTexture(copy, &src, &dstr, true);
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
        SDL_GPUBlitInfo blit = {
            .source      = { .texture = p->tex,  .w = p->src_w, .h = p->src_h },
            .destination = { .texture = swap,    .w = sw,       .h = sh },
            .load_op     = SDL_GPU_LOADOP_DONT_CARE,
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
