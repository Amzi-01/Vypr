/*
 * vyprd - the host session daemon.
 *
 * Owns three things the per-window clients must not each own separately:
 * the shared region and its allocation, the single control link to the guest
 * agent, and the decision about which guest windows become host windows.
 *
 *   agent (TCP 47820) ──▶ vyprd ──▶ spawns vypr-window per window
 *                          ▲                    │
 *                          └──── unix socket ───┘  (input travels back up)
 *
 * Input takes the long way round - client to daemon to agent - rather than each
 * window client holding its own link to the guest. One connection means one
 * place where window identity, slot ownership and reconnect are decided.
 */
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <time.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "msg.h"
#include "shm.h"

#define MAX_WINDOWS ((int)VYPR_MAX_SLOTS)

/*
 * Clock samples, kept so the offset can come from the *best* exchange rather
 * than the most recent one.
 *
 * The round-trip estimate assumes the guest read its counter halfway through
 * the trip, so the error it can hide is up to half the round trip. That is
 * fine at 0.3 ms and worthless at 2.8 s - and round trips do reach seconds
 * when the guest is saturated by a game, which is exactly when a latency
 * figure is wanted. A slow exchange is not evidence of a changed offset, it is
 * evidence of queueing, so it is discarded in favour of a faster one.
 *
 * Samples expire so that a very good but very old sample cannot outvote
 * genuine drift forever.
 */
#define CLOCK_SAMPLES     24
#define CLOCK_MAX_AGE_NS  (60ull * 1000000000ull)
#define MAX_MATCH   8

struct window {
    uint64_t id;
    int      fullscreen;    /* covers the guest desktop */
    uint64_t owner_id;      /* nonzero for popups */
    uint32_t slot;
    int      has_slot;
    int      attached;
    int      is_popup;      /* presented by its owner's client, not its own */
    pid_t    child;
    int      client_fd;
    int32_t  gx, gy;        /* guest screen position of the client area */
    uint32_t width, height;
    uint32_t chrome_top;
    int      minimized;
    int      is_fullscreen;
    char     title[192];
};

struct daemon {
    struct vypr_shm shm;
    const char     *shm_path;

    int  tcp_listen;
    int  unix_listen;
    int  agent_fd;
    struct msg_reader agent_rx;

    /*
     * The guest opens two connections to the same port: one for control and
     * one carrying nothing but audio. Which is which is not known until the
     * first message arrives, so a new connection waits here until it says.
     */
    int  audio_fd;
    struct msg_reader audio_rx;

    /*
     * One outgoing buffer per client.
     *
     * These sockets used to be blocking, and msg_send loops until everything
     * is written - so a client that was slow to read stalled the whole daemon
     * inside a write, including the guest's audio connection. The guest kept
     * sending, the kernel buffered megabytes of it, and when the client caught
     * up the backlog arrived as one burst that the far end could only discard.
     * Measured at twenty seconds of silence followed by 1745 packets landing
     * inside a millisecond of each other.
     *
     * Now nothing is written straight to a client. Messages are queued here and
     * flushed when the socket says it can take them, so one slow reader costs
     * only its own audio.
     */
    struct out_q {
        int      fd;
        uint8_t *buf;
        size_t   len, cap;
        uint64_t audio_dropped;
    } out[MAX_WINDOWS];

    struct {
        int fd;
        struct msg_reader rx;
        uint64_t at_ns;
    } pending[4];

    char unix_path[108];   /* sun_path is 108; anything longer truncates */

    struct window windows[MAX_WINDOWS];

    const char *match[MAX_MATCH];
    int         match_count;
    int         match_all;
    const char *launch;

    /*
     * Windows the user closed.
     *
     * The guest re-offers any window nothing is streaming, so an app that
     * ignores WM_CLOSE - which games routinely do - would have its window
     * reappear a few seconds after being closed. Remembering the dismissal
     * keeps it shut until the guest window genuinely goes away.
     */
    uint64_t    dismissed[MAX_WINDOWS];
    int         dismissed_count;

    int         clock_logged;
    uint64_t    last_ping_ns;

    struct clock_sample {
        uint64_t rtt_ns;
        int64_t  offset_ns;
        uint64_t at_ns;
    } clock[CLOCK_SAMPLES];
    int      clock_next;
    uint64_t clock_best_rtt;

    char self_dir[512];
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* ------------------------------------------------------------------ helpers */

static struct window *window_find(struct daemon *d, uint64_t id)
{
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (d->windows[i].id == id) return &d->windows[i];
    return NULL;
}

static struct window *window_alloc(struct daemon *d, uint64_t id)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (d->windows[i].id == 0) {
            memset(&d->windows[i], 0, sizeof(d->windows[i]));
            d->windows[i].id = id;
            d->windows[i].client_fd = -1;
            return &d->windows[i];
        }
    }
    return NULL;
}

static int is_dismissed(const struct daemon *d, uint64_t id)
{
    for (int i = 0; i < d->dismissed_count; i++)
        if (d->dismissed[i] == id) return 1;
    return 0;
}

