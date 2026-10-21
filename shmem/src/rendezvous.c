/* SPDX-License-Identifier: LGPL-3.0-or-later */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <float.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "blocked.h"
#include "fatal.h"
#include "internode-backend.h"
#include "job.h"
#include "listener.h"
#include "rendezvous.h"

/*
 * Each header's magic is its version too: a report whose shape differs is a
 * node running another library, and must be refused as one - read as this
 * shape, it would be refused for whichever value the misread happened to hit.
 * The up header's second shape, "osoeup-2", carries OSOE_MAX_TEAMS (phase 1
 * step 7) and is 48 bytes, where the first, "osomeup", was 40. The third,
 * "osoeup-3", carries OSOE_COLLECTIVE_PIPELINE (phase 1 step 5 slice 5) in the
 * word the second kept zero only so that nothing travelled as padding: the same
 * 48 bytes, so the size assert below cannot tell the two apart, and the magic
 * must. Read as the second shape, a third-shape report's D would be a word no
 * lead checks, and a node with another D would be let in. The fourth,
 * "osoeup-4", carries OSOE_SMALL_COLLECTIVE_BYTES (the team redesign's step E)
 * and a word kept zero after it, for the same reason the second kept one: 56
 * bytes. The fifth, "osoeup-5", carries OSOE_COLLECTIVE_SHAPE in that word and
 * the link's three numbers after it (v0.2 step 2): 80 bytes. The sixth,
 * "osoeup-6", carries OSOE_COLLECTIVE_PLACES after them and a word kept zero
 * (P30, P31): 88 bytes. The seventh, "osoeup-7", carries the node's
 * architecture in that word (P21): 88 bytes again, so the magic tells them
 * apart.
 */
#define UP_MAGIC   UINT64_C(0x372d7075656f736f)  /* "osoeup-7", little-endian */
#define DOWN_MAGIC UINT64_C(0x6e776f64656f736f)  /* "osoedown" */

/* What every node must agree on (§6.2): a node that differs is a mismatched
 * launch or a mismatched binary, refused with a message rather than found as an
 * undecodable message at PE 47 an hour in (§3.13). */
struct agreement {
    uint32_t world_size;
    uint32_t node_count;
    uint64_t symmetric_size;
    /* The internode backend's agreement word (internode-backend.h) - on AM the
     * handler table's checksum: a node whose messages mean other things runs
     * another binary (§3.13). Where the checksum travelled before P27, and the
     * same value on AM, so the report's shape and its magic stay. */
    uint32_t backend_agreement_word;
    uint32_t collective_fanout;     /* §5.3: a node building another tree hangs the job */
    /* §5.7: a node with another count of team numbers lays its internal
     * region out differently, and a put of one team's piece would land in
     * another's slot. */
    uint32_t max_teams;
    /* §5.8: D, the pieces in flight per lead, which fixes the internal
     * region's offsets as the fan-out and the team count do. */
    uint32_t collective_pipeline;
    /* §5.8: the largest small collective, which sizes every team number's small
     * rows and decides every data collective's class: a node that decided
     * otherwise would wait for a grant its parent never sends, or put a piece
     * into a lane nobody lent. */
    uint32_t small_collective_bytes;
    /* v0.2 step 2: the collective shape's setting and the link set by hand
     * (0 where not set): a node choosing a shape from other numbers would
     * build another tree for the same call. */
    uint32_t collective_shape;
    uint64_t link_hop_ns;
    uint64_t link_message_ns;
    uint64_t link_bytes_per_second;
    /* P30, P31: how collect finds its places, every lead of a team alike or
     * they wait on different words - "osoeup-6". */
    uint32_t collective_places;
    /* P21, "osoeup-7": the build's architecture as the library moves data -
     * byte order, long's size, and long double's size and format, which
     * x86-64 (the x87's 80-bit extended format) and aarch64 (binary128) do
     * not share - so that a job mixing them is refused rather than reduce
     * long doubles to garbage with no message (architecture_word). */
    uint32_t architecture;
};

struct up_header {
    uint64_t magic;                 /* UP_MAGIC: its shape */
    struct agreement agreement;
    uint32_t nodes;                 /* node records that follow */
    uint32_t progressions;          /* progressions that follow the records */
};

