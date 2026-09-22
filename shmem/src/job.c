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

#include "am.h"
#include "base/cacheline.h"
#include "base/deadline.h"
#include "base/environment.h"
#include "base/futex.h"
#include "base/stall-watch.h"
#include "fatal.h"
#include "shm-object.h"
#include "job.h"

#define OSOE_JOB_MAGIC UINT64_C(0x00626f6a656f736f)   /* "osoejob" */
#define OSOE_JOB_LAYOUT 3     /* 2: the per-rank claim array; 3: `opened` */

/* Not yet assigned to a node. The map starts full of these so that a rank no
 * lead claimed is a detectable gap rather than node 0 by accident. */
#define OSOE_NODE_UNSET UINT32_MAX

struct osoe_job_header {
    uint64_t magic;
    uint32_t layout;
    uint32_t world_size;
    uint32_t node_count;
    int32_t creator_pid;            /* for reclaiming a leftover (shm-object.h) */

    /*
     * What §6.2 has each lead send, and the server check: the symmetric size
     * and the handler-table checksum. Published by node 0's lead as the values
     * to agree with, then compared by every other lead - which is the same
     * check the server does, made symmetric because here there is no server.
     */
    uint64_t symmetric_size;
    uint32_t handler_checksum;
    uint32_t reserved2;

    _Alignas(OSOE_CACHELINE) _Atomic uint32_t ready;    /* the object is described */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t joined;   /* leads that have published */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t failed;
    /*
     * How many of the processes that must open this object have (§3.14's
     * unlink-early rule). Whichever increment reaches the count removes the
     * name: every process that will ever need it has it open, so from then on
     * no death of any process can strand it. The count is exact, so nothing
     * waits for it - the same shape as the control segment's `finalized` at the
     * other end of a session.
     */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t opened;
    /*
     * Two arrays of world_size entries follow, cache-line aligned: `node_of`,
     * the map every PE reads, and `claim`, one word per rank that a lead takes
     * by CAS before it writes that rank's entry of the map. The claim is what
     * makes the map single-writer: two leads naming the same rank used to both
     * test the map with a plain load and both store, a cross-process race that
     * could miss the duplicate entirely (second review, atomics F6). With the
     * claim, exactly one wins, the other is refused by name, and `node_of`
     * stays plain data - written once by the winner, published by the seq_cst
     * add to `joined`, and read by every put without an atomic.
     */
};

_Static_assert(sizeof(_Atomic uint32_t) == sizeof(uint32_t),
               "the claim array sits after node_of with the same stride");

static uint64_t map_offset(void)
{
    return (sizeof(struct osoe_job_header) + OSOE_CACHELINE - 1) / OSOE_CACHELINE *
           OSOE_CACHELINE;
}

/* osoe_fatal_then's last act: the message is out first (fatal.h). */
static void fail_job(void *context)
{
    struct osoe_job_header *h = context;
    atomic_store_explicit(&h->failed, 1, memory_order_release);
    osoe_futex_wake(&h->joined, INT_MAX, OSOE_FUTEX_SHARED);
    osoe_futex_wake(&h->ready, INT_MAX, OSOE_FUTEX_SHARED);
}