static void forget_dismissed(struct daemon *d, uint64_t id)
{
    for (int i = 0; i < d->dismissed_count; i++) {
        if (d->dismissed[i] == id) {
            d->dismissed[i] = d->dismissed[--d->dismissed_count];
            return;
        }
    }
}

/*
 * Reduce a window title to letters and digits, lowercased.
 *
 * Titles cannot be matched as they are written. Call of Duty's window title
 * carries fifty-two U+200B ZERO WIDTH SPACE characters, one between every
 * visible character, so the title that reads "Call of Duty: Modern Warfare II"
 * contains the substring "Call" nowhere in it. Games do this deliberately, and
 * it is invisible in any log you print it to.
 *
 * The same pass takes care of the ordinary reasons a title does not match what
 * a person typed: the registered-trademark signs, the colon, and the spacing.
 * Everything outside [a-z0-9] is dropped from both sides, so multi-byte
 * sequences - which is what the zero-width spaces and the ® are - disappear
 * byte by byte without needing to be decoded.
 */
static void title_key(const char *in, char *out, size_t cap)
{
    size_t j = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && j + 1 < cap; p++)
        if (isalnum(*p)) out[j++] = (char)tolower(*p);
    out[j] = '\0';
}

static int title_matches(struct daemon *d, const char *title)
{
    if (d->match_all) return 1;
    if (d->match_count == 0) return 0;

    char tkey[1024];
    title_key(title, tkey, sizeof tkey);

    for (int i = 0; i < d->match_count; i++) {
        char pkey[256];
        title_key(d->match[i], pkey, sizeof pkey);
        if (pkey[0] && strstr(tkey, pkey)) return 1;
    }
    return 0;
}

static void window_release(struct daemon *d, struct window *w, int tell_agent)
{
    if (!w || w->id == 0) return;

    if (w->child > 0) {
        kill(w->child, SIGTERM);
        w->child = 0;
    }
    if (w->client_fd >= 0 && !w->is_popup) {
        close(w->client_fd);
    }
    w->client_fd = -1;
    if (tell_agent && d->agent_fd >= 0 && w->attached) {
        struct vypr_msg_window_id gone = { .window_id = w->id };
        msg_send(d->agent_fd, VYPR_MSG_DETACH, &gone, sizeof(gone));
    }
    if (w->has_slot) vypr_shm_free(&d->shm, w->slot);
    memset(w, 0, sizeof(*w));
    w->client_fd = -1;
}

/* ------------------------------------------------------------- window client */

static int spawn_client(struct daemon *d, struct window *w)
{
    char exe[640], slot[16], id[32], chrome[16];
    snprintf(exe, sizeof(exe), "%s/vypr-window", d->self_dir);
    snprintf(slot, sizeof(slot), "%u", w->slot);
    snprintf(id, sizeof(id), "%" PRIu64, w->id);
    snprintf(chrome, sizeof(chrome), "%u", w->chrome_top);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        /* Direct pointer control by default, even for a fullscreen window.
         *
         * Starting captured hides the host cursor the moment the window is
         * clicked, which is right for a game in mouselook and plainly wrong for
         * everything else - including a game sitting in its own menus, which is
         * where a session begins. Capture is entered when the guest says an app
         * has taken the pointer, or when the user asks for it with
         * Ctrl+Alt+Shift+M. Guessing from "it is fullscreen" was worse than not
         * guessing. */
        char *const argv[] = {
            exe,
            "--shm",       (char *)d->shm_path,
            "--slot",      slot,
            "--title",     w->title[0] ? w->title : (char *)"vypr",
            "--window-id", id,
            "--sock",       d->unix_path,
            "--chrome-top", chrome,
            /* Direct control to begin with. Capture engages by itself when the
             * guest reports an app has taken the pointer, and Ctrl+Alt+Shift+M
             * forces it - but a captured pointer is locked in place, so while
             * it is held the window cannot be dragged and the cursor is hidden.
             * That is right for a game and wrong for everything before one. */
            "--no-capture",
            NULL
        };
        execv(exe, argv);
        fprintf(stderr, "vyprd: cannot exec %s: %s\n", exe, strerror(errno));
        _exit(127);
    }

    w->child = pid;
    fprintf(stderr, "vyprd: window '%s' -> slot %u, pid %d\n",
            w->title, w->slot, (int)pid);
    return 0;
}

/* --------------------------------------------------------------- agent input */

