/*
 * The segment reader (segment.h). libc only, and no header from the library.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "segment.h"

/* Only the two structures the design freezes may be known by layout, and a
 * reader that silently disagreed about their size would read nonsense from a
 * segment it had just declared valid. */
_Static_assert(sizeof(struct osoe_metric_descriptor) == 64, "frozen by §3.14");
_Static_assert(sizeof(struct osoe_segment_directory) == 112, "frozen by §3.14");

uint64_t osoe_segment_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* FNV-1a, the same hash the writer records. */
static uint32_t checksum(const void *p, size_t n)
{
    const unsigned char *b = p;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) {
        h ^= b[i];
        h *= 16777619u;
    }
    return h;
}

static int fail(char *err, size_t errlen, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

static int fail(char *err, size_t errlen, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    if (err && errlen)
        vsnprintf(err, errlen, fmt, ap);
    va_end(ap);
    return -1;
}

/* "/dev/shm/osoe.x", "osoe.x" and "/osoe.x" all name one object. */
static void normalize(const char *in, char *out, size_t len)
{
    const char *p = in;
    if (strncmp(p, "/dev/shm/", 9) == 0)
        p += 9;
    while (*p == '/')
        p++;
    snprintf(out, len, "/%s", p);
}

int osoe_segment_open(struct osoe_segment *s, const char *name, char *err, size_t errlen)
{
    memset(s, 0, sizeof *s);
    normalize(name, s->name, sizeof s->name);

    int fd = shm_open(s->name, O_RDONLY, 0);
    if (fd < 0)
        return fail(err, errlen, "cannot open /dev/shm%s: %s", s->name, strerror(errno));
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return fail(err, errlen, "cannot stat /dev/shm%s: %s", s->name, strerror(errno));
    }
    if ((uint64_t)st.st_size < sizeof(struct osoe_segment_directory)) {
        close(fd);
        return fail(err, errlen, "/dev/shm%s is %lld bytes: too small to hold a directory",
                    s->name, (long long)st.st_size);
    }
    void *p = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED)
        return fail(err, errlen, "cannot map /dev/shm%s: %s", s->name, strerror(errno));

    s->map = p;
    s->bytes = (size_t)st.st_size;
    s->dir = (const struct osoe_segment_directory *)p;

    if (s->dir->magic != OSOE_SEG_MAGIC) {
        unsigned long long magic = (unsigned long long)s->dir->magic;
        char shown[OSOE_SEGMENT_NAME_MAX];
        memcpy(shown, s->name, sizeof shown);
        osoe_segment_close(s);              /* nothing below may touch the mapping */
        return fail(err, errlen, "/dev/shm%s is not an osoe control segment "
                    "(magic %#llx)", shown, magic);
    }
    /*
     * From here on nothing is refused. An unknown layout, a short directory, a
     * table this reader cannot parse: each leaves `table_usable` false and the
     * caller prints what it does know. That is what freezing the directory
     * bought, and refusing here would spend it.
     */
    if (s->dir->directory_bytes < sizeof *s->dir)
        return 0;
    if (s->dir->metrics_count == 0 || s->dir->metric_descriptor_bytes != sizeof(struct osoe_metric_descriptor))
        return 0;
    uint64_t end = s->dir->metrics_offset + s->dir->metrics_bytes;
    if (end < s->dir->metrics_offset || end > s->bytes)
        return 0;
    if (s->dir->metrics_bytes != (uint64_t)s->dir->metrics_count * sizeof(struct osoe_metric_descriptor))
        return 0;

    const struct osoe_metric_descriptor *t =
        (const struct osoe_metric_descriptor *)(s->map + s->dir->metrics_offset);
    if (checksum(t, (size_t)s->dir->metrics_bytes) != s->dir->metrics_checksum)
        return 0;

    s->metrics = t;
    s->nmetrics = s->dir->metrics_count;
    s->table_usable = true;
    return 0;
}

void osoe_segment_close(struct osoe_segment *s)
{
    if (s->map)
        munmap((void *)(uintptr_t)s->map, s->bytes);
    s->map = NULL;
    s->dir = NULL;
    s->metrics = NULL;
    s->nmetrics = 0;
    s->table_usable = false;
}

const struct osoe_metric_descriptor *osoe_segment_metric(const struct osoe_segment *s, const char *name)
{
    for (uint32_t i = 0; i < s->nmetrics; i++) {
        /* Names in the segment are fixed width and need not be terminated. */
        if (strncmp(s->metrics[i].name, name, sizeof s->metrics[i].name) == 0)
            return &s->metrics[i];
    }
    return NULL;
}

/* The record a scope's `index` names, with its length, or NULL. */
static const unsigned char *record(const struct osoe_segment *s, unsigned scope, uint32_t index,
                                   uint64_t *len)
{
    uint64_t off, n;
    switch (scope) {
    case OSOE_MSCOPE_NODE:
        off = 0;
        n = s->dir->header_bytes;
        break;
    case OSOE_MSCOPE_PE:
        if (index >= s->dir->local_count)
            return NULL;
        n = s->dir->slot_bytes;
        off = s->dir->slots_offset + (uint64_t)index * n;
        break;
    case OSOE_MSCOPE_THREAD:
        if (index >= s->dir->block_count)
            return NULL;
        n = s->dir->block_bytes;
        off = s->dir->blocks_offset + (uint64_t)index * n;
        break;
    default:
        return NULL;
    }
    if (n == 0 || off + n < off || off + n > s->bytes)
        return NULL;
    *len = n;
    return s->map + off;
}

