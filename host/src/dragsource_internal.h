/*
 * Shared between dragsource.c and the per-server backends.
 *
 * The generic half owns the payload and the "is a drag running" flag; a backend
 * owns its connection and fills in the three function pointers. A backend that
 * cannot attach leaves them null and the generic half refuses politely.
 */
#ifndef VYPR_DRAGSOURCE_INTERNAL_H
#define VYPR_DRAGSOURCE_INTERNAL_H

#include "dragsource.h"

struct drag_source {
    SDL_Window *win;
    const char *backend;      /* static string, never freed */

    /* The current offer, as a text/uri-list. Held for the life of the drag
     * because the far side asks for it after the fact, on its own schedule. */
    char  *uri_list;
    size_t uri_len;

    bool active;
    bool refused_once;        /* so a session with no backend says so once */

    void *impl;               /* backend state */
    bool (*start)(struct drag_source *ds);
    void (*pump)(struct drag_source *ds);
    void (*destroy)(struct drag_source *ds);
};

/* Each returns true when it attached and filled in the hooks above. */
bool vypr_drag_wayland_init(struct drag_source *ds);

#endif /* VYPR_DRAGSOURCE_INTERNAL_H */