static void attach_window(struct daemon *d, const struct vypr_msg_window *desc,
                          const char *title)
{
    struct window *w = window_find(d, desc->window_id);
    if (w && w->has_slot) return;
    if (!w) w = window_alloc(d, desc->window_id);
    if (!w) {
        fprintf(stderr, "vyprd: no window slots left; ignoring '%s'\n", title);
        return;
    }

    snprintf(w->title, sizeof(w->title), "%s", title);
    w->width    = desc->width;
    w->height   = desc->height;
    w->gx       = desc->x;
    w->gy       = desc->y;
    w->owner_id = desc->owner_id;
    w->is_popup = (desc->flags & VYPR_WIN_POPUP) != 0 && desc->owner_id != 0;
    w->fullscreen = (desc->flags & VYPR_WIN_FULLSCREEN) != 0;
    w->chrome_top = desc->chrome_top;

    /* Headroom, so an ordinary resize does not force a re-attach. The bump
     * allocator never rewinds - reusing a freed range under a live writer is
     * how one window's pixels end up in another - so each re-attach costs
     * region permanently. */
    uint32_t alloc_w = desc->width  + 256;
    uint32_t alloc_h = desc->height + 256;

    struct vypr_msg_attach at;
    if (vypr_shm_alloc(&d->shm, w->id, alloc_w, alloc_h, &at) < 0) {
        fprintf(stderr, "vyprd: no region left for '%s'\n", title);
        memset(w, 0, sizeof(*w));
        w->client_fd = -1;
        return;
    }

    w->slot = at.slot;
    w->has_slot = 1;

    if (msg_send(d->agent_fd, VYPR_MSG_ATTACH, &at, sizeof(at)) < 0)
        fprintf(stderr, "vyprd: failed to send ATTACH for '%s'\n", title);
}


/*
 * Queue a message for a client instead of writing it.
 *
 * Control messages are always queued: dropping one would desynchronise the far
 * end, and they are small and rare. Audio is dropped once a client is far
 * enough behind, because it is realtime - a sound that cannot be delivered now
 * is worth less than the ones behind it, and letting the queue grow without
 * bound just moves the stall somewhere else.
 */
#define OUT_AUDIO_LIMIT (256 * 1024)   /* ~0.7s of 48 kHz stereo float */

static struct out_q *out_for(struct daemon *d, int fd)
{
    if (fd < 0) return NULL;
    struct out_q *free_slot = NULL;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (d->out[i].fd == fd) return &d->out[i];
        if (!free_slot && d->out[i].fd <= 0) free_slot = &d->out[i];
    }
    if (free_slot) {
        free_slot->fd = fd;
        free_slot->len = 0;
        return free_slot;
    }
    return NULL;
}

static void out_drop(struct daemon *d, int fd)
{
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (d->out[i].fd == fd) {
            free(d->out[i].buf);
            memset(&d->out[i], 0, sizeof(d->out[i]));
            d->out[i].fd = -1;
        }
    }
}

static void client_send(struct daemon *d, int fd, uint16_t type,
                        const void *payload, uint32_t bytes)
{
    struct out_q *q = out_for(d, fd);
    if (!q || bytes > VYPR_MAX_MSG_BYTES) return;

    if (type == VYPR_MSG_CLIENT_AUDIO && q->len > OUT_AUDIO_LIMIT) {
        if (++q->audio_dropped % 500 == 0)
            fprintf(stderr, "vyprd: client is behind; dropped %llu audio packets\n",
                    (unsigned long long)q->audio_dropped);
        return;
    }

    struct vypr_msg_head head = { .bytes = bytes, .type = type, .flags = 0 };
    const size_t need = q->len + sizeof(head) + bytes;
    if (need > q->cap) {
        size_t want = q->cap ? q->cap * 2 : 16384;
        while (want < need) want *= 2;
        uint8_t *grown = realloc(q->buf, want);
        if (!grown) return;
        q->buf = grown;
        q->cap = want;
    }
    memcpy(q->buf + q->len, &head, sizeof(head));
    q->len += sizeof(head);
    if (bytes) {
        memcpy(q->buf + q->len, payload, bytes);
        q->len += bytes;
    }
}

/* Returns -1 if the client is gone. */
static int out_flush(struct out_q *q)
{
    while (q->len) {
        ssize_t n = send(q->fd, q->buf, q->len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            return -1;
        }
        if (n == 0) return -1;
        memmove(q->buf, q->buf + n, q->len - (size_t)n);
        q->len -= (size_t)n;
    }
    return 0;
}

