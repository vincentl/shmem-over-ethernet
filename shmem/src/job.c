#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
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
#include "base/sweep.h"
#include "fatal.h"
#include "job.h"
#include "listener.h"
#include "rendezvous.h"
#include "shm-object.h"
#include "transport.h"

#define OSOE_JOB_MAGIC UINT64_C(0x00626f6a656f736f)   /* "osoejob" */
#define OSOE_JOB_LAYOUT 7     /* 2: the per-rank claim array; 3: `opened`;
                               * 4: the per-node backend blob;
                               * 5: collective_fanout;
                               * 6: the per-rank local index;
                               * 7 (phase 1 step 3): node-local, written by the
                               * lead from the socket rendezvous: the map as
                               * runs, each node's address, each node's blob */

/*
 * The node-local object a lead writes what the rendezvous taught it into, for
 * its node's PEs (job.h). Everything after the header is sized from it.
 */
struct osoe_job_header {
    uint64_t magic;
    uint32_t layout;
    uint32_t world_size;
    uint32_t node_count;
    uint32_t run_count;
    int32_t creator_pid;            /* for reclaiming a leftover (shm-object.h) */
    /* Everything above is written before `ready`, a futex word that is its own
     * predicate: release (§3.16). */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t ready;
    /* How many of this node's PEs have it open: the last removes the name. */
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t opened;
    /* Then, each cache-line aligned: run_count runs, node_count addresses,
     * node_count blobs. */
};

static uint64_t align_line(uint64_t n)
{
    return (n + OSOE_CACHELINE - 1) / OSOE_CACHELINE * OSOE_CACHELINE;
}

static uint64_t runs_offset(void)
{
    return align_line(sizeof(struct osoe_job_header));
}

static uint64_t addresses_offset(uint32_t run_count)
{
    return align_line(runs_offset() + (uint64_t)run_count * sizeof(struct osoe_rank_run));
}

static uint64_t blobs_offset(uint32_t run_count, uint32_t node_count)
{
    return align_line(addresses_offset(run_count) + (uint64_t)node_count * sizeof(uint32_t));
}

static uint64_t object_bytes(uint32_t run_count, uint32_t node_count)
{
    return blobs_offset(run_count, node_count) +
           (uint64_t)node_count * OSOE_JOB_BACKEND_BLOB_BYTES;
}

/* Point the job at the tables in the mapping. */
static void bind_tables(struct osoe_job *j, const struct osoe_job_header *h)
{
    const unsigned char *p = (const unsigned char *)h;
    j->runs = (const struct osoe_rank_run *)(p + runs_offset());
    j->run_count = h->run_count;
    j->ipv4 = (const uint32_t *)(p + addresses_offset(h->run_count));
    j->backend_blob = p + blobs_offset(h->run_count, h->node_count);
}

