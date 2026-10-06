#define _GNU_SOURCE
#include "fake_ram.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGE 4096ull
#define GIB4 (4ull << 30)

static uint64_t file_to_pfn(const struct fake_ram *r, uint64_t page)
{
    const uint64_t off = page * PAGE;
    return (off < r->lowmem ? off : off - r->lowmem + GIB4) / PAGE;
}

static uint64_t pfn_to_file(const struct fake_ram *r, uint64_t pfn)
{
    const uint64_t gpa = pfn * PAGE;
    return (gpa < GIB4 ? gpa : gpa - GIB4 + r->lowmem) / PAGE;
}

int fake_ram_open(struct fake_ram *r, const char *path, uint64_t bytes, uint64_t lowmem)
{
    memset(r, 0, sizeof(*r));
    r->fd = open(path, O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (r->fd < 0) { perror(path); return -1; }
    if (ftruncate(r->fd, (off_t)bytes) < 0) { perror("ftruncate"); close(r->fd); return -1; }
    r->bytes  = bytes;
    r->lowmem = lowmem < bytes ? lowmem : bytes;
    r->used   = calloc(bytes / PAGE, 1);
    /* Page 0 is never handed out, as no real guest hands it out either. */
    if (r->used) r->used[0] = 1;
    r->cursor = 12345;
    return r->used ? 0 : -1;
}

void fake_ram_close(struct fake_ram *r)
{
    if (r->fd >= 0) close(r->fd);
    free(r->used);
    memset(r, 0, sizeof(*r));
    r->fd = -1;
}

static int cmp_u64(const void *a, const void *b)
{
    const uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return x < y ? -1 : x > y;
}

void *fake_ram_alloc(struct fake_ram *r, uint32_t pages, uint64_t *pfns)
{
    const uint64_t total = r->bytes / PAGE;

    /* A prime stride through the file, skipping pages already in use. Mostly
     * scattered, with the odd adjacent pair - like the real thing. */
    uint32_t got = 0;
    for (uint64_t tries = 0; got < pages && tries < total * 2; tries++) {
        r->cursor = (r->cursor + 7919) % total;
        if (r->used[r->cursor]) continue;
        r->used[r->cursor] = 1;
        pfns[got++] = r->cursor;   /* file pages for now */
    }
    if (got < pages) {
        for (uint32_t i = 0; i < got; i++) r->used[pfns[i]] = 0;
        return NULL;
    }

    /* Sorted, as the agent sorts its own: it gives the host longer runs. */
    qsort(pfns, pages, sizeof(*pfns), cmp_u64);

    uint8_t *base = mmap(NULL, (size_t)pages * PAGE, PROT_NONE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (base == MAP_FAILED) return NULL;
    for (uint32_t i = 0; i < pages; ) {
        uint32_t n = 1;
        while (i + n < pages && pfns[i + n] == pfns[i] + n) n++;
        if (mmap(base + (size_t)i * PAGE, (size_t)n * PAGE, PROT_READ | PROT_WRITE,
                 MAP_SHARED | MAP_FIXED, r->fd, (off_t)(pfns[i] * PAGE)) == MAP_FAILED) {
            perror("fake_ram mmap");
            munmap(base, (size_t)pages * PAGE);
            return NULL;
        }
        i += n;
    }
    for (uint32_t i = 0; i < pages; i++) pfns[i] = file_to_pfn(r, pfns[i]);
    return base;
}

void fake_ram_free(struct fake_ram *r, void *base, uint32_t pages, const uint64_t *pfns)
{
    for (uint32_t i = 0; i < pages; i++) {
        const uint64_t page = pfn_to_file(r, pfns[i]);
        if (page < r->bytes / PAGE) r->used[page] = 0;
    }
    /* Like Windows, which zeroes a freed page before anybody else gets it. */
    memset(base, 0, (size_t)pages * PAGE);
    munmap(base, (size_t)pages * PAGE);
}
