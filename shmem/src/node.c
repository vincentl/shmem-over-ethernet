#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "base/deadline.h"
#include "base/futex.h"
#include "base/stall-watch.h"
#include "fatal.h"
#include "shm-object.h"
#include "connection-area.h"

#include "metrics.h"
#include "node.h"

/*
 * The writer's half of §3.14's frozen prefix. tools/stat declares the same
 * structure independently, so that the reader proves something about the
 * segment rather than about the library's own headers; these assertions are
 * what keep the two copies the same size. Both move together when a field is
 * appended, and never otherwise.
 */
_Static_assert(sizeof(struct osoe_metric_descriptor) == 64, "frozen by §3.14");
_Static_assert(sizeof(struct osoe_segment_directory) == 160,
               "frozen by §3.14: append only. 136 at layouts 6 and 7; 160 at layout 12, "
               "with the connections section");

/*
 * Candidate heap bases, tried in order by the lead; the first where its own
 * heap maps becomes B for the node (§4.1). No single address is free under
 * every build in the development VM (aarch64, 48-bit): AddressSanitizer and
 * ThreadSanitizer reserve disjoint ranges. The list covers a plain build, ASan,
 * TSan, and 39-bit address spaces, in that order. OSOE_HEAP_BASE replaces it.
 */
static const uint64_t heap_base_candidates[] = {
    UINT64_C(0x100000000000),
    UINT64_C(0x300000000000),
    UINT64_C(0x010000000000),
    UINT64_C(0x001000000000),
};

/* Sections of the control segment start on a cache line, so none shares one. */
static uint64_t align_up(uint64_t n)
{
    return (n + OSOE_CACHELINE - 1) / OSOE_CACHELINE * OSOE_CACHELINE;
}

static uint64_t page_round(uint64_t n)
{
    uint64_t page = (uint64_t)sysconf(_SC_PAGESIZE);
    if (n > UINT64_MAX - (page - 1))
        osoe_fatal("size %llu too large to map", (unsigned long long)n);
    return (n + page - 1) / page * page;
}

/* Publish failure so waiting siblings stop at their next sweep: osoe_fatal_then's
 * last act, after the message is out (fatal.h). */
static void publish_failure(void *context)
{
    struct osoe_node *n = context;
    if (n->hdr) {
        /* release: checked by waiters at each wake and sweep (§3.16). */
        atomic_store_explicit(&n->hdr->failed, 1, memory_order_release);
        osoe_futex_wake(&n->hdr->ready, INT_MAX, OSOE_FUTEX_SHARED);
        osoe_futex_wake(&n->hdr->joined, INT_MAX, OSOE_FUTEX_SHARED);
        osoe_futex_wake(&n->hdr->mapped, INT_MAX, OSOE_FUTEX_SHARED);
    }
}

_Noreturn static void fail_rendezvous(struct osoe_node *n, const char *what)
{
    osoe_fatal_then(publish_failure, n, "%s", what);
}

static void check_failed(struct osoe_node *n)
{
    if (atomic_load_explicit(&n->hdr->failed, memory_order_acquire))
        osoe_fatal("another local PE failed during rendezvous");
}

/*
 * Wait until *word == target, the rendezvous fails, or the bootstrap deadline
 * passes. A failure wake can be missed (the failing PE stores `failed` and then
 * wakes without bumping the word), so it is noticed at the next sweep at worst.
 */
/* Wait for a rendezvous count to reach its target; -1 once it has stopped
 * moving for the no-progress limit (§3.15), which by default is never. */
static int wait_count(struct osoe_node *n, _Atomic uint32_t *word, uint32_t target,
                      uint64_t no_progress_ns)
{
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, atomic_load_explicit(word, memory_order_acquire),
                           no_progress_ns);
    for (;;) {
        uint32_t v = atomic_load_explicit(word, memory_order_acquire);
        check_failed(n);
        if (v == target)
            return 0;
        if (osoe_stall_watch_stalled(&watch, v))
            return -1;
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_futex_wait(word, v, &sweep, OSOE_FUTEX_SHARED);
    }
}