_Static_assert(sizeof(struct up_header) == 88 &&
               offsetof(struct up_header, agreement) +
                       offsetof(struct agreement, collective_places) == 72 &&
               offsetof(struct up_header, agreement) +
                       offsetof(struct agreement, architecture) == 76,
               "the up header's shape is UP_MAGIC's: a new shape takes a new magic");

/* One node of a subtree, as it travels up. */
struct node_record {
    uint32_t node;
    uint32_t progression_count;
    uint32_t ipv4;                  /* network order */
    uint32_t reserved;
    unsigned char blob[OSOE_JOB_BACKEND_BLOB_BYTES];
};

struct down_header {
    uint64_t magic;
    uint32_t node_count;
    uint32_t run_count;
};

/* A subtree's report, growing as the children's arrive. */
struct report {
    struct node_record *records;
    uint32_t record_count;
    struct osoe_rank_progression *progressions;
    uint32_t progression_count;
};

static void report_add(struct report *r, const struct node_record *records, uint32_t n,
                       const struct osoe_rank_progression *progressions, uint32_t m)
{
    r->records = realloc(r->records, sizeof *r->records * (r->record_count + n));
    r->progressions = realloc(r->progressions,
                              sizeof *r->progressions * (r->progression_count + m + 1));
    if (!r->records || !r->progressions)
        osoe_fatal("out of memory for the rendezvous's report");
    memcpy(r->records + r->record_count, records, sizeof *records * n);
    memcpy(r->progressions + r->progression_count, progressions, sizeof *progressions * m);
    r->record_count += n;
    r->progression_count += m;
}

/*
 * The build's architecture as the rendezvous compares it (P21): long double's
 * mantissa in bits (LDBL_MANT_DIG) in the low byte, its size in the next, long's
 * size in the third, and 1 above them for little-endian. Facts of the compiler,
 * so a node's word is its binary's. OSOE_TEST_ARCHITECTURE=other gives a node
 * the word of the other of x86-64 and aarch64 - binary128's mantissa if its own
 * is not, the x87's if it is - so that a test sees the refusal on one machine.
 */
static uint32_t architecture_word(void)
{
    uint32_t mantissa = LDBL_MANT_DIG;
    const char *test = getenv("OSOE_TEST_ARCHITECTURE");
    if (test) {
        if (strcmp(test, "other") != 0)
            osoe_fatal("OSOE_TEST_ARCHITECTURE='%s': its one value is 'other', which gives "
                       "this node the word of the other of x86-64 and aarch64", test);
        mantissa = mantissa == 113 ? 64 : 113;
    }
    return mantissa | (uint32_t)sizeof(long double) << 8 | (uint32_t)sizeof(long) << 16 |
           (uint32_t)(__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__) << 24;
}

static void describe_architecture(uint32_t word, char *text, size_t len)
{
    uint32_t mantissa = word & 0xff;
    const char *format = mantissa == 64    ? "the x87's 80-bit extended format"
                         : mantissa == 113 ? "IEEE 754's binary128"
                         : mantissa == 53  ? "IEEE 754's binary64"
                         : mantissa == 106 ? "the double-double format"
                                           : "a format of its own";
    snprintf(text, len, "long double in %s (a %u-bit mantissa in %u bytes), a long of %u bytes "
             "and %s byte order", format, mantissa, (word >> 8) & 0xff, (word >> 16) & 0xff,
             (word >> 24) & 1 ? "little-endian" : "big-endian");
}

