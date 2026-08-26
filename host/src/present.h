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
#ifndef SASH_HOST_PRESENT_H
#define SASH_HOST_PRESENT_H

#include <SDL3/SDL.h>
#include <stdbool.h>
#include <stdint.h>

#include "shm.h"

struct presenter;

/* `backend` is "gpu", "render", or NULL for the default. */
struct presenter *presenter_create(SDL_Window *win, const char *backend);
void              presenter_destroy(struct presenter *p);
const char       *presenter_name(const struct presenter *p);

bool presenter_upload(struct presenter *p, const struct sash_frame_view *f);
void presenter_present(struct presenter *p);

/* Nanoseconds since the last call, then reset. */
void presenter_take_timings(struct presenter *p, uint64_t *upload_ns, uint64_t *present_ns);

#endif
