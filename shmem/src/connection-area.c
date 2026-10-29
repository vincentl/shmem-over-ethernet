/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
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
#include "base/print-line.h"
#include "base/stall-watch.h"
#include "base/sweep.h"
#include "connection-area.h"
#include "node.h"
#include "fatal.h"
#include "shm-object.h"

#define OSOE_CONNECTION_AREA_MAGIC UINT64_C(0x61657261656f736f)   /* "osoearea" */
#define OSOE_CONNECTION_AREA_LAYOUT 5   /* 2: `opened`; 3: `lead_sleeping` and the
                                         * dirty-connection bitmap; 4: each
                                         * connection's acknowledgement and
                                         * turnover wait words; 5: the flag and
                                         * the bitmap an owner (owner-map.h),
                                         * and a helper's wake word */

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
    int32_t creator_pid;        /* for reclaiming a leftover (shm-object.h), and
                                 * for osoe-stat --gc to find one */
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
    /*
     * Layout 5, after the words above, where layout 3 put its own, so that
     * `ready` and `opened` stay where a PE of another commit looks for them
     * whatever the line's size: the node's progress helpers (owner-map.h),
     * which shape what follows the header - helpers + 1 lines of struct
     * owner_words from owners_offset, then each owner's dirty bitmap on lines
     * of its own - and so must be every opener's too. A line of their own,
     * since every opener adds to `opened`.
     */
    _Alignas(OSOE_CACHELINE) uint32_t helpers;
    uint64_t owners_offset;
};

/*
 * An owner's flag and wake word (connection-area.h), on a line of their own:
 * every writer loads the flag after every operation, the owner stores it twice
 * a sleep, and the writer that claims a sleep's wake bumps the word beside it.
 * Each owner's dirty bitmap starts a line of its own too, since each is taken
 * by another thread; within one, the words share lines, as nothing yet
 * measures contention between them (step 2's rule: a line a word only if
 * measurement says so). 64 bytes hold 512 connections' bits: a line on
 * x86-64, half of one on aarch64 (base/cacheline.h).
 */
struct owner_words {
    _Alignas(OSOE_CACHELINE) _Atomic uint32_t sleeping;
    _Atomic uint32_t wake;
};

/*
 * osoe-stat --gc removes this object when a start leaves it behind. It reads the
 * creator's pid at the offset asserted here and knows the magic by value
 * (tools/stat/segment.c, which includes nothing from this directory): move the
 * field and the build says so, and the tool's number moves with it; change the
 * magic and the tool's copy does.
 */
_Static_assert(offsetof(struct osoe_connection_area_header, creator_pid) == 20,
               "osoe-stat --gc reads the creator's pid at byte 20 (tools/stat/segment.c)");

/* And a PE of another commit, whatever the line's size, finds `ready` and
 * `opened` where every layout since 3 has had them - the first two lines after
 * the 64 bytes above - so that it waits for the one and is refused by the
 * layout it then reads, rather than waiting on a field moved under it (the
 * adversarial read of v0.2 step 4's Part 1, N1). */
_Static_assert(offsetof(struct osoe_connection_area_header, ready) == OSOE_CACHELINE &&
               offsetof(struct osoe_connection_area_header, opened) == 2 * OSOE_CACHELINE,
               "a connection area's ready and opened words stay on its second and third "
               "lines, where every layout since 3 has them");

/* test-helper-wake.c reads each owner's flag where its own process maps the
 * area, with nothing from this directory: the helpers on the fourth line and
 * the owners' offset after them, then an owner a line, the flag first. */
_Static_assert(offsetof(struct osoe_connection_area_header, layout) == 8 &&
               offsetof(struct osoe_connection_area_header, helpers) == 3 * OSOE_CACHELINE &&
               offsetof(struct osoe_connection_area_header, owners_offset) ==
                   3 * OSOE_CACHELINE + 8 &&
               sizeof(struct owner_words) == OSOE_CACHELINE &&
               offsetof(struct owner_words, sleeping) == 0,
               "test-helper-wake.c reads the helpers, the owners' offset and each owner's flag "
               "at these places");

static uint64_t align_up(uint64_t n)
{
    return (n + OSOE_CACHELINE - 1) / OSOE_CACHELINE * OSOE_CACHELINE;
}

/* Peers are numbered compactly, skipping this node (osoe_peer_slot,
 * owner-map.h): leaving a hole for self would waste two buffers - 512 KiB at
 * the default - on every node for nothing. */

