/* SPDX-License-Identifier: LGPL-3.0-or-later */
/*
 * Detecting a program's own deadlock, by proof (§3.15; the author's decision of
 * 2026-09-27; tools/modelcheck/deadlock_model.py).
 *
 * A job is deadlocked when every PE waits inside the library, nothing any node
 * sent is still in flight and nothing waits to be sent: only a library
 * operation satisfies a SHMEM wait, and none is left to happen. No wait gives
 * up because time has passed (§3.15); this ends a job proved unable to move.
 *
 * What a node's lead can see. Each PE counts, in its slot's `waits` word, its
 * threads asleep in library waits and every change in that number
 * (base/sweep.h): a thread counts from just before its wait first sleeps
 * until the wait ends, and in between does nothing anyone could see. /proc
 * lists the PE's threads, the agent's and io_uring's workers' aside, and says
 * whether each sleeps - a thread woken and not yet run does not. The engine
 * says whether the node owes the network anything, and whether every segment
 * it sent that carried application work has been applied by its peer
 * (transport.h's node_quiet, which asks where a peer has yet to say). The
 * node's *activity* is the changes in its PEs' words and the application
 * messages its engine applied, cumulative.
 *
 * A node's *sample* is two halves on two turns of its lead's agent, a pass of
 * the engine between them. The first reads the activity, then /proc, and asks
 * that every PE has as many threads asleep in waits as it has threads; the
 * pass takes whatever the PEs posted before they began to wait, which the
 * engine cannot see until it has (on AM, a collective's arrivals); the second
 * reads /proc, then asks that the engine is quiet, then reads the activity,
 * and asks that nothing moved. So the interval from the first read to the last
 * holds no change: a thread that did anything within it was counted by its
 * word, or seen running by /proc.
 *
 * The root, node 0's lead, runs rounds of two collections, once its own node
 * has looked quiet for a second: it samples itself and asks every other node
 * for a sample; if every node was quiet, it waits OSOE_DEADLOCK_SECONDS -
 * giving up at once if its own node moves - then samples itself again and
 * asks again; if every node was quiet again with its activity unmoved since
 * its first sample began, the job is deadlocked. With t* the end of the first
 * collection, every node's two samples bracket t* and nothing happened at any
 * node between them; a message in flight at t* was sent before its sender's
 * first sample ended, so was acknowledged by then - applied before t*, which
 * is a contradiction. So at t* every PE waited, nothing was in flight and
 * nothing queued: a deadlock, which lasts. And a condition that came true with
 * no wake - a store through shmem_ptr, a wake lost - is found by its waiter
 * within a sweep interval, far inside OSOE_DEADLOCK_SECONDS, which ends its
 * wait and moves the activity. The model checks the argument, and that each
 * ingredient is needed: activity without applied messages, quiet without
 * acknowledgements or without the idle check, two halves with no pass between,
 * a wait counted from the start of a routine that posts, one collection
 * instead of two, or quiet without the PEs' waits, each declares a job that
 * could still move.
 *
 * Then every node prints where its PEs wait, and the job ends with status 1.
 * A PE with a thread the library cannot see waiting - one computing, one in a
 * condition variable, an idle OpenMP pool - is not blocked, and neither is a
 * stopped process: such a job is never ended, only not helped. Nor can the
 * detector see what reaches a PE from outside the library, which no SHMEM
 * program relies on: a signal handler's store, another program's write to
 * memory it shares with a PE. The cost is nothing while every node's PEs are
 * not all waiting; at the root, a look at its own PEs' words each sweep
 * interval; and once the root's node has looked quiet for a second, a round:
 * a message to and from each node and a sample of each, whose /proc reads are
 * taken only where every PE has a thread waiting, and which waits at most 5 ms
 * for a thread to fall asleep. After each round that fails the root waits
 * twice as long before the next, up to OSOE_DEADLOCK_SECONDS, until its node
 * is seen to move.
 */
#ifndef OSOE_DEADLOCK_H
#define OSOE_DEADLOCK_H

#include <stdbool.h>
#include <stdint.h>

enum osoe_deadlock_kind {
    OSOE_DEADLOCK_PROBE = 1,        /* the root asks for a sample */
    OSOE_DEADLOCK_REPORT,           /* a node's sample: quiet, and at a second probe unmoved */
    OSOE_DEADLOCK_DECLARE,          /* the root found the job deadlocked: say where */
    OSOE_DEADLOCK_DECLARED,         /* a node has said where its PEs wait */
};

/* A detector message between two nodes' leads; the backend carries it. */
struct osoe_deadlock_message {
    uint32_t round;
    uint16_t kind;                  /* enum osoe_deadlock_kind */
    uint16_t phase;                 /* a probe's or a report's collection: 1 or 2 */
    uint32_t ok;                    /* a report's verdict */
    uint32_t reserved;
};

/* Read OSOE_DEADLOCK_SECONDS: 0 turns detection off; the default is 10. */
void osoe_deadlock_init(void);

/* The lead's agent, every turn of its loop, before its pass: the samples, and
 * at the root the rounds. True while a sample is half taken, when the agent
 * goes round again without sleeping. */
bool osoe_deadlock_tick(void);

/* The lead's agent, when a detector message from `node`'s lead arrives. */
void osoe_deadlock_message(uint32_t node, const struct osoe_deadlock_message *m);

#endif /* OSOE_DEADLOCK_H */
