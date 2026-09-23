/*
 * The segment reader (segment.h). libc only, and no header from the library.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "segment.h"

/*
 * Only the two structures the design freezes may be known by layout, and a
 * reader that silently disagreed about their size would read nonsense from a
 * segment it had just declared valid.
 *
 * These numbers move when a field is *appended*, which §3.14 allows, and never
 * for any other reason - so a failure here is either an intended append whose
 * number needs updating in both copies, or a reorder or resize that must not
 * happen. The directory grew to 136 at layout 6, with the landing zones.
 *
 * The library declares the same structure independently in shmem/src/node.h,
 * deliberately: a tool that included the library's headers would only prove the
 * library agrees with itself. The cost is two copies that must not drift, and
 * the matching assertion there is what makes a drift in size fail the build
 * rather than the job.
 */
_Static_assert(sizeof(struct osoe_metric_descriptor) == 64, "frozen by §3.14");
_Static_assert(sizeof(struct osoe_segment_directory) == 160,
               "frozen by §3.14: append only. 136 at layout 6; 160 at layout 12");

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

/*
 * How many `element_bytes`-sized records fit between `offset` and `next` (the
 * following section's own offset, or `segment_bytes` when there is none, or
 * `next` is itself nonsense) - clamped further by `raw`, the directory's own
 * count, so a small honest count is never inflated (D-7). Used before a count
 * from the directory becomes a loop bound: a single record() call already
 * bounds one index against the mapping, but that check does nothing for a
 * count a directory claims and a loop believes without ever landing outside
 * the file - `local_count` or `block_count` set to four billion turns "every
 * PE" into a loop that runs for as long as the process is left running.
 */