/* The lead: meet the other leads, and write what it learned for its node. */
static void lead(struct osoe_job *j, const struct osoe_bootstrap *b, uint32_t session)
{
    struct sockaddr_in address;
    osoe_listener_node_address(b, b->node, &address);
    /* The backend readies what peers need to reach this node - on AM, its
     * node-pair listener - and describes it in the blob (transport.h). */
    unsigned char blob[OSOE_JOB_BACKEND_BLOB_BYTES];
    memset(blob, 0, sizeof blob);
    osoe_transport_advertise(blob);
    struct osoe_rendezvous_result r;
    osoe_rendezvous(b, session, &address, blob, &r);

    j->bytes = object_bytes(r.run_count, b->node_count);
    char err[256];
    int fd = osoe_shm_create(j->name, (long)offsetof(struct osoe_job_header, creator_pid),
                             err, sizeof err);
    if (fd < 0)
        osoe_fatal("this node's job tables: %s", err);
    if (ftruncate(fd, (off_t)j->bytes) != 0) {
        close(fd);
        shm_unlink(j->name);
        osoe_fatal("cannot size %s: %s", j->name, strerror(errno));
    }
    void *p = mmap(NULL, j->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (p == MAP_FAILED) {
        shm_unlink(j->name);
        osoe_fatal("cannot map %s: %s", j->name, strerror(errno));
    }
    j->map = p;
    struct osoe_job_header *h = p;
    h->magic = OSOE_JOB_MAGIC;
    h->layout = OSOE_JOB_LAYOUT;
    h->world_size = b->world_size;
    h->node_count = b->node_count;
    h->run_count = r.run_count;
    h->creator_pid = (int32_t)getpid();
    unsigned char *base = p;
    memcpy(base + runs_offset(), r.runs, sizeof *r.runs * r.run_count);
    memcpy(base + addresses_offset(r.run_count), r.ipv4, sizeof *r.ipv4 * b->node_count);
    memcpy(base + blobs_offset(r.run_count, b->node_count), r.blobs,
           (size_t)b->node_count * OSOE_JOB_BACKEND_BLOB_BYTES);
    free(r.runs);
    free(r.ipv4);
    free(r.blobs);
    atomic_store_explicit(&h->ready, 1, memory_order_release);
    osoe_futex_wake(&h->ready, INT_MAX, OSOE_FUTEX_SHARED);
}

/* Any other PE: wait for the lead's object, as §6.2's non-lead PEs wait for the
 * control segment - for the name, the length, then `ready`. */
static void follow(struct osoe_job *j, const struct osoe_bootstrap *b)
{
again:;
    /* Nothing moves until the lead has met every other lead, so the watch is
     * on a constant: by default this waits as long as that takes (§3.15). */
    struct osoe_stall_watch watch;
    osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
    struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
    struct timespec back = { 0, 200000 };        /* 0.2 ms, doubling to 10 ms */
    int fd;
    struct stat st;
    for (;;) {
        char err[256];
        fd = osoe_shm_open_ours(j->name, err, sizeof err);
        if (fd >= 0) {
            if (fstat(fd, &st) == 0 && (uint64_t)st.st_size >= sizeof(struct osoe_job_header))
                break;
            close(fd);
        } else if (errno != ENOENT) {
            osoe_fatal("this node's job tables: %s", err);
        }
        if (osoe_deadline_expired(&sweep)) {
            sweep = osoe_deadline_in(OSOE_SWEEP_NS);
            osoe_sweep_expired("shmem_init");       /* the lead's fatal ends this */
            if (osoe_stall_watch_stalled(&watch, 0))
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): this node's "
                           "lead has not finished the job rendezvous (%s)",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), j->name);
        }
        nanosleep(&back, NULL);
        if (back.tv_nsec < 10000000)
            back.tv_nsec *= 2;
    }
    /* ftruncate gave the object its whole length before anything was written. */
    j->bytes = (uint64_t)st.st_size;
    void *p = mmap(NULL, j->bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p == MAP_FAILED)
        osoe_fatal("cannot map %s: %s", j->name, strerror(errno));
    struct osoe_job_header *h = p;
    while (atomic_load_explicit(&h->ready, memory_order_acquire) == 0) {
        if (osoe_shm_orphaned(fd)) {            /* a leftover being reclaimed */
            munmap(p, j->bytes);
            close(fd);
            goto again;
        }
        struct osoe_deadline d = osoe_deadline_in(OSOE_SWEEP_NS);
        if (osoe_futex_wait(&h->ready, 0, &d, OSOE_FUTEX_SHARED) == ETIMEDOUT)
            osoe_sweep_expired("shmem_init");
    }
    close(fd);
    if (h->magic != OSOE_JOB_MAGIC || h->layout != OSOE_JOB_LAYOUT ||
        h->world_size != b->world_size || h->node_count != b->node_count ||
        object_bytes(h->run_count, h->node_count) > j->bytes)
        osoe_fatal("%s is not this job's tables in this version (magic %#llx layout %u)",
                   j->name, (unsigned long long)h->magic, h->layout);
    j->map = p;
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

    /* This node's ranks, for the question every put asks (job.h). */
    osoe_rank_set_build(&j->here, b->gtids, b->local_count);

    /*
     * One node: no rendezvous, no socket, no object. Its PEs are every rank, in
     * rank order, so the map is one run; and a job of one node must run on a
     * machine with no network at all.
     */
    if (b->node_count == 1) {
        static const unsigned char no_blob[OSOE_JOB_BACKEND_BLOB_BYTES];
        struct osoe_rank_progression *p = malloc(sizeof *p * (b->local_count + 1));
        if (!p)
            osoe_fatal("out of memory");
        struct osoe_rank_report only = { 0, p,
                                         osoe_rank_progressions(b->gtids, b->local_count, p) };
        char why[256];
        j->owned_runs = osoe_rank_runs_build(&only, 1, b->world_size, &j->run_count, why,
                                             sizeof why);
        free(p);
        if (!j->owned_runs)
            osoe_fatal("%s", why);
        j->runs = j->owned_runs;
        j->backend_blob = no_blob;
        return;
    }

    snprintf(j->name, sizeof j->name, "/osoe.%llu.%u.%u.job.%u",
             (unsigned long long)b->job, b->step, b->node, session);
    if (b->local_id == 0)
        lead(j, b, session);
    else
        follow(j, b);
    struct osoe_job_header *h = j->map;
    bind_tables(j, h);

    /* Every PE of this node opens it; the last to have done so removes the
     * name (§6.2), after which no death of any process can strand it. */
    if (atomic_fetch_add_explicit(&h->opened, 1, memory_order_seq_cst) + 1 == b->local_count &&
        shm_unlink(j->name) != 0)
        osoe_fatal("cannot remove %s: %s", j->name, strerror(errno));
}

void osoe_job_leave(struct osoe_job *j)
{
    if (j->map)
        munmap(j->map, j->bytes);       /* its name went when the last PE opened it */
    osoe_rank_set_free(&j->here);
    free(j->owned_runs);
    *j = (struct osoe_job){ 0 };
}
