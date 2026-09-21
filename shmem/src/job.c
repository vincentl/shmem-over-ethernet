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
#include "job.h"

#define OSOE_JOB_MAGIC UINT64_C(0x00626f6a656f736f)   /* "osoejob" */
#define OSOE_JOB_LAYOUT 1

/* Not yet assigned to a node. The map starts full of these so that a rank no
 * lead claimed is a detectable gap rather than node 0 by accident. */
#define OSOE_NODE_UNSET UINT32_MAX

struct osoe_job_header {
    uint64_t magic;
    uint32_t layout;
    uint32_t world_size;
    uint32_t node_count;
    uint32_t reserved;

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
     * PEs that have finished with the segment. The name is removed by whichever
     * leave reaches world_size, exactly as §6.2 removes the control segment's
     * name at the last finalize and for the same reason: node 0's lead
     * finishing first would otherwise unlink a name another PE has not opened
     * yet, and that PE would wait for an object that can never appear.
     */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t left;
    /* world_size entries follow, cache-line aligned. */
};

static uint64_t map_offset(void)
{
    return (sizeof(struct osoe_job_header) + OSOE_CACHELINE - 1) / OSOE_CACHELINE *
           OSOE_CACHELINE;
}

static void fail_job(struct osoe_job_header *h)
{
    atomic_store_explicit(&h->failed, 1, memory_order_seq_cst);
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
    j->bytes = map_offset() + (uint64_t)b->world_size * sizeof(uint32_t);
    /* §6.2: the step's rendezvous server is the lead of node 0. */
    j->creator = b->node == 0 && b->local_id == 0;

    int fd;
    if (j->creator) {
        fd = shm_open(j->name, O_CREAT | O_EXCL | O_RDWR, 0600);
        if (fd < 0)
            osoe_fatal("cannot create the job segment /dev/shm%s: %s%s", j->name,
                       strerror(errno),
                       errno == EEXIST ? "; probably left by a crashed job with the "
                                         "same job and step ids" : "");
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
        struct osoe_stall_watch watch;
        osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
        struct timespec back = { 0, 200000 };
        for (;;) {
            fd = shm_open(j->name, O_RDWR, 0);
            if (fd >= 0) {
                struct stat st;
                if (fstat(fd, &st) == 0 && (uint64_t)st.st_size >= j->bytes)
                    break;
                close(fd);
            } else if (errno != ENOENT) {
                osoe_fatal("cannot open the job segment %s: %s", j->name,
                           strerror(errno));
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
    close(fd);
    if (p == MAP_FAILED) {
        if (j->creator)
            shm_unlink(j->name);
        osoe_fatal("cannot map the job segment %s: %s", j->name, strerror(errno));
    }
    j->map = p;

    struct osoe_job_header *h = p;
    uint32_t *node_of = (uint32_t *)((char *)p + map_offset());

    if (j->creator) {
        h->magic = OSOE_JOB_MAGIC;
        h->layout = OSOE_JOB_LAYOUT;
        h->world_size = b->world_size;
        h->node_count = b->node_count;
        h->symmetric_size = b->symmetric_size;
        h->handler_checksum = osoe_am_table_checksum();
        for (uint32_t i = 0; i < b->world_size; i++)
            node_of[i] = OSOE_NODE_UNSET;
        atomic_store_explicit(&h->ready, 1, memory_order_seq_cst);
        osoe_futex_wake(&h->ready, INT_MAX, OSOE_FUTEX_SHARED);
    } else {
        struct osoe_stall_watch watch;
        osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
        while (atomic_load_explicit(&h->ready, memory_order_acquire) == 0) {
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

    /*
     * Only leads publish (§6.2). Each writes its own node's rank list - which
     * is exactly what OSOE_GTIDS gave it - and checks the two values every PE
     * of a job must agree on. Disagreement is a mismatched launch or a
     * mismatched binary, and it is caught here with a message rather than as an
     * undecodable message arriving at PE 47 an hour in (§3.13).
     */
    if (b->local_id == 0) {
        if (h->world_size != b->world_size || h->node_count != b->node_count) {
            fail_job(h);
            osoe_fatal("node %u was launched for a job of %u tasks on %u nodes; node 0 "
                       "says %u on %u", b->node, b->world_size, b->node_count,
                       h->world_size, h->node_count);
        }
        if (h->symmetric_size != b->symmetric_size) {
            fail_job(h);
            osoe_fatal("node %u has SHMEM_SYMMETRIC_SIZE %llu; node 0 has %llu: every "
                       "PE of a job must agree", b->node,
                       (unsigned long long)b->symmetric_size,
                       (unsigned long long)h->symmetric_size);
        }
        if (h->handler_checksum != osoe_am_table_checksum()) {
            fail_job(h);
            osoe_fatal("node %u has AM handler table checksum %#x; node 0 has %#x: the "
                       "nodes are running different binaries (§3.13)", b->node,
                       osoe_am_table_checksum(), h->handler_checksum);
        }
        for (uint32_t i = 0; i < b->local_count; i++) {
            uint32_t rank = b->gtids[i];
            if (node_of[rank] != OSOE_NODE_UNSET && node_of[rank] != b->node) {
                fail_job(h);
                osoe_fatal("rank %u is claimed by node %u and by node %u", rank,
                           node_of[rank], b->node);
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
            fail_job(h);
            osoe_fatal("rank %u was not claimed by any node after every lead arrived; "
                       "the launcher's rank lists do not cover the job", i);
        }

    j->node_of = node_of;
}

void osoe_job_leave(struct osoe_job *j)
{
    if (!j->map)
        return;
    struct osoe_job_header *h = j->map;
    uint32_t left = atomic_fetch_add_explicit(&h->left, 1, memory_order_seq_cst) + 1;
    bool last = left == j->world_size;
    munmap(j->map, j->bytes);
    if (last)
        shm_unlink(j->name);
    *j = (struct osoe_job){ 0 };
}