static void describe_missing(struct osoe_node *n, uint32_t count, enum osoe_phase phase,
                             char *buf, size_t len)
{
    size_t off = 0;
    buf[0] = '\0';
    for (uint32_t i = 0; i < count && off + 16 < len; i++)
        if (atomic_load_explicit(&n->slots[i].phase, memory_order_acquire) < phase)
            off += (size_t)snprintf(buf + off, len - off, " %u", i);
}

/* Create this PE's heap object and map it. With base == 0, try the
 * candidates and return the one that worked; 0 if none did. */
static uint64_t create_own_heap(struct osoe_node *n, const struct osoe_bootstrap *b,
                                struct osoe_pe_slot *slot, uint64_t base)
{
    /* A heap has nowhere to record its creator; a leftover with this name
     * predates the control segment this node's lead has just created, and so
     * is a dead job's, reclaimed if it is this user's. */
    char err[256];
    int fd = osoe_shm_create(slot->heap_name, OSOE_SHM_NO_OWNER, err, sizeof err);
    if (fd < 0) {
        char msg[320];
        snprintf(msg, sizeof msg, "this PE's heap: %s", err);
        fail_rendezvous(n, msg);
    }
    if (ftruncate(fd, (off_t)n->heap_bytes) != 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "cannot size heap %s to %llu bytes: %s", slot->heap_name,
                 (unsigned long long)n->heap_bytes, strerror(errno));
        close(fd);
        fail_rendezvous(n, msg);
    }

    const uint64_t *candidates = heap_base_candidates;
    size_t ncandidates = sizeof heap_base_candidates / sizeof heap_base_candidates[0];
    if (base) {
        candidates = &base;
        ncandidates = 1;
    } else if (b->heap_base) {
        candidates = &b->heap_base;
        ncandidates = 1;
    }

    /* B is what is returned and published; the heap itself sits past it by
     * this PE's shift, which is 0 unless §8.2's test mode is on. */
    uint64_t shift = b->heap_shift * b->rank;
    uint64_t mapped_at = 0;
    for (size_t i = 0; i < ncandidates && !mapped_at; i++) {
        void *want = (void *)(uintptr_t)(candidates[i] + shift);
        void *p = mmap(want, n->heap_bytes, PROT_READ | PROT_WRITE,
                       MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (p == want)
            mapped_at = candidates[i];
        else if (p != MAP_FAILED)
            munmap(p, n->heap_bytes);   /* a kernel without NOREPLACE placed it elsewhere */
    }
    close(fd);
    return mapped_at;
}

