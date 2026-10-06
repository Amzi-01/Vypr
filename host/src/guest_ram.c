#define _GNU_SOURCE
#include "guest_ram.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define GIB4 (4ull << 30)

static int fail(char *why, size_t len, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

#include <stdarg.h>
static int fail(char *why, size_t len, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (why && len) vsnprintf(why, len, fmt, ap);
    va_end(ap);
    return -1;
}

static int map_fd(struct vypr_guest_ram *g, int fd, const char *path, char *why, size_t len)
{
    struct stat st;
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        close(fd);
        return fail(why, len, "%s is empty", path);
    }
    void *base = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED | MAP_NORESERVE, fd, 0);
    if (base == MAP_FAILED) {
        int e = errno;
        close(fd);
        return fail(why, len, "cannot map %s: %s", path, strerror(e));
    }
    g->fd     = fd;
    g->base   = base;
    g->bytes  = (uint64_t)st.st_size;
    g->lowmem = 0;
    snprintf(g->path, sizeof(g->path), "%s", path);
    return 0;
}

int vypr_guest_ram_open(struct vypr_guest_ram *g, const char *path, char *why, size_t len)
{
    memset(g, 0, sizeof(*g));
    g->fd = -1;
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fail(why, len, "cannot open %s: %s", path, strerror(errno));
    return map_fd(g, fd, path, why, len);
}

void vypr_guest_ram_close(struct vypr_guest_ram *g)
{
    if (g->base) munmap((void *)g->base, g->bytes);
    if (g->fd >= 0) close(g->fd);
    memset(g, 0, sizeof(*g));
    g->fd = -1;
}

/*
 * The VM name libvirt put on a QEMU command line, or NULL if `pid` is not a
 * QEMU it started. libvirt writes `-name guest=NAME,debug-threads=on`, with any
 * comma in NAME doubled.
 */
static int qemu_name(pid_t pid, char *name, size_t len)
{
    char path[64], buf[8192];
    snprintf(path, sizeof(path), "/proc/%d/cmdline", (int)pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = 0;

    const char *exe = strrchr(buf, '/');
    exe = exe ? exe + 1 : buf;
    if (strncmp(exe, "qemu-system", 11) != 0) return -1;

    name[0] = 0;
    for (const char *a = buf; a < buf + n; a += strlen(a) + 1) {
        if (strcmp(a, "-name") != 0) continue;
        const char *v = a + strlen(a) + 1;
        if (v >= buf + n) break;
        if (!strncmp(v, "guest=", 6)) v += 6;
        size_t o = 0;
        for (; *v && o + 1 < len; v++) {
            if (*v == ',') {
                if (v[1] != ',') break;   /* end of the name */
                v++;                      /* ",," is a literal comma */
            }
            name[o++] = *v;
        }
        name[o] = 0;
        break;
    }
    return 0;
}

static gid_t qemu_gid(pid_t pid)
{
    char path[64], line[256];
    snprintf(path, sizeof(path), "/proc/%d/status", (int)pid);
    FILE *f = fopen(path, "r");
    if (!f) return (gid_t)-1;
    gid_t gid = (gid_t)-1;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "Gid:", 4)) { gid = (gid_t)strtoul(line + 4, NULL, 10); break; }
    fclose(f);
    return gid;
}

static const char *group_name(gid_t gid, char *buf, size_t len)
{
    struct group *gr = getgrgid(gid);
    if (gr) snprintf(buf, len, "%s", gr->gr_name);
    else    snprintf(buf, len, "%u", (unsigned)gid);
    return buf;
}

/*
 * A memory backend QEMU holds open. libvirt's memfd backing shows up as
 * "/memfd:memory-backend-memfd (deleted)"; file and hugepage backing put
 * "pc.ram" or "ram-node" in the path.
 */
static int looks_like_ram(const char *target)
{
    return strstr(target, "memory-backend") || strstr(target, "pc.ram") ||
           strstr(target, "ram-node");
}

