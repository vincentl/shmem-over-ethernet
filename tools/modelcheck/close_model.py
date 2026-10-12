#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""How the node leads close their connections at the end of a job (design §6.4;
phase 1 step 3).

On the step-2 wire a lead unmapped a shared object once both ends had said
they were done. On a socket the same rule has three parts, and each is a way
to lose something:

* **A lead drains, then says goodbye, then ends its stream.** §5.3's root
  releases its own node the instant the last node arrives and only *queues*
  the releases that free the others, so a lead that ended its stream first
  would strand a child that is still waiting for its release. The goodbye is a
  header-only segment with a flag, the last segment a lead sends; the end of
  stream (FIN, `shutdown(SHUT_WR)`) follows it.
* **An end of stream without a goodbye is a peer that died**, not one that
  finished: a crashed process's sockets are closed by its kernel, which sends
  the same FIN. So a lead that sees a FIN on a connection whose goodbye has not
  arrived ends the job naming the peer (§3.15). The goodbye is what makes that
  check possible, and it must therefore always precede the FIN.
* **A lead closes a socket only once the peer's FIN has arrived.** Closing
  with the peer still sending - an acknowledgement it owed, its own goodbye -
  makes the kernel answer with a reset, and the peer's send fails: a fatal on
  the node that did nothing wrong.

Why nothing is lost by refusing to send after the goodbye: after the final
barrier no lead needs anything from another but the barrier's own releases
(every quiet completed before its node arrived, so no acknowledgement is
wanted, and no get is outstanding). Acknowledgements a lead owes are sent if
it gets to them before its goodbye and may be dropped after it; the model
lets every lead send one, to the next node round, at any moment before its
goodbye.

Model: three nodes in §5.3's tree at fan-out 1 - 0 the root, 1 its child, 2
the child of 1 - so node 1 is interior and forwards the release it receives,
and node 2 depends on a message node 0 never sends it. (Four nodes at fan-out 2,
twelve channels, ran past half an hour.) Each direction of each node pair is a
FIFO channel. Each lead is one thread whose every step is one of: its members
arrive; send the next message on a connection; receive the next message on
one; begin exiting (once its members have left the barrier); close a socket.

Variants
  design          as above.
  goodbye_early   the goodbye does not wait for the drain: a lead may say
                  goodbye with a release still queued, and then may never send
                  it. Fails TERMINATES: a child is never released.
  no_goodbye      the stream ends without a goodbye. Fails NO_EARLY_EOF.
  close_early     a lead closes once its own FIN is out, without waiting for
                  the peer's. Fails NO_RESET.

Two wires a pair (v0.2 step 4, Part 2; docs/design/progress-helpers-proposal.md
§3.1): a **control wire** carrying the tree's ARRIVE and RELEASE, and a **data
wire** carrying everything else - here the acknowledgements - each a stream of
its own with its own goodbye and end of stream, and each closed only once its
own peer's end of stream has arrived. Nothing a control message says depends
on a data message, so the wires close independently: the data wire may end
before the control wire has carried its last release, and the other way round.
  two_wires              as above, the design.
  two_wires_one_goodbye  the goodbye on the control wire alone, the data wire
                         ending with its end of stream: fails NO_EARLY_EOF on
                         the data wire, whose end is then a peer's death.
  two_wires_close_either a lead closes a pair's two sockets once the peer's end
                         of stream has arrived on either: fails NO_RESET, the
                         other wire's bytes still coming.

Properties
  NO_RESET      no message is sent toward, or left unread by, a socket its
                receiver has closed.
  NO_EARLY_EOF  no lead receives a FIN on a connection before that
                connection's goodbye.
  TERMINATES    every lead closes every socket (the trapped-state check).

Wires are numbered: one wire, 0; two, CONTROL and DATA.