static void map_siblings(struct osoe_node *n, const struct osoe_bootstrap *b)
{
    for (uint32_t i = 0; i < b->local_count; i++) {
        if (i == b->local_id)
            continue;
        struct osoe_pe_slot *s = &n->slots[i];
        char err[256];
        int fd = osoe_shm_open_ours(s->heap_name, err, sizeof err);
        if (fd < 0) {
            char msg[512];
            snprintf(msg, sizeof msg, "local PE %u's heap: %s", i,
                     err[0] ? err : strerror(errno));
            fail_rendezvous(n, msg);
        }
        void *p = mmap(NULL, s->heap_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        close(fd);
        if (p == MAP_FAILED) {
            char msg[256];
            snprintf(msg, sizeof msg, "cannot map local PE %u's heap (%llu bytes): %s", i,
                     (unsigned long long)s->heap_bytes, strerror(errno));
            fail_rendezvous(n, msg);
        }
        /* A sibling's heap is in the sibling's own core; in this one it would
         * make every core L heaps large (review round two, omissions 4). */
        madvise(p, s->heap_bytes, MADV_DONTDUMP);
        n->heap_of[i] = p;
    }
}

/* Every local PE must have parsed the same SHMEM_SYMMETRIC_SIZE (§4.1, §4.4).
 * Every PE runs this check on the same slots, so all of them fail together. */
static void check_agreement(struct osoe_node *n, const struct osoe_bootstrap *b)
{
    for (uint32_t i = 0; i < b->local_count; i++) {
        if (n->slots[i].symmetric_size != b->symmetric_size) {
            char msg[256];
            snprintf(msg, sizeof msg, "SHMEM_SYMMETRIC_SIZE differs between PEs: local PE "
                     "%u has %llu bytes, this PE has %llu", i,
                     (unsigned long long)n->slots[i].symmetric_size,
                     (unsigned long long)b->symmetric_size);
            fail_rendezvous(n, msg);
        }
        if (n->slots[i].rank != b->gtids[i]) {
            char msg[256];
            snprintf(msg, sizeof msg, "local PE %u joined as rank %u, but %sGTIDS "
                     "names rank %u", i, n->slots[i].rank,
                     b->launcher ? b->launcher : "OSOE_", b->gtids[i]);
            fail_rendezvous(n, msg);
        }
    }
}

/* The lead creates and initializes the control segment. */
static void create_control(struct osoe_node *n, const struct osoe_bootstrap *b, uint32_t session)
{
    /*
     * Four sections: the header, the PE slots, the §3.14 metric table, and the
     * counter blocks. Each starts on a cache line, so no two sections share
     * one. The header records where each begins, which is the whole directory
     * osoe-stat needs before it can read the rest by name.
     */
    uint32_t per_pe = osoe_metrics_blocks_per_pe();
    uint32_t metrics_count;
    const struct osoe_metric_descriptor *metrics = osoe_metrics_table(&metrics_count);

    uint64_t slots_offset = align_up(sizeof(struct osoe_node_header));
    uint64_t metrics_offset =
        align_up(slots_offset + (uint64_t)b->local_count * sizeof(struct osoe_pe_slot));
    uint64_t metrics_bytes = (uint64_t)metrics_count * sizeof *metrics;
    uint64_t blocks_offset = align_up(metrics_offset + metrics_bytes);
    uint64_t block_count = (uint64_t)b->local_count * per_pe;
    uint64_t blocks_bytes = block_count * sizeof(struct osoe_counter_block);

    /*
     * The §3.12 landing zones, one slot per local PE, each holding one AM chunk
     * (§3.2). Sized from the same OSOE_AM_CHUNK the AM layer uses, because a
     * reply chunk has to fit: a landing zone smaller than a chunk would make
     * the wire's maximum message undeliverable, which is a hang rather than a
     * short read.
     */
    struct osoe_am_config am;
    osoe_am_config_read(&am);
    uint64_t landing_slot_bytes =
        align_up(sizeof(struct osoe_landing_slot) + am.chunk);
    uint64_t landing_offset = align_up(blocks_offset + blocks_bytes);
    uint64_t landing_bytes = (uint64_t)b->local_count * landing_slot_bytes;

    /*
     * One record per connection toward a peer node (§3.14's connection scope,
     * layout 12). Sized by node count, never by PE count (§1): 128 bytes per
     * peer, so 512 KiB per node at 4,096 nodes, in the segment every PE of the
     * node already maps. A job of one node has none.
     */
    uint64_t connections_offset = align_up(landing_offset + landing_bytes);
    uint64_t connection_count = b->node_count > 1 ? b->node_count - 1 : 0;
    uint64_t connections_bytes = connection_count * sizeof(struct osoe_am_connection_metrics);
    n->control_bytes = page_round(connections_offset + connections_bytes);

    /*
     * The node's whole request, checked once and before any of it exists: this
     * segment and every local PE's heap. The lead knows L and the heap size;
     * every PE has parsed the same size, or check_agreement fails them all.
     */
    uint64_t wanted = n->control_bytes + (uint64_t)b->local_count * n->heap_bytes;
    uint64_t free_bytes;
    if (!osoe_shm_room(wanted, &free_bytes)) {
        char msg[320];
        snprintf(msg, sizeof msg, "/dev/shm has %llu MiB free, but this node's control "
                 "segment and %u heaps of %llu MiB need %llu MiB: lower "
                 "SHMEM_SYMMETRIC_SIZE, run fewer PEs per node, or enlarge /dev/shm",
                 (unsigned long long)(free_bytes >> 20), b->local_count,
                 (unsigned long long)(n->heap_bytes >> 20),
                 (unsigned long long)((wanted + (1u << 20) - 1) >> 20));
        osoe_shm_short(b->shm_capacity_warning_only, msg);
    }

    char err[256];
    int fd = osoe_shm_create(n->control_name,
                             (long)offsetof(struct osoe_node_header, creator_pid),
                             err, sizeof err);
    if (fd < 0)
        osoe_fatal("the control segment: %s", err);
    if (ftruncate(fd, (off_t)n->control_bytes) != 0) {
        close(fd);
        shm_unlink(n->control_name);
        osoe_fatal("cannot size control segment: %s", strerror(errno));
    }
    void *p = mmap(NULL, n->control_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        shm_unlink(n->control_name);
        osoe_fatal("cannot map control segment: %s", strerror(errno));
    }
    n->control_fd = fd;             /* kept: see struct osoe_node */

    /* ftruncate zeroed it: every atomic word starts at 0. */
    n->hdr = p;
    n->hdr->creator_pid = (int32_t)getpid();
    n->slots = (struct osoe_pe_slot *)((char *)p + slots_offset);
    struct osoe_node_header *h = n->hdr;
    h->dir.magic = OSOE_NODE_MAGIC;
    h->dir.layout = OSOE_NODE_LAYOUT;
    h->dir.header_bytes = sizeof *h;
    h->dir.segment_bytes = n->control_bytes;
    h->job = b->job;
    h->step = b->step;
    h->node = b->node;
    h->session = session;
    h->dir.local_count = b->local_count;
    h->world_size = b->world_size;
    h->dir.slot_bytes = sizeof(struct osoe_pe_slot);
    h->dir.slots_offset = slots_offset;
    h->dir.metrics_offset = metrics_offset;
    h->dir.metrics_bytes = metrics_bytes;
    h->dir.metrics_count = metrics_count;
    h->dir.metrics_checksum = osoe_metric_checksum(metrics, metrics_count);
    h->dir.directory_bytes = sizeof h->dir;
    h->dir.metric_descriptor_bytes = sizeof *metrics;
    h->dir.blocks_offset = blocks_offset;
    h->dir.blocks_bytes = blocks_bytes;
    h->dir.block_count = (uint32_t)block_count;
    h->dir.blocks_per_pe = per_pe;
    h->dir.landing_offset = landing_offset;
    h->dir.landing_bytes = landing_bytes;
    h->dir.landing_slot_bytes = (uint32_t)landing_slot_bytes;
    h->dir.landing_count = b->local_count;
    h->dir.connections_offset = connections_offset;
    h->dir.connections_bytes = connections_bytes;
    h->dir.connection_bytes = (uint32_t)sizeof(struct osoe_am_connection_metrics);
    h->dir.connection_count = (uint32_t)connection_count;
    h->dir.block_bytes = (uint32_t)sizeof(struct osoe_counter_block);
    memcpy((char *)p + metrics_offset, metrics, metrics_bytes);
    n->blocks = (struct osoe_counter_block *)((char *)p + blocks_offset);
    n->landing = (unsigned char *)p + landing_offset;
    n->landing_slot_bytes = (uint32_t)landing_slot_bytes;
    n->connection_metrics = connection_count
        ? (struct osoe_am_connection_metrics *)((char *)p + connections_offset) : NULL;
    n->connection_count = (uint32_t)connection_count;
    /*
     * Which machine this logical node is on (§3.14): the node numbers a
     * hung collective names are logical, and a walk down the tree needs the
     * host to run osoe-stat on next. Truncation is not an error - it is a
     * label - so the result is terminated by hand.
     */
    if (gethostname(h->hostname, sizeof h->hostname) != 0)
        snprintf(h->hostname, sizeof h->hostname, "unknown");
    h->hostname[sizeof h->hostname - 1] = '\0';
    osoe_epoch_init(&h->world_arrived);
    osoe_epoch_init(&h->world_released);
    osoe_epoch_init(&h->world_copied);
    osoe_epoch_init(&h->shared_arrived);
    osoe_epoch_init(&h->shared_released);
    osoe_epoch_init(&h->shared_copied);
    osoe_epoch_init(&h->finalize_arrived);
    osoe_epoch_init(&h->finalize_released);
}

/* A non-lead opens the control segment by name, waiting for the lead to
 * create and publish it. */
static void open_control(struct osoe_node *n, const struct osoe_bootstrap *b, uint32_t session)
{
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
again:;
    struct timespec backoff = { 0, 1000 * 1000 };   /* 1 ms, doubling to 64 ms */
    for (;;) {
        char err[256];
        int fd = osoe_shm_open_ours(n->control_name, err, sizeof err);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && (uint64_t)st.st_size >= sizeof(struct osoe_node_header)) {
                void *p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                               fd, 0);
                if (p == MAP_FAILED) {
                    close(fd);
                    osoe_fatal("cannot map control segment %s: %s", n->control_name,
                               strerror(errno));
                }
                n->control_fd = fd;     /* kept: see struct osoe_node */
                n->hdr = p;
                n->control_bytes = (uint64_t)st.st_size;
                break;
            }
            close(fd);          /* created but not yet sized */
        } else if (errno != ENOENT) {
            osoe_fatal("the control segment: %s", err);
        }
        if (osoe_stall_watch_stalled(&watch, 0))
            osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the local lead "
                       "has not created %s",
                       (unsigned long long)osoe_stall_watch_seconds(&watch), n->control_name);
        nanosleep(&backoff, NULL);
        if (backoff.tv_nsec < 64 * 1000 * 1000)
            backoff.tv_nsec *= 2;
    }

    /* Sized is not ready: the lead publishes `ready` after the header. What
     * was opened may also be a leftover the lead is about to reclaim, which
     * will never be ready (shm-object.h): then start over. */
    for (;;) {
        if (atomic_load_explicit(&n->hdr->ready, memory_order_acquire))
            break;
        if (osoe_shm_orphaned(n->control_fd)) {
            munmap(n->hdr, n->control_bytes);
            close(n->control_fd);
            n->hdr = NULL;
            n->control_fd = -1;
            goto again;
        }
        check_failed(n);
        if (osoe_stall_watch_stalled(&watch, 0))
            osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the local lead "
                       "has not initialized %s",
                       (unsigned long long)osoe_stall_watch_seconds(&watch), n->control_name);
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_futex_wait(&n->hdr->ready, 0, &sweep, OSOE_FUTEX_SHARED);
    }

    struct osoe_node_header *h = n->hdr;
    if (h->dir.magic != OSOE_NODE_MAGIC || h->dir.layout != OSOE_NODE_LAYOUT ||
        h->dir.header_bytes != sizeof *h || h->dir.slot_bytes != sizeof(struct osoe_pe_slot))
        osoe_fatal("control segment %s has layout %u; this library expects %u (mixed "
                   "library versions?)", n->control_name, h->dir.layout, OSOE_NODE_LAYOUT);
    if (h->job != b->job || h->step != b->step || h->node != b->node ||
        h->session != session || h->dir.local_count != b->local_count ||
        h->world_size != b->world_size)
        osoe_fatal("control segment %s describes a different job: %u local of %u PEs, "
                   "session %u; this PE expects %u of %u, session %u", n->control_name,
                   h->dir.local_count, h->world_size, h->session, b->local_count,
                   b->world_size, session);
    if (h->dir.slots_offset + (uint64_t)h->dir.local_count * h->dir.slot_bytes > n->control_bytes ||
        h->dir.blocks_offset + h->dir.blocks_bytes > n->control_bytes ||
        h->dir.connections_offset + h->dir.connections_bytes > n->control_bytes ||
        h->dir.metrics_offset + h->dir.metrics_bytes > n->control_bytes)
        osoe_fatal("control segment %s is too small for its own directory",
                   n->control_name);
    n->slots = (struct osoe_pe_slot *)((char *)n->hdr + h->dir.slots_offset);
    n->blocks = (struct osoe_counter_block *)((char *)n->hdr + h->dir.blocks_offset);
    /* Read from the directory rather than recomputed: a PE that derived the
     * offset itself would disagree with the lead the moment the two ran
     * different builds, which is what the frozen directory exists to prevent. */
    n->landing = (unsigned char *)n->hdr + h->dir.landing_offset;
    n->landing_slot_bytes = h->dir.landing_slot_bytes;
    n->connection_metrics = h->dir.connection_count
        ? (struct osoe_am_connection_metrics *)((char *)n->hdr + h->dir.connections_offset)
        : NULL;
    n->connection_count = h->dir.connection_count;

    /* The metric table is fixed for a build, so a mismatch is a mixed-library
     * job that the layout check above did not catch (§3.13's argument for
     * checksumming the handler table, applied here). */
    uint32_t want_count;
    const struct osoe_metric_descriptor *want = osoe_metrics_table(&want_count);
    if (h->dir.metrics_count != want_count ||
        h->dir.metrics_checksum != osoe_metric_checksum(want, want_count))
        osoe_fatal("control segment %s carries a different metric table (%u metrics, "
                   "checksum %#x); this library has %u and %#x", n->control_name,
                   h->dir.metrics_count, h->dir.metrics_checksum, want_count,
                   osoe_metric_checksum(want, want_count));
}

