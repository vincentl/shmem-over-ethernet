/*
 * A node lead's listening socket, the port rule by which another lead finds
 * it, and the hello that says who is on each end of a connection (§6.2; phase
 * 1 step 3).
 *
 * **Only node leads listen and connect** (§3.12, invariant 25). Each binds the
 * first free port of a candidate sequence hashed from (job, step, node) inside
 * OSOE_PORT_RANGE, and another lead finds it by trying the same sequence in the
 * same order and asking whoever answers who it is. §6.2 hashed the job and step
 * alone, which finds one server per job; with the node in the hash every lead
 * has a listener another can find, which is what a tree-shaped rendezvous and
 * the node-pair mesh both need, and on one host - where §8.1's logical nodes
 * all live - two leads' sequences start in different places.
 *
 * **The hello is three steps, not two**: the caller's hello, the answer, and
 * the caller's confirmation. A caller that gives up waiting for an answer - it
 * may be a stranger's port that accepted and says nothing - closes and moves
 * on; with only two steps the answering lead could not tell that from a
 * connection it now owns, and would count a peer whose socket is already
 * closed. It counts a connection only once the confirmation arrives.
 *
 * Every wait here sweeps (§4.10) and gives up only by the no-progress rule
 * (§3.15): a peer that has not started yet is waited for as long as it takes.
 */
#ifndef OSOE_LISTENER_H
#define OSOE_LISTENER_H

#include <netinet/in.h>
#include <stdint.h>

#include "bootstrap.h"

/* What a connection is for; a lead accepts one purpose at a time and tells a
 * caller wanting the other to come back. */
enum osoe_hello_purpose {
    OSOE_HELLO_MESH = 1,        /* a node-pair connection (§3.1) */
    OSOE_HELLO_RENDEZVOUS,      /* a child of the rendezvous tree (§6.2) */
};

struct osoe_listener {
    int fd;
    uint16_t port;              /* the candidate this lead bound */
    uint32_t node;
    uint32_t session;
    const struct osoe_bootstrap *bootstrap;
};

/*
 * Bind this lead's listener: the first candidate port of (job, step, node)
 * that is free. Fatal if none of the candidates is (a host using every one of
 * them is misconfigured, and the message names the range).
 */
void osoe_listener_open(struct osoe_listener *l, const struct osoe_bootstrap *b,
                        uint32_t session);
void osoe_listener_close(struct osoe_listener *l);

/*
 * Accept the next connection from this job and session for `purpose`, and
 * return its socket, with the caller's node in `peer`. Answers every other
 * caller - another job, another node of this job looking for someone else, the
 * wrong purpose - and keeps waiting.
 */
int osoe_listener_accept(struct osoe_listener *l, enum osoe_hello_purpose purpose,
                         uint32_t *peer);

/*
 * Connect to node `node`'s listener at `host` (whose port is ignored), trying
 * its candidates in order until one answers as that node of this job and
 * session, for `purpose`. Retries the sequence with backoff while nobody has
 * answered, which is how a caller waits for a peer that has not started yet.
 */
int osoe_listener_connect(const struct osoe_bootstrap *b, uint32_t session,
                          const struct sockaddr_in *host, uint32_t node,
                          enum osoe_hello_purpose purpose);

/* The i-th candidate port of (job, step, node), for the tier-1 test. */
uint16_t osoe_listener_candidate(const struct osoe_bootstrap *b, uint32_t node, uint32_t i);

#endif /* OSOE_LISTENER_H */
