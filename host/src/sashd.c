/*
 * sashd - the host session daemon.
 *
 * Owns three things the per-window clients must not each own separately:
 * the shared region and its allocation, the single control link to the guest
 * agent, and the decision about which guest windows become host windows.
 *
 *   agent (TCP 47820) ──▶ sashd ──▶ spawns sash-host per window
 *                          ▲                    │
 *                          └──── unix socket ───┘  (input travels back up)
 *
 * Input takes the long way round - client to daemon to agent - rather than each
 * window client holding its own link to the guest. One connection means one
 * place where window identity, slot ownership and reconnect are decided.
 */
#define _GNU_SOURCE
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

#define MAX_WINDOWS ((int)SASH_MAX_SLOTS)

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
    uint64_t owner_id;      /* nonzero for popups */
    uint32_t slot;
    int      has_slot;
    int      attached;
    int      is_popup;      /* presented by its owner's client, not its own */
    pid_t    child;
    int      client_fd;
    int32_t  gx, gy;        /* guest screen position of the client area */
    uint32_t width, height;
    char     title[192];
};

struct daemon {
    struct sash_shm shm;
    const char     *shm_path;

    int  tcp_listen;
    int  unix_listen;
    int  agent_fd;
    struct msg_reader agent_rx;

    char unix_path[108];   /* sun_path is 108; anything longer truncates */

    struct window windows[MAX_WINDOWS];

    const char *match[MAX_MATCH];
    int         match_count;
    int         match_all;
    const char *launch;
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

static int title_matches(struct daemon *d, const char *title)
{
    if (d->match_all) return 1;
    if (d->match_count == 0) return 0;
    for (int i = 0; i < d->match_count; i++)
        if (strcasestr(title, d->match[i])) return 1;
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
        struct sash_msg_window_id gone = { .window_id = w->id };
        msg_send(d->agent_fd, SASH_MSG_DETACH, &gone, sizeof(gone));
    }
    if (w->has_slot) sash_shm_free(&d->shm, w->slot);
    memset(w, 0, sizeof(*w));
    w->client_fd = -1;
}

/* ------------------------------------------------------------- window client */

static int spawn_client(struct daemon *d, struct window *w)
{
    char exe[640], slot[16], id[32];
    snprintf(exe, sizeof(exe), "%s/sash-host", d->self_dir);
    snprintf(slot, sizeof(slot), "%u", w->slot);
    snprintf(id, sizeof(id), "%" PRIu64, w->id);

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return -1; }

    if (pid == 0) {
        char *const argv[] = {
            exe,
            "--shm",       (char *)d->shm_path,
            "--slot",      slot,
            "--title",     w->title[0] ? w->title : (char *)"sash",
            "--window-id", id,
            "--sock",      d->unix_path,
            NULL
        };
        execv(exe, argv);
        fprintf(stderr, "sashd: cannot exec %s: %s\n", exe, strerror(errno));
        _exit(127);
    }

    w->child = pid;
    fprintf(stderr, "sashd: window '%s' -> slot %u, pid %d\n",
            w->title, w->slot, (int)pid);
    return 0;
}

/* --------------------------------------------------------------- agent input */

static void attach_window(struct daemon *d, const struct sash_msg_window *desc,
                          const char *title)
{
    struct window *w = window_find(d, desc->window_id);
    if (w && w->has_slot) return;
    if (!w) w = window_alloc(d, desc->window_id);
    if (!w) {
        fprintf(stderr, "sashd: no window slots left; ignoring '%s'\n", title);
        return;
    }

    snprintf(w->title, sizeof(w->title), "%s", title);
    w->width    = desc->width;
    w->height   = desc->height;
    w->gx       = desc->x;
    w->gy       = desc->y;
    w->owner_id = desc->owner_id;
    w->is_popup = (desc->flags & SASH_WIN_POPUP) != 0 && desc->owner_id != 0;

    /* Headroom, so an ordinary resize does not force a re-attach. The bump
     * allocator never rewinds - reusing a freed range under a live writer is
     * how one window's pixels end up in another - so each re-attach costs
     * region permanently. */
    uint32_t alloc_w = desc->width  + 256;
    uint32_t alloc_h = desc->height + 256;

    struct sash_msg_attach at;
    if (sash_shm_alloc(&d->shm, w->id, alloc_w, alloc_h, &at) < 0) {
        fprintf(stderr, "sashd: no region left for '%s'\n", title);
        memset(w, 0, sizeof(*w));
        w->client_fd = -1;
        return;
    }

    w->slot = at.slot;
    w->has_slot = 1;

    if (msg_send(d->agent_fd, SASH_MSG_ATTACH, &at, sizeof(at)) < 0)
        fprintf(stderr, "sashd: failed to send ATTACH for '%s'\n", title);
}