void osoe_job_join(struct osoe_job *j, const struct osoe_bootstrap *b, uint32_t session)
{
    *j = (struct osoe_job){ 0 };
    j->world_size = b->world_size;
    j->node_count = b->node_count;
    j->node = b->node;
    j->me = b->rank;

    uint32_t force = 0;
    if (osoe_environment_u32("OSOE_FORCE_REMOTE", &force) == OSOE_ENVIRONMENT_INVALID ||
        force > 1)
        osoe_fatal("OSOE_FORCE_REMOTE='%s' is not 0 or 1", getenv("OSOE_FORCE_REMOTE"));
    j->force_remote = force != 0;
    /*
     * §8.1's flag routes traffic that would be intranode over the network, so
     * it needs a network to route it to. On one logical node there is no peer
     * and no connection, and every operation would be addressed to a link that
     * does not exist. Refusing says so at init rather than at the first put.
     */
    if (j->force_remote && j->node_count < 2)
        osoe_fatal("OSOE_FORCE_REMOTE=1 needs a job of at least two logical nodes; "
                   "this one has %u, so there is nowhere to route to", j->node_count);

    snprintf(j->name, sizeof j->name, "/osoe.%llu.%u.job.%u",
             (unsigned long long)b->job, b->step, session);
    j->bytes = map_offset() + (uint64_t)b->world_size * 2 * sizeof(uint32_t);
    /* §6.2: the step's rendezvous server is the lead of node 0. */
    j->creator = b->node == 0 && b->local_id == 0;

    int fd;
    if (j->creator) {
        char err[256];
        fd = osoe_shm_create(j->name, (long)offsetof(struct osoe_job_header, creator_pid),
                             err, sizeof err);
        if (fd < 0)
            osoe_fatal("the job segment: %s", err);
        if (ftruncate(fd, (off_t)j->bytes) != 0) {
            close(fd);
            shm_unlink(j->name);
            osoe_fatal("cannot size the job segment: %s", strerror(errno));
        }
    } else {
        /* The name appears before the object is sized and before the header is
         * written, so wait for the length here and for `ready` below. */
        /* Nothing moves until the object appears, so the watch is on a constant:
         * with OSOE_NO_PROGRESS_TIMEOUT unset, this waits as long as it takes. */
again:;
        struct osoe_stall_watch watch;
        osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
        struct timespec back = { 0, 200000 };
        for (;;) {
            char err[256];
            fd = osoe_shm_open_ours(j->name, err, sizeof err);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0 && (uint64_t)st.st_size >= j->bytes)
                    break;
                close(fd);
            } else if (errno != ENOENT) {
                osoe_fatal("the job segment: %s", err);
            }
            if (osoe_stall_watch_stalled(&watch, 0))
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the lead of node 0 has not created %s, the step's "
                           "rendezvous point (§6.2)",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), j->name);
            nanosleep(&back, NULL);
            if (back.tv_nsec < 10000000)
                back.tv_nsec *= 2;
        }
    }

    void *p = mmap(NULL, j->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED) {
        close(fd);
        if (j->creator)
            shm_unlink(j->name);
        osoe_fatal("cannot map the job segment %s: %s", j->name, strerror(errno));
    }
    j->map = p;

    struct osoe_job_header *h = p;
    uint32_t *node_of = (uint32_t *)((char *)p + map_offset());
    _Atomic uint32_t *claim = (_Atomic uint32_t *)(node_of + b->world_size);

    if (j->creator) {
        h->magic = OSOE_JOB_MAGIC;
        h->layout = OSOE_JOB_LAYOUT;
        h->world_size = b->world_size;
        h->node_count = b->node_count;
        h->symmetric_size = b->symmetric_size;
        h->handler_checksum = osoe_am_table_checksum();
        h->creator_pid = (int32_t)getpid();
        for (uint32_t i = 0; i < b->world_size; i++) {
            node_of[i] = OSOE_NODE_UNSET;
            atomic_store_explicit(&claim[i], OSOE_NODE_UNSET, memory_order_release);
        }
        atomic_store_explicit(&h->ready, 1, memory_order_release);
        osoe_futex_wake(&h->ready, INT_MAX, OSOE_FUTEX_SHARED);
    } else {
        struct osoe_stall_watch watch;
        osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
        while (atomic_load_explicit(&h->ready, memory_order_acquire) == 0) {
            /* A leftover node 0's lead is about to reclaim will never be
             * ready: start over (shm-object.h). */
            if (osoe_shm_orphaned(fd)) {
                munmap(p, j->bytes);
                close(fd);
                j->map = NULL;
                goto again;
            }
            struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
            osoe_futex_wait(&h->ready, 0, &sweep, OSOE_FUTEX_SHARED);
            if (atomic_load_explicit(&h->failed, memory_order_acquire))
                osoe_fatal("the job rendezvous failed on another node");
            if (osoe_stall_watch_stalled(&watch, 0))
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the lead of node 0 has not finished describing %s",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), j->name);
        }
        if (h->magic != OSOE_JOB_MAGIC || h->layout != OSOE_JOB_LAYOUT)
            osoe_fatal("%s is not a job segment of this version (magic %#llx layout %u)",
                       j->name, (unsigned long long)h->magic, h->layout);
    }
    close(fd);

    /*
     * Only leads publish (§6.2). Each writes its own node's rank list - which
     * is exactly what OSOE_GTIDS gave it - and checks the two values every PE
     * of a job must agree on. Disagreement is a mismatched launch or a
     * mismatched binary, and it is caught here with a message rather than as an
     * undecodable message arriving at PE 47 an hour in (§3.13).
     */
    if (b->local_id == 0) {
        if (h->world_size != b->world_size || h->node_count != b->node_count) {
            osoe_fatal_then(fail_job, h, "node %u was launched for a job of %u tasks on %u nodes; node 0 "
                       "says %u on %u", b->node, b->world_size, b->node_count,
                       h->world_size, h->node_count);
        }
        if (h->symmetric_size != b->symmetric_size) {
            osoe_fatal_then(fail_job, h, "node %u has SHMEM_SYMMETRIC_SIZE %llu; node 0 has %llu: every "
                       "PE of a job must agree", b->node,
                       (unsigned long long)b->symmetric_size,
                       (unsigned long long)h->symmetric_size);
        }
        if (h->handler_checksum != osoe_am_table_checksum()) {
            osoe_fatal_then(fail_job, h, "node %u has AM handler table checksum %#x; node 0 has %#x: the "
                       "nodes are running different binaries (§3.13)", b->node,
                       osoe_am_table_checksum(), h->handler_checksum);
        }
        for (uint32_t i = 0; i < b->local_count; i++) {
            uint32_t rank = b->gtids[i];
            uint32_t holder = OSOE_NODE_UNSET;
            if (!atomic_compare_exchange_strong_explicit(&claim[rank], &holder, b->node,
                                                         memory_order_seq_cst,
                                                         memory_order_acquire) &&
                holder != b->node) {
                osoe_fatal_then(fail_job, h, "rank %u is claimed by node %u and by node %u", rank, holder,
                           b->node);
            }
            node_of[rank] = b->node;
        }
        atomic_fetch_add_explicit(&h->joined, 1, memory_order_seq_cst);
        osoe_futex_wake(&h->joined, INT_MAX, OSOE_FUTEX_SHARED);
    }

    /* Every PE waits for the map to be complete: a PE that started using it
     * early would resolve a peer's node before that peer's lead had said where
     * it was. */
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, atomic_load_explicit(&h->joined, memory_order_acquire),
                           b->no_progress_ns);
    for (;;) {
        uint32_t joined = atomic_load_explicit(&h->joined, memory_order_acquire);
        if (joined >= b->node_count)
            break;
        if (atomic_load_explicit(&h->failed, memory_order_acquire))
            osoe_fatal("the job rendezvous failed on another node");
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        osoe_futex_wait(&h->joined, joined, &sweep, OSOE_FUTEX_SHARED);
        if (osoe_stall_watch_stalled(&watch, joined))
            osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the job rendezvous has stayed at %u of %u node leads",
                       (unsigned long long)osoe_stall_watch_seconds(&watch), joined,
                       b->node_count);
    }

    for (uint32_t i = 0; i < b->world_size; i++)
        if (node_of[i] == OSOE_NODE_UNSET) {
            osoe_fatal_then(fail_job, h, "rank %u was not claimed by any node after every lead arrived; "
                       "the launcher's rank lists do not cover the job", i);
        }

    j->node_of = node_of;

    /* Every PE of the job opens this; the last to have done so removes the name. */
    if (atomic_fetch_add_explicit(&h->opened, 1, memory_order_seq_cst) + 1 == b->world_size &&
        shm_unlink(j->name) != 0)
        osoe_fatal("cannot remove the job segment's name %s: %s", j->name, strerror(errno));
}

void osoe_job_leave(struct osoe_job *j)
{
    if (!j->map)
        return;
    munmap(j->map, j->bytes);       /* its name went when the last PE opened it */
    *j = (struct osoe_job){ 0 };
}
