/* SPDX-License-Identifier: LGPL-3.0-or-later */
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
 * **Callers part-way through the hello are kept side by side** (the round-three
 * review's N2): an accepted connection's hello is read within 0.1 s, as before -
 * safe, since an unanswered caller retries - and a caller that has been answered
 * waits in a small set, polled with the listening socket, for its
 * confirmation, with no time limit: a caller that is answered writes its
 * confirmation and carries on, and one dropped for time would end the job. One
 * silent caller no longer holds the loop. With OSOE_JOB_SECRET, nothing a hello
 * claims is acted on - a count that differs, a node out of range - until its
 * caller has proved the secret; without one nothing can be proved, and the
 * hello is judged as it arrives.
 *
 * Every wait here sweeps (§4.10) and gives up only by the no-progress rule
 * (§3.15): a peer that has not started yet is waited for as long as it takes.
 * Each says what it waits for once it has lasted 30 s, and every minute after:
 * the node, its host list entry, the address and port dialled and the last
 * error, or the nodes it waits to be called by (base/stall-watch.h).
 */
#ifndef OSOE_LISTENER_H
#define OSOE_LISTENER_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "bootstrap.h"

/* What a connection is for; a lead accepts the purposes it is waiting for and
 * tells a caller wanting another to come back. A node pair is joined by two
 * connections, a control wire and a data wire (§3.1; the progress helpers
 * proposal's §3.1), which the hello tells apart. */
enum osoe_hello_purpose {
    OSOE_HELLO_DATA_WIRE = 1,   /* a node pair's data wire (§3.1) */
    OSOE_HELLO_RENDEZVOUS,      /* a child of the rendezvous tree (§6.2) */
    OSOE_HELLO_CONTROL_WIRE,    /* a node pair's control wire */
};

/* A set of purposes, a bit each, for osoe_listener_accept. */
#define OSOE_HELLO_PURPOSE(purpose) (UINT32_C(1) << (purpose))

/* A caller part-way through the hello (listener.c). */
struct osoe_listener_caller;

/* The callers a listener keeps at once, part-way through the hello; a full
 * set accepts no more until one finishes, and the rest wait in the kernel's
 * backlog, as a caller of a lead that is busy waits. */
enum { OSOE_LISTENER_CALLERS = 16 };

struct osoe_listener {
    int fd;
    uint16_t port;              /* the candidate this lead bound */
    uint32_t node;
    uint32_t session;
    const struct osoe_bootstrap *bootstrap;
    struct osoe_listener_caller *callers;   /* OSOE_LISTENER_CALLERS of them */
};

/*
 * Bind this lead's listener on `address` (NULL: every address): the first
 * candidate port of (job, step, node) that is free. Fatal if none of the
 * candidates is (a host using every one of them is misconfigured, and the
 * message names the range).
 */
void osoe_listener_open(struct osoe_listener *l, const struct osoe_bootstrap *b,
                        uint32_t session, const struct sockaddr_in *address);

/*
 * The same, on a port the kernel chooses rather than the port rule's: for a
 * listener whose port is told to the callers rather than found by them - the
 * node-pair mesh's, whose port rides in the node's backend blob (§6.2).
 */
void osoe_listener_open_any_port(struct osoe_listener *l, const struct osoe_bootstrap *b,
                                 uint32_t session, const struct sockaddr_in *address);
void osoe_listener_close(struct osoe_listener *l);

/*
 * Accept the next connection from this job and session for one of `purposes`
 * (OSOE_HELLO_PURPOSE), and return its socket, with the caller's node in
 * `peer` and, unless NULL, its purpose in `purpose`. Answers every other
 * caller - another job, another node of this job looking for someone else, a
 * purpose not in the set - and keeps waiting. `expecting` says whom it waits
 * for, for its notice: "node 5", "nodes 1 and 3 of the mesh".
 */
int osoe_listener_accept(struct osoe_listener *l, uint32_t purposes, uint32_t *peer,
                         enum osoe_hello_purpose *purpose, const char *expecting);

/*
 * Node `node`'s address from the host list: OSOE_STEP_NODELIST, which mirrors
 * SLURM_STEP_NODELIST (§6.1), resolved to IPv4, with no port. Fatal naming the
 * entry if the list or the name is bad.
 */
void osoe_listener_node_address(const struct osoe_bootstrap *b, uint32_t node,
                                struct sockaddr_in *out);

/*
 * This node's own address, to listen at and to tell its peers (§6.2): its host
 * list entry, resolved - unless that entry is a name this host resolves to a
 * loopback address, as Debian's /etc/hosts does a host's own name (127.0.1.1),
 * in a job of several nodes, which could never reach it there. Then the address
 * this host would send from toward another node of the job. An IP literal is
 * taken as given: osoe-launch's logical nodes are 127.0.0.[1-N] on purpose.
 */
void osoe_listener_own_address(const struct osoe_bootstrap *b, struct sockaddr_in *out);

/* The rule osoe_listener_own_address applies: the host list's `entry` for this
 * node, `resolved` as this host resolves it, is a name, not an IP literal, that
 * lands on 127.0.0.0/8 in a job of several nodes. */
static inline bool osoe_listener_self_is_loopback(const char *entry,
                                                  const struct sockaddr_in *resolved,
                                                  uint32_t node_count)
{
    struct in_addr literal;
    return node_count > 1 && inet_pton(AF_INET, entry, &literal) != 1 &&
           (ntohl(resolved->sin_addr.s_addr) >> 24) == 127;
}

/*
 * Connect to node `node`'s listener at `host` (whose port is ignored), trying
 * its candidates in order until one answers as that node of this job and
 * session, for `purpose`. Retries the sequence with backoff while nobody has
 * answered, which is how a caller waits for a peer that has not started yet.
 */
int osoe_listener_connect(const struct osoe_bootstrap *b, uint32_t session,
                          const struct sockaddr_in *host, uint32_t node,
                          enum osoe_hello_purpose purpose);

/*
 * Connect to node `node`'s listener at host:port, a port the caller was told
 * rather than one it must find. The listener must answer as that node of this
 * job and session; anything else is a disagreement about the job, and fatal.
 * Retries a refused connection with backoff, as a peer may not be listening
 * yet.
 */
int osoe_listener_connect_port(const struct osoe_bootstrap *b, uint32_t session,
                               const struct sockaddr_in *host, uint16_t port, uint32_t node,
                               enum osoe_hello_purpose purpose);

/* The i-th candidate port of (job, step, node), for the tier-1 test. */
uint16_t osoe_listener_candidate(const struct osoe_bootstrap *b, uint32_t node, uint32_t i);

/*
 * Blocking reads and writes on a connection the hello has made to node `peer`,
 * for the rendezvous's messages. Each waits as long as the peer takes,
 * sweeping (§4.10), and ends the job naming the peer only by the no-progress
 * rule (§3.15); false at an end of stream or an error, which the caller names.
 */
bool osoe_listener_read(const struct osoe_bootstrap *b, int fd, void *bytes, size_t n,
                        uint32_t peer);
bool osoe_listener_write(const struct osoe_bootstrap *b, int fd, const void *bytes, size_t n,
                         uint32_t peer);

#endif /* OSOE_LISTENER_H */