static void on_agent_message(struct daemon *d, uint16_t type,
                             const uint8_t *payload, uint32_t bytes)
{
    switch (type) {
    case SASH_MSG_HELLO: {
        if (bytes < sizeof(struct sash_msg_hello)) break;
        const struct sash_msg_hello *h = (const void *)payload;
        fprintf(stderr, "sashd: agent up, protocol %u, pid %u, %.0f MiB region\n",
                h->version, h->agent_pid, h->shm_bytes / 1048576.0);
        if (h->version != SASH_PROTO_VERSION)
            fprintf(stderr, "sashd: WARNING agent speaks %u, host speaks %u\n",
                    h->version, SASH_PROTO_VERSION);
        if (d->launch)
            msg_send(d->agent_fd, SASH_MSG_LAUNCH, d->launch, (uint32_t)strlen(d->launch));

        /* Line the clocks up straight away, so the first frames can already be
         * given an age. */
        struct sash_msg_ping ping = { .token = now_ns() };
        msg_send(d->agent_fd, SASH_MSG_PING, &ping, sizeof(ping));
        break;
    }

    case SASH_MSG_WINDOW_ADDED:
    case SASH_MSG_WINDOW_CHANGED: {
        if (bytes < sizeof(struct sash_msg_window)) break;
        const struct sash_msg_window *desc = (const void *)payload;

        char title[192] = {0};
        uint32_t tlen = desc->title_bytes;
        if (tlen > bytes - sizeof(*desc)) tlen = (uint32_t)(bytes - sizeof(*desc));
        if (tlen > sizeof(title) - 1) tlen = sizeof(title) - 1;
        memcpy(title, payload + sizeof(*desc), tlen);

        if (type == SASH_MSG_WINDOW_ADDED)
            fprintf(stderr, "sashd: guest window '%s' %ux%u at %d,%d "
                            "flags=0x%x owner=0x%llx%s\n", title,
                    desc->width, desc->height, desc->x, desc->y,
                    desc->flags, (unsigned long long)desc->owner_id,
                    title_matches(d, title) ? "" : " (no title match)");

        /* A popup is streamed because its owner is, not because of its title -
         * menus have no title to match against. */
        int wanted = title_matches(d, title);
        if (!wanted && (desc->flags & SASH_WIN_POPUP) && desc->owner_id) {
            struct window *owner = window_find(d, desc->owner_id);
            wanted = owner && owner->has_slot && owner->client_fd >= 0;
        }
        if (!wanted) break;

        struct window *w = window_find(d, desc->window_id);
        if (w && w->has_slot) {
            /* Outgrew its ring: tear the stream down and re-attach bigger. The
             * alternative, resizing under a live writer, shows a torn frame. */
            const struct sash_slot *slot = &d->shm.hdr->slots[w->slot];
            if (desc->width > slot->max_width || desc->height > slot->max_height) {
                fprintf(stderr, "sashd: '%s' grew to %ux%u; re-attaching\n",
                        title, desc->width, desc->height);
                window_release(d, w, 1);
                attach_window(d, desc, title);
            }
            break;
        }
        attach_window(d, desc, title);
        break;
    }

    case SASH_MSG_WINDOW_REMOVED: {
        if (bytes < sizeof(struct sash_msg_window_id)) break;
        const struct sash_msg_window_id *m = (const void *)payload;
        struct window *w = window_find(d, m->window_id);
        if (w) {
            if (w->is_popup) {
                struct window *owner = window_find(d, w->owner_id);
                if (owner && owner->client_fd >= 0) {
                    struct sash_msg_window_id gone = { .window_id = w->id };
                    msg_send(owner->client_fd, SASH_MSG_CLIENT_POPUP_END,
                             &gone, sizeof(gone));
                }
            } else {
                fprintf(stderr, "sashd: guest closed '%s'\n", w->title);
            }
            window_release(d, w, 0);
        }
        break;
    }

    case SASH_MSG_ATTACH_RESULT: {
        if (bytes < sizeof(struct sash_msg_attach_result)) break;
        const struct sash_msg_attach_result *r = (const void *)payload;
        struct window *w = window_find(d, r->window_id);
        if (!w) break;
        if (r->status != 0) {
            fprintf(stderr, "sashd: agent refused '%s' (status %d)\n", w->title, r->status);
            window_release(d, w, 0);
            break;
        }
        w->attached = 1;

        if (w->is_popup) {
            /* Hand it to the owner's client: a popup surface has to be parented
             * to its owner, which only that process can do. */
            struct window *owner = window_find(d, w->owner_id);
            if (!owner || owner->client_fd < 0) {
                fprintf(stderr, "sashd: popup for a window with no client; dropping\n");
                window_release(d, w, 1);
                break;
            }
            struct sash_msg_client_popup msg = {0};
            msg.window_id = w->id;
            msg.owner_id  = owner->id;
            msg.slot      = w->slot;
            msg.dx        = w->gx - owner->gx;
            msg.dy        = w->gy - owner->gy;
            msg.width     = w->width;
            msg.height    = w->height;
            msg_send(owner->client_fd, SASH_MSG_CLIENT_POPUP, &msg, sizeof(msg));
            fprintf(stderr, "sashd: popup %ux%u at +%d,+%d of '%s' -> slot %u\n",
                    w->width, w->height, msg.dx, msg.dy, owner->title, w->slot);
            break;
        }

        spawn_client(d, w);
        break;
    }

    case SASH_MSG_PONG: {
        if (bytes < sizeof(struct sash_msg_pong)) break;
        const struct sash_msg_pong *p = (const void *)payload;
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
            fprintf(stderr, "sashd: clock offset from a %.2f ms round trip "
                            "(this sample %.2f ms)\n",
                    best->rtt_ns / 1e6, slot->rtt_ns / 1e6);
            d->clock_logged  = 1;
            d->clock_best_rtt = best->rtt_ns;
        }
        break;
    }

    case SASH_MSG_POINTER_LOCK: {
        if (bytes < sizeof(struct sash_msg_pointer_lock)) break;
        const struct sash_msg_pointer_lock *m = (const void *)payload;

        /* Goes to whichever client is presenting that window - a popup's input
         * belongs to its owner's process. */
        struct window *w = window_find(d, m->window_id);
        if (w && w->is_popup && w->owner_id) {
            struct window *owner = window_find(d, w->owner_id);
            if (owner) w = owner;
        }
        if (w && w->client_fd >= 0)
            msg_send(w->client_fd, SASH_MSG_CLIENT_LOCK, m, sizeof(*m));
        fprintf(stderr, "sashd: guest %s the pointer\n",
                m->locked ? "captured" : "released");
        break;
    }

    case SASH_MSG_LOG:
        fprintf(stderr, "sashd: agent: %.*s\n", (int)bytes, payload);
        break;

    default:
        break;
    }
}

