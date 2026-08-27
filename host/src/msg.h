/*
 * Message framing, used for both the TCP link to the agent and the unix socket
 * to the per-window clients. Both carry vypr_proto messages, so they share one
 * reader rather than growing two subtly different ones.
 */
#ifndef VYPR_HOST_MSG_H
#define VYPR_HOST_MSG_H

#include <stdint.h>
#include <stddef.h>
#include "vypr_proto.h"

struct msg_reader {
    uint8_t *buf;
    size_t   cap;
    size_t   len;
    /* Bytes of a message already handed to the caller. Compaction is deferred
     * to the next call so the payload pointer stays valid while it is being
     * used - shifting the buffer immediately would invalidate the pointer this
     * call just returned. */
    size_t   consumed;
};

/* Blocking write of one framed message. Header and payload go in a single
 * writev so a small message is one packet. */
int msg_send(int fd, uint16_t type, const void *payload, uint32_t bytes);

/* Reads whatever is available without blocking. Returns 1 on data, 0 if the
 * socket had nothing, -1 on EOF or error. */
int msg_reader_fill(struct msg_reader *r, int fd);

/* Pops one complete message. Returns 1 and points `payload` into the reader's
 * buffer (valid until the next call), 0 if none is complete yet, -1 if the peer
 * framed something impossible. */
int msg_reader_next(struct msg_reader *r, struct vypr_msg_head *head,
                    const uint8_t **payload);

void msg_reader_free(struct msg_reader *r);

#endif