/* A child's value against this node's, by name. */
static void agree(const struct osoe_bootstrap *b, uint32_t child, const struct agreement *mine,
                  const struct agreement *theirs)
{
    if (theirs->world_size != mine->world_size || theirs->node_count != mine->node_count)
        osoe_fatal("node %u (or a node below it in the rendezvous tree) was launched for a "
                   "job of %u tasks on %u nodes; node %u says %u on %u", child,
                   theirs->world_size, theirs->node_count, b->node, mine->world_size,
                   mine->node_count);
    if (theirs->symmetric_size != mine->symmetric_size)
        osoe_fatal("node %u (or a node below it) has SHMEM_SYMMETRIC_SIZE %llu; node %u has "
                   "%llu: every PE of a job must agree", child,
                   (unsigned long long)theirs->symmetric_size, b->node,
                   (unsigned long long)mine->symmetric_size);
    if (theirs->backend_agreement_word != mine->backend_agreement_word)
        osoe_fatal("node %u (or a node below it) has the internode backend's agreement word "
                   "%#x; node %u has %#x: the nodes are running different binaries (on AM the "
                   "word is the handler table's checksum, §3.13)", child,
                   theirs->backend_agreement_word, b->node, mine->backend_agreement_word);
    if (theirs->collective_fanout != mine->collective_fanout)
        osoe_fatal("node %u (or a node below it) has OSOE_COLLECTIVE_FANOUT=%u; node %u has "
                   "%u: every node must build the same internode tree (§5.3)", child,
                   theirs->collective_fanout, b->node, mine->collective_fanout);
    if (theirs->max_teams != mine->max_teams)
        osoe_fatal("node %u (or a node below it) has OSOE_MAX_TEAMS=%u; node %u has %u: every "
                   "node must number teams alike (§5.7)", child, theirs->max_teams, b->node,
                   mine->max_teams);
    if (theirs->collective_pipeline != mine->collective_pipeline)
        osoe_fatal("node %u (or a node below it) has OSOE_COLLECTIVE_PIPELINE=%u; node %u has "
                   "%u: every node must keep as many pieces in flight, since it places them "
                   "in its internal region by that number (§5.8)", child,
                   theirs->collective_pipeline, b->node, mine->collective_pipeline);
    if (theirs->small_collective_bytes != mine->small_collective_bytes)
        osoe_fatal("node %u (or a node below it) has OSOE_SMALL_COLLECTIVE_BYTES=%u; node %u "
                   "has %u: every node must decide alike which collectives are small, since "
                   "a small one moves through a team's own slots and a large one through "
                   "lanes its parent grants (§5.8)", child, theirs->small_collective_bytes,
                   b->node, mine->small_collective_bytes);
    if (theirs->collective_shape != mine->collective_shape)
        osoe_fatal("node %u (or a node below it) has OSOE_COLLECTIVE_SHAPE=%s; node %u has %s: "
                   "every node must shape a collective's tree alike", child,
                   osoe_collective_shape_name(theirs->collective_shape), b->node,
                   osoe_collective_shape_name(mine->collective_shape));
    if (theirs->collective_places != mine->collective_places)
        osoe_fatal("node %u (or a node below it) has OSOE_COLLECTIVE_PLACES=%s; node %u has %s: "
                   "every node must find collect's places the same way", child,
                   osoe_collective_places_name(theirs->collective_places), b->node,
                   osoe_collective_places_name(mine->collective_places));
    if (theirs->architecture != mine->architecture) {
        char theirs_text[160], mine_text[160];
        describe_architecture(theirs->architecture, theirs_text, sizeof theirs_text);
        describe_architecture(mine->architecture, mine_text, sizeof mine_text);
        osoe_fatal("node %u (or a node below it) has %s; node %u has %s: every node of a job "
                   "must be of one architecture, since the library moves data between nodes "
                   "as bytes", child, theirs_text, b->node, mine_text);
    }
    if (theirs->link_hop_ns != mine->link_hop_ns ||
        theirs->link_message_ns != mine->link_message_ns ||
        theirs->link_bytes_per_second != mine->link_bytes_per_second)
        osoe_fatal("node %u (or a node below it) has OSOE_LINK_* of a hop of %llu ns, a "
                   "message cost of %llu ns and %llu bytes a second; node %u has %llu, %llu and "
                   "%llu (0: not set): every node must choose a collective's shape from the "
                   "same link", child, (unsigned long long)theirs->link_hop_ns,
                   (unsigned long long)theirs->link_message_ns,
                   (unsigned long long)theirs->link_bytes_per_second, b->node,
                   (unsigned long long)mine->link_hop_ns,
                   (unsigned long long)mine->link_message_ns,
                   (unsigned long long)mine->link_bytes_per_second);
}

static void lost(const struct osoe_bootstrap *b, uint32_t node, const char *role)
{
    osoe_fatal("node %u: the rendezvous lost node %u, its %s: that node's lead has exited or "
               "died (it may have refused the job; its own message says why)", b->node, node,
               role);
}