int vypr_guest_ram_find(struct vypr_guest_ram *g, const char *vm, char *why, size_t len)
{
    memset(g, 0, sizeof(*g));
    g->fd = -1;

    DIR *proc = opendir("/proc");
    if (!proc) return fail(why, len, "cannot read /proc: %s", strerror(errno));

    pid_t found = 0;
    int qemus = 0;
    char name[256], seen[256] = "";
    struct dirent *de;
    while ((de = readdir(proc))) {
        char *end;
        long pid = strtol(de->d_name, &end, 10);
        if (*end || pid <= 0) continue;
        if (qemu_name((pid_t)pid, name, sizeof(name)) < 0) continue;
        qemus++;
        if (!seen[0]) snprintf(seen, sizeof(seen), "%s", name);
        if (vm ? strcmp(name, vm) == 0 : 1) {
            if (!vm && found) {   /* more than one, and nothing to choose by */
                closedir(proc);
                return fail(why, len, "more than one VM is running; say which "
                                      "with --vm (or VM= in vypr.conf)");
            }
            found = (pid_t)pid;
            if (vm) break;
        }
    }
    closedir(proc);

    if (!found) {
        if (vm && qemus)
            return fail(why, len, "the VM '%s' is not running (found '%s')", vm, seen);
        return fail(why, len, vm ? "the VM '%s' is not running" : "no VM is running", vm);
    }

    /* Find its RAM among its open files. */
    char dir[64];
    snprintf(dir, sizeof(dir), "/proc/%d/fd", (int)found);
    DIR *fds = opendir(dir);
    int denied = 0, best_fd = -1;
    uint64_t best_bytes = 0;
    char best_path[96] = "";
    if (fds) {
        while ((de = readdir(fds))) {
            if (de->d_name[0] == '.') continue;
            char link[160], target[512];
            snprintf(link, sizeof(link), "%s/%.20s", dir, de->d_name);
            ssize_t n = readlink(link, target, sizeof(target) - 1);
            if (n < 0) { if (errno == EACCES || errno == EPERM) denied = 1; continue; }
            target[n] = 0;
            if (!looks_like_ram(target)) continue;

            int fd = open(link, O_RDONLY | O_CLOEXEC);
            if (fd < 0) { if (errno == EACCES || errno == EPERM) denied = 1; continue; }
            struct stat st;
            if (fstat(fd, &st) < 0 || (uint64_t)st.st_size <= best_bytes) { close(fd); continue; }
            if (best_fd >= 0) close(best_fd);
            best_fd = fd;
            best_bytes = (uint64_t)st.st_size;
            snprintf(best_path, sizeof(best_path), "%.95s", link);
        }
        closedir(fds);
    } else if (errno == EACCES || errno == EPERM) {
        denied = 1;
    }

    if (best_fd < 0 && denied) {
        /* The usual cause: QEMU runs as this user but under libvirt's group,
         * and Linux only lets a process see another's files when both match. */
        char mine[64], theirs[64];
        const gid_t qg = qemu_gid(found);
        return fail(why, len,
            "cannot read the VM's memory: QEMU runs under group '%s' and this "
            "session under '%s'. Set group = \"%s\" in /etc/libvirt/qemu.conf "
            "(next to user =), restart virtqemud, then restart the VM",
            group_name(qg, theirs, sizeof(theirs)),
            group_name(getegid(), mine, sizeof(mine)), mine);
    }
    if (best_fd < 0)
        return fail(why, len,
            "the VM's memory is not shared with the host. Its domain needs "
            "<memoryBacking><source type='memfd'/><access mode='shared'/></memoryBacking> "
            "('vypr install' adds it); restart the VM afterwards");

    if (map_fd(g, best_fd, best_path, why, len) < 0) return -1;
    g->qemu_pid = found;
    return 0;
}

/* Where guest physical address `gpa` lives in the RAM file under a layout with
 * `low` bytes below the hole, or UINT64_MAX if nowhere. */
