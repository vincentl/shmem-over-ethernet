#define _GNU_SOURCE
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "am.h"
#include "base/hostlist.h"
#include "fatal.h"
#include "job.h"
#include "listener.h"
#include "rendezvous.h"

#define UP_MAGIC   UINT64_C(0x7075656d6f736f)    /* "osomeup", little-endian */
#define DOWN_MAGIC UINT64_C(0x6e776f64656f736f)  /* "osoedown" */

/* What every node must agree on (§6.2): a node that differs is a mismatched
 * launch or a mismatched binary, refused with a message rather than found as an
 * undecodable message at PE 47 an hour in (§3.13). */
struct agreement {
    uint32_t world_size;
    uint32_t node_count;
    uint64_t symmetric_size;
    uint32_t handler_checksum;
    uint32_t collective_fanout;     /* §5.3: a node building another tree hangs the job */
};

struct up_header {
    uint64_t magic;
    struct agreement agreement;
    uint32_t nodes;                 /* node records that follow */
    uint32_t progressions;          /* progressions that follow the records */
};

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

void osoe_rendezvous_address(const struct osoe_bootstrap *b, uint32_t node,
                             struct sockaddr_in *out)
{
    char host[256];
    if (!b->node_list || !osoe_hostlist_entry(b->node_list, node, host, sizeof host))
        osoe_fatal("OSOE_STEP_NODELIST='%s' has no usable entry for node %u",
                   b->node_list ? b->node_list : "", node);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM }, *found;
    int e = getaddrinfo(host, NULL, &hints, &found);
    if (e != 0)
        osoe_fatal("cannot resolve %s, node %u's host in OSOE_STEP_NODELIST: %s", host, node,
                   gai_strerror(e));
    *out = *(const struct sockaddr_in *)found->ai_addr;
    out->sin_port = 0;
    freeaddrinfo(found);
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
    if (theirs->handler_checksum != mine->handler_checksum)
        osoe_fatal("node %u (or a node below it) has AM handler table checksum %#x; node %u "
                   "has %#x: the nodes are running different binaries (§3.13)", child,
                   theirs->handler_checksum, b->node, mine->handler_checksum);
    if (theirs->collective_fanout != mine->collective_fanout)
        osoe_fatal("node %u (or a node below it) has OSOE_COLLECTIVE_FANOUT=%u; node %u has "
                   "%u: every node must build the same internode tree (§5.3)", child,
                   theirs->collective_fanout, b->node, mine->collective_fanout);
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
        osoe_fatal("node %u sent a rendezvous report this build cannot read", child);
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
                              osoe_am_table_checksum(), b->collective_fanout };

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
    osoe_listener_open(&l, b, session, address);
    int up = -1;
    uint32_t parent = 0;
    if (b->node != 0) {
        parent = (b->node - 1) / OSOE_RENDEZVOUS_FANOUT;
        struct sockaddr_in there;
        osoe_rendezvous_address(b, parent, &there);
        up = osoe_listener_connect(b, session, &there, parent, OSOE_HELLO_RENDEZVOUS);
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
        uint32_t peer;
        int fd = osoe_listener_accept(&l, OSOE_HELLO_RENDEZVOUS, &peer);
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
    for (uint32_t i = 0; i < children; i++)
        receive_up(b, child_fd[i], first_child + i, &mine, &r);

    if (b->node == 0) {
        build(b, &r, out);
    } else {
        send_up(b, up, parent, &mine, &r);
        receive_down(b, up, parent, out);
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
