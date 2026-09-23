/*
 * The progress loop between two logical nodes (§3.3, §3.7, §3.8, §3.9, §3.12).
 *
 * The first test in which every layer runs together: an application thread
 * reserves into a connection buffer (§3.5), the lead's agent seals it and hands
 * it to the wire (§3.9), the peer's agent parses the stream (§3.3), applies the
 * message, and - for a get or a fetching AMO - takes a reply descriptor (§3.8)
 * that a later pass drains back. A byte that survives that round trip has been
 * through all of it.
 *
 * It does not call shmem_init. A job of more than one logical node is still
 * refused until the internode barrier lands, and nothing here needs a job: the
 * loop asks its host for three things - resolve an address, find a landing
 * zone, wake a PE's waiters - and this file supplies them over ordinary local
 * memory. That is also the point of the host callbacks: the loop can be tested
 * without a job, and cannot reach into state that only exists inside one.
 *
 * The wire is sized far below a segment and its split injection is on, so the
 * stream really does arrive in pieces (§8.2).
 *
 * **Two PEs per node, not one**, and that is what makes the connection rules
 * bind. §3.5's buffers are shared by every local PE, so a single writer never
 * exercises the seal bit, the one-reservable rule, or the "sendable only when
 * every reservation has been committed" condition - a first version of this
 * test ran one PE per node and passed with the seal removed entirely. The
 * non-lead PE here posts into the same buffer the lead is sealing, which is the
 * arrangement §3.12 describes and the only one where those rules can fail.
 *
 * The get is deliberately larger than one AM chunk, so its reply is drained
 * over several passes and the descriptor survives partial draining (§3.8) -
 * another rule a single-chunk reply never reaches.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "am.h"
#include "arrivals.h"
#include "base/deadline.h"
#include "bootstrap.h"
#include "check.h"
#include "collective-tree.h"
#include "connection-area.h"
#include "node.h"
#include "progress.h"
#include "ring.h"
#include "reply.h"
#include "wire.h"

_Noreturn void osoe_fatal(const char *fmt, ...);
_Noreturn void osoe_fatal(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osoe_fatal: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    exit(1);
}

enum { HEAP = 8192, DESCRIPTORS = 32, SCRATCH = 4096, PAYLOAD = 600 };

/*
 * Turn the engine until this node owes the peer nothing.
 *
 * **"Nothing moved this pass" is not "nothing is left to send."** A pass moves
 * nothing whenever the wire is momentarily full - the ring here is 256 bytes
 * and the split injection truncates transfers on purpose - and a loop that
 * stops there walks away from a sealed segment that has not finished going out.
 * With the barrier release in that segment, the peer waits forever for a node
 * that has stopped sending.
 *
 * This is the same defect the library had at teardown, and osoe_progress_idle
 * is the predicate written for it: every queue empty, nothing sealed, nothing
 * in flight, no committed bytes waiting in an open buffer. Using it here rather
 * than a second condition of the test's own is the point - if the two ever
 * disagree, one of them is wrong.
 */
static void drain(struct osoe_progress *p, const struct osoe_deadline *give_up)
{
    while (!osoe_progress_idle(p)) {
        if (!osoe_progress_poll(p))
            osoe_ring_wait(p->ring, UINT64_C(1000000));
        if (osoe_deadline_expired(give_up))
            return;
    }
}

/*
 * Then close as a lead does (§6.4, close_model.py): goodbye, end of stream, and
 * read until the peer's has ended - which is also what keeps this node reading
 * until the peer has taken everything it was owed.
 */
static void close_connections(struct osoe_progress *p, const struct osoe_deadline *give_up)
{
    osoe_progress_close(p);
    while (!osoe_progress_closed(p) && !osoe_deadline_expired(give_up))
        if (!osoe_progress_poll(p))
            osoe_ring_wait(p->ring, UINT64_C(1000000));
    CHECK(osoe_progress_closed(p));
}

/*
 * Disjoint regions of the peer's heap, one per operation. They must not
 * overlap: an earlier version read back a range the put was writing, and the
 * reply correctly returned the put's bytes - §4.6's deferred-sampling hazard
 * in miniature, and undefined by the specification, so the test was wrong
 * rather than the code.
 */
