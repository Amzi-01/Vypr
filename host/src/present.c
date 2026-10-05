#include "present_internal.h"

#include <stdio.h>
#include <string.h>

struct presenter {
    const struct present_ops *ops;
    void                     *impl;
};

struct presenter *presenter_create(SDL_Window *win, const char *backend,
                                   struct presenter *share)
{
    /*
     * Fastest first, each falling back to the next: Vulkan needs the host
     * pointer import and a window created for Vulkan; SDL_GPU needs a working
     * Vulkan or D3D12; the renderer always works. Falling back beats refusing
     * to show the window at all. A named backend starts the chain there.
     */
    const struct present_ops *chain[3];
    int n = 0;
#ifdef VYPR_HAVE_VULKAN
    if (!backend || !strcmp(backend, "vulkan") || !strcmp(backend, "vk"))
        chain[n++] = &present_vk_ops;
#endif
    if (!backend || strcmp(backend, "render"))
        chain[n++] = &present_gpu_ops;
    chain[n++] = &present_render_ops;

    struct presenter *p = SDL_calloc(1, sizeof(*p));
    if (!p) return NULL;

    for (int i = 0; i < n && !p->impl; i++) {
        if (i > 0)
            fprintf(stderr, "vypr: '%s' backend unavailable, falling back to '%s'\n",
                    chain[i - 1]->name, chain[i]->name);
        /* Sharing only makes sense between the same backend. */
        p->impl = chain[i]->create(win, share && share->ops == chain[i] ? share->impl : NULL);
        p->ops  = chain[i];
    }
    if (!p->impl) { SDL_free(p); return NULL; }
    return p;
}

void presenter_destroy(struct presenter *p)
{
    if (!p) return;
    p->ops->destroy(p->impl);
    SDL_free(p);
}

const char *presenter_name(const struct presenter *p) { return p->ops->driver(p->impl); }

bool presenter_upload(struct presenter *p, const struct vypr_frame_view *f)
{
    return p->ops->upload(p->impl, f);
}

void presenter_present(struct presenter *p) { p->ops->present(p->impl); }

void presenter_take_timings(struct presenter *p, uint64_t *upload_ns, uint64_t *present_ns)
{
    p->ops->take_timings(p->impl, upload_ns, present_ns);
}