static uint64_t translate(uint64_t gpa, uint64_t low, uint64_t bytes)
{
    uint64_t off;
    if (gpa < low && gpa < GIB4)  off = gpa;
    else if (gpa >= GIB4)         off = gpa - GIB4 + low;
    else                          return UINT64_MAX;   /* in the hole */
    if (off > bytes || bytes - off < VYPR_PAGE_BYTES) return UINT64_MAX;
    return off;
}

/* Every page holds the stamp it should under layout `low`. */
static int stamps_match(const struct vypr_guest_ram *g, const uint64_t *pfns, uint32_t count,
                        uint64_t nonce, uint64_t low, int *reaches_high)
{
    *reaches_high = 0;
    for (uint32_t i = 0; i < count; i++) {
        if (pfns[i] > (UINT64_MAX >> 12)) return 0;
        const uint64_t gpa = pfns[i] * VYPR_PAGE_BYTES;
        const uint64_t off = translate(gpa, low, g->bytes);
        if (off == UINT64_MAX) return 0;
        if (gpa >= GIB4) *reaches_high = 1;

        struct vypr_page_stamp st;
        memcpy(&st, g->base + off, sizeof(st));
        if (st.magic != VYPR_PAGE_MAGIC || st.nonce != nonce ||
            st.index != i || st.pfn != pfns[i])
            return 0;
    }
    return 1;
}

int vypr_guest_ram_verify(struct vypr_guest_ram *g, const uint64_t *pfns, uint32_t count,
                          uint64_t nonce, struct vypr_ring_run *runs, uint32_t max_runs,
                          char *why, size_t len)
{
    if (!g->base) return fail(why, len, "no guest RAM is mapped");
    if (count == 0 || count > VYPR_MAX_RING_PAGES)
        return fail(why, len, "a ring of %u pages is not believable", count);

    /*
     * Which layout to read the pages through.
     *
     * Once one is known it is the only one tried: the layout cannot change
     * while QEMU runs, so a ring that fails under it is a bad ring, not a sign
     * to start guessing again. Until then every layout QEMU's x86 machines use
     * is tried in turn - q35 and i440fx put 2, 2.75 or 3 GiB below the hole
     * depending on how much RAM there is, and "all of it" covers a guest small
     * enough to have no high memory. Exactly one of them makes every stamp
     * line up, and a wrong one fails on the first page above 4 GiB.
     */
    uint64_t tries[6];
    int ntries = 0;
    if (g->lowmem) {
        tries[ntries++] = g->lowmem;
    } else {
        const uint64_t lows[] = { 2ull << 30, 0xB0000000ull, 3ull << 30, 0xE0000000ull };
        for (size_t i = 0; i < sizeof(lows) / sizeof(lows[0]); i++)
            if (lows[i] < g->bytes) tries[ntries++] = lows[i];
        tries[ntries++] = g->bytes;
    }

    uint64_t low = 0;
    int high = 0, ok = 0;
    for (int t = 0; t < ntries && !ok; t++) {
        if (stamps_match(g, pfns, count, nonce, tries[t], &high)) {
            low = tries[t];
            ok = 1;
        }
    }
    if (!ok)
        return fail(why, len,
            "the ring's %u pages did not read back as the guest stamped them%s",
            count, g->lowmem ? "" : " under any memory layout QEMU uses");

    /* A ring that never went above 4 GiB fits every layout equally well, so it
     * says nothing about which one this VM has. */
    if (high && !g->lowmem) g->lowmem = low;

    /* Ring order, merged wherever the next page sits right after this one. */
    uint32_t n = 0;
    for (uint32_t i = 0; i < count; i++) {
        const uint64_t off = translate(pfns[i] * VYPR_PAGE_BYTES, low, g->bytes);
        if (n && runs[n - 1].offset + runs[n - 1].pages * VYPR_PAGE_BYTES == off) {
            runs[n - 1].pages++;
            continue;
        }
        if (n == max_runs) return fail(why, len, "the ring is in more than %u pieces", max_runs);
        runs[n].offset = off;
        runs[n].pages  = 1;
        n++;
    }
    return (int)n;
}
