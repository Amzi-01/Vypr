/*
 * The Wayland drag source.
 *
 * Starting a drag needs two things SDL will not give us: a wl_data_device,
 * which comes from a wl_seat, and the serial of the button press that began
 * the gesture. SDL exposes the wl_display and the wl_surface and no seat at
 * all - not per window, not in the global properties.
 *
 * So we take SDL's connection and bind our own. A second wl_seat binding is
 * still the same seat, and the compositor delivers input to every binding of
 * it, which means our own wl_pointer sees the button presses SDL is also
 * seeing, and the serial it hands us is one the compositor will accept as an
 * implicit grab. Verified against KWin 6.7.4.
 *
 * Nothing here dispatches the display. SDL already does that every frame, and
 * two dispatchers on one connection is a way to lose events.
 */
#include "dragsource_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>

struct wl_state {
    struct wl_display             *dpy;
    struct wl_surface             *surface;
    struct wl_registry            *registry;
    struct wl_seat                *seat;
    struct wl_pointer             *pointer;
    struct wl_data_device_manager *ddm;
    struct wl_data_device         *dd;
    struct wl_data_source         *source;
    uint32_t ddm_version;
    uint32_t grab_serial;          /* last button press we saw */
    struct drag_source *owner;
};

/* ------------------------------------------------------------- data source */

static void source_target(void *data, struct wl_data_source *s, const char *mime) {}

static void source_send(void *data, struct wl_data_source *s,
                        const char *mime, int32_t fd)
{
    struct wl_state *w = data;
    const char *p = w->owner->uri_list;
    size_t left = w->owner->uri_len;

    /* A pipe, so a short write is ordinary rather than an error. */
    while (left) {
        ssize_t n = write(fd, p, left);
        if (n > 0) { p += n; left -= (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fd);
}

static void source_end(struct wl_state *w)
{
    if (w->source) {
        wl_data_source_destroy(w->source);
        w->source = NULL;
    }
    w->owner->active = false;
}

static void source_cancelled(void *data, struct wl_data_source *s)
{
    source_end(data);
}
static void source_dnd_drop_performed(void *data, struct wl_data_source *s) {}
static void source_dnd_finished(void *data, struct wl_data_source *s)
{
    source_end(data);
}
static void source_action(void *data, struct wl_data_source *s, uint32_t action) {}

static const struct wl_data_source_listener source_listener = {
    .target             = source_target,
    .send               = source_send,
    .cancelled          = source_cancelled,
    .dnd_drop_performed = source_dnd_drop_performed,
    .dnd_finished       = source_dnd_finished,
    .action             = source_action,
};

/* ----------------------------------------------------------------- pointer */
/*
 * We listen only to know the serial of the most recent press. Everything the
 * user actually does with the pointer is SDL's business, and this binding does
 * not take any of it away: a seat delivers to all of its bindings.
 */
static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {}
static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s) {}
static void ptr_motion(void *data, struct wl_pointer *p, uint32_t t,
                       wl_fixed_t x, wl_fixed_t y) {}
static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
                       uint32_t time, uint32_t button, uint32_t state)
{
    struct wl_state *w = data;
    if (state == WL_POINTER_BUTTON_STATE_PRESSED) w->grab_serial = serial;
}
static void ptr_axis(void *data, struct wl_pointer *p, uint32_t t, uint32_t a, wl_fixed_t v) {}
static void ptr_frame(void *data, struct wl_pointer *p) {}
static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t s) {}
static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t t, uint32_t a) {}
static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t a, int32_t v) {}
static void ptr_axis_value120(void *data, struct wl_pointer *p, uint32_t a, int32_t v) {}
static void ptr_axis_relative_direction(void *data, struct wl_pointer *p,
                                        uint32_t a, uint32_t dir) {}

static const struct wl_pointer_listener ptr_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion,
    .button = ptr_button, .axis = ptr_axis, .frame = ptr_frame,
    .axis_source = ptr_axis_source, .axis_stop = ptr_axis_stop,
    .axis_discrete = ptr_axis_discrete, .axis_value120 = ptr_axis_value120,
    .axis_relative_direction = ptr_axis_relative_direction,
};

/* -------------------------------------------------------------------- seat */

static void seat_caps(void *data, struct wl_seat *seat, uint32_t caps)
{
    struct wl_state *w = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !w->pointer) {
        w->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(w->pointer, &ptr_listener, w);
    }
}
static void seat_name(void *data, struct wl_seat *seat, const char *name) {}
static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps, .name = seat_name,
};

