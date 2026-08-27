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
    const struct present_ops *ops = &present_gpu_ops;
    if (backend && !strcmp(backend, "render")) ops = &present_render_ops;

    /* Sharing only makes sense between the same backend. */
    if (share && share->ops != ops) share = NULL;

    struct presenter *p = SDL_calloc(1, sizeof(*p));
    if (!p) return NULL;

    p->impl = ops->create(win, share ? share->impl : NULL);
    if (!p->impl) {
        /* The GPU backend needs a working Vulkan or D3D12; falling back beats
         * refusing to show the window at all. */
        if (ops != &present_render_ops) {
            fprintf(stderr, "vypr: '%s' backend unavailable, falling back to 'render'\n",
                    ops->name);
            ops = &present_render_ops;
            p->impl = ops->create(win, NULL);
        }
        if (!p->impl) { SDL_free(p); return NULL; }
    }
    p->ops = ops;
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