static void on_agent_message(struct daemon *d, uint16_t type,
                             const uint8_t *payload, uint32_t bytes)
{
    switch (type) {
    case VYPR_MSG_HELLO: {
        if (bytes < sizeof(struct vypr_msg_hello)) break;
        const struct vypr_msg_hello *h = (const void *)payload;
        fprintf(stderr, "vyprd: agent up, protocol %u, pid %u, %.0f MiB region\n",
                h->version, h->agent_pid, h->shm_bytes / 1048576.0);
        if (h->version != VYPR_PROTO_VERSION)
            fprintf(stderr, "vyprd: WARNING agent speaks %u, host speaks %u\n",
                    h->version, VYPR_PROTO_VERSION);
        if (d->launch)
            msg_send(d->agent_fd, VYPR_MSG_LAUNCH, d->launch, (uint32_t)strlen(d->launch));

        /* Line the clocks up straight away, so the first frames can already be
         * given an age. */
        struct vypr_msg_ping ping = { .token = now_ns() };
        msg_send(d->agent_fd, VYPR_MSG_PING, &ping, sizeof(ping));
        break;
    }

    case VYPR_MSG_WINDOW_ADDED:
    case VYPR_MSG_WINDOW_CHANGED: {
        if (bytes < sizeof(struct vypr_msg_window)) break;
        const struct vypr_msg_window *desc = (const void *)payload;

        char title[192] = {0};
        uint32_t tlen = desc->title_bytes;
        if (tlen > bytes - sizeof(*desc)) tlen = (uint32_t)(bytes - sizeof(*desc));
        if (tlen > sizeof(title) - 1) tlen = sizeof(title) - 1;
        memcpy(title, payload + sizeof(*desc), tlen);

        if (type == VYPR_MSG_WINDOW_ADDED)
            fprintf(stderr, "vyprd: guest window '%s' %ux%u at %d,%d "
                            "flags=0x%x owner=0x%llx%s\n", title,
                    desc->width, desc->height, desc->x, desc->y,
                    desc->flags, (unsigned long long)desc->owner_id,
                    title_matches(d, title) ? "" : " (no title match)");

        /* A popup is streamed because its owner is, not because of its title -
         * menus have no title to match against. */
        int wanted = title_matches(d, title);
        if (!wanted && (desc->flags & VYPR_WIN_POPUP) && desc->owner_id) {
            struct window *owner = window_find(d, desc->owner_id);
            wanted = owner && owner->has_slot && owner->client_fd >= 0;
        }
        if (!wanted) break;
        if (is_dismissed(d, desc->window_id)) break;

        struct window *w = window_find(d, desc->window_id);

        /* Mirror the guest's minimised state onto the host window, so a window
         * that stops producing frames is not left on screen looking frozen. */
        if (w && w->has_slot && w->client_fd >= 0) {
            const int mini = (desc->flags & VYPR_WIN_MINIMIZED) != 0;
            const int fs   = (desc->flags & VYPR_WIN_FULLSCREEN) != 0;
            if (mini != w->minimized || fs != w->is_fullscreen) {
                w->minimized     = mini;
                w->is_fullscreen = fs;
                struct vypr_msg_window_state st = {0};
                st.window_id  = w->id;
                st.minimized  = (uint32_t)mini;
                st.fullscreen = (uint32_t)fs;
                client_send(d, w->client_fd, VYPR_MSG_CLIENT_STATE, &st, sizeof(st));
            }
        }

        /* Keep the client's notion of the title bar in step with the window. */
        if (w && w->has_slot && w->client_fd >= 0 &&
            desc->chrome_top != w->chrome_top) {
            w->chrome_top = desc->chrome_top;
            struct vypr_msg_client_geom geom = {0};
            geom.window_id  = w->id;
            geom.chrome_top = desc->chrome_top;
            client_send(d, w->client_fd, VYPR_MSG_CLIENT_GEOM, &geom, sizeof(geom));
        }

        if (w && w->has_slot) {
            /* Outgrew its ring: tear the stream down and re-attach bigger. The
             * alternative, resizing under a live writer, shows a torn frame. */
            const struct vypr_slot *slot = &d->shm.hdr->slots[w->slot];
            if (desc->width > slot->max_width || desc->height > slot->max_height) {
                fprintf(stderr, "vyprd: '%s' grew to %ux%u; re-attaching\n",
                        title, desc->width, desc->height);
                window_release(d, w, 1);
                attach_window(d, desc, title);
            }
            break;
        }
        attach_window(d, desc, title);
        break;
    }

    case VYPR_MSG_WINDOW_REMOVED: {
        if (bytes < sizeof(struct vypr_msg_window_id)) break;
        const struct vypr_msg_window_id *m = (const void *)payload;
        forget_dismissed(d, m->window_id);
        struct window *w = window_find(d, m->window_id);
        if (w) {
            if (w->is_popup) {
                struct window *owner = window_find(d, w->owner_id);
                if (owner && owner->client_fd >= 0) {
                    struct vypr_msg_window_id gone = { .window_id = w->id };
                    client_send(d, owner->client_fd, VYPR_MSG_CLIENT_POPUP_END,
                             &gone, sizeof(gone));
                }
            } else {
                fprintf(stderr, "vyprd: guest closed '%s'\n", w->title);
            }
            window_release(d, w, 0);
        }
        break;
    }

    case VYPR_MSG_ATTACH_RESULT: {
        if (bytes < sizeof(struct vypr_msg_attach_result)) break;
        const struct vypr_msg_attach_result *r = (const void *)payload;
        struct window *w = window_find(d, r->window_id);
        if (!w) break;
        if (r->status != 0) {
            fprintf(stderr, "vyprd: agent refused '%s' (status %d)\n", w->title, r->status);
            window_release(d, w, 0);
            break;
        }
        w->attached = 1;

        if (w->is_popup) {
            /* Hand it to the owner's client: a popup surface has to be parented
             * to its owner, which only that process can do. */
            struct window *owner = window_find(d, w->owner_id);
            if (!owner || owner->client_fd < 0) {
                fprintf(stderr, "vyprd: popup for a window with no client; dropping\n");
                window_release(d, w, 1);
                break;
            }
            struct vypr_msg_client_popup msg = {0};
            msg.window_id = w->id;
            msg.owner_id  = owner->id;
            msg.slot      = w->slot;
            msg.dx        = w->gx - owner->gx;
            msg.dy        = w->gy - owner->gy;
            msg.width     = w->width;
            msg.height    = w->height;
            client_send(d, owner->client_fd, VYPR_MSG_CLIENT_POPUP, &msg, sizeof(msg));
            fprintf(stderr, "vyprd: popup %ux%u at +%d,+%d of '%s' -> slot %u\n",
                    w->width, w->height, msg.dx, msg.dy, owner->title, w->slot);
            break;
        }

        spawn_client(d, w);
        break;
    }

    case VYPR_MSG_PONG: {
        if (bytes < sizeof(struct vypr_msg_pong)) break;
        const struct vypr_msg_pong *p = (const void *)payload;
        if (p->guest_qpc_freq == 0) break;

        const uint64_t t3 = now_ns();
        const uint64_t t1 = p->token;
        if (t3 < t1) break;

        /* Assume the guest read its counter halfway through the round trip. */
        const uint64_t host_mid = t1 + (t3 - t1) / 2;
        const uint64_t guest_ns = (uint64_t)((__int128)p->guest_qpc * 1000000000
                                             / p->guest_qpc_freq);

        struct clock_sample *slot = &d->clock[d->clock_next];
        slot->rtt_ns    = t3 - t1;
        slot->offset_ns = (int64_t)host_mid - (int64_t)guest_ns;
        slot->at_ns     = t3;
        d->clock_next   = (d->clock_next + 1) % CLOCK_SAMPLES;

        /* Best surviving sample wins, not the newest. */
        const struct clock_sample *best = NULL;
        for (int i = 0; i < CLOCK_SAMPLES; i++) {
            const struct clock_sample *c = &d->clock[i];
            if (c->at_ns == 0) continue;
            if (t3 - c->at_ns > CLOCK_MAX_AGE_NS) continue;
            if (!best || c->rtt_ns < best->rtt_ns) best = c;
        }
        if (!best) break;

        d->shm.hdr->guest_offset_ns = best->offset_ns;
        d->shm.hdr->offset_rtt_us   = (uint32_t)(best->rtt_ns / 1000);
        __atomic_store_n(&d->shm.hdr->offset_valid, 1u, __ATOMIC_RELEASE);

        if (!d->clock_logged || best->rtt_ns != d->clock_best_rtt) {
            fprintf(stderr, "vyprd: clock offset from a %.2f ms round trip "
                            "(this sample %.2f ms)\n",
                    best->rtt_ns / 1e6, slot->rtt_ns / 1e6);
            d->clock_logged  = 1;
            d->clock_best_rtt = best->rtt_ns;
        }
        break;
    }

    case VYPR_MSG_POINTER_LOCK: {
        if (bytes < sizeof(struct vypr_msg_pointer_lock)) break;
        const struct vypr_msg_pointer_lock *m = (const void *)payload;

        /* Goes to whichever client is presenting that window - a popup's input
         * belongs to its owner's process. */
        struct window *w = window_find(d, m->window_id);
        if (w && w->is_popup && w->owner_id) {
            struct window *owner = window_find(d, w->owner_id);
            if (owner) w = owner;
        }
        if (w && w->client_fd >= 0)
            client_send(d, w->client_fd, VYPR_MSG_CLIENT_LOCK, m, sizeof(*m));
        fprintf(stderr, "vyprd: guest %s the pointer\n",
                m->locked ? "captured" : "released");
        break;
    }

    case VYPR_MSG_AUDIO: {
        /* One endpoint's worth of audio, so it goes to one window - the first
         * top-level with a client. Handing it to every client would play the
         * same sound several times over. */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &d->windows[i];
            if (w->id && !w->is_popup && w->client_fd >= 0) {
                client_send(d, w->client_fd, VYPR_MSG_CLIENT_AUDIO, payload, bytes);
                break;
            }
        }
        break;
    }

    case VYPR_MSG_CLIPBOARD: {
        /* To every client, not one: the clipboard is the desktop's, not a
         * window's, and whichever host window the user pastes into should
         * already have it. */
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &d->windows[i];
            if (w->id && w->client_fd >= 0)
                client_send(d, w->client_fd, VYPR_MSG_CLIENT_CLIPBOARD, payload, bytes);
        }
        break;
    }

    case VYPR_MSG_LOG:
        fprintf(stderr, "vyprd: agent: %.*s\n", (int)bytes, payload);
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------- client input */

