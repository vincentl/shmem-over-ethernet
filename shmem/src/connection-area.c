#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "base/cacheline.h"
#include "base/deadline.h"
#include "base/environment.h"
#include "base/futex.h"
#include "base/stall-watch.h"
#include "connection-area.h"
#include "fatal.h"
#include "shm-object.h"

#define OSOE_CONNECTION_AREA_MAGIC UINT64_C(0x61657261656f736f)   /* "osoearea" */
#define OSOE_CONNECTION_AREA_LAYOUT 2   /* 2: `opened` */

/*
 * The frozen prefix, for the same reason the control segment has one (§3.14):
 * a reader - or a PE built from a different commit - finds everything else
 * from it. `ready` is last and on its own line because it is the publication
 * point: everything above it is written before it is set.
 */
struct osoe_connection_area_header {
    uint64_t magic;
    uint32_t layout;
    uint32_t node;
    uint32_t nodes;
    int32_t creator_pid;        /* for reclaiming a leftover (shm-object.h) */
    uint64_t buffer_bytes;
    uint64_t chunk_bytes;
    uint64_t conns_offset;      /* nodes-1 struct osoe_connection_shared */
    uint64_t data_offset;       /* nodes-1 pairs of buffer_bytes */
    uint64_t area_bytes;

    /*
     * Published by the lead once the header and every connection are
     * initialized. An opener that trusted the name alone would map an object
     * whose connections still read as "unsealed and empty" - two reservable
     * buffers, the state §3.5 forbids - and reserve into them. The wire learned
     * this the expensive way; the control segment has always had it (§6.2).
     */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t ready;
    /*
     * How many of the processes that must open this object have (§3.14's
     * unlink-early rule). Whichever increment reaches the count removes the
     * name: every process that will ever need it has it open, so from then on
     * no death of any process can strand it. The count is exact, so nothing
     * waits for it - the same shape as the control segment's `finalized` at the
     * other end of a session.
     */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t opened;
};

static uint64_t align_up(uint64_t n)
{
    return (n + OSOE_CACHELINE - 1) / OSOE_CACHELINE * OSOE_CACHELINE;
}

/*
 * Peers are numbered compactly, skipping this node: with 4 nodes, node 1 holds
 * slots for peers 0, 2 and 3 as 0, 1 and 2. Leaving a hole for self would waste
 * two buffers - 256 KiB at the default - on every node for nothing.
 */
static uint32_t peer_slot(uint32_t self, uint32_t peer)
{
    return peer < self ? peer : peer - 1;
}

uint64_t osoe_am_area_bytes(uint32_t nodes, uint64_t buffer)
{
    if (nodes < 2)
        return 0;
    uint64_t peers = nodes - 1;
    uint64_t conns = align_up(sizeof(struct osoe_connection_area_header)) +
                     align_up(peers * sizeof(struct osoe_connection_shared));
    return conns + peers * 2 * buffer;
}

enum osoe_am_size_status osoe_am_choose_buffer(uint64_t chunk, uint64_t requested,
                                               uint64_t budget, uint32_t nodes,
                                               uint64_t *chosen)
{
    uint64_t floor = 2 * chunk;
    *chosen = requested;
    if (nodes < 2)
        return OSOE_AM_SIZE_OK;         /* no peers, so no connections */

    uint64_t peers = nodes - 1;
    uint64_t b = requested;
    while (b > floor && peers * 2 * b > budget)
        b >>= 1;                        /* stays a power of two, lands on the floor */
    *chosen = b;
    if (peers * 2 * b > budget)
        return OSOE_AM_SIZE_NO_ROOM;
    return b < requested ? OSOE_AM_SIZE_SHRUNK : OSOE_AM_SIZE_OK;
}

