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
#include "fatal.h"
#include "metrics.h"
#include "node.h"

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

/* Publish failure so waiting siblings stop at their next sweep, then die. */
_Noreturn static void fail_rendezvous(struct osoe_node *n, const char *what)
{
    if (n->hdr) {
        atomic_store_explicit(&n->hdr->failed, 1, memory_order_release);
        osoe_futex_wake(&n->hdr->ready, INT_MAX, OSOE_FUTEX_SHARED);
        osoe_futex_wake(&n->hdr->joined, INT_MAX, OSOE_FUTEX_SHARED);
        osoe_futex_wake(&n->hdr->mapped, INT_MAX, OSOE_FUTEX_SHARED);
    }
    osoe_fatal("%s", what);
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
static int wait_count(struct osoe_node *n, _Atomic uint32_t *word, uint32_t target,
                      const struct osoe_deadline *overall)
{
    for (;;) {
        uint32_t v = atomic_load_explicit(word, memory_order_acquire);
        check_failed(n);
        if (v == target)
            return 0;
        if (osoe_deadline_expired(overall))
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
    int fd = shm_open(slot->heap_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        char msg[256];
        snprintf(msg, sizeof msg, "cannot create heap %s: %s", slot->heap_name,
                 strerror(errno));
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

    uint64_t mapped_at = 0;
    for (size_t i = 0; i < ncandidates && !mapped_at; i++) {
        void *p = mmap((void *)(uintptr_t)candidates[i], n->heap_bytes,
                       PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        if (p == (void *)(uintptr_t)candidates[i])
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
        int fd = shm_open(s->heap_name, O_RDWR, 0);
        if (fd < 0) {
            char msg[256];
            snprintf(msg, sizeof msg, "cannot open local PE %u's heap %s: %s", i,
                     s->heap_name, strerror(errno));
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
            snprintf(msg, sizeof msg, "local PE %u joined as rank %u, but OSOE_GTIDS "
                     "names rank %u", i, n->slots[i].rank, b->gtids[i]);
            fail_rendezvous(n, msg);
        }
    }
}

/* The lead creates and initializes the control segment. */
static void create_control(struct osoe_node *n, const struct osoe_bootstrap *b, uint32_t session)
{
    int fd = shm_open(n->control_name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
        if (errno == EEXIST)
            osoe_fatal("control segment /dev/shm%s already exists, probably left by a "
                       "crashed job with the same job and step ids; remove it and retry",
                       n->control_name);
        osoe_fatal("cannot create control segment %s: %s", n->control_name,
                   strerror(errno));
    }
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
    n->control_bytes = page_round(blocks_offset + blocks_bytes);
    if (ftruncate(fd, (off_t)n->control_bytes) != 0) {
        close(fd);
        shm_unlink(n->control_name);
        osoe_fatal("cannot size control segment: %s", strerror(errno));
    }
    void *p = mmap(NULL, n->control_bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        shm_unlink(n->control_name);
        osoe_fatal("cannot map control segment: %s", strerror(errno));
    }

    /* ftruncate zeroed it: every atomic word starts at 0. */
    n->hdr = p;
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
    h->dir.block_bytes = (uint32_t)sizeof(struct osoe_counter_block);
    memcpy((char *)p + metrics_offset, metrics, metrics_bytes);
    n->blocks = (struct osoe_counter_block *)((char *)p + blocks_offset);
    osoe_epoch_init(&h->world_arrived);
    osoe_epoch_init(&h->world_released);
    osoe_epoch_init(&h->world_copied);
    osoe_epoch_init(&h->shared_arrived);
    osoe_epoch_init(&h->shared_released);
    osoe_epoch_init(&h->shared_copied);
}

/* A non-lead opens the control segment by name, waiting for the lead to
 * create and publish it. */
static void open_control(struct osoe_node *n, const struct osoe_bootstrap *b, uint32_t session,
                         const struct osoe_deadline *overall)
{
    struct timespec backoff = { 0, 1000 * 1000 };   /* 1 ms, doubling to 64 ms */
    for (;;) {
        int fd = shm_open(n->control_name, O_RDWR, 0);
        if (fd >= 0) {
            struct stat st;
            if (fstat(fd, &st) == 0 && (uint64_t)st.st_size >= sizeof(struct osoe_node_header)) {
                void *p = mmap(NULL, (size_t)st.st_size, PROT_READ | PROT_WRITE, MAP_SHARED,
                               fd, 0);
                close(fd);
                if (p == MAP_FAILED)
                    osoe_fatal("cannot map control segment %s: %s", n->control_name,
                               strerror(errno));
                n->hdr = p;
                n->control_bytes = (uint64_t)st.st_size;
                break;
            }
            close(fd);          /* created but not yet sized */
        } else if (errno != ENOENT) {
            osoe_fatal("cannot open control segment %s: %s", n->control_name,
                       strerror(errno));
        }
        if (osoe_deadline_expired(overall))
            osoe_fatal("timed out waiting for the local lead to create %s "
                       "(OSOE_BOOTSTRAP_TIMEOUT)", n->control_name);
        nanosleep(&backoff, NULL);
        if (backoff.tv_nsec < 64 * 1000 * 1000)
            backoff.tv_nsec *= 2;
    }

    /* Sized is not ready: the lead publishes `ready` after the header. */
    for (;;) {
        if (atomic_load_explicit(&n->hdr->ready, memory_order_acquire))
            break;
        check_failed(n);
        if (osoe_deadline_expired(overall))
            osoe_fatal("timed out waiting for the local lead to initialize %s",
                       n->control_name);
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
        h->dir.metrics_offset + h->dir.metrics_bytes > n->control_bytes)
        osoe_fatal("control segment %s is too small for its own directory",
                   n->control_name);
    n->slots = (struct osoe_pe_slot *)((char *)n->hdr + h->dir.slots_offset);
    n->blocks = (struct osoe_counter_block *)((char *)n->hdr + h->dir.blocks_offset);

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

void osoe_node_join(struct osoe_node *n, const struct osoe_bootstrap *b, uint32_t session)
{
    struct osoe_deadline overall = osoe_deadline_in(b->timeout_ns);
    bool lead = b->local_id == 0;

    snprintf(n->control_name, sizeof n->control_name, "/osoe.%llu.%u.%u.%u",
             (unsigned long long)b->job, b->step, b->node, session);
    n->heap_size = b->symmetric_size;
    n->heap_bytes = page_round(b->symmetric_size ? b->symmetric_size : 1);
    n->heap_of = calloc(b->local_count, sizeof *n->heap_of);
    n->local_of_rank = malloc(sizeof *n->local_of_rank * b->world_size);
    if (!n->heap_of || !n->local_of_rank)
        osoe_fatal("out of memory");
    for (uint32_t r = 0; r < b->world_size; r++)
        n->local_of_rank[r] = -1;
    for (uint32_t i = 0; i < b->local_count; i++)
        n->local_of_rank[b->gtids[i]] = (int32_t)i;

    if (lead)
        create_control(n, b, session);
    else
        open_control(n, b, session, &overall);

    struct osoe_pe_slot *me = &n->slots[b->local_id];
    if (atomic_load_explicit(&me->phase, memory_order_acquire) != OSOE_PHASE_EMPTY)
        fail_rendezvous(n, "another process already joined with this OSOE_LOCALID");
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
            snprintf(msg, sizeof msg, "cannot map this PE's heap at the node's base %#llx, "
                     "which the local lead chose; set OSOE_HEAP_BASE to an address free in "
                     "every PE", (unsigned long long)n->hdr->heap_base);
        shm_unlink(me->heap_name);
        fail_rendezvous(n, msg);
    }
    n->heap_of[b->local_id] = (void *)(uintptr_t)base;

    if (lead) {
        n->hdr->heap_base = base;
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

    if (wait_count(n, &n->hdr->joined, b->local_count, &overall) != 0) {
        char missing[256], msg[400];
        describe_missing(n, b->local_count, OSOE_PHASE_JOINED, missing, sizeof missing);
        snprintf(msg, sizeof msg, "timed out waiting for local PEs to join "
                 "(OSOE_BOOTSTRAP_TIMEOUT); missing local ids:%s", missing);
        fail_rendezvous(n, msg);
    }
    check_agreement(n, b);
    map_siblings(n, b);
    osoe_metrics_init(n->blocks, n->hdr->dir.blocks_per_pe, b->local_id);

    atomic_store_explicit(&me->phase, OSOE_PHASE_MAPPED, memory_order_release);
    atomic_fetch_add_explicit(&n->hdr->mapped, 1, memory_order_seq_cst);
    osoe_futex_wake(&n->hdr->mapped, INT_MAX, OSOE_FUTEX_SHARED);

    /* The lead removes heap names once every PE holds a mapping, so a later
     * crash leaves no heap behind (§6.2). */
    if (lead) {
        if (wait_count(n, &n->hdr->mapped, b->local_count, &overall) != 0) {
            char missing[256], msg[400];
            describe_missing(n, b->local_count, OSOE_PHASE_MAPPED, missing, sizeof missing);
            snprintf(msg, sizeof msg, "timed out waiting for local PEs to map their "
                     "siblings' heaps; missing local ids:%s", missing);
            fail_rendezvous(n, msg);
        }
        for (uint32_t i = 0; i < b->local_count; i++)
            if (shm_unlink(n->slots[i].heap_name) != 0)
                osoe_fatal("cannot remove heap name %s: %s", n->slots[i].heap_name,
                           strerror(errno));
    }
}

/*
 * Two-step publication: the rank and status first, then the flag that makes
 * them readable. A reader loads the flag with acquire before it reads either,
 * so it never prints a rank from a half-written abort. Single-writer
 * publication (§3.16) - there is no protocol here, only a word a poller reads,
 * so no wake can be lost: a waiter that misses the store sees it one sweep
 * later, which is the whole cost.
 */
void osoe_node_declare_abort(struct osoe_node *n, uint32_t pe, int status)
{
    if (!n->hdr)
        return;
    atomic_store_explicit(&n->hdr->abort_pe, pe, memory_order_release);
    atomic_store_explicit(&n->hdr->abort_status, status, memory_order_release);
    atomic_store_explicit(&n->hdr->aborting, 1, memory_order_release);
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
    free(n->local_of_rank);
    n->local_of_rank = NULL;

    atomic_store_explicit(&me->phase, OSOE_PHASE_FINALIZED, memory_order_release);
    uint32_t done = atomic_fetch_add_explicit(&n->hdr->finalized, 1, memory_order_seq_cst) + 1;
    /* Exactly one PE's increment reaches L; that PE removes the name. Others
     * may still hold mappings, which stay valid. The session number in the name
     * keeps this from removing a segment a PE has already re-created (§6.4). */
    if (done == b->local_count && shm_unlink(n->control_name) != 0)
        osoe_fatal("cannot remove control segment %s: %s", n->control_name, strerror(errno));

    munmap(n->hdr, n->control_bytes);
    n->hdr = NULL;
    n->slots = NULL;
    n->blocks = NULL;
    n->control_name[0] = '\0';
}