Usage: close_model.py [substring ...]
"""
import sys

from engine import assemble, explore, report, select

CAPACITY = 2            # messages a channel holds; a sender waits while it is full

# The tree, set for each case (use_topology): three nodes - node 1 interior,
# forwarding the release it receives - for one wire a pair; for two, a pair of
# nodes, root and child, since twelve channels at three nodes ran past the
# half-hour bound even with one acknowledgement (9.1 million states). What two
# wires add is each wire closing on its own, which a pair shows; an interior
# node's forwarding during the close is the one-wire cases'.
TOPOLOGIES = {
    'three': ((0, 1, 2), {0: None, 1: 0, 2: 1}, {0: (1,), 1: (2,), 2: ()}),
    'pair': ((0, 1), {0: None, 1: 0}, {0: (1,), 1: ()}),
}
NODES, PARENT, CHILDREN = TOPOLOGIES['three']

ACK, ARRIVE, RELEASE, GOODBYE, FIN = 1, 2, 3, 4, 5
NAME = {ACK: 'ack', ARRIVE: 'ARRIVE', RELEASE: 'RELEASE', GOODBYE: 'goodbye', FIN: 'FIN'}
CONTROL, DATA = 0, 1
WIRE_NAME = {CONTROL: 'control', DATA: 'data'}


def Q(x, y, w, i):     return ('$q', x, y, w, i)       # wire w's channel x -> y, slot i
def QLEN(x, y, w):     return ('$qlen', x, y, w)
def OWN(x):            return ('$own', x)              # this node's members have arrived
def CHILD_IN(x, c):    return ('$child_in', x, c)      # child c's ARRIVE has arrived at x
def LEFT(x):           return ('$left', x)             # x's members have left the barrier
def OWES(x, y, m):     return ('$owes', x, y, m)       # message m queued at x for y
def ACK_LEFT(x, y):    return ('$ack_left', x, y)      # acknowledgements x may still send y
def EXITING(x):        return ('$exiting', x)
def BYE(x, y, w):      return ('$bye', x, y, w)        # x has sent y its goodbye on wire w
def FINNED(x, y, w):   return ('$fin', x, y, w)        # x has sent y its FIN on wire w
def HEARD_BYE(x, y, w):return ('$heard_bye', x, y, w)  # x has received y's goodbye on w
def EOF(x, y, w):      return ('$eof', x, y, w)        # x has received y's FIN on w
def CLOSED(x, y, w):   return ('$closed', x, y, w)     # x has closed its socket to y on w


def wires_of(variant):
    return (CONTROL, DATA) if variant.startswith('two_wires') else (0,)


def wire_label(variant, w):
    return f' ({WIRE_NAME[w]})' if len(wires_of(variant)) > 1 else ''


def peers(x):
    return tuple(y for y in NODES if y != x)


def close_epoch_if_complete(M, x, upd):
    """§5.3: a node closes the epoch once its own members and every child are in:
    the root releases (itself at once, its children by queued messages), anyone
    else queues ARRIVE to its parent."""
    own = upd.get(OWN(x), M[OWN(x)])
    kids = all(upd.get(CHILD_IN(x, c), M[CHILD_IN(x, c)]) for c in CHILDREN[x])
    if not (own and kids):
        return
    if PARENT[x] is None:
        upd[LEFT(x)] = 1
        for c in CHILDREN[x]:
            upd[OWES(x, c, RELEASE)] = 1
    else:
        upd[OWES(x, PARENT[x], ARRIVE)] = 1


def lead(variant, x):
    wires = wires_of(variant)
    two = len(wires) > 1

    def step(M, L):
        if all(M[CLOSED(x, y, w)] for y in peers(x) for w in wires):
            return [({}, {'finished': 1}, f'node {x}: every socket closed')]
        out = []
        # Its members arrive at the final barrier (the intranode phase is
        # arrival_model.py's; here it is one step).
        if not M[OWN(x)]:
            upd = {OWN(x): 1}
            close_epoch_if_complete(M, x, upd)
            out.append((upd, {}, f'node {x}: members arrive'))
        # Begin exiting once its members have left: shmem_finalize has run the
        # barrier and stopped the agent, whose exit is what follows.
        if M[LEFT(x)] and not M[EXITING(x)]:
            out.append(({EXITING(x): 1}, {}, f'node {x}: begins its exit'))
        for y in peers(x):
            for w in wires:
                n = M[QLEN(x, y, w)]
                # What x would put on wire w to y next, in the order the engine
                # sends it: queued collective messages - the control wire's,
                # with two - an acknowledgement - the data wire's - then,
                # exiting and drained, the goodbye, then the FIN.
                queued = [m for m in (ARRIVE, RELEASE) if M[OWES(x, y, m)]] \
                    if not two or w == CONTROL else []
                send = None
                if queued:
                    send = queued[0]
                elif M[ACK_LEFT(x, y)] and not M[BYE(x, y, w)] and (not two or w == DATA):
                    send = ACK            # optional; offered alongside what follows
                says_goodbye = variant != 'no_goodbye' and \
                    not (variant == 'two_wires_one_goodbye' and w == DATA)
                if M[EXITING(x)] and not M[BYE(x, y, w)] and says_goodbye:
                    drained = not queued or variant == 'goodbye_early'
                    if drained:
                        out.append(send_step(M, x, y, w, n, GOODBYE, variant))
                # The FIN follows the goodbye; nothing follows the FIN.
                if M[EXITING(x)] and not M[FINNED(x, y, w)] and not queued and \
                        (M[BYE(x, y, w)] or not says_goodbye):
                    out.append(send_step(M, x, y, w, n, FIN, variant))
                if send is not None and not M[BYE(x, y, w)] and not M[FINNED(x, y, w)]:
                    out.append(send_step(M, x, y, w, n, send, variant))
                # Receive the next message from y, unless x has closed that socket.
                if M[QLEN(y, x, w)] and not M[CLOSED(x, y, w)]:
                    out.append(receive_step(M, x, y, w, variant))
                # Close the socket to y: once the peer's end of stream has
                # arrived on this wire - or, broken, on either.
                peer_ended = M[EOF(x, y, w)] or variant == 'close_early' or \
                    (variant == 'two_wires_close_either' and any(M[EOF(x, y, v)] for v in wires))
                if M[FINNED(x, y, w)] and peer_ended and not M[CLOSED(x, y, w)]:
                    upd = {CLOSED(x, y, w): 1}
                    out.append((upd, {}, f'node {x}: closes its socket to node {y}'
                                         f'{wire_label(variant, w)}'))
        # Each property is broken by the step that breaks it - a send toward a
        # closed socket, a close over unread bytes, a FIN received first - so
        # each alternative is checked against its own result, and the check
        # instruction runs only on a failing path instead of adding a scheduling
        # point after every step (which doubled the states).
        checked = []
        for o in out:
            if o is None:
                continue
            upd, loc, label = o
            bad = violation_index(lambda k, upd=upd: upd[k] if k in upd else M[k])
            checked.append((upd, dict(loc, bad=bad), label))
        return checked

    return assemble([('STEP', ('nd', step, '@BAD?')),
                     ('BAD?', ('br', lambda L: L['bad'], '@FAIL', '@FINISHED?')),
                     ('FAIL', ('check', lambda M, L: MESSAGES[L['bad'] - 1], '@END')),
                     ('FINISHED?', ('br', lambda L: L['finished'], '@END', '@STEP')),
                     ('END', ('done',))])


def send_step(M, x, y, w, n, m, variant):
    if n >= CAPACITY:
        return None                     # the channel is full: wait
    upd = {Q(x, y, w, n): m, QLEN(x, y, w): n + 1}
    if m in (ARRIVE, RELEASE):
        upd[OWES(x, y, m)] = 0
    elif m == ACK:
        upd[ACK_LEFT(x, y)] = M[ACK_LEFT(x, y)] - 1
    elif m == GOODBYE:
        upd[BYE(x, y, w)] = 1
    elif m == FIN:
        upd[FINNED(x, y, w)] = 1
    sent = ('$sent_to_closed', x, y, w)
    upd[sent] = 1 if M[CLOSED(y, x, w)] else M[sent]
    return (upd, {}, f'node {x} -> {y}: {NAME[m]}{wire_label(variant, w)}')


def receive_step(M, x, y, w, variant):
    n = M[QLEN(y, x, w)]
    m = M[Q(y, x, w, 0)]
    upd = {QLEN(y, x, w): n - 1}
    for i in range(n - 1):
        upd[Q(y, x, w, i)] = M[Q(y, x, w, i + 1)]
    upd[Q(y, x, w, n - 1)] = 0
    if m == ARRIVE:
        c = CHILDREN[x].index(y)
        upd[CHILD_IN(x, CHILDREN[x][c])] = 1
        close_epoch_if_complete(M, x, upd)
    elif m == RELEASE:
        upd[LEFT(x)] = 1
        for c in CHILDREN[x]:           # forward first (§5.3), then the members leave
            upd[OWES(x, c, RELEASE)] = 1
    elif m == GOODBYE:
        upd[HEARD_BYE(x, y, w)] = 1
    elif m == FIN:
        upd[EOF(x, y, w)] = 1
        upd[('$early_eof', x, y, w)] = 0 if M[HEARD_BYE(x, y, w)] else 1
    return (upd, {}, f'node {x} <- {y}: {NAME[m]}{wire_label(variant, w)}')


MESSAGES = []


def use_topology(variant):
    """Set the tree for `variant` (TOPOLOGIES), and the violations' messages."""
    global NODES, PARENT, CHILDREN
    NODES, PARENT, CHILDREN = TOPOLOGIES['pair' if len(wires_of(variant)) > 1 else 'three']
    MESSAGES.clear()
    for x in NODES:
        for y in NODES:
            if x != y:
                for w in (CONTROL, DATA):
                    on = f' on wire {w}'
                    MESSAGES.extend([
                        f'NO_EARLY_EOF: node {x} received node {y}\'s FIN before its goodbye{on}, '
                        f'and would end the job as if node {y} had died',
                        f'NO_RESET: node {x} sent to node {y} after node {y} closed that socket{on}',
                        f'NO_RESET: node {y} closed its socket{on} with node {x}\'s bytes unread'])


