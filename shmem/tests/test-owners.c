/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Owners at a helper count above none, driven from one thread (owner-map.h;
 * v0.2 step 4, Part 1; the adversarial read of Part 1, S1).
 *
 * With no helpers an owner's index of a wire is its peer slot, every
 * conversion between the two is the identity, and a line that takes one for
 * the other behaves as the right one does: no test that runs the library could
 * fail on it. The engine needs no thread to run at a helper count, though: a
 * pass is a function of an owner (osoe_progress_poll), and a thread that
 * enables several rings is the one submitter of each. So this test lays a node
 * out at OSOE_TEST_HELPERS helpers - the area, the mesh, a ring an owner, the
 * engine's owners - and turns every owner from its one thread.
 *
 * Point-to-point traffic alone, since the control plane is owner 0's and at a
 * helper count owner 0 holds no wire: from every node to every other, a put, a
 * blocking get and a fetching AMO, each checked to land on the node it names
 * with the bytes its sender wrote; and the per-connection records read by peer
 * slot, each counting what came from its own peer. Run at 2 and 3 helpers on 5
 * to 9 nodes, where index and slot differ for most wires, it fails on an index
 * taken for a slot in the mesh's attach, in the pass's wire or in the
 * connection records (the read's mutations).
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "am.h"
#include "base/deadline.h"
#include "bootstrap.h"
#include "check.h"
#include "connection-area.h"
#include "node.h"
#include "owner-map.h"
#include "progress.h"
#include "reply.h"
#include "ring.h"
#include "wire.h"

/* bootstrap.c's default fan-out is the backend's (internode.h); the tree is
 * not this test's. */
uint32_t osoe_internode_fanout_default(void);
uint32_t osoe_internode_fanout_default(void)
{
    return 4;
}

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

/* The library's notice (fatal.h), which the listener's waits print once they
 * have lasted (base/stall-watch.h): here, to stderr as it is. */
void osoe_notice(const char *fmt, ...);
void osoe_notice(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "osoe_notice: ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

enum { MAX_NODES = 16, MAX_OWNERS = 4, DESCRIPTORS = 64 };

/*
 * Each node's heap: a region a sender for the puts, the region the gets read,
 * and a word a sender for the fetching AMOs - disjoint, so that each arrival is
 * checked where its sender, and only its sender, wrote.
 */
enum { PUT_BYTES = 700, GET_AT = 16384, GET_BYTES = 1500, RMW_AT = 20480,
       HEAP = 24576 };

static uint32_t my_node;            /* and my PE: one PE a node */
static unsigned char heap[HEAP];
static struct {
    struct osoe_landing_record record;
    unsigned char zone[HEAP];
} landing;
static struct osoe_thread_record thread_slot;

static unsigned char put_byte(uint32_t from, uint32_t to, uint32_t i)
{
    return (unsigned char)(from * 31 + to * 7 + i + 1);
}

static unsigned char get_byte(uint32_t node, uint32_t i)
{
    return (unsigned char)(node * 13 + i * 3 + 5);
}

static void *resolve(void *context, uint32_t pe, uint16_t region, uint64_t offset,
                     uint32_t length)
{
    (void)context; (void)region;
    if (pe != my_node) {
        fprintf(stderr, "node %u: a message for PE %u reached it\n", my_node, pe);
        exit(1);
    }
    if (offset + length > HEAP) {
        fprintf(stderr, "node %u: offset %llu length %u is outside the heap\n", my_node,
                (unsigned long long)offset, length);
        exit(1);
    }
    return heap + offset;
}

static void notify(void *context, uint32_t pe, uint16_t region, uint64_t offset,
                   uint32_t length)
{
    (void)context; (void)pe; (void)region; (void)offset; (void)length;
}

static void release(void *context, uint32_t team, uint32_t epoch)
{
    (void)context;
    fprintf(stderr, "node %u: a release of team %u epoch %u, and this test has no team\n",
            my_node, team, epoch);
    exit(1);
}

static unsigned char *landing_request(void *context, uint32_t pe, uint64_t token,
                                      uint32_t *length)
{
    (void)context;
    struct osoe_landing_record *r = &landing.record;
    if (pe != my_node || atomic_load_explicit(&r->state, memory_order_acquire) != OSOE_LANDING_ISSUED ||
        atomic_load_explicit(&r->token, memory_order_acquire) != token)
        return NULL;
    *length = atomic_load_explicit(&r->length, memory_order_acquire);
    return landing.zone;
}

static bool completed(void *context, uint32_t pe, uint64_t token, uint32_t length,
                      uint64_t value)
{
    (void)context;
    if (pe != my_node ||
        atomic_load_explicit(&thread_slot.armed_token, memory_order_acquire) != token)
        return false;
    atomic_store_explicit(&landing.record.state, OSOE_LANDING_LANDED_BLOCKING,
                          memory_order_release);
    atomic_store_explicit(&thread_slot.length, length, memory_order_release);
    atomic_store_explicit(&thread_slot.value, value, memory_order_release);
    atomic_store_explicit(&thread_slot.ready, 1, memory_order_release);
    return true;
}

/* Arm the slot, and the landing record for a get, before its request goes. */
static void arm(uint64_t token, uint32_t length)
{
    atomic_store_explicit(&thread_slot.ready, 0, memory_order_release);
    atomic_store_explicit(&thread_slot.armed_token, token, memory_order_release);
    atomic_store_explicit(&landing.record.token, token, memory_order_release);
    atomic_store_explicit(&landing.record.length, length, memory_order_release);
    atomic_store_explicit(&landing.record.state, OSOE_LANDING_ISSUED, memory_order_release);
}

static struct osoe_progress engine;
static struct osoe_progress_owner owners[MAX_OWNERS];
static uint32_t owner_count;
static struct osoe_connection_area area;
static struct osoe_deadline give_up;

/* A turn of every owner, as its own thread would take it; a short sleep when
 * none moved, since the job's nodes share the machine's CPUs. */
static void turn(void)
{
    bool moved = false;
    for (uint32_t o = 0; o < owner_count; o++)
        moved |= osoe_progress_poll(&owners[o]);
    if (!moved) {
        struct timespec pause = { 0, 20000 };
        nanosleep(&pause, NULL);
    }
    if (osoe_deadline_expired(&give_up)) {
        fprintf(stderr, "node %u: no progress in time\n", my_node);
        exit(1);
    }
}

/* An application thread's half: reserve, copy, commit, mark (§3.5, §3.10). */
static void post(uint32_t peer, uint16_t kind, const void *fixed, uint32_t fixed_bytes,
                 const void *payload, uint32_t payload_bytes)
{
    struct osoe_connection *c = osoe_connection_to(&area, peer);
    CHECK_FATAL(c != NULL);
    struct osoe_reservation r;
    uint32_t len = osoe_message_padded(fixed_bytes + payload_bytes);
    while (osoe_connection_reserve(c, len, &r) != OSOE_RESERVATION_OK)
        turn();
    struct osoe_message_header h = { kind, 0, fixed_bytes + payload_bytes };
    unsigned char *at = osoe_reservation_bytes(c, &r);
    memcpy(at, &h, sizeof h);
    memcpy(at + sizeof h, fixed, fixed_bytes);
    if (payload_bytes)
        memcpy(at + sizeof h + fixed_bytes, payload, payload_bytes);
    osoe_connection_commit(c, &r);
    osoe_connection_area_mark(&area, peer);
}

static void await_reply(const char *what, uint32_t peer)
{
    while (atomic_load_explicit(&thread_slot.ready, memory_order_acquire) == 0) {
        if (osoe_deadline_expired(&give_up)) {
            fprintf(stderr, "node %u: no reply to its %s from node %u\n", my_node, what, peer);
            exit(1);
        }
        turn();
    }
}

int main(void)
{
    struct osoe_bootstrap b = { 0 };
    osoe_bootstrap_read(&b);
    const char *helpers_text = getenv("OSOE_TEST_HELPERS");
    uint32_t helpers = helpers_text ? (uint32_t)strtoul(helpers_text, NULL, 10) : 0;
    CHECK_FATAL(b.node_count >= 2 && b.node_count <= MAX_NODES && b.local_count == 1);
    CHECK_FATAL(helpers + 1 <= MAX_OWNERS);
    my_node = b.node;
    CHECK_FATAL(b.rank == my_node);     /* one PE a node, ranks in node order */
    uint32_t nodes = b.node_count, peers = nodes - 1;
    owner_count = helpers + 1;
    give_up = osoe_deadline_in(UINT64_C(40000000000));

    osoe_connection_area_open(&area, &b, nodes, helpers, 0, OSOE_SHM_ANY_CREATOR);

    /* The mesh, each wire on its owner's ring at its index; every ring enabled
     * here, this thread the one submitter of each (ring.h). */
    static struct osoe_wire wires[MAX_NODES];
    static struct osoe_ring rings[MAX_OWNERS];
    struct osoe_ring *ring_of[MAX_OWNERS];
    for (uint32_t o = 0; o < owner_count; o++)
        ring_of[o] = &rings[o];
    osoe_wires_open(wires, ring_of, helpers, &b, 0, NULL);
    for (uint32_t o = 0; o < owner_count; o++)
        osoe_ring_enable(&rings[o]);

    static struct osoe_progress_connection connections[MAX_NODES];
    static uint64_t busy[MAX_OWNERS];
    static struct osoe_reply descriptors[MAX_OWNERS][DESCRIPTORS];
    static struct osoe_am_connection_metrics metrics[MAX_NODES];
    struct osoe_progress_host host = { resolve, notify, release, NULL, NULL, landing_request,
                                       NULL, completed, NULL };
    osoe_progress_init(&engine, &area, owners, &host);
    for (uint32_t o = 0; o < owner_count; o++) {
        uint32_t first = osoe_owner_first(o, peers, helpers);
        osoe_progress_attach(&engine, o, &rings[o], &wires[first], &connections[first],
                             &busy[o], descriptors[o], DESCRIPTORS);
    }
    /* The connection records, by peer slot, as progress-host.c binds them. */
    for (uint32_t s = 0; s < peers; s++)
        atomic_store_explicit(&metrics[s].peer, s < my_node ? s : s + 1, memory_order_release);
    engine.connection_metrics = metrics;

    for (uint32_t i = 0; i < GET_BYTES; i++)
        heap[GET_AT + i] = get_byte(my_node, i);

    /* A put to every peer, into the region its sender owns there. */
    unsigned char payload[PUT_BYTES];
    for (uint32_t peer = 0; peer < nodes; peer++) {
        if (peer == my_node)
            continue;
        for (uint32_t i = 0; i < PUT_BYTES; i++)
            payload[i] = put_byte(my_node, peer, i);
        struct osoe_am_put put = { .target_pe = peer, .region = 0,
                                   .offset = (uint64_t)my_node * PUT_BYTES };
        post(peer, OSOE_AM_PUT, &put, sizeof put, payload, sizeof payload);
    }

    /* A blocking get from every peer, one at a time: each node's get region
     * says whose it is. */
    uint64_t token = 1;
    for (uint32_t peer = 0; peer < nodes; peer++) {
        if (peer == my_node)
            continue;
        arm(token, GET_BYTES);
        struct osoe_am_get_request request = { .target_pe = peer, .origin_pe = my_node,
                                               .region = 0, .length = GET_BYTES,
                                               .offset = GET_AT, .token = token };
        post(peer, OSOE_AM_GET_REQUEST, &request, sizeof request, NULL, 0);
        await_reply("get", peer);
        CHECK(atomic_load_explicit(&thread_slot.length, memory_order_acquire) == GET_BYTES);
        for (uint32_t i = 0; i < GET_BYTES; i++)
            if (landing.zone[i] != get_byte(peer, i)) {
                fprintf(stderr, "node %u: byte %u of its get from node %u is 0x%02x, not "
                                "0x%02x\n", my_node, i, peer, landing.zone[i],
                        get_byte(peer, i));
                CHECK(false);
                break;
            }
        token++;
    }

    /* A fetching AMO to every peer, into the word its sender owns there: its
     * old value 0, its new one the sender's number plus 1. */
    for (uint32_t peer = 0; peer < nodes; peer++) {
        if (peer == my_node)
            continue;
        uint64_t amo_token = token++ | OSOE_LANDING_NO_EXTENT;
        arm(amo_token, 0);
        struct osoe_am_rmw_request rmw = { .target_pe = peer, .origin_pe = my_node,
                                           .region = 0, .op = OSOE_RMW_ADD, .width = 8,
                                           .offset = RMW_AT + 8 * (uint64_t)my_node,
                                           .operand = my_node + 1, .token = amo_token,
                                           .flags = OSOE_TRANSPORT_FETCH };
        post(peer, OSOE_AM_RMW_REQUEST, &rmw, sizeof rmw, NULL, 0);
        await_reply("fetching AMO", peer);
        CHECK(atomic_load_explicit(&thread_slot.value, memory_order_acquire) == 0);
    }

    /*
     * What every peer sent here. Its AMO is the last thing it asks of this
     * node, after its put and its get, so once every word is in nothing more
     * will come but replies' acknowledgements, and the close may begin: it
     * says goodbye on a connection only once its replies have gone (§6.4).
     */
    for (;;) {
        bool all = true;
        for (uint32_t peer = 0; peer < nodes && all; peer++) {
            if (peer == my_node)
                continue;
            uint64_t word;
            memcpy(&word, heap + RMW_AT + 8 * peer, sizeof word);
            all = word == peer + 1 &&
                  heap[peer * PUT_BYTES + PUT_BYTES - 1] == put_byte(peer, my_node, PUT_BYTES - 1);
        }
        if (all)
            break;
        turn();
    }
    for (uint32_t peer = 0; peer < nodes; peer++) {
        if (peer == my_node)
            continue;
        for (uint32_t i = 0; i < PUT_BYTES; i++)
            if (heap[peer * PUT_BYTES + i] != put_byte(peer, my_node, i)) {
                fprintf(stderr, "node %u: byte %u of node %u's put is 0x%02x, not 0x%02x\n",
                        my_node, i, peer, heap[peer * PUT_BYTES + i],
                        put_byte(peer, my_node, i));
                CHECK(false);
                break;
            }
    }

    /* The close, every owner's connections (§6.4). */
    for (uint32_t o = 0; o < owner_count; o++)
        osoe_progress_close(&owners[o]);
    for (;;) {
        bool closed = true;
        for (uint32_t o = 0; o < owner_count; o++)
            closed &= osoe_progress_closed(&owners[o]);
        if (closed)
            break;
        turn();
    }

    /*
     * Each connection's record, by peer slot, counts what came from its own
     * peer: its put, get request and AMO request, and its replies to this
     * node's get and AMO - five messages at least. Counted into another
     * slot's record, two records would hold ten and another none.
     */
    for (uint32_t s = 0; s < peers; s++) {
        uint64_t messages = atomic_load_explicit(&metrics[s].messages_applied,
                                                 memory_order_acquire);
        uint64_t segments = atomic_load_explicit(&metrics[s].segments_applied,
                                                 memory_order_acquire);
        if (messages < 5 || segments < 1) {
            fprintf(stderr, "node %u: the record of slot %u, node %u, counts %llu messages "
                            "in %llu segments\n", my_node, s, s < my_node ? s : s + 1,
                    (unsigned long long)messages, (unsigned long long)segments);
            CHECK(false);
        }
        CHECK(osoe_progress_acked(&engine, s < my_node ? s : s + 1) > 0);
    }
    /* And each owner counted its own wires' traffic, owner 0 none at a helper
     * count. */
    for (uint32_t o = 0; o < owner_count; o++) {
        uint64_t applied = atomic_load_explicit(&owners[o].metrics->messages_applied,
                                                memory_order_acquire);
        uint32_t held = osoe_owner_wires(o, peers, helpers);
        if (held ? applied < 5 * (uint64_t)held : applied != 0) {
            fprintf(stderr, "node %u: owner %u, of %u wires, applied %llu messages\n",
                    my_node, o, held, (unsigned long long)applied);
            CHECK(false);
        }
    }

    for (uint32_t i = 0; i < peers; i++)
        osoe_wire_close(&wires[i]);
    for (uint32_t o = 0; o < owner_count; o++)
        osoe_ring_close(&rings[o]);
    osoe_connection_area_close(&area);
    osoe_bootstrap_free(&b);
    if (CHECK_DONE())
        return 1;
    printf("node %u: owners ok at %u helpers\n", my_node, helpers);
    return 0;
}
