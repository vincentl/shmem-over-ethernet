/*
 * The per-node connection area across processes (§3.4, §3.5, §3.12).
 *
 * The local lead creates the area and every local PE opens it, which is the
 * arrangement §3.12 describes: writers are application threads in several
 * processes, the reaper is the lead's agent in another. What this checks is
 * that they agree on one mapping and that the §3.5 protocol works across it -
 * a message reserved by one PE is readable, intact and exactly once, by the
 * lead.
 *
 * It is also where the area's publication is tested. The name appears at
 * shm_open, before ftruncate has sized the object and before the connections
 * have been initialized; an opener that trusted the name would map an object
 * whose buffers still read as unsealed and empty - two reservable buffers,
 * which §3.5 forbids - and reserve into them. The wire learned this by failing
 * a preset after its commit was green, so the area had the `ready` word from
 * the start.
 *
 * It does not call shmem_init: a job of more than one logical node is still
 * refused until the backend lands, and the area needs only the bootstrap
 * environment osoe-launch already sets.
 */
#define _GNU_SOURCE
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "base/deadline.h"
#include "bootstrap.h"
#include "check.h"
#include "connection-area.h"

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

enum { NODES = 3 };     /* this node plus two peers, so two connections */

static uint32_t env_u32(const char *name)
{
    const char *v = getenv(name);
    if (!v) {
        fprintf(stderr, "%s is not set; run this under osoe-launch\n", name);
        exit(1);
    }
    return (uint32_t)strtoul(v, NULL, 10);
}

struct payload {
    uint32_t pe;
    uint32_t peer;
    uint64_t seq;
};