static void on_client_message(struct daemon *d, struct window **owner, int fd,
                              uint16_t type, const uint8_t *payload, uint32_t bytes)
{
    if (type == VYPR_MSG_CLIENT_HELLO) {
        if (bytes < sizeof(struct vypr_msg_window_id)) return;
        const struct vypr_msg_window_id *m = (const void *)payload;
        struct window *w = window_find(d, m->window_id);
        if (!w) return;
        w->client_fd = fd;
        *owner = w;
        return;
    }

    if (type == VYPR_MSG_CLIPBOARD) {
        /* Straight through to the agent, and to the other clients so their
         * own idea of the clipboard does not go stale and bounce back. */
        if (d->agent_fd >= 0) msg_send(d->agent_fd, VYPR_MSG_CLIPBOARD, payload, bytes);
        for (int i = 0; i < MAX_WINDOWS; i++) {
            struct window *w = &d->windows[i];
            if (w->id && w->client_fd >= 0 && w->client_fd != fd)
                client_send(d, w->client_fd, VYPR_MSG_CLIENT_CLIPBOARD, payload, bytes);
        }
        return;
    }

    if (type == VYPR_MSG_CLOSE && bytes >= sizeof(struct vypr_msg_window_id)) {
        const struct vypr_msg_window_id *m = (const void *)payload;
        if (!is_dismissed(d, m->window_id) && d->dismissed_count < MAX_WINDOWS)
            d->dismissed[d->dismissed_count++] = m->window_id;
        fprintf(stderr, "vyprd: closing guest window on request\n");
    }

    /* Everything else is input, and goes straight through. vyprd does not
     * interpret it: the client already converted to guest client-area pixels,
     * which is the only place the host window's geometry is known. */
    if (d->agent_fd >= 0)
        msg_send(d->agent_fd, type, payload, bytes);
}