def violation_index(get):
    """0 if the state `get` reads is sound, else 1 + the index of its message."""
    i = 0
    for x in NODES:
        for y in peers(x):
            for w in (CONTROL, DATA):
                if get(('$early_eof', x, y, w)):
                    return i + 1
                if get(('$sent_to_closed', x, y, w)):
                    return i + 2
                if get(CLOSED(y, x, w)) and get(QLEN(x, y, w)):
                    return i + 3
                i += 3
    return 0


def case(variant):
    use_topology(variant)
    mem = {}
    for x in NODES:
        mem[OWN(x)] = mem[LEFT(x)] = mem[EXITING(x)] = 0
        for c in CHILDREN[x]:
            mem[CHILD_IN(x, c)] = 0
        for y in peers(x):
            # One acknowledgement per node, to the next node round, at any moment
            # before its goodbye: enough for each lead to have one in flight
            # toward a peer that may already be exiting. More only multiplies
            # interleavings; a goodbye and a FIN already cross every closing.
            mem[ACK_LEFT(x, y)] = 1 if y == (x + 1) % len(NODES) else 0
            for m in (ARRIVE, RELEASE):
                mem[OWES(x, y, m)] = 0
            for w in (CONTROL, DATA):
                mem[QLEN(x, y, w)] = 0
                for i in range(CAPACITY):
                    mem[Q(x, y, w, i)] = 0
                for k in (BYE, FINNED, HEARD_BYE, EOF, CLOSED):
                    mem[k(x, y, w)] = 0
                mem[('$early_eof', x, y, w)] = 0
                mem[('$sent_to_closed', x, y, w)] = 0
    threads = [lead(variant, x) for x in NODES]
    return f'close/{variant}', threads, mem


def run(variant, out=sys.stdout):
    name, threads, mem = case(variant)
    wires = wires_of(variant)
    return explore(name, threads, mem, out=out,
                   done=lambda M: all(M[CLOSED(x, y, w)] for x in NODES for y in peers(x)
                                      for w in wires),
                   liveness=True, max_seconds=1800, liveness_max_states=12_000_000)


CASES = [
    ('design', True),
    ('goodbye_early', 'STUCK'),
    ('no_goodbye', 'NO_EARLY_EOF'),
    ('close_early', 'NO_RESET'),
    ('two_wires', True),
    ('two_wires_one_goodbye', 'NO_EARLY_EOF'),
    ('two_wires_close_either', 'NO_RESET'),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for variant, expect in CASES:
        name, _, _ = case(variant)
        if not keep(name):
            continue
        ok &= report(run(variant), expect=expect, tso=False)
    sys.exit(0 if ok else 1)