bool osoe_segment_read(const struct osoe_segment *s, const struct osoe_metric_descriptor *m,
                   uint32_t index, uint64_t *out)
{
    if (!m || m->type == OSOE_MTYPE_STRING)
        return false;
    uint64_t len;
    const unsigned char *base = record(s, m->scope, index, &len);
    if (!base)
        return false;

    unsigned width = m->type == OSOE_MTYPE_U64 ? 8 : 4;
    if (m->offset > len || len - m->offset < width)
        return false;
    const unsigned char *at = base + m->offset;
    /* The writer's fields are aligned by construction; a segment where they are
     * not is corrupt, and an unaligned atomic load is undefined rather than
     * merely wrong. */
    if ((uintptr_t)at % width != 0)
        return false;

    if (width == 8) {
        *out = atomic_load_explicit((const _Atomic uint64_t *)(const void *)at,
                                    memory_order_acquire);
    } else {
        uint32_t v = atomic_load_explicit((const _Atomic uint32_t *)(const void *)at,
                                          memory_order_acquire);
        *out = m->type == OSOE_MTYPE_I32 ? (uint64_t)(int64_t)(int32_t)v : v;
    }
    return true;
}

bool osoe_segment_read_name(const struct osoe_segment *s, const char *name, uint32_t index,
                        uint64_t *out)
{
    return osoe_segment_read(s, osoe_segment_metric(s, name), index, out);
}

bool osoe_segment_read_str(const struct osoe_segment *s, const char *name, uint32_t index,
                       char *buf, size_t len)
{
    if (len == 0)
        return false;
    buf[0] = '\0';
    const struct osoe_metric_descriptor *m = osoe_segment_metric(s, name);
    if (!m || m->type != OSOE_MTYPE_STRING || m->width == 0)
        return false;
    uint64_t rlen;
    const unsigned char *base = record(s, m->scope, index, &rlen);
    if (!base || m->offset > rlen || rlen - m->offset < m->width)
        return false;

    size_t n = m->width < len - 1 ? m->width : len - 1;
    memcpy(buf, base + m->offset, n);
    buf[n] = '\0';
    return true;
}

uint32_t osoe_segment_block_pe(const struct osoe_segment *s, uint32_t block)
{
    uint32_t per_pe = s->dir->blocks_per_pe;
    return per_pe ? block / per_pe : 0;
}

/*
 * Load the generation, read the fields, load it again. Accept only an
 * unchanged even value; retry otherwise. A PE that republishes through every
 * attempt is publishing waits faster than this loop runs, which is a busy PE
 * and not the hung one this record exists for, so it is reported as unreadable
 * rather than retried forever.
 */
bool osoe_segment_blocked(const struct osoe_segment *s, uint32_t pe, struct osoe_segment_blocked *out)
{
    memset(out, 0, sizeof *out);
    const struct osoe_metric_descriptor *gen = osoe_segment_metric(s, "pe.blocked.gen");
    const struct osoe_metric_descriptor *detail = osoe_segment_metric(s, "pe.blocked.detail");
    const struct osoe_metric_descriptor *since = osoe_segment_metric(s, "pe.blocked.since_ns");
    if (!gen || !detail || !since)
        return false;

    for (int attempt = 0; attempt < 64; attempt++) {
        uint64_t g0, g1;
        if (!osoe_segment_read(s, gen, pe, &g0))
            return false;
        if (g0 & 1)
            continue;                    /* mid-publication */
        if (!osoe_segment_read(s, detail, pe, &out->detail) ||
            !osoe_segment_read(s, since, pe, &out->since_ns))
            return false;
        osoe_segment_read_str(s, "pe.blocked.routine", pe, out->routine, sizeof out->routine);
        if (!osoe_segment_read(s, gen, pe, &g1))
            return false;
        if (g1 != g0)
            continue;
        out->valid = true;
        out->gen = g0;
        out->blocked = out->routine[0] != '\0';
        return true;
    }
    return true;                         /* valid stays false: say so, do not guess */
}

static int by_name(const void *a, const void *b)
{
    return strcmp(a, b);
}

int osoe_segment_list(char (*names)[OSOE_SEGMENT_NAME_MAX], int max)
{
    DIR *d = opendir("/dev/shm");
    if (!d)
        return 0;
    int found = 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        /* Control segments only: a per-PE heap is <control>.heap.<id> and is
         * gigabytes of application memory, not something to list or map. */
        if (strncmp(e->d_name, "osoe.", 5) != 0 || strstr(e->d_name, ".heap."))
            continue;
        /* A control segment's name is bounded by the library's own buffer, so
         * anything longer is some other program's object that happens to start
         * "osoe." - not ours, and not truncated into a name we might unlink. */
        size_t n = strlen(e->d_name);
        if (n + 2 > OSOE_SEGMENT_NAME_MAX)
            continue;
        if (found < max) {
            names[found][0] = '/';
            memcpy(names[found] + 1, e->d_name, n + 1);
        }
        found++;
    }
    closedir(d);
    int n = found < max ? found : max;
    qsort(names, (size_t)n, OSOE_SEGMENT_NAME_MAX, by_name);
    return found;
}