/* A child's subtree report, checked, into ours. */
static void receive_up(const struct osoe_bootstrap *b, int fd, uint32_t child,
                       const struct agreement *mine, struct report *r)
{
    struct up_header h;
    if (!osoe_listener_read(b, fd, &h, sizeof h, child))
        lost(b, child, "child in the rendezvous tree");
    if (h.magic != UP_MAGIC)
        osoe_fatal("node %u sent a rendezvous report this build cannot read: the nodes are "
                   "running different osoe libraries", child);
    agree(b, child, mine, &h.agreement);
    if (h.nodes == 0 || h.nodes > b->node_count || h.progressions > b->world_size)
        osoe_fatal("node %u's rendezvous report claims %u nodes and %u progressions in a job "
                   "of %u nodes", child, h.nodes, h.progressions, b->node_count);
    struct node_record *records = malloc(sizeof *records * h.nodes);
    struct osoe_rank_progression *progressions =
        malloc(sizeof *progressions * (h.progressions + 1));
    if (!records || !progressions)
        osoe_fatal("out of memory for node %u's rendezvous report", child);
    if (!osoe_listener_read(b, fd, records, sizeof *records * h.nodes, child) ||
        !osoe_listener_read(b, fd, progressions, sizeof *progressions * h.progressions, child))
        lost(b, child, "child in the rendezvous tree");
    uint64_t claimed = 0;
    for (uint32_t i = 0; i < h.nodes; i++)
        claimed += records[i].progression_count;
    if (claimed != h.progressions)
        osoe_fatal("node %u's rendezvous report does not add up", child);
    report_add(r, records, h.nodes, progressions, h.progressions);
    free(records);
    free(progressions);
}

static void send_up(const struct osoe_bootstrap *b, int fd, uint32_t parent,
                    const struct agreement *mine, const struct report *r)
{
    struct up_header h = { UP_MAGIC, *mine, r->record_count, r->progression_count };
    if (!osoe_listener_write(b, fd, &h, sizeof h, parent) ||
        !osoe_listener_write(b, fd, r->records, sizeof *r->records * r->record_count, parent) ||
        !osoe_listener_write(b, fd, r->progressions,
                             sizeof *r->progressions * r->progression_count, parent))
        lost(b, parent, "parent in the rendezvous tree");
}

/* The root: every node's report is in; build what every node learns. */
static void build(const struct osoe_bootstrap *b, const struct report *r,
                  struct osoe_rendezvous_result *out)
{
    if (r->record_count != b->node_count)
        osoe_fatal("the rendezvous heard from %u nodes of %u", r->record_count, b->node_count);
    struct osoe_rank_report *reports = calloc(b->node_count, sizeof *reports);
    out->ipv4 = calloc(b->node_count, sizeof *out->ipv4);
    out->blobs = calloc(b->node_count, OSOE_JOB_BACKEND_BLOB_BYTES);
    bool *seen = calloc(b->node_count, sizeof *seen);
    if (!reports || !out->ipv4 || !out->blobs || !seen)
        osoe_fatal("out of memory for the rendezvous's tables");
    const struct osoe_rank_progression *p = r->progressions;
    for (uint32_t i = 0; i < r->record_count; i++) {
        const struct node_record *record = &r->records[i];
        if (record->node >= b->node_count || seen[record->node])
            osoe_fatal("node %u reported twice, or is not a node of this job", record->node);
        seen[record->node] = true;
        reports[record->node] = (struct osoe_rank_report){ record->node, p,
                                                           record->progression_count };
        p += record->progression_count;
        out->ipv4[record->node] = record->ipv4;
        memcpy(out->blobs + (size_t)record->node * OSOE_JOB_BACKEND_BLOB_BYTES, record->blob,
               OSOE_JOB_BACKEND_BLOB_BYTES);
    }
    char why[256];
    out->runs = osoe_rank_runs_build(reports, b->node_count, b->world_size, &out->run_count,
                                     why, sizeof why);
    if (!out->runs)
        osoe_fatal("%s", why);
    free(reports);
    free(seen);
}