/* Owner `owner`'s dirty bitmap: a bit a wire it holds. */
static uint32_t dirty_words(uint32_t owner, uint32_t peers, uint32_t helpers)
{
    return (osoe_owner_wires(owner, peers, helpers) + 63) / 64;
}

/* Where the parts after the header start, for `nodes` nodes (two or more) and
 * `helpers` helpers. */
struct area_layout {
    uint64_t owners;    /* helpers + 1 struct owner_words */
    uint64_t dirty;     /* owner 0's bitmap, the others' after it, each on its own lines */
    uint64_t conns;     /* nodes - 1 struct osoe_connection_shared */
    uint64_t data;      /* nodes - 1 pairs of buffers */
};

static struct area_layout layout_of(uint32_t nodes, uint32_t helpers)
{
    uint32_t peers = nodes - 1;
    struct area_layout l;
    l.owners = align_up(sizeof(struct osoe_connection_area_header));
    l.dirty = l.owners + (uint64_t)(helpers + 1) * sizeof(struct owner_words);
    l.conns = l.dirty;
    for (uint32_t o = 0; o <= helpers; o++)
        l.conns += align_up((uint64_t)dirty_words(o, peers, helpers) * sizeof(uint64_t));
    l.data = l.conns + align_up((uint64_t)peers * sizeof(struct osoe_connection_shared));
    return l;
}

uint64_t osoe_am_area_bytes(uint32_t nodes, uint32_t helpers, uint64_t buffer)
{
    if (nodes < 2)
        return 0;
    return layout_of(nodes, helpers).data + (uint64_t)(nodes - 1) * 2 * buffer;
}

uint64_t osoe_am_area_written_bytes(uint32_t nodes, uint32_t helpers, uint64_t buffer,
                                    uint32_t wires)
{
    if (nodes < 2)
        return 0;
    return layout_of(nodes, helpers).data + (uint64_t)wires * 2 * buffer;
}