/* -------------------------------------------------------------- client input */

static void on_client_message(struct daemon *d, struct window **owner, int fd,
                              uint16_t type, const uint8_t *payload, uint32_t bytes)
{
    if (type == SASH_MSG_CLIENT_HELLO) {
        if (bytes < sizeof(struct sash_msg_window_id)) return;
        const struct sash_msg_window_id *m = (const void *)payload;
        struct window *w = window_find(d, m->window_id);
        if (!w) return;
        w->client_fd = fd;
        *owner = w;
        return;
    }

    /* Everything else is input, and goes straight through. sashd does not
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
        fprintf(stderr, "sashd: '%s' is not an address\n", bind_addr);
        close(fd);
        return -1;
    }

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "sashd: bind :%u: %s\n", port, strerror(errno));
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
        fprintf(stderr, "sashd: bind %s: %s\n", path, strerror(errno));
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
    fputs("usage: sashd [--shm PATH] [--bind ADDR] [--port N] [--match SUBSTR]... [--all]\n"
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
    d.shm_path   = "/dev/shm/sash";
    d.agent_fd   = -1;
    uint16_t port = SASH_CONTROL_PORT;
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
        fputs("sashd: nothing would be streamed; pass --match or --all\n", stderr);
        usage();
        return 2;
    }

    signal(SIGINT,  on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);   /* a dead client must not kill the session */

    find_self_dir(&d);

    const char *runtime = getenv("XDG_RUNTIME_DIR");
    snprintf(d.unix_path, sizeof(d.unix_path), "%s/sash.sock",
             runtime ? runtime : "/tmp");

    /* Format before listening: the agent looks for the magic to tell our region
     * from Looking Glass's, so it must already be there when it connects. */
    if (sash_shm_open(&d.shm, d.shm_path, 1) < 0) return 1;

    d.tcp_listen  = listen_tcp(bind_addr, port);
    d.unix_listen = listen_unix(d.unix_path);
    if (d.tcp_listen < 0 || d.unix_listen < 0) return 1;

    fprintf(stderr, "sashd: region %s (%.0f MiB), waiting for agent on %s:%u\n",
            d.shm_path, d.shm.bytes / 1048576.0, bind_addr, port);

    struct msg_reader client_rx[MAX_WINDOWS] = {0};
    struct window    *client_owner[MAX_WINDOWS] = {0};
    int               client_fd[MAX_WINDOWS];
    for (int i = 0; i < MAX_WINDOWS; i++) client_fd[i] = -1;

    while (!g_stop) {
        struct pollfd pfd[3 + MAX_WINDOWS];
        int n = 0;

        pfd[n].fd = d.tcp_listen;  pfd[n].events = POLLIN; n++;
        pfd[n].fd = d.unix_listen; pfd[n].events = POLLIN; n++;

        int agent_slot = -1;
        if (d.agent_fd >= 0) {
            agent_slot = n;
            pfd[n].fd = d.agent_fd; pfd[n].events = POLLIN; n++;
        }

        int first_client = n;
        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (client_fd[i] < 0) continue;
            pfd[n].fd = client_fd[i]; pfd[n].events = POLLIN; n++;
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
                struct sash_msg_ping ping = { .token = t };
                if (msg_send(d.agent_fd, SASH_MSG_PING, &ping, sizeof(ping)) < 0)
                    fprintf(stderr, "sashd: ping send failed\n");
            }
        }

        /* Reap window clients the user closed. */
        for (;;) {
            int status;
            pid_t gone = waitpid(-1, &status, WNOHANG);
            if (gone <= 0) break;
            for (int i = 0; i < MAX_WINDOWS; i++) {
                if (d.windows[i].child == gone) {
                    fprintf(stderr, "sashd: '%s' window closed\n", d.windows[i].title);
                    d.windows[i].child = 0;
                    window_release(&d, &d.windows[i], 1);
                }
            }
        }

        if (pfd[0].revents & POLLIN) {
            int fd = accept(d.tcp_listen, NULL, NULL);
            if (fd >= 0) {
                if (d.agent_fd >= 0) {
                    /* One agent per session. A second connection is a stale
                     * agent from a previous VM boot; the newest wins. */
                    fprintf(stderr, "sashd: replacing existing agent link\n");
                    close(d.agent_fd);
                    msg_reader_free(&d.agent_rx);
                    for (int i = 0; i < MAX_WINDOWS; i++)
                        window_release(&d, &d.windows[i], 0);
                }
                int one = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
                d.agent_fd = fd;
                fprintf(stderr, "sashd: agent connected\n");
            }
        }

        if (pfd[1].revents & POLLIN) {
            int fd = accept(d.unix_listen, NULL, NULL);
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
                fprintf(stderr, "sashd: agent disconnected\n");
                close(d.agent_fd);
                d.agent_fd = -1;
                msg_reader_free(&d.agent_rx);
                for (int i = 0; i < MAX_WINDOWS; i++)
                    window_release(&d, &d.windows[i], 0);
            } else {
                struct sash_msg_head head;
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
            if (!(pfd[slot].revents & (POLLIN | POLLHUP))) continue;

            if (msg_reader_fill(&client_rx[i], client_fd[i]) < 0) {
                if (client_owner[i]) client_owner[i]->client_fd = -1;
                close(client_fd[i]);
                client_fd[i] = -1;
                client_owner[i] = NULL;
                msg_reader_free(&client_rx[i]);
                continue;
            }
            struct sash_msg_head head;
            const uint8_t *payload;
            while (msg_reader_next(&client_rx[i], &head, &payload) == 1)
                on_client_message(&d, &client_owner[i], client_fd[i],
                                  head.type, payload, head.bytes);
        }
    }

    fprintf(stderr, "\nsashd: shutting down\n");
    for (int i = 0; i < MAX_WINDOWS; i++) window_release(&d, &d.windows[i], 1);
    for (int i = 0; i < MAX_WINDOWS; i++)
        if (client_fd[i] >= 0) { close(client_fd[i]); msg_reader_free(&client_rx[i]); }
    if (d.agent_fd >= 0) close(d.agent_fd);
    close(d.tcp_listen);
    close(d.unix_listen);
    unlink(d.unix_path);
    msg_reader_free(&d.agent_rx);
    sash_shm_close(&d.shm);
    return 0;
}
