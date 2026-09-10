/*
 * Offering a drag to the rest of the desktop.
 *
 * SDL receives drops and cannot start one: SDL_EVENT_DROP_* is entirely
 * inbound, and there is no call anywhere in SDL to say "I am now dragging
 * these files". So this goes under SDL to the display server, which means one
 * backend per server and a small interface over the top.
 *
 * Callers need no #ifdefs. On a build or a session with no usable backend,
 * drag_source_create still returns a handle; drag_source_start on it refuses
 * and says why, once.
 */
#ifndef VYPR_DRAGSOURCE_H
#define VYPR_DRAGSOURCE_H

#include <stdbool.h>
#include <stddef.h>
#include <SDL3/SDL.h>

struct drag_source;

/* Bind to the window a drag will originate from. Never returns NULL for want
 * of a backend - only for want of memory. */
struct drag_source *drag_source_create(SDL_Window *win);
void drag_source_destroy(struct drag_source *ds);

/* "wayland", "x11", or "none" - for the startup line and for vypr doctor. */
const char *drag_source_backend(const struct drag_source *ds);

/*
 * Offer these paths to the desktop as a drag of files.
 *
 * The paths are absolute host paths; they are turned into a text/uri-list
 * here, which is the one type every Linux file manager and application agrees
 * on. Returns false when there is no backend, when a drag is already running,
 * or when the server refused to start one.
 */
bool drag_source_start(struct drag_source *ds, const char *const *paths, int count);

/* Once a frame. Flushes the connection and clears the drag once it ends. */
void drag_source_pump(struct drag_source *ds);

/* True between a successful start and the drop or cancellation that ends it. */
bool drag_source_active(const struct drag_source *ds);

#endif /* VYPR_DRAGSOURCE_H */