enum { PUT_AT = 128, PEER_PUT_AT = 768, GET_AT = 4096, GET_BYTES = 3000,
       SENTINEL_AT = 1024, RMW_AT = 1152 };
enum { PEER_PUT_BYTES = 200 };

/* This process stands in for one node holding one PE, whose rank is its node
 * number. The heap and the landing zone are ordinary memory: the loop does not
 * care where they came from. */
static uint32_t my_pe;
static unsigned char heap[HEAP];
/* The landing zone, then memory that is not the landing zone: the canary is
 * what a forged get reply would land on (OSOE_TEST_FORGE below). */
static struct {
    unsigned char zone[sizeof(struct osoe_landing_slot) + HEAP];
    unsigned char canary[256];
} landing_block;
#define landing_storage landing_block.zone
static uint64_t notifies;

static void *resolve(void *context, uint32_t pe, uint16_t region, uint64_t offset,
                     uint32_t length)
{
    (void)context; (void)region;
    if (pe != my_pe)
        return NULL;
    /* The host bounds-checks, as the real one must: a message naming an offset
     * outside the region is a peer disagreeing about the job. */
    if (offset + length > HEAP) {
        fprintf(stderr, "PE %u: offset %llu length %u is outside the heap\n", pe,
                (unsigned long long)offset, length);
        exit(1);
    }
    return heap + offset;
}

static struct osoe_landing_slot *landing(void *context, uint32_t pe, uint32_t *capacity)
{
    (void)context;
    *capacity = HEAP;
    return pe == my_pe ? (struct osoe_landing_slot *)landing_storage : NULL;
}

static void notify(void *context, uint32_t pe, uint16_t region, uint64_t offset,
                   uint32_t length)
{
    (void)context; (void)pe; (void)region; (void)offset; (void)length;
    notifies++;
}

/* Stands in for the team's epoch counters (§5.4). The real one advances the
 * team's `released` and wakes every local waiter; here it is enough to record
 * which epoch was released, which is what a waiter would be waiting for. */
static uint32_t released_epoch;
static uint32_t releases;

static void release(void *context, uint32_t team, uint32_t epoch)
{
    (void)context; (void)team;
    released_epoch = epoch;
    releases++;
}

/* The area and peer post() marks: every writer says it committed, as the
 * library's do, because the lead drains only connections whose dirty bit is
 * set or which it already knows are busy (§3.10). */
static struct osoe_connection_area *mark_area;
static uint32_t mark_peer;

/* An application thread's half: reserve, copy, commit (§3.5). Never seals -
 * that is the progress thread's (§3.12). */
static bool post(struct osoe_connection *c, uint16_t kind, const void *fixed,
                 uint32_t fixed_bytes, const void *payload, uint32_t payload_bytes)
{
    struct osoe_reservation r;
    uint32_t len = osoe_message_padded(fixed_bytes + payload_bytes);
    if (osoe_connection_reserve(c, len, &r) != OSOE_RESERVATION_OK)
        return false;
    struct osoe_message_header h = { kind, 0, fixed_bytes + payload_bytes };
    unsigned char *at = osoe_reservation_bytes(c, &r);
    memcpy(at, &h, sizeof h);
    memcpy(at + sizeof h, fixed, fixed_bytes);
    if (payload_bytes)
        memcpy(at + sizeof h + fixed_bytes, payload, payload_bytes);
    osoe_connection_commit(c, &r);
    osoe_connection_area_mark(mark_area, mark_peer);
    return true;
}

static uint32_t env_u32(const char *name)
{
    const char *v = getenv(name);
    if (!v) {
        fprintf(stderr, "%s is not set; run this under osoe-launch\n", name);
        exit(1);
    }
    return (uint32_t)strtoul(v, NULL, 10);
}

static unsigned char pattern(size_t i) { return (unsigned char)((i * 37 + 11) & 0xff); }