static void send_down(const struct osoe_bootstrap *b, int fd, uint32_t child,
                      const struct osoe_rendezvous_result *res)
{
    struct down_header h = { DOWN_MAGIC, b->node_count, res->run_count };
    if (!osoe_listener_write(b, fd, &h, sizeof h, child) ||
        !osoe_listener_write(b, fd, res->runs, sizeof *res->runs * res->run_count, child) ||
        !osoe_listener_write(b, fd, res->ipv4, sizeof *res->ipv4 * b->node_count, child) ||
        !osoe_listener_write(b, fd, res->blobs, (size_t)b->node_count * OSOE_JOB_BACKEND_BLOB_BYTES,
                             child))
        lost(b, child, "child in the rendezvous tree");
}

static void receive_down(const struct osoe_bootstrap *b, int fd, uint32_t parent,
                         struct osoe_rendezvous_result *out)
{
    struct down_header h;
    if (!osoe_listener_read(b, fd, &h, sizeof h, parent))
        lost(b, parent, "parent in the rendezvous tree");
    if (h.magic != DOWN_MAGIC || h.node_count != b->node_count || h.run_count == 0 ||
        h.run_count > b->world_size)
        osoe_fatal("node %u's rendezvous tables are not this job's (%u nodes, %u runs)", parent,
                   h.node_count, h.run_count);
    out->run_count = h.run_count;
    out->runs = malloc(sizeof *out->runs * h.run_count);
    out->ipv4 = malloc(sizeof *out->ipv4 * b->node_count);
    out->blobs = malloc((size_t)b->node_count * OSOE_JOB_BACKEND_BLOB_BYTES);
    if (!out->runs || !out->ipv4 || !out->blobs)
        osoe_fatal("out of memory for the rendezvous's tables");
    if (!osoe_listener_read(b, fd, out->runs, sizeof *out->runs * h.run_count, parent) ||
        !osoe_listener_read(b, fd, out->ipv4, sizeof *out->ipv4 * b->node_count, parent) ||
        !osoe_listener_read(b, fd, out->blobs, (size_t)b->node_count * OSOE_JOB_BACKEND_BLOB_BYTES,
                            parent))
        lost(b, parent, "parent in the rendezvous tree");
}

void osoe_rendezvous(const struct osoe_bootstrap *b, uint32_t session,
                     const struct sockaddr_in *address, const void *blob,
                     struct osoe_rendezvous_result *out)
{
    *out = (struct osoe_rendezvous_result){ 0 };
    struct agreement mine = { b->world_size, b->node_count, b->symmetric_size,
                              osoe_internode_backend_agreement_word(),
                              b->collective_fanout, b->max_teams,
                              b->collective_pipeline, b->small_collective_bytes,
                              b->collective_shape, b->link_hop_ns, b->link_message_ns,
                              b->link_bytes_per_second, b->collective_places,
                              architecture_word() };

    /* This node's record: its address, its ranks as progressions, its blob. */
    struct report r = { 0 };
    struct osoe_rank_progression *own = malloc(sizeof *own * (b->local_count + 1));
    if (!own)
        osoe_fatal("out of memory");
    struct node_record record = { b->node, osoe_rank_progressions(b->gtids, b->local_count, own),
                               address->sin_addr.s_addr, 0, { 0 } };
    memcpy(record.blob, blob, OSOE_JOB_BACKEND_BLOB_BYTES);
    report_add(&r, &record, 1, own, record.progression_count);
    free(own);

    /*
     * Parent first, then children (tools/modelcheck/rendezvous_model.py). A lead
     * expects children by its own node count, so one given too large a count
     * waits for children that do not exist; had it done so before calling its
     * parent, nobody would ever learn why - its parent waiting for it, the root
     * for the parent, every other lead for the root's tables, and no lead
     * refusing anything. Calling first puts every edge in place before any
     * lead waits by its count, and the hello carries the count, so the first
     * edge that disagrees is refused (listener.c). The model's broken orders
     * are STUCK, and shmem.job.disagree.node-count hung until its timeout.
     */
    struct osoe_listener l;
    osoe_listener_check_own_address(b, address);
    osoe_listener_open(&l, b, session);
    int up = -1;
    uint32_t parent = 0;
    /* Each wait on another node here is in the PE's "blocked in" record as
     * shmem_init and the node, so that osoe-stat shows an init hang where it
     * read "-" (D7): the parent while it is called and heard from, each child
     * while it is awaited. */
    if (b->node != 0) {
        parent = (b->node - 1) / OSOE_RENDEZVOUS_FANOUT;
        struct sockaddr_in there;
        osoe_listener_node_address(b, parent, &there);
        osoe_blocked_enter("shmem_init", OSOE_BLOCKED_DETAIL_NODE, parent);
        up = osoe_listener_connect(b, session, &there, parent, OSOE_HELLO_RENDEZVOUS);
        osoe_blocked_leave();
    }