/*
 * The O(L) answer to "which local id is world rank r" (node.h). O(L log L)
 * once at init - an insertion sort would be L^2 = 65,536 steps at 256, which
 * is also fine, but qsort says what it does.
 */
static int compare_rank(const void *a, const void *b)
{
    uint32_t x = ((const uint32_t *)a)[0], y = ((const uint32_t *)b)[0];
    return x < y ? -1 : x > y;
}

static void build_local_index(struct osoe_node *n, const struct osoe_bootstrap *b)
{
    uint32_t count = b->local_count;
    n->local_count = count;
    n->first_rank = count ? b->gtids[0] : 0;
    n->local_block = true;
    for (uint32_t i = 0; i < count; i++)
        if (b->gtids[i] != n->first_rank + i)
            n->local_block = false;
    n->sorted_rank = NULL;
    n->sorted_local = NULL;
    if (n->local_block)
        return;
    uint32_t (*pairs)[2] = malloc(sizeof *pairs * count);
    n->sorted_rank = malloc(sizeof *n->sorted_rank * count);
    n->sorted_local = malloc(sizeof *n->sorted_local * count);
    if (!pairs || !n->sorted_rank || !n->sorted_local)
        osoe_fatal("out of memory");
    for (uint32_t i = 0; i < count; i++) {
        pairs[i][0] = b->gtids[i];
        pairs[i][1] = i;
    }
    qsort(pairs, count, sizeof *pairs, compare_rank);
    for (uint32_t i = 0; i < count; i++) {
        n->sorted_rank[i] = pairs[i][0];
        n->sorted_local[i] = pairs[i][1];
    }
    free(pairs);
}