int main(void)
{
    uint32_t node = env_u32("OSOE_NODEID");
    uint32_t local_id = env_u32("OSOE_LOCALID");
    uint32_t peer = node == 0 ? 1 : 0;
    my_pe = env_u32("OSOE_PROCID");
    /* The peer node's lead, which is every message's target here: keeping the
     * targets on one PE keeps this test about the connection and the loop
     * rather than about mapping a sibling's heap. */
    uint32_t peer_lead = peer == 0 ? 0 : 2;
    bool lead = local_id == 0;

    struct osoe_bootstrap b = {
        .job = strtoull(getenv("OSOE_JOB_ID") ? getenv("OSOE_JOB_ID") : "0", NULL, 10),
        .step = env_u32("OSOE_STEP_ID"),
        .node = node,
        .local_id = local_id,
        .rank = my_pe,
        .local_count = 2,           /* map 0,0,1,1: the last to open the area removes its name */
        .node_count = 2,
        .node_list = getenv("OSOE_STEP_NODELIST"),  /* where the other node listens */
        .no_progress_ns = UINT64_C(30000000000),  /* fail rather than hang */
        .port_low = OSOE_DEFAULT_PORT_LOW,
        .port_high = OSOE_DEFAULT_PORT_HIGH,
    };

    struct osoe_connection_area area;
    osoe_connection_area_open(&area, &b, 2, 0);

    struct osoe_connection *c = osoe_connection_to(&area, peer);
    CHECK_FATAL(c != NULL);  /* a non-lead PE dereferences it below via post() */
    mark_area = &area;
    mark_peer = peer;
    arrivals_open();

    /*
     * A non-lead PE never touches a wire or a ring (§3.12). It reserves into
     * the shared connection buffer, copies, commits, and is done - the lead's
     * agent seals and sends it. Two writers in two processes on one buffer is
     * the whole point of running this at more than one PE per node.
     */
    if (!lead) {
        unsigned char mine[PEER_PUT_BYTES];
        for (size_t i = 0; i < sizeof mine; i++)
            mine[i] = (unsigned char)(i + 0x30 + node);
        struct osoe_am_put put = { .target_pe = peer_lead, .region = 0,
                                   .offset = PEER_PUT_AT };
        struct osoe_deadline until = osoe_deadline_in(UINT64_C(20000000000));
        while (!post(c, OSOE_AM_PUT, &put, sizeof put, mine, sizeof mine)) {
            if (osoe_deadline_expired(&until)) {
                fprintf(stderr, "PE %u: never got a reservation\n", my_pe);
                CHECK(false);
                break;
            }
        }
        /*
         * Say that the bytes are committed. The lead must not stop the engine
         * before they have gone out, and it has no other way to know whether a
         * sibling is still coming: "nothing moved this pass" is not "nothing
         * more will move".
         */
        arrivals_row()[my_pe] = 1.0;
        osoe_connection_area_close(&area);
        /*
         * Deliberately not arrivals_close(): it unlinks the name, and a lead
         * that opens the table after its sibling has finished would then create
         * a fresh, zeroed one and wait for a flag that had already been set.
         * osoe-launch removes /dev/shm/osoetest.<job> itself, quietly, so
         * leaving it is not a leak.
         */
        return CHECK_DONE();
    }

    /* The lead's connection to the other node, and the ring it drives it with
     * (§6.2, ring.h). */
    static struct osoe_wire wire;
    static struct osoe_ring ring;
    osoe_wires_open(&wire, &ring, &b, 0, NULL);
    osoe_ring_enable(&ring);

    static struct osoe_reply descriptors[DESCRIPTORS];
    static unsigned char scratch[SCRATCH];
    static struct osoe_progress_connection connections[1];
    static uint64_t busy[1];
    struct osoe_progress_host host = { resolve, landing, notify, release, NULL, NULL };
    struct osoe_progress p;
    osoe_progress_init(&p, &area, &wire, &ring, descriptors, DESCRIPTORS, scratch, SCRATCH,
                       &host);
    osoe_progress_attach(&p, connections, busy);
    /* Two nodes, so the tree is node 0 with node 1 as its only child. */
    static const uint32_t both[] = { 0, 1 };
    struct osoe_collective_tree tree;
    CHECK_FATAL(osoe_collective_tree(both, 2, b.node, 2, &tree));
    osoe_progress_team(&p, 0, &tree);

    struct osoe_landing_slot *slot = (struct osoe_landing_slot *)landing_storage;
    struct osoe_deadline give_up = osoe_deadline_in(UINT64_C(25000000000));

    /*
     * Symmetric: both leads do the same work to each other, and each one's
     * finish condition is something it can see in its own memory. An earlier
     * version had node 0 drive and node 1 follow, ending on a sentinel and a
     * shared coordination table, and it deadlocked about four runs in five -
     * every extra piece of choreography was another way for the two sides to
     * wait for each other. Nothing here waits for the peer to say it is done.
     */
    for (size_t i = 0; i < HEAP; i++)
        heap[i] = pattern(i);
    memset(heap + RMW_AT, 0, 8);

    unsigned char mine[PAYLOAD];
    for (size_t i = 0; i < sizeof mine; i++)
        mine[i] = (unsigned char)((i ^ 0x5a) + node);

    /*
     * OSOE_TEST_FORGE=get-reply: node 0's lead first sends a get reply nobody
     * asked for, whose token matches nothing and whose chunk_offset points past
     * the landing zone (review round two, D4; the review's forged-get-reply
     * patch). The receiving node must refuse it by name rather than write it -
     * shmem.progress.forged-get-reply asserts the fatal, and the canary check
     * at the end says so if it was written instead.
     */
    const char *forge = getenv("OSOE_TEST_FORGE");
    bool bounds = forge && strcmp(forge, "get-reply-bounds") == 0;
    /* get-reply-bounds: the receiving lead arms a known token before anything
     * moves and never re-arms, and the forged reply carries it, so the token
     * check passes and the offset check is the one that must refuse it. The
     * reply is the first thing on the 0 -> 1 stream, so it arrives before any
     * reply this lead's own requests could get. */
    enum { FORGED_TOKEN = 0x77 };
    if (bounds && node == 1 && lead)
        atomic_store_explicit(&slot->armed_token, FORGED_TOKEN, memory_order_release);
    if (forge && (strcmp(forge, "get-reply") == 0 || bounds) && node == 0 && lead) {
        unsigned char forged[64];
        memset(forged, 0xEE, sizeof forged);
        struct osoe_am_get_reply lie = { .origin_pe = peer_lead, .chunk_offset = HEAP + 32,
                                         .total = 0xffffffffu,
                                         .token = bounds ? FORGED_TOKEN : 0xdeadbeef };
        while (!post(c, OSOE_AM_GET_REPLY, &lie, sizeof lie, forged, sizeof forged))
            osoe_progress_poll(&p);
    }

    /* A put larger than the wire's ring, so it crosses in pieces. */
    struct osoe_am_put put = { .target_pe = peer_lead, .region = 0, .offset = PUT_AT };
    while (!post(c, OSOE_AM_PUT, &put, sizeof put, mine, sizeof mine))
        osoe_progress_poll(&p);

    /* A get larger than one AM chunk, so its reply is drained over several
     * passes and the descriptor survives partial draining (§3.8). */
    /* Armed before the request, as transport-am.c's arm() does: the lead
     * refuses a reply for any other token (§3.4's trust boundary). */
    if (!bounds)
        atomic_store_explicit(&slot->armed_token, 1, memory_order_release);
    struct osoe_am_get_request request = { .target_pe = peer_lead, .origin_pe = my_pe,
                                           .region = 0, .length = GET_BYTES,
                                           .offset = GET_AT, .token = 1 };
    while (!post(c, OSOE_AM_GET_REQUEST, &request, sizeof request, NULL, 0))
        osoe_progress_poll(&p);

    /*
     * One outstanding operation at a time, because that is what the slot is
     * for: SERIALIZED allows one (§4.6), so a PE has one landing slot.
     */
    while (atomic_load_explicit(&slot->ready, memory_order_acquire) == 0) {
        osoe_progress_poll(&p);
        if (osoe_deadline_expired(&give_up)) {
            fprintf(stderr, "PE %u: no get reply landed\n", my_pe);
            CHECK(false);
            goto done;
        }
    }
    CHECK(atomic_load_explicit(&slot->token, memory_order_acquire) == 1);
    CHECK(atomic_load_explicit(&slot->length, memory_order_acquire) == GET_BYTES);
    for (uint32_t i = 0; i < GET_BYTES; i++)
        if (slot->data[i] != pattern(GET_AT + i)) {
            fprintf(stderr, "PE %u: get reply byte %u is 0x%02x, expected 0x%02x\n",
                    my_pe, i, slot->data[i], pattern(GET_AT + i));
            CHECK(false);
            break;
        }
    atomic_store_explicit(&slot->ready, 0, memory_order_seq_cst);

    /* A fetching AMO: applied at handler time, its value carried back on the
     * control queue (§3.8). */
    if (!bounds)
        atomic_store_explicit(&slot->armed_token, 2, memory_order_release);
    struct osoe_am_rmw_request rmw = { .target_pe = peer_lead, .origin_pe = my_pe,
                                       .region = 0, .op = OSOE_RMW_ADD, .width = 8,
                                       .offset = RMW_AT, .operand = 7, .token = 2,
                                       .flags = OSOE_TRANSPORT_FETCH };
    while (!post(c, OSOE_AM_RMW_REQUEST, &rmw, sizeof rmw, NULL, 0))
        osoe_progress_poll(&p);

    while (atomic_load_explicit(&slot->ready, memory_order_acquire) == 0) {
        osoe_progress_poll(&p);
        if (osoe_deadline_expired(&give_up)) {
            fprintf(stderr, "PE %u: no rmw reply landed\n", my_pe);
            CHECK(false);
            goto done;
        }
    }
    CHECK(atomic_load_explicit(&slot->token, memory_order_acquire) == 2);
    /* The peer's word was zero and exactly one add reached it. */
    CHECK(atomic_load_explicit(&slot->value, memory_order_acquire) == 0);

    /*
     * Flush this node's outbox before waiting on the peer's. The sibling's
     * bytes are committed to the shared buffer but only this thread sends
     * them, and each lead below stops as soon as its own inbox is satisfied -
     * so a lead that waited first could exit with its sibling's put still
     * sitting in a buffer. Both leads reach the wait below only after their
     * own side is fully sent, which is what makes the pair terminate.
     */
    while (arrivals_row()[my_pe + 1] == 0.0) {
        osoe_progress_poll(&p);
        if (osoe_deadline_expired(&give_up)) {
            fprintf(stderr, "PE %u: this node's other PE never posted\n", my_pe);
            CHECK(false);
            goto done;
        }
    }
    drain(&p, &give_up);

    /*
     * Now wait for what the peer node sent *here*: its lead's put, its
     * sibling's put, and its rmw. Each is checked in this process's own memory,
     * so neither side has to announce anything.
     */
    /*
     * Watch the **last** byte of each put, not the first. Payload is copied
     * straight to its destination as it arrives (§3.3), so a put becomes
     * visible progressively and its first byte says only that it has started.
     * Checking the first byte here read a message that was still arriving. The
     * bytes within one message do land in order, so its last byte is a sound
     * signal - and this is precisely why the specification has waits on a
     * signal variable (§4.9) rather than on the payload itself.
     */
    uint32_t last_lead = PAYLOAD - 1;
    uint32_t last_sibling = PEER_PUT_BYTES - 1;
    unsigned char want_lead = (unsigned char)((last_lead ^ 0x5a) + peer);
    unsigned char want_sibling = (unsigned char)(last_sibling + 0x30 + peer);
    uint64_t applied_rmw = 0;
    for (;;) {
        osoe_progress_poll(&p);
        memcpy(&applied_rmw, heap + RMW_AT, sizeof applied_rmw);
        if (heap[PUT_AT + last_lead] == want_lead &&
            heap[PEER_PUT_AT + last_sibling] == want_sibling && applied_rmw == 7)
            break;
        if (osoe_deadline_expired(&give_up)) {
            fprintf(stderr, "PE %u: peer traffic incomplete: put %#x/%#x sibling "
                            "%#x/%#x rmw %llu\n", my_pe, heap[PUT_AT + last_lead],
                    want_lead, heap[PEER_PUT_AT + last_sibling], want_sibling,
                    (unsigned long long)applied_rmw);
            CHECK(false);
            goto done;
        }
    }

    /* The peer lead's put landed whole, where it was addressed and nowhere
     * else. */
    for (uint32_t i = 0; i < PAYLOAD; i++)
        if (heap[PUT_AT + i] != (unsigned char)((i ^ 0x5a) + peer)) {
            fprintf(stderr, "PE %u: put byte %u is 0x%02x, expected 0x%02x\n", my_pe,
                    i, heap[PUT_AT + i], (unsigned char)((i ^ 0x5a) + peer));
            CHECK(false);
            break;
        }
    CHECK(heap[PUT_AT - 1] == pattern(PUT_AT - 1));
    CHECK(heap[PUT_AT + PAYLOAD] == pattern(PUT_AT + PAYLOAD));

    /* And the peer node's *other* PE wrote into the same buffer, from its own
     * process, so its bytes must be here and untouched too. */
    for (uint32_t i = 0; i < PEER_PUT_BYTES; i++)
        if (heap[PEER_PUT_AT + i] != (unsigned char)(i + 0x30 + peer)) {
            fprintf(stderr, "PE %u: sibling put byte %u is 0x%02x, expected 0x%02x\n",
                    my_pe, i, heap[PEER_PUT_AT + i], (unsigned char)(i + 0x30 + peer));
            CHECK(false);
            break;
        }

    /*
     * The internode phase of a barrier (§5.1, §5.3). Node 0 is the root of a
     * two-node tree: both nodes arrive, the root counts them, and the release
     * comes back and is
     * applied by the progress thread. The epoch is arbitrary here - the point
     * is that no node is released before every node has arrived, which is the
     * only thing a barrier promises.
     */
    enum { EPOCH = 11 };
    CHECK(releases == 0);
    osoe_progress_arrive(&p, 0, EPOCH);
    /*
     * No assertion about *when* the release happens here. The root's own
     * arrival may legitimately be the last one - the peer's may already have
     * been received during the polling above - so "the root has not released
     * yet" is an ordering the protocol does not promise, and asserting it
     * failed about one run in twenty. What a barrier does promise is checked
     * where it can be: shmem.barrier-am runs three nodes and §8.2's arrival
     * table, which is the only configuration where releasing too early and
     * releasing correctly differ at all.
     */
    while (releases == 0) {
        osoe_progress_poll(&p);
        if (osoe_deadline_expired(&give_up)) {
            fprintf(stderr, "PE %u: the barrier never released\n", my_pe);
            CHECK(false);
            goto done;
        }
    }
    CHECK(released_epoch == EPOCH);
    CHECK(releases == 1);

    /*
     * Keep the engine turning. The root counts the last arrival and releases
     * itself in the same call, so its own wait is satisfied while the releases
     * it enqueued for every other node are still in its outbox - and only this
     * thread sends them. In the library the agent never stops, and §3.10's
     * safety sweep submits anything sealed regardless of signalling; a test
     * that stops polling is simulating an agent that died.
     *
     * **And "keep turning until a pass moves nothing" is not enough**, which is
     * what this loop said before. Under load a pass moves nothing while the
     * wire is merely full, so the root walked away with the peer's release
     * still in its outbox and the peer hung - about one run in thirty, and only
     * ever in the full suite, where the machine is busy enough for a send to
     * come back short at the wrong moment. drain() waits on the same predicate
     * the library's own teardown uses.
     */
    drain(&p, &give_up);
    close_connections(&p, &give_up);

    /* Every applied rmw notifies; a put notifies best effort (§4.10). */
    CHECK(notifies >= 3);
    CHECK(atomic_load_explicit(&p.metrics->messages_applied, memory_order_acquire) >= 4);
    CHECK(atomic_load_explicit(&p.metrics->segments_applied, memory_order_acquire) >= 1);
    /* Acknowledgements advance: §4.6 makes a peer's acknowledgement the
     * origin's evidence that its writes are in place. */
    CHECK(osoe_progress_acked(&p, peer) > 0);

done:
    {
        unsigned hit = 0;
        for (size_t i = 0; i < sizeof landing_block.canary; i++)
            hit += landing_block.canary[i] != 0;
        if (hit) {
            fprintf(stderr, "PE %u: FORGED REPLY WROTE PAST THE LANDING ZONE: %u bytes\n",
                    my_pe, hit);
            check_fails++;
        }
    }
    osoe_wire_close(&wire);
    osoe_ring_close(&ring);
    osoe_connection_area_close(&area);
    arrivals_close();
    return CHECK_DONE();
}
