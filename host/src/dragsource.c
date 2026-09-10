/*
 * The server-independent half of offering a drag.
 *
 * Turns paths into the one thing every Linux application understands - a
 * text/uri-list - and hands it to whichever backend attached.
 */
#include "dragsource_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * Percent-encoding for a file: URI.
 *
 * RFC 3986 unreserved characters pass through, and so does '/', because it is
 * the path separator rather than data. Everything else goes out as %XX -
 * spaces above all, since a raw space in a uri-list is a parse error on the
 * far side rather than a filename with a space in it.
 */
static void uri_encode(const char *s, char **out)
{
    static const char hex[] = "0123456789ABCDEF";
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
            (*p >= '0' && *p <= '9') ||
            *p == '-' || *p == '_' || *p == '.' || *p == '~' || *p == '/') {
            *(*out)++ = (char)*p;
        } else {
            *(*out)++ = '%';
            *(*out)++ = hex[*p >> 4];
            *(*out)++ = hex[*p & 0xf];
        }
    }
}

static char *build_uri_list(const char *const *paths, int count, size_t *len_out)
{
    /* Worst case every byte triples, plus "file://" and a CRLF per entry. */
    size_t cap = 1;
    for (int i = 0; i < count; i++)
        cap += strlen(paths[i]) * 3 + sizeof("file://") + 2;

    char *buf = malloc(cap);
    if (!buf) return NULL;

    char *w = buf;
    for (int i = 0; i < count; i++) {
        memcpy(w, "file://", 7); w += 7;
        uri_encode(paths[i], &w);
        /* CRLF: the uri-list spec says so, and some toolkits are strict. */
        *w++ = '\r'; *w++ = '\n';
    }
    *w = '\0';
    *len_out = (size_t)(w - buf);
    return buf;
}

struct drag_source *drag_source_create(SDL_Window *win)
{
    struct drag_source *ds = calloc(1, sizeof(*ds));
    if (!ds) return NULL;
    ds->win = win;
    ds->backend = "none";

    const char *driver = SDL_GetCurrentVideoDriver();
    (void)driver;
#ifdef VYPR_HAVE_WAYLAND
    if (driver && !strcmp(driver, "wayland")) {
        if (vypr_drag_wayland_init(ds)) ds->backend = "wayland";
    }
#endif
    /* x11 goes here, once XDND is written. */

    return ds;
}

void drag_source_destroy(struct drag_source *ds)
{
    if (!ds) return;
    if (ds->destroy) ds->destroy(ds);
    free(ds->uri_list);
    free(ds);
}

const char *drag_source_backend(const struct drag_source *ds)
{
    return ds ? ds->backend : "none";
}

bool drag_source_active(const struct drag_source *ds)
{
    return ds && ds->active;
}

bool drag_source_start(struct drag_source *ds, const char *const *paths, int count)
{
    if (!ds || count <= 0) return false;

    if (!ds->start) {
        if (!ds->refused_once) {
            ds->refused_once = true;
            fprintf(stderr, "vypr: dragging files out needs a display server this "
                            "build can offer drags on; '%s' is not one\n",
                    SDL_GetCurrentVideoDriver() ? SDL_GetCurrentVideoDriver() : "?");
        }
        return false;
    }

    /* One at a time. The far side has no way to tell two concurrent offers
     * apart, and neither would the user. */
    if (ds->active) return false;

    size_t len = 0;
    char *list = build_uri_list(paths, count, &len);
    if (!list) return false;

    free(ds->uri_list);
    ds->uri_list = list;
    ds->uri_len  = len;

    if (!ds->start(ds)) {
        free(ds->uri_list);
        ds->uri_list = NULL;
        ds->uri_len = 0;
        return false;
    }
    ds->active = true;
    return true;
}

void drag_source_pump(struct drag_source *ds)
{
    if (ds && ds->pump) ds->pump(ds);
}