    const uint32_t first_child = b->node * OSOE_RENDEZVOUS_FANOUT + 1;
    uint32_t children = 0;
    while (children < OSOE_RENDEZVOUS_FANOUT && first_child + children < b->node_count &&
           first_child + children > b->node)
        children++;
    int child_fd[OSOE_RENDEZVOUS_FANOUT];
    for (uint32_t i = 0; i < OSOE_RENDEZVOUS_FANOUT; i++)
        child_fd[i] = -1;
    for (uint32_t accepted = 0; accepted < children; accepted++) {
        /* For the notice of a wait that lasts: the children still to call. */
        char nodes[96] = "";
        int used = 0;
        uint32_t missing = children - accepted;
        for (uint32_t i = 0; i < children && used >= 0 && (size_t)used < sizeof nodes - 16; i++)
            if (child_fd[i] < 0)
                used += snprintf(nodes + used, sizeof nodes - (size_t)used, "%s%u",
                                 used ? ", " : "", first_child + i);
        char expecting[160];
        snprintf(expecting, sizeof expecting, "node%s %s, its child%s in the rendezvous tree,",
                 missing == 1 ? "" : "s", nodes, missing == 1 ? "" : "ren");
        uint32_t peer, awaited = first_child;
        while (child_fd[awaited - first_child] >= 0)
            awaited++;
        osoe_blocked_enter("shmem_init", OSOE_BLOCKED_DETAIL_NODE, awaited);
        int fd = osoe_listener_accept(&l, OSOE_HELLO_PURPOSE(OSOE_HELLO_RENDEZVOUS), &peer,
                                      NULL, expecting);
        osoe_blocked_leave();
        /* The hello agreed on the node count, so only a second lead with one
         * node id can be a child this lead is not waiting for. */
        if (peer < first_child || peer >= first_child + children ||
            child_fd[peer - first_child] >= 0)
            osoe_fatal("node %u was called as a parent in the rendezvous tree by node %u, which "
                       "is not a child it is waiting for (%u to %u): two leads were given the "
                       "same node id", b->node, peer, first_child, first_child + children - 1);
        child_fd[peer - first_child] = fd;
    }
    osoe_listener_close(&l);
    /* Each child reports once its own subtree has, so a lead waits for its
     * subtree and never for a node outside it. */
    for (uint32_t i = 0; i < children; i++) {
        osoe_blocked_enter("shmem_init", OSOE_BLOCKED_DETAIL_NODE, first_child + i);
        receive_up(b, child_fd[i], first_child + i, &mine, &r);
        osoe_blocked_leave();
    }

    if (b->node == 0) {
        build(b, &r, out);
    } else {
        osoe_blocked_enter("shmem_init", OSOE_BLOCKED_DETAIL_NODE, parent);
        send_up(b, up, parent, &mine, &r);
        receive_down(b, up, parent, out);
        osoe_blocked_leave();
        close(up);
    }
    free(r.records);
    free(r.progressions);

    /* Then the tables, down: each child forwards them to its own. */
    for (uint32_t i = 0; i < children; i++) {
        send_down(b, child_fd[i], first_child + i, out);
        close(child_fd[i]);
    }
}

/*
 * internode-backend.h's rendezvous, on AM: this lead's address - its own
 * host-list entry's, or the one it would use to reach a peer where its name
 * resolves to loopback (listener.h) - and the blob, to every other lead and
 * back, by the tree above.
 */
void osoe_internode_backend_rendezvous(const struct osoe_bootstrap *b, uint32_t session,
                                       const void *blob, struct osoe_rendezvous_result *result)
{
    struct sockaddr_in address;
    osoe_listener_own_address(b, &address);
    osoe_rendezvous(b, session, &address, blob, result);
}