static uint64_t env_bytes(const char *name, uint64_t dflt, uint64_t lo, uint64_t hi,
                          bool power_of_two)
{
    uint64_t v = dflt;
    switch (osoe_environment_u64(name, &v)) {
    case OSOE_ENVIRONMENT_UNSET:
        return dflt;
    case OSOE_ENVIRONMENT_OK:
        break;
    default:
        osoe_fatal("%s='%s' is not a decimal byte count", name, getenv(name));
    }
    if (v < lo || v > hi)
        osoe_fatal("%s=%llu is outside %llu..%llu", name, (unsigned long long)v,
                   (unsigned long long)lo, (unsigned long long)hi);
    if (power_of_two && (v & (v - 1)) != 0)
        osoe_fatal("%s=%llu is not a power of two", name, (unsigned long long)v);
    return v;
}

void osoe_am_config_read(struct osoe_am_config *c)
{
    c->chunk = env_bytes("OSOE_AM_CHUNK", OSOE_AM_DEFAULT_CHUNK, OSOE_AM_MIN_CHUNK,
                         OSOE_AM_MAX_CHUNK, true);
    /*
     * §3.4's floor, refused rather than silently raised: a buffer below twice
     * the chunk means one message per segment forever, and a user who asked for
     * it has misunderstood something that will not show up until a benchmark.
     */
    c->buffer = env_bytes("OSOE_AM_BUFFER", 2 * c->chunk, 2 * c->chunk,
                          UINT64_C(1) << 30, true);
    c->budget = env_bytes("OSOE_AM_BUFFER_BUDGET", OSOE_AM_DEFAULT_BUDGET,
                          2 * c->chunk, UINT64_C(64) << 30, false);

    /*
     * §3.8's credit C. Tunable because the only way to exercise exhaustion in a
     * test is to make it small: at the default a test would have to issue tens
     * of thousands of gets before the bound bound anything, and a credit that is
     * never reached is a credit nothing checks.
     */
    uint32_t credits = OSOE_AM_DEFAULT_CREDITS;
    if (osoe_environment_u32("OSOE_AM_CREDITS", &credits) == OSOE_ENVIRONMENT_INVALID ||
        credits < OSOE_AM_MIN_CREDITS || credits > OSOE_AM_MAX_CREDITS)
        osoe_fatal("OSOE_AM_CREDITS='%s' is not between %u and %u",
                   getenv("OSOE_AM_CREDITS"), OSOE_AM_MIN_CREDITS, OSOE_AM_MAX_CREDITS);
    c->credits = credits;

    uint32_t huge = 0;
    if (osoe_environment_u32("OSOE_AM_HUGEPAGES", &huge) == OSOE_ENVIRONMENT_INVALID || huge > 1)
        osoe_fatal("OSOE_AM_HUGEPAGES='%s' is not 0 or 1", getenv("OSOE_AM_HUGEPAGES"));
    c->hugepages = huge != 0;
}

