#ifndef VYPR_GUEST_RAM_H
#define VYPR_GUEST_RAM_H

/*
 * The guest's RAM, as the daemon sees it: QEMU's memory file, mapped read-only.
 *
 * Read-only on purpose. Rings are pages the guest agent owns, and pages a dead
 * agent left behind go back to Windows the moment it exits - so a host write
 * into one, however it came about, would land in some unrelated guest process.
 * The daemon only ever needs to read stamps, and this makes that the only thing
 * it can do.
 */

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "vypr_shm.h"

struct vypr_guest_ram {
    int            fd;
    const uint8_t *base;
    uint64_t       bytes;
    char           path[96];   /* how a presenter can open the same file */
    pid_t          qemu_pid;

    /*
     * Bytes of RAM QEMU put below the 4 GiB PCI hole; the rest starts at
     * 4 GiB. Zero until a ring with pages above the hole has shown which
     * layout this VM uses - see vypr_guest_ram_verify.
     */
    uint64_t       lowmem;
};

/*
 * Find the RAM of the running VM called `vm` (its libvirt name), or of the only
 * VM running when `vm` is NULL, and map it. 0 on success; on failure, -1 with a
 * sentence in `why` saying what to do about it.
 */
int  vypr_guest_ram_find(struct vypr_guest_ram *g, const char *vm, char *why, size_t why_len);

/* Map an explicit file as guest RAM. For tests, which have no QEMU. */
int  vypr_guest_ram_open(struct vypr_guest_ram *g, const char *path, char *why, size_t why_len);

void vypr_guest_ram_close(struct vypr_guest_ram *g);

/*
 * Check that `pfns` really are a ring the guest stamped with `nonce`, and turn
 * them into runs of the RAM file. Returns the run count, or -1 with the reason
 * in `why`. Learns the memory layout the first time a ring reaches above
 * 4 GiB, and holds it to that layout afterwards.
 */
int  vypr_guest_ram_verify(struct vypr_guest_ram *g, const uint64_t *pfns, uint32_t count,
                           uint64_t nonce, struct vypr_ring_run *runs, uint32_t max_runs,
                           char *why, size_t why_len);

#endif
