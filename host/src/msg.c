#define _GNU_SOURCE
#include "msg.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

int msg_send(int fd, uint16_t type, const void *payload, uint32_t bytes)
{
    if (fd < 0) return -1;
    if (bytes > VYPR_MAX_MSG_BYTES) return -1;

    struct vypr_msg_head head = { .bytes = bytes, .type = type, .flags = 0 };
    struct iovec iov[2] = {
        { .iov_base = &head, .iov_len = sizeof(head) },
        { .iov_base = (void *)payload, .iov_len = bytes },
    };

    size_t total = sizeof(head) + bytes;
    size_t sent  = 0;
    int    first = 0;

    while (sent < total) {
        ssize_t n = writev(fd, &iov[first], bytes && first == 0 ? 2 : 1);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        sent += (size_t)n;

        /* Advance past whatever went out. Short writes on a socket are rare for
         * messages this size but not impossible under back-pressure. */
        while (n > 0 && first < 2) {
            if ((size_t)n >= iov[first].iov_len) {
                n -= (ssize_t)iov[first].iov_len;
                first++;
            } else {
                iov[first].iov_base = (uint8_t *)iov[first].iov_base + n;
                iov[first].iov_len -= (size_t)n;
                n = 0;
            }
        }
        if (first >= 2) break;
    }
    return 0;
}

static void msg_reader_compact(struct msg_reader *r)
{
    if (!r->consumed) return;
    memmove(r->buf, r->buf + r->consumed, r->len - r->consumed);
    r->len -= r->consumed;
    r->consumed = 0;
}

int msg_reader_fill(struct msg_reader *r, int fd)
{
    msg_reader_compact(r);

    if (r->cap - r->len < 8192) {
        size_t want = r->cap ? r->cap * 2 : 16384;
        if (want > VYPR_MAX_MSG_BYTES * 4u) want = VYPR_MAX_MSG_BYTES * 4u;
        uint8_t *grown = realloc(r->buf, want);
        if (!grown) return -1;
        r->buf = grown;
        r->cap = want;
    }

    ssize_t n = recv(fd, r->buf + r->len, r->cap - r->len, 0);
    if (n == 0) return -1;
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        if (errno == EINTR) return 0;
        return -1;
    }
    r->len += (size_t)n;
    return 1;
}

int msg_reader_next(struct msg_reader *r, struct vypr_msg_head *head,
                    const uint8_t **payload)
{
    msg_reader_compact(r);

    if (r->len < sizeof(*head)) return 0;
    memcpy(head, r->buf, sizeof(*head));

    if (head->bytes > VYPR_MAX_MSG_BYTES) {
        fprintf(stderr, "vyprd: peer framed a %u byte message; dropping link\n",
                head->bytes);
        return -1;
    }

    size_t total = sizeof(*head) + head->bytes;
    if (r->len < total) return 0;

    *payload = r->buf + sizeof(*head);
    r->consumed = total;
    return 1;
}

void msg_reader_free(struct msg_reader *r)
{
    free(r->buf);
    r->buf = NULL;
    r->cap = r->len = r->consumed = 0;
}