void osoe_connection_area_open(struct osoe_connection_area *a, const struct osoe_bootstrap *b,
                        uint32_t nodes, uint32_t session)
{
    *a = (struct osoe_connection_area){ 0 };
    a->node = b->node;
    a->nodes = nodes;
    if (nodes < 2)
        return;                          /* one node: no peers, nothing to map */

    struct osoe_am_config config;
    osoe_am_config_read(&config);
    a->credits = config.credits;

    uint64_t buffer;
    switch (osoe_am_choose_buffer(config.chunk, config.buffer, config.budget, nodes, &buffer)) {
    case OSOE_AM_SIZE_OK:
        break;
    case OSOE_AM_SIZE_SHRUNK:
        /* Loud, because it changes aggregation behaviour and the operator
         * chose neither the size nor the shrink. */
        fprintf(stderr, "osoe: connection buffers reduced from %llu to %llu bytes to "
                        "fit OSOE_AM_BUFFER_BUDGET=%llu over %u nodes\n",
                (unsigned long long)config.buffer, (unsigned long long)buffer,
                (unsigned long long)config.budget, nodes);
        break;
    case OSOE_AM_SIZE_NO_ROOM:
        osoe_fatal("%u nodes need %llu bytes of connection buffers at the smallest "
                   "size §3.4 allows (2 x OSOE_AM_CHUNK = %llu), which exceeds "
                   "OSOE_AM_BUFFER_BUDGET=%llu; raise the budget or lower the chunk",
                   nodes, (unsigned long long)((nodes - 1) * 2 * buffer),
                   (unsigned long long)buffer, (unsigned long long)config.budget);
    }

    a->buffer_bytes = buffer;
    a->chunk_bytes = config.chunk;
    a->bytes = osoe_am_area_bytes(nodes, buffer);
    a->creator = b->local_id == 0;      /* the local lead (§6.2) */
    snprintf(a->name, sizeof a->name, "/osoe.%llu.%u.%u.conn.%u",
             (unsigned long long)b->job, b->step, b->node, session);

    uint32_t peers = nodes - 1;
    int fd;
    if (a->creator) {
        /* The node's buffers to every peer; checked before any of it exists
         * (shm-object.h). */
        uint64_t free_bytes;
        if (!osoe_shm_room(a->bytes, &free_bytes))
            osoe_fatal("/dev/shm has %llu MiB free, but this node's connection area "
                       "for %u nodes needs %llu MiB: lower OSOE_AM_BUFFER, or enlarge "
                       "/dev/shm",
                       (unsigned long long)(free_bytes >> 20), nodes,
                       (unsigned long long)((a->bytes + (1u << 20) - 1) >> 20));
        char err[256];
        fd = osoe_shm_create(a->name,
                             (long)offsetof(struct osoe_connection_area_header, creator_pid),
                             err, sizeof err);
        if (fd < 0)
            osoe_fatal("the connection area: %s", err);
        if (ftruncate(fd, (off_t)a->bytes) != 0) {
            close(fd);
            shm_unlink(a->name);
            osoe_fatal("cannot size connection area: %s", strerror(errno));
        }
    } else {
        /* The name appears before the object is sized and before the header is
         * written, so wait for the length here and for `ready` below (§6.2). */
again:;
        struct osoe_stall_watch watch;
        osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
        struct timespec back = { 0, 200000 };
        for (;;) {
            char err[256];
            fd = osoe_shm_open_ours(a->name, err, sizeof err);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0 && (uint64_t)st.st_size >= a->bytes)
                    break;
                close(fd);
            } else if (errno != ENOENT) {
                osoe_fatal("the connection area: %s", err);
            }
            if (osoe_stall_watch_stalled(&watch, 0))
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the local lead has not created %s",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), a->name);
            nanosleep(&back, NULL);
            if (back.tv_nsec < 10000000)
                back.tv_nsec *= 2;
        }
    }

    /*
     * MAP_HUGETLB only where asked for. This is the opposite of §6.2's rule for
     * heaps, deliberately: heaps are touched densely, while connection buffers
     * are touched sparsely - a halo exchange faults six peers' worth of the
     * whole area - and hugetlbfs pages are reserved up front, which trades away
     * exactly the demand paging that makes a large node count affordable.
     */
    int flags = MAP_SHARED;
    if (config.hugepages)
        flags |= MAP_HUGETLB;
    void *p = mmap(NULL, a->bytes, PROT_READ | PROT_WRITE, flags, fd, 0);
    if (p == MAP_FAILED && config.hugepages) {
        fprintf(stderr, "osoe: OSOE_AM_HUGEPAGES=1 but the connection area could not "
                        "be mapped with huge pages (%s); using ordinary pages\n",
                strerror(errno));
        p = mmap(NULL, a->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    if (p == MAP_FAILED) {
        close(fd);
        if (a->creator)
            shm_unlink(a->name);
        osoe_fatal("cannot map connection area %s: %s", a->name, strerror(errno));
    }
    /* Every PE of the node maps it, and a core of each would hold the node's
     * whole traffic (review round two, omissions 4). */
    madvise(p, a->bytes, MADV_DONTDUMP);
    a->map = p;

    struct osoe_connection_area_header *h = p;
    uint64_t conns_offset = align_up(sizeof *h);
    uint64_t data_offset = conns_offset +
                           align_up((uint64_t)peers * sizeof(struct osoe_connection_shared));

    a->conn = calloc(peers, sizeof *a->conn);
    if (!a->conn)
        osoe_fatal("out of memory for %u connection handles", peers);
    for (uint32_t i = 0; i < peers; i++) {
        a->conn[i].s = (struct osoe_connection_shared *)((char *)p + conns_offset) + i;
        a->conn[i].data = (unsigned char *)p + data_offset + (uint64_t)i * 2 * buffer;
        a->conn[i].capacity = (uint32_t)buffer;
    }

    if (a->creator) {
        h->magic = OSOE_CONNECTION_AREA_MAGIC;
        h->layout = OSOE_CONNECTION_AREA_LAYOUT;
        h->node = b->node;
        h->nodes = nodes;
        h->buffer_bytes = buffer;
        h->chunk_bytes = config.chunk;
        h->conns_offset = conns_offset;
        h->data_offset = data_offset;
        h->area_bytes = a->bytes;
        h->creator_pid = (int32_t)getpid();
        /* Both buffers of every connection sealed, so nothing is reservable
         * until the agent opens one. Zeroed memory would say the opposite
         * (§3.5). */
        for (uint32_t i = 0; i < peers; i++)
            osoe_connection_init(&a->conn[i], a->credits);
        atomic_store_explicit(&h->ready, 1, memory_order_seq_cst);
        osoe_futex_wake(&h->ready, INT_MAX, OSOE_FUTEX_SHARED);
    } else {
        struct osoe_stall_watch watch;
        osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
        while (atomic_load_explicit(&h->ready, memory_order_acquire) == 0) {
            /* A leftover the local lead is about to reclaim will never be
             * ready: start over (shm-object.h). */
            if (osoe_shm_orphaned(fd)) {
                munmap(p, a->bytes);
                close(fd);
                free(a->conn);
                a->conn = NULL;
                a->map = NULL;
                goto again;
            }
            struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
            osoe_futex_wait(&h->ready, 0, &sweep, OSOE_FUTEX_SHARED);
            if (osoe_stall_watch_stalled(&watch, 0))
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the local lead has not finished creating %s",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), a->name);
        }
        if (h->magic != OSOE_CONNECTION_AREA_MAGIC || h->layout != OSOE_CONNECTION_AREA_LAYOUT ||
            h->buffer_bytes != buffer || h->chunk_bytes != config.chunk ||
            h->nodes != nodes)
            osoe_fatal("connection area %s was created with different AM sizing "
                       "(buffer %llu chunk %llu nodes %u, not %llu/%llu/%u): every PE "
                       "of a job must set OSOE_AM_* the same", a->name,
                       (unsigned long long)h->buffer_bytes,
                       (unsigned long long)h->chunk_bytes, h->nodes,
                       (unsigned long long)buffer, (unsigned long long)config.chunk, nodes);
    }
    close(fd);
    /* Every local PE opens the area; the last to have done so removes its name. */
    if (atomic_fetch_add_explicit(&h->opened, 1, memory_order_seq_cst) + 1 == b->local_count &&
        shm_unlink(a->name) != 0)
        osoe_fatal("cannot remove the connection area's name %s: %s", a->name, strerror(errno));
}

void osoe_connection_area_close(struct osoe_connection_area *a)
{
    free(a->conn);
    if (a->map)
        munmap(a->map, a->bytes);       /* its name went when the last PE opened it */
    *a = (struct osoe_connection_area){ 0 };
}

struct osoe_connection *osoe_connection_to(struct osoe_connection_area *a, uint32_t peer)
{
    if (peer == a->node || peer >= a->nodes || !a->conn)
        return NULL;
    return &a->conn[peer_slot(a->node, peer)];
}