uint32_t osoe_am_data_wires_most(uint64_t chunk, uint64_t requested, uint64_t budget,
                                 uint32_t nodes, bool on_demand)
{
    if (nodes < 2)
        return 0;
    uint64_t buffer;
    if (osoe_am_choose_buffer(chunk, requested, budget, nodes, &buffer) != OSOE_AM_SIZE_NO_ROOM ||
        !on_demand)
        return nodes - 1;
    uint64_t most = budget / (2 * buffer);      /* the floor, where the choice landed */
    return most < nodes - 1 ? (uint32_t)most : nodes - 1;
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


/*
 * Whether this node's lead of this start made the object open at `fd`: its
 * recorded creator is `lead_pid`. A size that differs from this PE's is the
 * lead's settings only then - an area a failed start by the same ids left can
 * be of another size, and its lead replaces it (the review fixes' adversarial
 * read) - and an object whose creator has not yet recorded itself is waited
 * for, as one not yet sized is. With OSOE_SHM_ANY_CREATOR every object is
 * taken for the lead's.
 */
static bool made_by(int fd, int32_t lead_pid)
{
    if (lead_pid == OSOE_SHM_ANY_CREATOR)
        return true;
    int32_t pid = 0;
    if (pread(fd, &pid, sizeof pid,
              (off_t)offsetof(struct osoe_connection_area_header, creator_pid)) != (ssize_t)sizeof pid)
        return false;
    return pid == lead_pid;
}

void osoe_connection_area_open(struct osoe_connection_area *a, const struct osoe_bootstrap *b,
                               uint32_t nodes, uint32_t helpers, uint32_t session,
                               int32_t lead_pid)
{
    *a = (struct osoe_connection_area){ 0 };
    a->node = b->node;
    a->nodes = nodes;
    a->helpers = helpers;
    if (nodes < 2)
        return;                          /* one node: no peers, nothing to map */

    struct osoe_am_configuration config;
    osoe_am_configuration_read(&config, b->local_count);
    a->credits = config.credits;

    uint64_t buffer;
    enum osoe_am_size_status size =
        osoe_am_choose_buffer(config.chunk, config.buffer, config.budget, nodes, &buffer);
    a->budget = config.budget;
    a->data_wires_most = osoe_am_data_wires_most(config.chunk, config.buffer, config.budget, nodes,
                                                 b->data_wires_on_demand);
    if (size == OSOE_AM_SIZE_NO_ROOM && a->data_wires_most > 0 && a->data_wires_most < nodes - 1) {
        /*
         * On demand past the budget's reach (H23; the author, 2026-10-27): the
         * floor, and a count of the data wires a node may make, since it pages
         * in only the buffers it writes (tmpfs allocates a page as it is first
         * written; an idle peer's buffers never are). Said once a node, from its
         * lead, as the shrink below is: it is a limit the operator chose neither.
         */
        if (b->local_id == 0)
            osoe_print_line("osoe: connection buffers reduced from %llu to %llu bytes, and at "
                            "most %u data wires a node, to fit OSOE_AM_BUFFER_BUDGET=%llu over "
                            "%u nodes (OSOE_MESH_DATA_WIRES=on-demand)\n",
                            (unsigned long long)config.buffer, (unsigned long long)buffer,
                            a->data_wires_most, (unsigned long long)config.budget, nodes);
        size = OSOE_AM_SIZE_OK;
    }
    switch (size) {
    case OSOE_AM_SIZE_OK:
        break;
    case OSOE_AM_SIZE_SHRUNK:
        /* Loud, because it changes aggregation behaviour and the operator
         * chose neither the size nor the shrink. Once a node, from its lead
         * (§6.2), as print_info is (init.c): a node's PEs read one environment
         * and reach one size - a PE that did not is refused below, by name -
         * so a line from each said nothing more, and was 128,000 lines at 500
         * nodes of 256 PEs. */
        if (b->local_id == 0)
            osoe_print_line("osoe: connection buffers reduced from %llu to %llu bytes to "
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
    a->bytes = osoe_am_area_bytes(nodes, helpers, buffer);
    a->creator = b->local_id == 0;      /* the local lead (§6.2) */
    snprintf(a->name, sizeof a->name, "/osoe.%llu.%u.%u.conn.%u",
             (unsigned long long)b->job, b->step, b->node, session);

    uint32_t peers = nodes - 1;
    int fd;
    if (a->creator) {
        /* The node's buffers to every peer - or to as many as its count of data
         * wires allows, on demand past the budget's reach, the rest never
         * written (H23); checked before any of it exists (shm-object.h). */
        uint64_t free_bytes;
        uint64_t written = osoe_am_area_written_bytes(nodes, helpers, buffer, a->data_wires_most);
        if (!osoe_shm_room(written, &free_bytes)) {
            char msg[256];
            snprintf(msg, sizeof msg, "/dev/shm has %llu MiB free, but this node's "
                     "connection area for %u nodes needs %llu MiB: lower OSOE_AM_BUFFER, "
                     "or enlarge /dev/shm",
                     (unsigned long long)(free_bytes >> 20), nodes,
                     (unsigned long long)((written + (1u << 20) - 1) >> 20));
            osoe_shm_short(b->shm_capacity_warning_only, msg);
        }
        char err[256];
        fd = osoe_shm_create(a->name,
                             (long)offsetof(struct osoe_connection_area_header, creator_pid),
                             OSOE_SHM_NO_NAMESPACE, err, sizeof err);
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
        /* Noticed, by local id 1 alone - one line a node, the lead being the one
         * waited for (§3.15): the tests of 2026-10-02 found this wait silent. */
        struct osoe_stall_watch watch;
        if (b->local_id == 1)
            osoe_stall_watch_begin_noticed(&watch, 0, b->no_progress_ns);
        else
            osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
        struct timespec back = { 0, 200000 };
        struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
        for (;;) {
            char err[256];
            fd = osoe_shm_open_ours(a->name, err, sizeof err);
            const char *state = "does not exist yet";
            bool leftover = false;
            if (fd >= 0) {
                struct stat st;
                bool sized = fstat(fd, &st) == 0 && st.st_size > 0;
                if (sized && (uint64_t)st.st_size >= a->bytes)
                    break;
                /* The lead sizes the area in one step, from nothing to the
                 * whole of it, so a size that is neither is the lead's, and
                 * smaller than this PE's settings make it: they differ, and
                 * waiting for it to grow would be waiting for ever (the
                 * round-three review's settings sweep). A larger one is
                 * refused below, by the header. Unless another start by these
                 * ids made it: then it is a leftover, which the lead replaces,
                 * as below (the review fixes' adversarial read). */
                if (sized && made_by(fd, lead_pid)) {
                    close(fd);
                    osoe_fatal("connection area %s is %llu bytes, as the local lead's "
                               "OSOE_AM_* settings made it; this PE's make it %llu: every "
                               "PE of a job must set OSOE_AM_* the same", a->name,
                               (unsigned long long)st.st_size, (unsigned long long)a->bytes);
                }
                leftover = sized;
                state = sized ? "an earlier start by these ids left, smaller than this "
                                "PE's settings make it"
                              : "exists but is not sized yet";
                close(fd);
            } else if (errno != ENOENT) {
                osoe_fatal("the connection area: %s", err);
            }
            if (osoe_stall_watch_stalled(&watch, 0)) {
                if (leftover)
                    osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the local "
                               "lead, pid %d, has not replaced %s, which an earlier start by "
                               "these ids left",
                               (unsigned long long)osoe_stall_watch_seconds(&watch),
                               (int)lead_pid, a->name);
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the local lead has not created %s",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), a->name);
            }
            if (osoe_stall_watch_notice_due(&watch))
                osoe_notice("shmem_init on node %u has waited %llu s for the local lead, "
                            "rank %u, to %s the node's connection area %s, which %s; "
                            "still waiting", b->node,
                            (unsigned long long)osoe_stall_watch_waited_seconds(&watch),
                            b->gtids[0], leftover ? "replace" : "create", a->name, state);
            /* §4.10: every wait sweeps - a local lead that died before sizing
             * the area would leave its siblings here for good. */
            if (osoe_deadline_expired(&sweep)) {
                osoe_sweep_expired("shmem_init");
                sweep = osoe_deadline_in(OSOE_SWEEP_NS);
            }
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
        osoe_print_line("osoe: OSOE_AM_HUGEPAGES=1 but the connection area could not "
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
    struct area_layout l = layout_of(nodes, helpers);

    a->owner = calloc(helpers + 1, sizeof *a->owner);
    a->connection = calloc(peers, sizeof *a->connection);
    if (!a->owner || !a->connection)
        osoe_fatal("out of memory for %u connection handles", peers);
    uint64_t dirty = l.dirty;
    for (uint32_t o = 0; o <= helpers; o++) {
        struct owner_words *w = (struct owner_words *)((char *)p + l.owners) + o;
        a->owner[o] = (struct osoe_connection_area_owner){
            .dirty = (_Atomic uint64_t *)((char *)p + dirty),
            .dirty_words = dirty_words(o, peers, helpers),
            .sleeping = &w->sleeping,
            .wake = &w->wake,
        };
        dirty += align_up((uint64_t)a->owner[o].dirty_words * sizeof(uint64_t));
    }
    for (uint32_t i = 0; i < peers; i++) {
        a->connection[i].s = (struct osoe_connection_shared *)((char *)p + l.conns) + i;
        a->connection[i].data = (unsigned char *)p + l.data + (uint64_t)i * 2 * buffer;
        a->connection[i].capacity = (uint32_t)buffer;
        a->connection[i].wake = b->turnover_wake;
        a->connection[i].chunk = (uint32_t)config.chunk;
    }

    if (a->creator) {
        h->magic = OSOE_CONNECTION_AREA_MAGIC;
        h->layout = OSOE_CONNECTION_AREA_LAYOUT;
        h->node = b->node;
        h->nodes = nodes;
        h->buffer_bytes = buffer;
        h->chunk_bytes = config.chunk;
        h->conns_offset = l.conns;
        h->data_offset = l.data;
        h->helpers = helpers;
        h->owners_offset = l.owners;
        h->area_bytes = a->bytes;
        h->creator_pid = (int32_t)getpid();
        /* Both buffers of every connection sealed, so nothing is reservable
         * until the agent opens one. Zeroed memory would say the opposite
         * (§3.5). */
        for (uint32_t i = 0; i < peers; i++)
            osoe_connection_init(&a->connection[i], a->credits);
        atomic_store_explicit(&h->ready, 1, memory_order_release);
        osoe_futex_wake(&h->ready, INT_MAX, OSOE_FUTEX_SHARED);
    } else {
        struct osoe_stall_watch watch;
        if (b->local_id == 1)
            osoe_stall_watch_begin_noticed(&watch, 0, b->no_progress_ns);
        else
            osoe_stall_watch_begin(&watch, 0, b->no_progress_ns);
        while (atomic_load_explicit(&h->ready, memory_order_acquire) == 0) {
            /* A leftover the local lead is about to reclaim will never be
             * ready: start over (shm-object.h). */
            if (osoe_shm_orphaned(fd)) {
                munmap(p, a->bytes);
                close(fd);
                free(a->connection);
                free(a->owner);
                a->connection = NULL;
                a->owner = NULL;
                a->map = NULL;
                goto again;
            }
            struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
            osoe_futex_wait(&h->ready, 0, &sweep, OSOE_FUTEX_SHARED);
            /* §4.10: every wait sweeps - a local lead that died before
             * finishing the area would leave its siblings here for good. */
            if (osoe_deadline_expired(&sweep))
                osoe_sweep_expired("shmem_init");
            if (osoe_stall_watch_stalled(&watch, 0))
                osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the local lead has not finished creating %s",
                           (unsigned long long)osoe_stall_watch_seconds(&watch), a->name);
            if (osoe_stall_watch_notice_due(&watch))
                osoe_notice("shmem_init on node %u has waited %llu s for the local lead, "
                            "rank %u, to finish the node's connection area %s, which it "
                            "has created; still waiting", b->node,
                            (unsigned long long)osoe_stall_watch_waited_seconds(&watch),
                            b->gtids[0], a->name);
        }
        /* A failed start by the same ids leaves its area ready too, and only the
         * area this node's lead made for this launch is this job's (job.c's
         * follow, the release fixes' review, correctness F1): wait for the lead
         * to remove the leftover's name as it makes its own, and start over. */
        if (lead_pid != OSOE_SHM_ANY_CREATOR && h->creator_pid != lead_pid) {
            struct timespec pause = { 0, 200000 };
            struct osoe_deadline sweep = osoe_deadline_in(OSOE_SWEEP_NS);
            while (!osoe_shm_orphaned(fd)) {
                if (osoe_deadline_expired(&sweep)) {
                    osoe_sweep_expired("shmem_init");
                    sweep = osoe_deadline_in(OSOE_SWEEP_NS);
                }
                if (osoe_stall_watch_stalled(&watch, 0))
                    osoe_fatal("no progress for %llu s (OSOE_NO_PROGRESS_TIMEOUT): the local "
                               "lead, pid %d, has not replaced %s, which pid %d left",
                               (unsigned long long)osoe_stall_watch_seconds(&watch),
                               (int)lead_pid, a->name, (int)h->creator_pid);
                if (osoe_stall_watch_notice_due(&watch))
                    osoe_notice("shmem_init on node %u has waited %llu s for the local lead, "
                                "rank %u, to replace the node's connection area %s, which an "
                                "earlier start by these ids left; still waiting", b->node,
                                (unsigned long long)osoe_stall_watch_waited_seconds(&watch),
                                b->gtids[0], a->name);
                nanosleep(&pause, NULL);
                if (pause.tv_nsec < 10000000)
                    pause.tv_nsec *= 2;
            }
            munmap(p, a->bytes);
            close(fd);
            free(a->connection);
            free(a->owner);
            a->connection = NULL;
            a->owner = NULL;
            a->map = NULL;
            goto again;
        }
        if (h->magic != OSOE_CONNECTION_AREA_MAGIC || h->layout != OSOE_CONNECTION_AREA_LAYOUT)
            osoe_fatal("connection area %s was laid out by a library of layout %u, and this "
                       "PE's lays out %u: every PE of a node must run the same library",
                       a->name, h->layout, OSOE_CONNECTION_AREA_LAYOUT);
        if (h->buffer_bytes != buffer || h->chunk_bytes != config.chunk || h->nodes != nodes)
            osoe_fatal("connection area %s was created with different AM sizing "
                       "(buffer %llu chunk %llu nodes %u, not %llu/%llu/%u): every PE "
                       "of a job must set OSOE_AM_* the same", a->name,
                       (unsigned long long)h->buffer_bytes,
                       (unsigned long long)h->chunk_bytes, h->nodes,
                       (unsigned long long)buffer, (unsigned long long)config.chunk, nodes);
        if (h->helpers != helpers)
            osoe_fatal("connection area %s was laid out for %u progress helpers, and this PE "
                       "runs %u: every PE of a node must run the same count", a->name,
                       h->helpers, helpers);
    }
    close(fd);
    /* Every local PE opens the area; the last to have done so removes its name. */
    if (atomic_fetch_add_explicit(&h->opened, 1, memory_order_seq_cst) + 1 == b->local_count &&
        shm_unlink(a->name) != 0)
        osoe_fatal("cannot remove the connection area's name %s: %s", a->name, strerror(errno));
}

void osoe_connection_area_close(struct osoe_connection_area *a)
{
    free(a->connection);
    free(a->owner);
    if (a->map)
        munmap(a->map, a->bytes);       /* its name went when the last PE opened it */
    *a = (struct osoe_connection_area){ 0 };
}

struct osoe_connection *osoe_connection_to(struct osoe_connection_area *a, uint32_t peer)
{
    if (peer == a->node || peer >= a->nodes || !a->connection)
        return NULL;
    return &a->connection[osoe_peer_slot(a->node, peer)];
}