int32_t osoe_node_local_search(const struct osoe_node *n, uint32_t rank)
{
    uint32_t lo = 0, hi = n->local_count;
    while (lo < hi) {
        uint32_t mid = lo + (hi - lo) / 2;
        if (n->sorted_rank[mid] < rank)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < n->local_count && n->sorted_rank[lo] == rank ? (int32_t)n->sorted_local[lo]
                                                             : -1;
}

void osoe_node_join(struct osoe_node *n, const struct osoe_bootstrap *b, uint32_t session)
{
    bool lead = b->local_id == 0;
    n->control_fd = -1;
    n->keep_name = b->keep_control_segment;

    snprintf(n->control_name, sizeof n->control_name, "/osoe.%llu.%u.%u.%u",
             (unsigned long long)b->job, b->step, b->node, session);
    n->heap_size = b->symmetric_size;
    n->heap_bytes = page_round(b->symmetric_size ? b->symmetric_size : 1);
    n->heap_of = calloc(b->local_count, sizeof *n->heap_of);
    if (!n->heap_of)
        osoe_fatal("out of memory");
    build_local_index(n, b);

    if (lead)
        create_control(n, b, session);
    else
        open_control(n, b, session);

    struct osoe_pe_slot *me = &n->slots[b->local_id];
    if (atomic_load_explicit(&me->phase, memory_order_acquire) != OSOE_PHASE_EMPTY) {
        char msg[128];
        snprintf(msg, sizeof msg, "another process already joined with this %sLOCALID",
                 b->launcher ? b->launcher : "OSOE_");
        fail_rendezvous(n, msg);
    }
    int len = snprintf(me->heap_name, sizeof me->heap_name, "%s.heap.%u", n->control_name,
                       b->local_id);
    if (len < 0 || (size_t)len >= sizeof me->heap_name)
        fail_rendezvous(n, "heap object name too long");

    uint64_t base = create_own_heap(n, b, me, lead ? 0 : n->hdr->heap_base);
    if (!base) {
        char msg[256];
        if (lead)
            snprintf(msg, sizeof msg, "no candidate heap base is free for a %llu-byte heap; "
                     "set OSOE_HEAP_BASE to a free address", (unsigned long long)n->heap_bytes);
        else
            snprintf(msg, sizeof msg, "cannot map this PE's heap at the node's base %#llx "
                     "plus its shift %#llx (OSOE_HEAP_SHIFT), which the local lead chose; set "
                     "OSOE_HEAP_BASE to an address free in every PE",
                     (unsigned long long)n->hdr->heap_base,
                     (unsigned long long)(b->heap_shift * b->rank));
        shm_unlink(me->heap_name);
        fail_rendezvous(n, msg);
    }
    n->heap_of[b->local_id] = (void *)(uintptr_t)(base + b->heap_shift * b->rank);

    if (lead) {
        n->hdr->heap_base = base;
        /* release: a futex word that is its own predicate (§3.16). */
        atomic_store_explicit(&n->hdr->ready, 1, memory_order_release);
        osoe_futex_wake(&n->hdr->ready, INT_MAX, OSOE_FUTEX_SHARED);
    }

    /* Publish this PE's slot, then count it. */
    me->rank = b->rank;
    me->pid = (int32_t)getpid();
    me->tid = (int32_t)gettid();
    me->symmetric_size = b->symmetric_size;
    me->heap_bytes = n->heap_bytes;
    atomic_store_explicit(&me->phase, OSOE_PHASE_JOINED, memory_order_release);
    atomic_fetch_add_explicit(&n->hdr->joined, 1, memory_order_seq_cst);
    osoe_futex_wake(&n->hdr->joined, INT_MAX, OSOE_FUTEX_SHARED);

    if (wait_count(n, &n->hdr->joined, b->local_count, b->no_progress_ns) != 0) {
        char missing[256], msg[400];
        describe_missing(n, b->local_count, OSOE_PHASE_JOINED, missing, sizeof missing);
        snprintf(msg, sizeof msg, "no local PE has joined for OSOE_NO_PROGRESS_TIMEOUT "
                 "seconds; missing local ids:%s", missing);
        fail_rendezvous(n, msg);
    }
    check_agreement(n, b);
    map_siblings(n, b);
    osoe_metrics_init(n->blocks, n->hdr->dir.blocks_per_pe, b->local_id);

    atomic_store_explicit(&me->phase, OSOE_PHASE_MAPPED, memory_order_release);
    atomic_fetch_add_explicit(&n->hdr->mapped, 1, memory_order_seq_cst);
    osoe_futex_wake(&n->hdr->mapped, INT_MAX, OSOE_FUTEX_SHARED);

    /* The lead removes heap names once every PE holds a mapping, so a later
     * crash leaves no heap behind (§6.2) - and the control segment's name with
     * them, since every local PE opened it before it could map a sibling's
     * heap. From here nothing this node created in /dev/shm has a name: no
     * death of any process can strand it, and osoe-stat finds the segment
     * through a live PE's descriptor (§3.14). OSOE_KEEP_CONTROL_SEGMENT keeps
     * the control segment's name until finalize instead.
     *
     * Only the lead waits for `mapped`, yet no local PE returns from init with
     * these names still there: the lead publishes lead_agent_running after
     * removing them, and every other PE waits for that word before it returns
     * (init.c, §6.2). So the removal belongs here, before that store; moved
     * after it, a PE could leave init while the names remain. */
    if (lead) {
        if (wait_count(n, &n->hdr->mapped, b->local_count, b->no_progress_ns) != 0) {
            char missing[256], msg[400];
            describe_missing(n, b->local_count, OSOE_PHASE_MAPPED, missing, sizeof missing);
            snprintf(msg, sizeof msg, "no local PE has finished mapping its siblings' "
                     "heaps for OSOE_NO_PROGRESS_TIMEOUT seconds; missing local ids:%s",
                     missing);
            fail_rendezvous(n, msg);
        }
        for (uint32_t i = 0; i < b->local_count; i++)
            if (shm_unlink(n->slots[i].heap_name) != 0)
                osoe_fatal("cannot remove heap name %s: %s", n->slots[i].heap_name,
                           strerror(errno));
        if (!n->keep_name && shm_unlink(n->control_name) != 0)
            osoe_fatal("cannot remove the control segment's name %s: %s", n->control_name,
                       strerror(errno));
    }
}

/*
 * First writer wins: one CAS from zero installs the disposition, the status and
 * the rank together (node.h), so a global exit's status is never overwritten
 * by a fatal that follows it, and a reader's one load has all three. An
 * earlier draft claimed the word, wrote the rank and status beside it and then
 * published; a declarer killed between the claim and the publish would have
 * left every reader waiting for a publish that could not come.
 */
bool osoe_node_declare_abort(struct osoe_node *n, uint32_t pe, int status,
                             enum osoe_abort_disposition how)
{
    if (!n->hdr)
        return false;
    uint64_t none = 0;
    uint64_t word = (uint64_t)how << OSOE_ABORT_DISPOSITION_SHIFT |
                    (uint64_t)(uint8_t)status << OSOE_ABORT_STATUS_SHIFT | pe;
    if (!atomic_compare_exchange_strong_explicit(&n->hdr->aborting, &none, word,
                                                 memory_order_seq_cst, memory_order_acquire))
        return false;
    osoe_node_wake_agents(n);
    return true;
}

/*
 * The stop protocol's order (agent.c): the word is stored, then each agent's
 * wake word bumped, then woken - so an agent that loaded its wake word before
 * the bump finds the futex compare failing, and one that loaded it after sees
 * the word. In local-id order, so the lead's agent first: it is the one that
 * tells the other nodes, and it wakes them all again when it does (agent.c),
 * which covers an agent a declarer did not reach - its own agent can see the
 * word and end the process before the declarer is done. O(L).
 */
void osoe_node_wake_agents(struct osoe_node *n)
{
    for (uint32_t i = 0; n->slots && i < n->local_count; i++) {
        atomic_fetch_add_explicit(&n->slots[i].agent_wake, 1, memory_order_seq_cst);
        osoe_futex_wake(&n->slots[i].agent_wake, INT_MAX, OSOE_FUTEX_SHARED);
    }
}

enum osoe_abort_disposition osoe_node_abort_read(const struct osoe_node *n, uint32_t *pe,
                                                 int *status)
{
    if (!n->hdr)
        return OSOE_ABORT_NONE;
    uint64_t word = atomic_load_explicit(&n->hdr->aborting, memory_order_acquire);
    if (word != 0) {
        *pe = (uint32_t)word;
        *status = (uint8_t)(word >> OSOE_ABORT_STATUS_SHIFT);
    }
    return (enum osoe_abort_disposition)(uint8_t)(word >> OSOE_ABORT_DISPOSITION_SHIFT);
}

void osoe_node_leave(struct osoe_node *n, const struct osoe_bootstrap *b)
{
    struct osoe_pe_slot *me = &n->slots[b->local_id];
    osoe_metrics_fini();
    for (uint32_t i = 0; i < b->local_count; i++)
        if (n->heap_of[i])
            munmap(n->heap_of[i], n->slots[i].heap_bytes);
    free(n->heap_of);
    n->heap_of = NULL;
    free(n->sorted_rank);
    free(n->sorted_local);
    n->sorted_rank = n->sorted_local = NULL;
    n->local_block = false;
    n->local_count = 0;

    atomic_store_explicit(&me->phase, OSOE_PHASE_FINALIZED, memory_order_release);
    uint32_t done = atomic_fetch_add_explicit(&n->hdr->finalized, 1, memory_order_seq_cst) + 1;
    /* With OSOE_KEEP_CONTROL_SEGMENT the name lasted until now: exactly one
     * PE's increment reaches L, and that PE removes it. Others may still hold
     * mappings, which stay valid. The session number in the name keeps this
     * from removing a segment a PE has already re-created (§6.4). Otherwise the
     * name went at init, and there is nothing to remove. ENOENT is not an error
     * either way: someone - osoe-stat --gc, an operator - may have beaten us
     * to it, and the object is what matters, not its name. */
    if (n->keep_name && done == b->local_count && shm_unlink(n->control_name) != 0 &&
        errno != ENOENT)
        osoe_fatal("cannot remove control segment %s: %s", n->control_name, strerror(errno));

    munmap(n->hdr, n->control_bytes);
    if (n->control_fd >= 0)
        close(n->control_fd);
    n->control_fd = -1;
    n->hdr = NULL;
    n->slots = NULL;
    n->blocks = NULL;
    n->landing = NULL;
    n->control_name[0] = '\0';
}
