/*
 * Getting a frame from the shared region onto the screen.
 *
 * Two implementations, because the cost of this step decides whether 4K is
 * usable at all. The measured split at 3840x2160 was 16 ms of upload against
 * 14.5 GB/s of available memory bandwidth - a factor of seven off what the
 * hardware can do, and enough on its own to miss vsync.
 *
 *   "render"  SDL_Renderer streaming textures. Simple, and what the first
 *             version used. LockTexture hands back a staging buffer, so the
 *             frame is copied twice before it reaches the GPU.
 *   "gpu"     SDL_GPU transfer buffers - a persistently mappable upload heap.
 *             One memcpy straight into memory the GPU will DMA from.
 */
#ifndef VYPR_HOST_PRESENT_H
#define VYPR_HOST_PRESENT_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "shm.h"

struct presenter;

/*
 * Where the guest's picture goes inside the host window.
 *
 * The two are usually the same shape, because resizing the host window asks the
 * guest window to follow - but only usually. A guest window has a minimum size,
 * a maximised one cannot be resized at all, and plenty of applications simply
 * refuse the size they are given. When that happens the host window keeps the
 * shape the user dragged it to and the guest keeps its own, and stretching one
 * onto the other distorts the picture for as long as they disagree.
 *
 * So the picture is fitted rather than stretched: same aspect ratio, centred,
 * with whatever is left over as empty margin. Distortion is always wrong;
 * a margin is only ever temporary.
 *
 * Input is mapped through the same rectangle, or a click would land where the
 * pointer is not.
 */
static inline void vypr_fit_rect(int win_w, int win_h,
                                 uint32_t src_w, uint32_t src_h,
                                 int *x, int *y, int *w, int *h)
{
    if (win_w <= 0 || win_h <= 0 || src_w == 0 || src_h == 0) {
        *x = *y = 0;
        *w = win_w > 0 ? win_w : 0;
        *h = win_h > 0 ? win_h : 0;
        return;
    }

    /* Compare width/height as a cross-multiplication, so this stays exact and
     * needs no floating point. */
    if ((int64_t)win_w * src_h > (int64_t)win_h * src_w) {
        *h = win_h;                                       /* height-limited */
        *w = (int)(((int64_t)win_h * src_w) / src_h);
    } else {
        *w = win_w;                                       /* width-limited */
        *h = (int)(((int64_t)win_w * src_h) / src_w);
    }
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;

    *x = (win_w - *w) / 2;
    *y = (win_h - *h) / 2;
}

/* `backend` is "gpu", "render", or NULL for the default.
 *
 * `share` may name an existing presenter to borrow GPU state from. Creating a
 * GPU device costs milliseconds, which is fine once per window and far too slow
 * for a menu that has to appear the instant it opens - so popups share their
 * owner's device rather than standing one up each time. */
struct presenter *presenter_create(SDL_Window *win, const char *backend,
                                   struct presenter *share);
void              presenter_destroy(struct presenter *p);
const char       *presenter_name(const struct presenter *p);

bool presenter_upload(struct presenter *p, const struct vypr_frame_view *f);
void presenter_present(struct presenter *p);

/* Nanoseconds since the last call, then reset. */
void presenter_take_timings(struct presenter *p, uint64_t *upload_ns, uint64_t *present_ns);

#endif