int main(void)
{
    /* How many PEs share this node: the length of OSOE_GTIDS (§6.2). The area
     * needs it too - the last of them to open it removes its name (§3.14). */
    uint32_t local_count = 1;
    for (const char *g = getenv("OSOE_GTIDS"); g && *g; g++)
        if (*g == ',')
            local_count++;

    struct osoe_bootstrap b = {
        .job = strtoull(getenv("OSOE_JOB_ID") ? getenv("OSOE_JOB_ID") : "0", NULL, 10),
        .step = env_u32("OSOE_STEP_ID"),
        .node = env_u32("OSOE_NODEID"),
        .local_id = env_u32("OSOE_LOCALID"),
        .rank = env_u32("OSOE_PROCID"),
        .local_count = local_count,
        .no_progress_ns = UINT64_C(30000000000),  /* fail rather than hang */
    };

    /*
     * Every PE of this node opens the same area. The node count is a job
     * property that the global rendezvous would supply; here it is a constant,
     * since this test is about the area and not about bootstrap.
     */
    struct osoe_connection_area area;
    osoe_connection_area_open(&area, &b, NODES, 0);

    /* One connection per peer node, and none to itself. */
    CHECK(osoe_connection_to(&area, b.node) == NULL);
    uint32_t peers = 0;
    for (uint32_t n = 0; n < NODES; n++)
        if (n != b.node) {
            struct osoe_connection *c = osoe_connection_to(&area, n);
            CHECK(c != NULL);
            if (c) {
                CHECK(c->capacity == area.buffer_bytes);
                peers++;
            }
        }
    CHECK(peers == NODES - 1);

    /* Distinct connections get distinct buffers: an indexing slip that aliased
     * two peers would otherwise be invisible until traffic crossed. */
    if (NODES >= 3) {
        struct osoe_connection *a = osoe_connection_to(&area, (b.node + 1) % NODES);
        struct osoe_connection *c = osoe_connection_to(&area, (b.node + 2) % NODES);
        CHECK(a && c && a != c);
        if (a && c) {
            CHECK(a->s != c->s);
            CHECK(a->data != c->data);
        }
    }

    /*
     * Every buffer must arrive sealed. Only the lead can check this without a
     * race - it is the one PE that knows nothing has opened a buffer yet,
     * because it is the thread that will - and it is the check the `ready`
     * word exists for: a PE mapping the area before the lead initialized it
     * would find both buffers unsealed and reservable, which §3.5 forbids.
     */
    uint32_t len = osoe_message_padded(sizeof(struct payload));
    if (b.local_id == 0) {
        for (uint32_t n = 0; n < NODES; n++) {
            if (n == b.node)
                continue;
            struct osoe_connection *c = osoe_connection_to(&area, n);
            struct osoe_reservation r;
            CHECK(c && osoe_connection_reserve(c, len, &r) == OSOE_RESERVATION_NONE);
        }
        /* Now play the agent: open one buffer per connection (§3.12). */
        for (uint32_t n = 0; n < NODES; n++)
            if (n != b.node)
                osoe_connection_open(osoe_connection_to(&area, n), 0, 1000 + n);
    }

    /*
     * Every PE of the node writes one message into each connection. They are
     * in different processes writing into one mapping, which is the whole
     * arrangement §3.12 describes and the thing this test exists to exercise.
     */
    struct osoe_deadline give_up = osoe_deadline_in(UINT64_C(20000000000));
    for (uint32_t n = 0; n < NODES; n++) {
        if (n == b.node)
            continue;
        struct osoe_connection *c = osoe_connection_to(&area, n);
        struct osoe_reservation r;
        for (;;) {
            enum osoe_reservation_status st = osoe_connection_reserve(c, len, &r);
            if (st == OSOE_RESERVATION_OK)
                break;
            if (osoe_deadline_expired(&give_up)) {
                fprintf(stderr, "PE %u: no reservation on the connection to node %u\n",
                        b.rank, n);
                CHECK(false);
                osoe_connection_area_close(&area);
                return CHECK_DONE();
            }
            sched_yield();
        }
        CHECK(r.seq == 1000 + n);           /* the seq this PE's message goes out in */
        unsigned char *at = osoe_reservation_bytes(c, &r);
        struct osoe_message_header mh = { (uint16_t)b.local_id, 0,
                                      (uint32_t)sizeof(struct payload) };
        struct payload p = { b.rank, n, r.seq };
        memcpy(at, &mh, sizeof mh);
        memcpy(at + sizeof mh, &p, sizeof p);
        osoe_connection_commit(c, &r);
    }

    /*
     * The lead reads them back. It waits for every local PE to have reserved -
     * `head` says how many - then seals and waits for sendable, which is only
     * true once every one of them has committed (§3.5).
     */
    if (b.local_id == 0) {
        uint32_t want = (uint32_t)sizeof(struct osoe_am_segment_header) + local_count * len;
        for (uint32_t n = 0; n < NODES; n++) {
            if (n == b.node)
                continue;
            struct osoe_connection *c = osoe_connection_to(&area, n);
            while (OSOE_BUFFER_USED(atomic_load_explicit(&c->s->buffer[0].head,
                                                      memory_order_acquire)) < want) {
                if (osoe_deadline_expired(&give_up)) {
                    fprintf(stderr, "lead: only %u of %u bytes reserved toward node %u\n",
                            OSOE_BUFFER_USED(atomic_load_explicit(&c->s->buffer[0].head,
                                                               memory_order_acquire)),
                            want, n);
                    CHECK(false);
                    osoe_connection_area_close(&area);
                    return CHECK_DONE();
                }
                sched_yield();
            }
            osoe_connection_seal(c, 0);
            uint32_t bytes = 0;
            while (!osoe_connection_sendable(c, 0, &bytes)) {
                if (osoe_deadline_expired(&give_up)) {
                    fprintf(stderr, "lead: connection to node %u never became sendable\n", n);
                    CHECK(false);
                    osoe_connection_area_close(&area);
                    return CHECK_DONE();
                }
                sched_yield();
            }
            CHECK(bytes == want);

            /* Exactly one message from each local PE, intact. */
            bool seen[64] = { false };
            uint32_t off = (uint32_t)sizeof(struct osoe_am_segment_header), count = 0;
            while (off < bytes) {
                struct osoe_message_header mh;
                struct payload p;
                memcpy(&mh, c->data + off, sizeof mh);
                memcpy(&p, c->data + off + sizeof mh, sizeof p);
                CHECK(mh.len == sizeof p);
                CHECK(p.peer == n);
                CHECK(p.seq == 1000 + n);
                CHECK(mh.kind < local_count && mh.kind < 64);
                if (mh.kind < 64) {
                    CHECK(!seen[mh.kind]);
                    seen[mh.kind] = true;
                }
                off += osoe_message_padded(mh.len);
                count++;
            }
            CHECK(count == local_count);
        }
    }

    osoe_connection_area_close(&area);
    return CHECK_DONE();
}