static uint32_t clamp_count(uint64_t offset, uint32_t element_bytes, uint64_t next,
                            uint64_t segment_bytes, uint32_t raw)
{
    if (element_bytes == 0 || offset > segment_bytes)
        return 0;
    uint64_t bound = (next >= offset && next <= segment_bytes) ? next : segment_bytes;
    uint64_t max_count = (bound - offset) / element_bytes;
    if (max_count > UINT32_MAX)
        max_count = UINT32_MAX;
    return raw < (uint32_t)max_count ? raw : (uint32_t)max_count;
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

/* A path to open as it is, not an object name: /proc/<pid>/fd/<n>, or a plain
 * file anywhere but /dev/shm. */
static bool is_plain_path(const char *spec)
{
    return spec[0] == '/' && strchr(spec + 1, '/') && strncmp(spec, "/dev/shm/", 9) != 0;
}

/*
 * A control segment's name: "osoe." and four decimal fields, job, step, node
 * and session. Everything else the library names starts "osoe." too - a heap,
 * a connection area, the job segment, a wire - and is not a control segment.
 */
static bool is_control_name(const char *n)
{
    if (strncmp(n, "osoe.", 5) != 0)
        return false;
    int fields = 0;
    for (const char *p = n + 5; *p; ) {
        if (*p < '0' || *p > '9')
            return false;
        while (*p >= '0' && *p <= '9')
            p++;
        fields++;
        if (*p == '.')
            p++;
        else if (*p)
            return false;
    }
    return fields == 4;
}

/* Why a search of /proc found nothing: the three cases a user has to tell apart. */
enum proc_search { PROC_NOTHING, PROC_DENIED, PROC_RESTRICTED };

/* Is /proc mounted with hidepid, which hides other users' processes entirely? */
static bool proc_restricted(void)
{
    FILE *f = fopen("/proc/self/mountinfo", "r");
    if (!f)
        return false;
    char line[1024];
    bool hidden = false;
    while (fgets(line, sizeof line, f))
        if (strstr(line, " /proc ") && strstr(line, "hidepid=") &&
            !strstr(line, "hidepid=0") && !strstr(line, "hidepid=off"))
            hidden = true;
    fclose(f);
    return hidden;
}

/*
 * Every file descriptor of every visible process that refers to a /dev/shm
 * object named like a control segment - linked or not - calls `visit` with the
 * object's name ("/osoe....") and the path that opens it ("/proc/<pid>/fd/<n>").
 * A visit returning true stops the walk. This is how a segment is found once
 * its name has been removed (§3.14): every PE keeps its control segment open
 * for the life of the session, so the kernel still knows the object, and
 * /proc/<pid>/fd/<n> opens that same object for any process allowed to see it.
 * /proc/<pid>/map_files/ is not used: it needs CAP_SYS_ADMIN.
 */
static enum proc_search walk_proc(bool (*visit)(void *, const char *, const char *), void *ctx)
{
    DIR *procs = opendir("/proc");
    if (!procs)
        return PROC_RESTRICTED;
    bool denied = false;
    const struct dirent *p;
    while ((p = readdir(procs))) {
        if (p->d_name[0] < '1' || p->d_name[0] > '9')
            continue;
        char dir[300];
        snprintf(dir, sizeof dir, "/proc/%.255s/fd", p->d_name);
        DIR *fds = opendir(dir);
        if (!fds) {
            if (errno == EACCES)
                denied = true;
            continue;
        }
        const struct dirent *f;
        while ((f = readdir(fds))) {
            if (f->d_name[0] == '.')
                continue;
            char link[600], target[256];
            snprintf(link, sizeof link, "%s/%.255s", dir, f->d_name);
            ssize_t n = readlink(link, target, sizeof target - 1);
            if (n <= 0)
                continue;
            target[n] = '\0';
            if (strncmp(target, "/dev/shm/", 9) != 0)
                continue;
            char *deleted = strstr(target, " (deleted)");
            if (deleted)
                *deleted = '\0';
            if (!is_control_name(target + 9))
                continue;
            if (visit(ctx, target + 8, link)) {
                closedir(fds);
                closedir(procs);
                return PROC_NOTHING;
            }
        }
        closedir(fds);
    }
    closedir(procs);
    if (proc_restricted())
        return PROC_RESTRICTED;
    return denied ? PROC_DENIED : PROC_NOTHING;
}

struct find_one {
    const char *name;
    char path[600];
    bool found;
};

static bool match_name(void *ctx, const char *name, const char *path)
{
    struct find_one *f = ctx;
    if (strcmp(name, f->name) != 0)
        return false;
    snprintf(f->path, sizeof f->path, "%s", path);
    f->found = true;
    return true;
}

int osoe_segment_open(struct osoe_segment *s, const char *name, char *err, size_t errlen)
{
    memset(s, 0, sizeof *s);
    int fd;
    if (is_plain_path(name)) {
        snprintf(s->name, sizeof s->name, "%s", name);
        fd = open(name, O_RDONLY);
        if (fd < 0)
            return fail(err, errlen, "cannot open %s: %s", name, strerror(errno));
    } else {
        normalize(name, s->name, sizeof s->name);
        /* A named object first: a segment kept by OSOE_KEEP_CONTROL_SEGMENT, or
         * one from a library that removed names only at finalize. Then the
         * route that works for a live job's unlinked segment: a process that
         * holds it open, through /proc (§3.14). */
        fd = shm_open(s->name, O_RDONLY, 0);
        if (fd < 0 && errno == ENOENT) {
            struct find_one f = { .name = s->name };
            enum proc_search why = walk_proc(match_name, &f);
            if (f.found)
                fd = open(f.path, O_RDONLY);
            else if (why == PROC_RESTRICTED)
                return fail(err, errlen, "%s: not found, and /proc is restricted (hidepid), "
                            "so only this user's processes can be searched at all", s->name);
            else
                /* Other users' processes cannot be looked into (their /proc/<pid>/fd
                 * is theirs), so "not your job" is a possibility to name, never a
                 * finding: every machine has root's processes in /proc. */
                return fail(err, errlen, "%s: no such job - nothing in /dev/shm by that name, "
                            "and no process of this user holds it%s", s->name,
                            why == PROC_DENIED ? " (a job of another user would not be visible "
                                                 "here: its processes cannot be searched)" : "");
        }
        if (fd < 0)
            return fail(err, errlen, "cannot open %s: %s", s->name, strerror(errno));
    }
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

    if (s->dir->magic != OSOE_SEGMENT_MAGIC) {
        unsigned long long magic = (unsigned long long)s->dir->magic;
        char shown[OSOE_SEGMENT_NAME_MAX];
        memcpy(shown, s->name, sizeof shown);
        osoe_segment_close(s);              /* nothing below may touch the mapping */
        return fail(err, errlen, "/dev/shm%s is not an osoe control segment "
                    "(magic %#llx)", shown, magic);
    }
    /*
     * From here on nothing is refused. An unknown layout is read like any
     * other, since everything past the directory is found by name; a short
     * directory or a table this reader cannot parse leaves `table_usable` false
     * and the caller prints what it does know. That is what freezing the
     * directory bought, and refusing here would spend it.
     */

    /* Clamp the two counts a caller loops over, before anything loops over
     * them (D-7). The slots section's "next section" is metrics; the blocks
     * section's is landing, appended at layout 6, or the end of the segment
     * on an older directory that has none. */
    if (OSOE_SEGMENT_HAS(s->dir, local_count))
        s->local_count = clamp_count(s->dir->slots_offset, s->dir->slot_bytes,
                                     s->dir->metrics_offset, s->bytes, s->dir->local_count);
    if (OSOE_SEGMENT_HAS(s->dir, connection_count))
        s->connection_count = clamp_count(s->dir->connections_offset, s->dir->connection_bytes,
                                          s->bytes, s->bytes, s->dir->connection_count);
    if (OSOE_SEGMENT_HAS(s->dir, block_count)) {
        uint64_t after_blocks = OSOE_SEGMENT_HAS(s->dir, landing_offset)
                                 ? s->dir->landing_offset : s->bytes;
        s->block_count = clamp_count(s->dir->blocks_offset, s->dir->block_bytes,
                                     after_blocks, s->bytes, s->dir->block_count);
    }

    /* Ask for the fields the table needs, not for the whole directory: see
     * OSOE_SEGMENT_HAS. Requiring the whole structure discarded the table of
     * every segment written by an older library. */
    if (!OSOE_SEGMENT_HAS(s->dir, metric_descriptor_bytes))
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
        if (index >= s->local_count)
            return NULL;
        n = s->dir->slot_bytes;
        off = s->dir->slots_offset + (uint64_t)index * n;
        break;
    case OSOE_MSCOPE_THREAD:
        if (index >= s->block_count)
            return NULL;
        n = s->dir->block_bytes;
        off = s->dir->blocks_offset + (uint64_t)index * n;
        break;
    case OSOE_MSCOPE_CONNECTION:
        if (index >= s->connection_count)
            return NULL;
        n = s->dir->connection_bytes;
        off = s->dir->connections_offset + (uint64_t)index * n;
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
        uint64_t kind = 0;
        const struct osoe_metric_descriptor *k = osoe_segment_metric(s, "pe.blocked.detail_kind");
        if (k && osoe_segment_read(s, k, pe, &kind))
            out->detail_kind = (uint32_t)kind;
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

void osoe_segment_blocked_detail(const struct osoe_segment_blocked *b, char *buf, size_t len)
{
    switch (b->detail_kind) {
    case 1:  snprintf(buf, len, "node %" PRIu64, b->detail); break;
    case 2:  snprintf(buf, len, "epoch %" PRIu64, b->detail); break;
    case 3:  snprintf(buf, len, "offset %#" PRIx64, b->detail); break;
    case 4:  snprintf(buf, len, "PE %" PRIu64, b->detail); break;
    default: snprintf(buf, len, "+%#" PRIx64, b->detail); break;
    }
}

static int by_name(const void *a, const void *b)
{
    return strcmp(a, b);
}

struct name_list {
    char (*names)[OSOE_SEGMENT_NAME_MAX];
    int max, found;
};

static void add_name(struct name_list *l, const char *name)
{
    int n = l->found < l->max ? l->found : l->max;
    for (int i = 0; i < n; i++)
        if (strcmp(l->names[i], name) == 0)
            return;
    if (l->found < l->max)
        snprintf(l->names[l->found], OSOE_SEGMENT_NAME_MAX, "%.*s", OSOE_SEGMENT_NAME_MAX - 1,
                 name);
    l->found++;
}

static bool collect(void *ctx, const char *name, const char *path)
{
    (void)path;
    add_name(ctx, name);
    return false;
}

static int list_named(struct name_list *l)
{
    DIR *d = opendir("/dev/shm");
    if (!d)
        return 0;
    const struct dirent *e;
    while ((e = readdir(d))) {
        if (!is_control_name(e->d_name) || strlen(e->d_name) + 2 > OSOE_SEGMENT_NAME_MAX)
            continue;
        char name[OSOE_SEGMENT_NAME_MAX];
        snprintf(name, sizeof name, "/%.*s", OSOE_SEGMENT_NAME_MAX - 2, e->d_name);
        add_name(l, name);
    }
    closedir(d);
    return l->found;
}

int osoe_segment_list_named(char (*names)[OSOE_SEGMENT_NAME_MAX], int max)
{
    struct name_list l = { names, max, 0 };
    list_named(&l);
    int n = l.found < max ? l.found : max;
    qsort(names, (size_t)n, OSOE_SEGMENT_NAME_MAX, by_name);
    return l.found;
}

bool osoe_segment_mapped(const char *name)
{
    char want[OSOE_SEGMENT_NAME_MAX + 16];
    snprintf(want, sizeof want, "/dev/shm%s", name);
    size_t want_len = strlen(want);
    DIR *procs = opendir("/proc");
    if (!procs)
        return true;                    /* cannot tell: assume the worst */
    bool mapped = false;
    char self[16];
    snprintf(self, sizeof self, "%d", (int)getpid());
    const struct dirent *p;
    while (!mapped && (p = readdir(procs))) {
        /* Not this process: the caller has just mapped the segment to read
         * its slots, and that mapping says nothing about the job. */
        if (p->d_name[0] < '1' || p->d_name[0] > '9' || strcmp(p->d_name, self) == 0)
            continue;
        char path[300];
        snprintf(path, sizeof path, "/proc/%.255s/maps", p->d_name);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        char line[1024];
        while (fgets(line, sizeof line, f)) {
            char *at = strstr(line, want);
            if (at && (at[want_len] == '\n' || at[want_len] == ' ' || at[want_len] == '\0')) {
                mapped = true;
                break;
            }
        }
        fclose(f);
    }
    closedir(procs);
    return mapped;
}

int osoe_segment_list(char (*names)[OSOE_SEGMENT_NAME_MAX], int max)
{
    struct name_list l = { names, max, 0 };
    /* Named segments: kept ones, and those of libraries that kept the name.
     * A control segment's name is bounded by the library's own buffer, so
     * anything longer is some other program's object that happens to start
     * "osoe." - not ours, and not truncated into a name we might unlink. */
    list_named(&l);
    /* Live jobs, whose segments have no name: found through /proc (§3.14). */
    walk_proc(collect, &l);
    int n = l.found < max ? l.found : max;
    qsort(names, (size_t)n, OSOE_SEGMENT_NAME_MAX, by_name);
    return l.found;
}
