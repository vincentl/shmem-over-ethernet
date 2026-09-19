/*
 * The node-pair wire (§3.1, §3.10, §7.1 step 2).
 *
 * §7.1 step 2 runs the AM layer's queues in memory rather than on sockets, so
 * that every internode semantic is validated before a socket exists. This is
 * what stands in for the socket: one shared-memory object per node pair,
 * holding a single-producer single-consumer byte ring in each direction, with a
 * futex word so the reader sleeps rather than spins. Step 3 replaces it with
 * io_uring over TCP and nothing above it changes.
 *
 * **It is a byte stream, not a message queue, and that is the whole point.**
 * osoe_wire_send accepts a prefix of what it was offered and says how much;
 * osoe_wire_recv delivers whatever has arrived, which may be a fraction of a
 * segment or several segments' worth. §3.3's receive path is written against
 * exactly that - a parse cursor per connection, headers accumulated in a small
 * per-connection area, payload copied straight to its destination as it
 * arrives - and §8.2 warns that loopback "will never produce a short write or
 * partial read", which is where the most common defect in hand-rolled TCP
 * framing lives. A wire that handed over whole segments would leave §3.3's
 * parser meeting its first partial read on real hardware in step 3. This one
 * produces them on purpose: the ring is small, and OSOE_WIRE_SPLIT truncates
 * transfers besides, deterministically (§3.14's hash-derived injection, so the
 * same seed gives the same splits and a failing case is a permanent regression
 * case rather than a story about a bad afternoon).
 *
 * Only the two node leads touch a wire (§3.12: non-lead agents never touch a
 * ring or a socket), so each direction has exactly one writer and one reader.
 * The lower node index creates the object and the higher opens it, matching
 * §6.2's rule that the lower node index connects to the higher, so each pair
 * has exactly one wire.
 *
 * The wake protocol is §4.6's, the one used for every counter a waiter blocks
 * on: the reader loads the wake word **before** the data it guards, and the
 * writer publishes the data **before** bumping the word. The kernel closes what
 * remains, as §3.10 says of the io_uring form - futex_wait compares the word,
 * so a bump between the reader's load and its wait returns at once instead of
 * sleeping on a stale value. Reversing those two loads is a hung job, and is
 * the `wake_after` case in docs/design/evidence/am-backend/wire_model.py.
 *
 * Use it the way §4.9's watch is used, and for the same reason:
 *
 *     for (;;) {
 *         uint32_t seq = osoe_wire_seq(w);       // BEFORE draining
 *         n = osoe_wire_recv(w, buffer, sizeof buffer);
 *         if (n > 0) { parse(buffer, n); continue; }
 *         osoe_wire_wait(w, seq, &sweep_deadline);
 *     }
 */
#ifndef OSOE_WIRE_H
#define OSOE_WIRE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "base/deadline.h"
#include "bootstrap.h"

/*
 * Ring capacity per direction, and the split rate, both OSOE_ (§6.2) because
 * they are this implementation's, not the specification's.
 *
 * The default capacity is deliberately far below a socket's buffer: it is
 * chosen so that a full-sized segment (§3.2 chunks at 64 KB) cannot be written
 * in one go, so every test exercises the short-write path rather than only the
 * tests that thought to.
 */
#define OSOE_WIRE_DEFAULT_BYTES  (UINT64_C(16) << 10)
#define OSOE_WIRE_MIN_BYTES      UINT64_C(64)
#define OSOE_WIRE_MAX_BYTES      (UINT64_C(64) << 20)

/* Process-private handle. */
struct osoe_wire {
    char name[96];
    void *map;                  /* the whole shared object */
    uint64_t bytes;             /* its mapped length */
    struct osoe_wire_direction *tx;   /* this node writes here */
    struct osoe_wire_direction *rx;   /* and reads here */
    uint64_t capacity;               /* bytes per direction */
    uint64_t seed;              /* split injection, derived from job and step */
    uint32_t split;             /* truncations per myriad; 0 disables */
    uint64_t sends;             /* this process's transfers, for the seed */
    uint64_t recvs;
    bool creator;               /* this node created it, so it removes the name */
};

/*
 * Open the wire between this node and `peer`, creating it if this node's index
 * is the lower of the two. Fatal on failure (§3.15: a transport failure aborts
 * the job).
 */
void osoe_wire_open(struct osoe_wire *w, const struct osoe_bootstrap *b, uint32_t peer,
                    uint32_t session);

/* Unmap; the creating node also removes the name. */
void osoe_wire_close(struct osoe_wire *w);

/*
 * Write up to len bytes. Returns how many were accepted, which may be 0 and is
 * often less than len: a short write is the normal case, not an error. The
 * caller keeps the remainder and offers it again, exactly as §3.3 says of a
 * short send on a real socket ("advance past a short send and resubmit the
 * remainder").
 */
size_t osoe_wire_send(struct osoe_wire *w, const void *buffer, size_t len);

/* Read up to len bytes. Returns how many were delivered, which may be 0. */
size_t osoe_wire_recv(struct osoe_wire *w, void *buffer, size_t len);

/*
 * The receive side's wake value. Load it BEFORE draining, and pass it to
 * osoe_wire_wait: that ordering is what makes the wait safe (§4.6).
 */
uint32_t osoe_wire_seq(const struct osoe_wire *w);

/*
 * Sleep until the peer writes, or until the deadline. Returns false on a
 * deadline expiry, which the caller counts and treats as a safety sweep
 * (§3.10): every blocking call here sweeps at least every OSOE_SWEEP_NS,
 * whether or not anything arrives.
 */
bool osoe_wire_wait(struct osoe_wire *w, uint32_t seq, const struct osoe_deadline *deadline);

/* Bytes this direction can hold, for a caller sizing its own staging. */
uint64_t osoe_wire_capacity(const struct osoe_wire *w);

#endif /* OSOE_WIRE_H */