/* --------------------------------------------------------------------- setup */

static int listen_tcp(const char *bind_addr, uint16_t port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (!bind_addr || !strcmp(bind_addr, "any")) {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    } else if (inet_pton(AF_INET, bind_addr, &addr.sin_addr) != 1) {
        fprintf(stderr, "vyprd: '%s' is not an address\n", bind_addr);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "vyprd: bind :%u: %s\n", port, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, 4) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

static int listen_unix(const char *path)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return -1; }

    unlink(path);
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "vyprd: bind %s: %s\n", path, strerror(errno));
        close(fd);
        return -1;
    }
    if (listen(fd, MAX_WINDOWS) < 0) { perror("listen"); close(fd); return -1; }
    return fd;
}

static void find_self_dir(struct daemon *d)
{
    ssize_t n = readlink("/proc/self/exe", d->self_dir, sizeof(d->self_dir) - 1);
    if (n <= 0) { snprintf(d->self_dir, sizeof(d->self_dir), "."); return; }
    d->self_dir[n] = 0;
    char *slash = strrchr(d->self_dir, '/');
    if (slash) *slash = 0;
}

static void usage(void)
{
    fputs("usage: vyprd [--shm PATH] [--bind ADDR] [--port N] [--match SUBSTR]... [--all]\n"
          "             [--launch 'C:\\path\\app.exe']\n"
          "\n"
          "  --match  stream guest windows whose title contains SUBSTR (repeatable)\n"
          "  --all    stream every guest window; useful for seeing what is there\n"
          "  --bind   interface to accept the agent on; defaults to the virtual\n"
          "           bridge (192.168.122.1). 'any' listens everywhere.\n"
          "  --launch ask the agent to start this command once it connects\n", stderr);
}

