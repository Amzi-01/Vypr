/* Backend contract. Two implementations link side by side so they can be
 * compared on the same frames rather than across separate builds. */
#ifndef SASH_HOST_PRESENT_INTERNAL_H
#define SASH_HOST_PRESENT_INTERNAL_H

#include "present.h"

struct present_ops {
    const char *name;
    void       *(*create)(SDL_Window *win);
    void        (*destroy)(void *impl);
    const char *(*driver)(void *impl);
    bool        (*upload)(void *impl, const struct sash_frame_view *f);
    void        (*present)(void *impl);
    void        (*take_timings)(void *impl, uint64_t *upload_ns, uint64_t *present_ns);
};

extern const struct present_ops present_gpu_ops;
extern const struct present_ops present_render_ops;

#endif