/* ---------------------------------------------------------------- registry */

static void reg_global(void *data, struct wl_registry *reg, uint32_t id,
                       const char *iface, uint32_t version)
{
    struct wl_state *w = data;
    if (!strcmp(iface, wl_seat_interface.name) && !w->seat) {
        uint32_t v = version < 7 ? version : 7;
        w->seat = wl_registry_bind(reg, id, &wl_seat_interface, v);
        wl_seat_add_listener(w->seat, &seat_listener, w);
    } else if (!strcmp(iface, wl_data_device_manager_interface.name) && !w->ddm) {
        w->ddm_version = version < 3 ? version : 3;
        w->ddm = wl_registry_bind(reg, id, &wl_data_device_manager_interface,
                                  w->ddm_version);
    }
}
static void reg_global_remove(void *data, struct wl_registry *reg, uint32_t id) {}
static const struct wl_registry_listener reg_listener = {
    .global = reg_global, .global_remove = reg_global_remove,
};

/* ------------------------------------------------------------------- hooks */

static bool wl_start(struct drag_source *ds)
{
    struct wl_state *w = ds->impl;

    /* No press seen yet means no implicit grab, and the compositor would
     * reject the serial. Nothing has gone wrong; there is just nothing to
     * hang a drag on. */
    if (!w->grab_serial) return false;

    w->source = wl_data_device_manager_create_data_source(w->ddm);
    if (!w->source) return false;
    wl_data_source_add_listener(w->source, &source_listener, w);

    wl_data_source_offer(w->source, "text/uri-list");
    /* Some older toolkits ask for this spelling instead, and it costs a line. */
    wl_data_source_offer(w->source, "text/x-moz-url");
    if (w->ddm_version >= 3)
        wl_data_source_set_actions(w->source, WL_DATA_DEVICE_MANAGER_DND_ACTION_COPY);

    wl_data_device_start_drag(w->dd, w->source, w->surface, NULL, w->grab_serial);
    wl_display_flush(w->dpy);
    return true;
}

static void wl_pump(struct drag_source *ds)
{
    struct wl_state *w = ds->impl;
    if (ds->active) wl_display_flush(w->dpy);
}

static void wl_destroy(struct drag_source *ds)
{
    struct wl_state *w = ds->impl;
    if (!w) return;
    if (w->source)  wl_data_source_destroy(w->source);
    if (w->dd)      wl_data_device_destroy(w->dd);
    if (w->pointer) wl_pointer_release(w->pointer);
    if (w->seat)    wl_seat_release(w->seat);
    if (w->ddm)     wl_data_device_manager_destroy(w->ddm);
    if (w->registry) wl_registry_destroy(w->registry);
    free(w);
    ds->impl = NULL;
}

bool vypr_drag_wayland_init(struct drag_source *ds)
{
    struct wl_display *dpy = SDL_GetPointerProperty(
        SDL_GetGlobalProperties(), SDL_PROP_GLOBAL_VIDEO_WAYLAND_WL_DISPLAY_POINTER, NULL);
    struct wl_surface *surface = SDL_GetPointerProperty(
        SDL_GetWindowProperties(ds->win), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    if (!dpy || !surface) return false;

    struct wl_state *w = calloc(1, sizeof(*w));
    if (!w) return false;
    w->dpy = dpy;
    w->surface = surface;
    w->owner = ds;

    w->registry = wl_display_get_registry(dpy);
    if (!w->registry) { free(w); return false; }
    wl_registry_add_listener(w->registry, &reg_listener, w);

    /* Two round trips: the first brings the globals, the second the seat's
     * capabilities, which is what tells us there is a pointer to listen to. */
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);

    if (!w->seat || !w->ddm) {
        fprintf(stderr, "vypr: no %s on this compositor; files cannot be dragged out\n",
                w->seat ? "wl_data_device_manager" : "wl_seat");
        goto fail;
    }

    w->dd = wl_data_device_manager_get_data_device(w->ddm, w->seat);
    if (!w->dd) goto fail;

    ds->impl    = w;
    ds->start   = wl_start;
    ds->pump    = wl_pump;
    ds->destroy = wl_destroy;
    return true;

fail:
    if (w->pointer) wl_pointer_release(w->pointer);
    if (w->seat)    wl_seat_release(w->seat);
    if (w->ddm)     wl_data_device_manager_destroy(w->ddm);
    wl_registry_destroy(w->registry);
    free(w);
    return false;
}
