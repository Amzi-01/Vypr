/*
 * fake_ram - guest RAM for the Linux test tools, which have no VM.
 *
 * A sparse file stands in for QEMU's memory file, laid out the way q35 lays out
 * a large guest: the first `lowmem` bytes sit below the 4 GiB hole and the rest
 * starts at 4 GiB. Pages are handed out scattered, the way a Windows guest that
 * has been running a while hands them out, so the host's verification, layout
 * detection and run merging are all exercised rather than the easy case.
 */
#ifndef VYPR_FAKE_RAM_H
#define VYPR_FAKE_RAM_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct fake_ram {
    int      fd;
    uint64_t bytes;
    uint64_t lowmem;
    uint8_t *used;      /* one byte per page */
    uint64_t cursor;
};

/* Create (or reuse) `path` as `bytes` of guest RAM. 0, or -1. */
int   fake_ram_open(struct fake_ram *r, const char *path, uint64_t bytes, uint64_t lowmem);
void  fake_ram_close(struct fake_ram *r);

/* `pages` scattered pages, mapped contiguously and returned, with each page's
 * guest physical page number in `pfns` (ring order). NULL if out of pages. */
void *fake_ram_alloc(struct fake_ram *r, uint32_t pages, uint64_t *pfns);
void  fake_ram_free(struct fake_ram *r, void *base, uint32_t pages, const uint64_t *pfns);

#ifdef __cplusplus
}
#endif

#endif