int main(int argc, char **argv)
{
    struct daemon d = {0};
    d.shm_path   = "/dev/shm/vypr";
    d.agent_fd   = -1;
    d.audio_fd   = -1;
    for (size_t i = 0; i < sizeof(d.pending) / sizeof(d.pending[0]); i++)
        d.pending[i].fd = -1;
    uint16_t port = VYPR_CONTROL_PORT;
    /* The guest reaches the host across the virtual bridge, so that is the only
     * interface the control port ever needs to exist on. Listening on every
     * interface would put it on the real network too. */
    const char *bind_addr = "192.168.122.1";

    for (int i = 0; i < MAX_WINDOWS; i++) d.windows[i].client_fd = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--shm") && i + 1 < argc)    d.shm_path = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) port = (uint16_t)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bind") && i + 1 < argc) bind_addr = argv[++i];
        else if (!strcmp(argv[i], "--match") && i + 1 < argc) {
            if (d.match_count < MAX_MATCH) d.match[d.match_count++] = argv[++i];
        }
        else if (!strcmp(argv[i], "--all"))  d.match_all = 1;
        else if (!strcmp(argv[i], "--launch") && i + 1 < argc) d.launch = argv[++i];
        else { usage(); return 2; }
    }

    if (!d.match_all && d.match_count == 0) {
        fputs("vyprd: nothing would be streamed; pass --match or --all\n", stderr);
        usage();
        return 2;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);   /* a dead client must not kill the session */

    find_self_dir(&d);

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    snprintf(d.unix_path, sizeof(d.unix_path), "%s/vypr.sock",
             runtime ? runtime : "/tmp");

    /* Format before listening: the agent looks for the magic to tell our region
     * from Looking Glass's, so it must already be there when it connects. */
    if (vypr_shm_open(&d.shm, d.shm_path, 1) < 0) return 1;

    d.tcp_listen  = listen_tcp(bind_addr, port);
    d.unix_listen = listen_unix(d.unix_path);
    if (d.tcp_listen < 0 || d.unix_listen < 0) return 1;

    fprintf(stderr, "vyprd: region %s (%.0f MiB), waiting for agent on %s:%u\n",
            d.shm_path, d.shm.bytes / 1048576.0, bind_addr, port);

    struct msg_reader client_rx[MAX_WINDOWS] = {0};
    struct window    *client_owner[MAX_WINDOWS] = {0};
    int               client_fd[MAX_WINDOWS];
    for (int i = 0; i < MAX_WINDOWS; i++) client_fd[i] = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) d.out[i].fd = -1;

    while (!g_stop) {
        struct pollfd pfd[8 + MAX_WINDOWS];
        int n = 0;

        pfd[n].fd = d.tcp_listen;  pfd[n].events = POLLIN; n++;
        pfd[n].fd = d.unix_listen; pfd[n].events = POLLIN; n++;

        int agent_slot = -1;
        if (d.agent_fd >= 0) {
            agent_slot = n;
            pfd[n].fd = d.agent_fd; pfd[n].events = POLLIN; n++;
        }

        int audio_slot = -1;
        if (d.audio_fd >= 0) {
            audio_slot = n;
            pfd[n].fd = d.audio_fd; pfd[n].events = POLLIN; n++;
        }

        int first_pending = n;
        for (size_t i = 0; i < sizeof(d.pending) / sizeof(d.pending[0]); i++) {
            if (d.pending[i].fd < 0) continue;
            pfd[n].fd = d.pending[i].fd; pfd[n].events = POLLIN; n++;
        }

        int first_client = n;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (client_fd[i] < 0) continue;
            pfd[n].fd = client_fd[i];
            pfd[n].events = POLLIN;
            /* Only while something is queued, so an idle client does not spin
             * the loop on a socket that is permanently writable. */
            struct out_q *q = out_for(&d, client_fd[i]);
            if (q && q->len) pfd[n].events |= POLLOUT;
            n++;
        }

        if (poll(pfd, (nfds_t)n, 500) < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        /* Re-align periodically: the two clocks drift, and a stale offset shows
         * up as latency slowly wandering away from the truth. */
        if (d.agent_fd >= 0) {
            const uint64_t t = now_ns();
            if (t - d.last_ping_ns > 2000000000ull) {
                d.last_ping_ns = t;
                struct vypr_msg_ping ping = { .token = t };
                if (msg_send(d.agent_fd, VYPR_MSG_PING, &ping, sizeof(ping)) < 0)
                    fprintf(stderr, "vyprd: ping send failed\n");
            }
        }

        /* A parked connection announcing itself. */
        for (size_t i = 0, p = (size_t)first_pending;
             i < sizeof(d.pending) / sizeof(d.pending[0]); i++) {
            if (d.pending[i].fd < 0) continue;
            const int slot = (int)p++;

            /* Drop one that never says anything, rather than leaking it. */
            if (now_ns() - d.pending[i].at_ns > 10000000000ull) {
                close(d.pending[i].fd);
                msg_reader_free(&d.pending[i].rx);
                d.pending[i].fd = -1;
                continue;
            }
            if (!(pfd[slot].revents & (POLLIN | POLLHUP))) continue;

            if (msg_reader_fill(&d.pending[i].rx, d.pending[i].fd) < 0) {
                close(d.pending[i].fd);
                msg_reader_free(&d.pending[i].rx);
                d.pending[i].fd = -1;
                continue;
            }

            struct vypr_msg_head head;
            const uint8_t *payload;
            if (msg_reader_next(&d.pending[i].rx, &head, &payload) != 1) continue;

            if (head.type == VYPR_MSG_AUDIO_HELLO) {
                if (d.audio_fd >= 0) { close(d.audio_fd); msg_reader_free(&d.audio_rx); }
                d.audio_fd = d.pending[i].fd;
                d.audio_rx = d.pending[i].rx;      /* keep anything already buffered */
                memset(&d.pending[i].rx, 0, sizeof(d.pending[i].rx));
                d.pending[i].fd = -1;
                fprintf(stderr, "vyprd: audio channel connected\n");
            } else if (head.type == VYPR_MSG_HELLO) {
                if (d.agent_fd >= 0) {
                    /* One agent per session. A second is a stale agent from a
                     * previous VM boot; the newest wins. */
                    fprintf(stderr, "vyprd: replacing existing agent link\n");
                    close(d.agent_fd);
                    msg_reader_free(&d.agent_rx);
                    for (int w = 0; w < MAX_WINDOWS; w++)
                        window_release(&d, &d.windows[w], 0);
                }
                d.agent_fd = d.pending[i].fd;
                d.agent_rx = d.pending[i].rx;
                memset(&d.pending[i].rx, 0, sizeof(d.pending[i].rx));
                d.pending[i].fd = -1;
                fprintf(stderr, "vyprd: agent connected\n");
                on_agent_message(&d, head.type, payload, head.bytes);
            } else {
                close(d.pending[i].fd);
                msg_reader_free(&d.pending[i].rx);
                d.pending[i].fd = -1;
            }
        }

        /* Audio, on its own connection so nothing queues in front of it. */
        if (audio_slot >= 0 && (pfd[audio_slot].revents & (POLLIN | POLLHUP))) {
            if (msg_reader_fill(&d.audio_rx, d.audio_fd) < 0) {
                fprintf(stderr, "vyprd: audio channel closed\n");
                close(d.audio_fd);
                d.audio_fd = -1;
                msg_reader_free(&d.audio_rx);
            } else {
                struct vypr_msg_head head;
                const uint8_t *payload;
                while (msg_reader_next(&d.audio_rx, &head, &payload) == 1)
                    on_agent_message(&d, head.type, payload, head.bytes);
            }
        }

        /* Reap window clients the user closed. */
        for (;;) {
            int status;
            pid_t gone = waitpid(-1, &status, WNOHANG);
            if (gone <= 0) break;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (d.windows[i].child == gone) {
                    fprintf(stderr, "vyprd: '%s' window closed\n", d.windows[i].title);
                    d.windows[i].child = 0;
                    window_release(&d, &d.windows[i], 1);
                }
            }
        }

        if (pfd[0].revents & POLLIN) {
            int fd = accept(d.tcp_listen, NULL, NULL);
            if (fd >= 0) {
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

                /* Park it until it says what it is. */
                int placed = 0;
                for (size_t i = 0; i < sizeof(d.pending) / sizeof(d.pending[0]); i++) {
                    if (d.pending[i].fd < 0) {
                        d.pending[i].fd = fd;
                        d.pending[i].at_ns = now_ns();
                        placed = 1;
                        break;
                    }
                }
                if (!placed) close(fd);
            }
        }

        if (pfd[1].revents & POLLIN) {
            int fd = accept(d.unix_listen, NULL, NULL);
            /* Non-blocking, so a client that stops reading costs it its own
             * audio rather than stalling every other connection. */
            if (fd >= 0) {
                int fl = fcntl(fd, F_GETFL, 0);
                if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
            }
            if (fd >= 0) {
                int placed = 0;
                for (int i = 0; i < MAX_WINDOWS; i++) {
                    if (client_fd[i] < 0) { client_fd[i] = fd; placed = 1; break; }
                }
                if (!placed) close(fd);
            }
        }

        if (agent_slot >= 0 && (pfd[agent_slot].revents & (POLLIN | POLLHUP))) {
            if (msg_reader_fill(&d.agent_rx, d.agent_fd) < 0) {
                fprintf(stderr, "vyprd: agent disconnected\n");
                close(d.agent_fd);
                d.agent_fd = -1;
                msg_reader_free(&d.agent_rx);
                for (int i = 0; i < MAX_WINDOWS; i++)
                    window_release(&d, &d.windows[i], 0);
            } else {
                struct vypr_msg_head head;
                const uint8_t *payload;
                int rc;
                while ((rc = msg_reader_next(&d.agent_rx, &head, &payload)) == 1)
                    on_agent_message(&d, head.type, payload, head.bytes);
                if (rc < 0) { close(d.agent_fd); d.agent_fd = -1; }
            }
        }

        for (int i = 0, p = first_client; i < MAX_WINDOWS; i++) {
            if (client_fd[i] < 0) continue;
            int slot = p++;

            /* Send first: draining what is queued is what lets the next audio
             * packet be queued rather than dropped. */
            if (pfd[slot].revents & POLLOUT) {
                struct out_q *q = out_for(&d, client_fd[i]);
                if (q && out_flush(q) < 0) {
                    if (client_owner[i]) client_owner[i]->client_fd = -1;
                    out_drop(&d, client_fd[i]);
                    close(client_fd[i]);
                    client_fd[i] = -1;
                    client_owner[i] = NULL;
                    msg_reader_free(&client_rx[i]);
                    continue;
                }
            }

            if (!(pfd[slot].revents & (POLLIN | POLLHUP))) continue;

            if (msg_reader_fill(&client_rx[i], client_fd[i]) < 0) {
                if (client_owner[i]) client_owner[i]->client_fd = -1;
                out_drop(&d, client_fd[i]);
                close(client_fd[i]);
                client_fd[i] = -1;
                client_owner[i] = NULL;
                msg_reader_free(&client_rx[i]);
                continue;
            }
            struct vypr_msg_head head;
            const uint8_t *payload;
            while (msg_reader_next(&client_rx[i], &head, &payload) == 1)
                on_client_message(&d, &client_owner[i], client_fd[i],
                                  head.type, payload, head.bytes);
        }
    }

    fprintf(stderr, "\nsashd: shutting down\n");
    for (int i = 0; i < MAX_WINDOWS; i++) window_release(&d, &d.windows[i], 1);
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (client_fd[i] >= 0) {
            out_drop(&d, client_fd[i]);
            close(client_fd[i]);
            msg_reader_free(&client_rx[i]);
        }
    if (d.agent_fd >= 0) close(d.agent_fd);
    if (d.audio_fd >= 0) { close(d.audio_fd); msg_reader_free(&d.audio_rx); }
    for (size_t i = 0; i < sizeof(d.pending) / sizeof(d.pending[0]); i++)
        if (d.pending[i].fd >= 0) { close(d.pending[i].fd); msg_reader_free(&d.pending[i].rx); }
    close(d.tcp_listen);
    close(d.unix_listen);
    unlink(d.unix_path);
    msg_reader_free(&d.agent_rx);
    vypr_shm_close(&d.shm);
    return 0;
}
