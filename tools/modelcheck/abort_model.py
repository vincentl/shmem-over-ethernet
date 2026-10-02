#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""How an abort crosses nodes (design §3.15; phase 1 step 4).

A job ends abnormally in one of three ways: a PE calls shmem_global_exit(status),
a PE dies or calls a fatal, or a whole node dies. §9.1.6 asks, for the first,
that every PE of the job terminate "regardless of their current execution state",
and that if several PEs call it "the exit status returned to the environment
shall be one of the values passed". Within a node the abort word already stops
every wait; this model is the part between nodes.

The mesh already carries one thing: a node whose processes end has its kernel
close every socket, and §6.4 has a peer that reads an end of stream (FIN) not
preceded by a goodbye take it for a crash and end the job - with a failure
status, since a FIN carries none. That path is always there, and it is one hop
on the eager mesh. So an abort message sent any slower than that races it: a
peer that reads a dying node's FIN before the abort message reaches it exits
with a failure where a caller asked for a status.

Two designs for the abort message:
  tree      a lead that aborts sends ABORT(status) to its world-tree parent and
            children, never back to the node it heard from, once per node, and
            its processes then exit. O(k) messages a node, O(log N) hops.
  goodbye   a lead that aborts sends ABORT(status) as the last segment on every
            connection whose peer has not had its goodbye - an abortive goodbye -
            and its processes then exit. O(open connections) a node, one hop:
            the eager mesh's N, which is what a normal goodbye costs too. A peer
            reads it before the FIN it precedes, so the FIN is not a crash.
A node that learns of an abort - a caller, a fatal, an ABORT, a FIN without a
goodbye - sets its abort word once (first writer wins, and its status stays),
stops the job's own traffic, and sends as its design says.

A node's processes that exit with input unread reset their connections, and a
peer still sending to them finds that as a failed send - at once, before it has
read what arrived ahead of the reset, the ABORT among it. The goodbye design
counts an ABORT once the peer's kernel has acknowledged it, so it is there to
be read; what the peer does with its failed send decides whether it is. It
stops sending on that connection and goes on receiving until the stream ends,
and is judged there, by the ABORT or its absence (third review of step 4's
first slice).

Model: three nodes in §5.3's tree at fan-out 1 (0 the root, 1 interior, 2 its
child) on the eager mesh, so every pair has a connection; each direction is a
FIFO channel. Each lead is one thread whose steps are: its members arrive at
the final barrier; send the next message on a connection; receive the next
one; begin its exit; close a socket; and, when the case says, start an abort
(a global exit with status 1 or 2, or a fatal) or crash. A node's processes
exiting is one step: its kernel ends every stream (a FIN each, after what is
queued) and closes every socket.

Variants
  tree              the tree design. Fails STATUS: a FIN beats the ABORT.
  goodbye           the goodbye design.
  exit_first        the goodbye design, but the processes exit as soon as the
                    word is set. Fails STATUS: only FINs cross.
  closing_drops     the goodbye design, but a node already in its own close
                    sends ABORT to nobody. Passes, and that is a finding: a
                    global exit is a call, so none can start once a node's
                    members are in the final barrier, and by the time any node
                    is closing every node's are. An abort during the close is
                    a fatal's or a crash's, whose status is a failure anyway.
  resend            the goodbye design, but a node forgets what it has sent.
                    Fails ONCE.
  no_abort          no abort message at all, only FINs (step 3 as built).
                    Terminates - the FIN path always does - and fails STATUS.
  send_error_ends   the goodbye design, but a failed send ends its node with a
                    failure at once, as the engine did until the third review:
                    a node whose data to an exiting peer was never read ends on
                    the reset before reading the ABORT ahead of it. Fails
                    STATUS.

Properties
  TERMINATES  every node ends: its sockets closed after the handshake, or its
              processes exited (the trapped-state check).
  STATUS      in a case with no crash and no fatal, every node whose processes
              exit because of the abort learns a status some caller gave, never
              the failure a death leaves. The status here is the node's word,
              what it learnt; its processes then leave with it if their program
              called shmem_global_exit and with 0 if not (abort_exit_model.py,
              since the round-three review), which is a process's concern and
              not this model's.
  ONCE        no ABORT is sent twice on a connection, nor, in the tree, back
              to the node it came from.

Usage: abort_model.py [substring ...]
"""
import sys

from engine import assemble, explore, report, select

NODES = (0, 1, 2)
PARENT = {0: None, 1: 0, 2: 1}
CHILDREN = {0: (1,), 1: (2,), 2: ()}
CAPACITY = 2
FAIL = 3                # the status a fatal or a crash exits with

ARRIVE, RELEASE, GOODBYE, FIN, ABORT = 1, 2, 3, 4, 5
NAME = {ARRIVE: 'ARRIVE', RELEASE: 'RELEASE', GOODBYE: 'goodbye', FIN: 'FIN', ABORT: 'ABORT'}


def Q(x, y, i):     return ('$q', x, y, i)      # channel x -> y, slot i: kind*8 + status
def QLEN(x, y):     return ('$qlen', x, y)
def KFIN(x, y):     return ('$kfin', x, y)      # x's kernel ended the stream to y
def OWN(x):         return ('$own', x)
def CHILD_IN(x, c): return ('$child_in', x, c)
def LEFT(x):        return ('$left', x)
def OWES(x, y, m):  return ('$owes', x, y, m)
def EXITING(x):     return ('$exiting', x)
def BYE(x, y):      return ('$bye', x, y)
def FINNED(x, y):   return ('$fin', x, y)
def HEARD_BYE(x, y):return ('$heard_bye', x, y)
def EOF(x, y):      return ('$eof', x, y)
def CLOSED(x, y):   return ('$closed', x, y)
def WORD(x):        return ('$word', x)         # the abort word: 0, or the status
def FROM(x):        return ('$from', x)         # the peer x heard it from, or -1
def SENT(x, y):     return ('$sent', x, y)      # x sent y an ABORT
def SEND_FAILED(x, y): return ('$send_failed', x, y)  # x's send to y met y's reset
def ENDED(x):       return ('$ended', x)        # 1 closed, 2 exited on the abort, 3 crashed
def STARTS(x):      return ('$starts', x)       # a start of an abort still to come here


def peers(x):
    return tuple(y for y in NODES if y != x)


def tree_neighbours(x):
    return tuple(c for c in CHILDREN[x]) + (() if PARENT[x] is None else (PARENT[x],))


def close_epoch_if_complete(M, x, upd):
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


def abort_targets(variant, M, x):
    """The peers x must still send ABORT to before its processes may exit."""
    if variant == 'no_abort' or variant == 'exit_first':
        return ()
    if variant == 'closing_drops' and M[EXITING(x)]:
        return ()
    if variant == 'tree':
        wanted = tree_neighbours(x)
    else:
        wanted = peers(x)
    out = []
    for y in wanted:
        if y == M[FROM(x)] and variant == 'tree':
            continue
        if (M[SENT(x, y)] and variant != 'resend') or M[FINNED(x, y)] or M[CLOSED(x, y)]:
            continue            # already told, or this side's stream has ended
        if M[SEND_FAILED(x, y)]:
            continue            # its end reset the connection: nobody reads there
        if M[EOF(x, y)] and not M[HEARD_BYE(x, y)]:
            continue            # the peer died: nobody reads there
        if variant != 'tree' and M[BYE(x, y)]:
            continue            # it had the goodbye: this FIN is not a crash to it
        # A peer that has ended its own stream after a goodbye is closing, and
        # still reads until this side's goodbye or FIN: it is told.
        out.append(y)
    return tuple(out)


def learn(M, upd, x, status, source):
    """First writer wins: the word keeps the first status it was given."""
    if upd.get(WORD(x), M[WORD(x)]) == 0:
        upd[WORD(x)] = status
        upd[FROM(x)] = source


def lead(variant, x, starts):
    """`starts` lists the aborts that may start at x: statuses, or 'crash'."""

    def step(M, L):
        if M[ENDED(x)]:
            return [({}, {'finished': 1}, f'node {x}: ended')]
        if all(M[CLOSED(x, y)] for y in peers(x)):
            return [({ENDED(x): 1}, {'finished': 1}, f'node {x}: every socket closed')]
        out = []
        word = M[WORD(x)]
        # An abort may start here. A crash or a fatal, at any moment until the
        # node has ended; a crash sends nothing at all. A global exit is a call
        # the application makes, so only before its members reach the final
        # barrier - after that no PE of the node may call anything.
        n = M[STARTS(x)]
        if n and starts:
            what = starts[len(starts) - n]
            if what == 'crash':
                upd = {STARTS(x): n - 1, ENDED(x): 3}
                end_streams(M, x, upd)
                out.append((upd, {}, f'node {x}: crashes'))
            elif word == 0 and (what == FAIL or not M[OWN(x)]):
                upd = {STARTS(x): n - 1}
                learn(M, upd, x, what, -1)
                out.append((upd, {}, f'node {x}: starts an abort, status {what}'))
        # A peer whose processes exited with this node's data unread has reset
        # the connection, and this node's send fails - at any moment from
        # then, ahead of or behind its reading what the peer sent first. Only
        # while this node is not aborting: an aborting node sends such a peer
        # nothing more either way (flush_aborting), and the peer is gone, so
        # the step would change nothing there but the number of states.
        for y in peers(x):
            if (word == 0 and M[ENDED(y)] in (2, 3) and M[QLEN(x, y)]
                    and not M[SEND_FAILED(x, y)] and not M[CLOSED(x, y)]):
                upd = {SEND_FAILED(x, y): 1}
                if variant == 'send_error_ends':
                    learn(M, upd, x, FAIL, y)
                out.append((upd, {}, f'node {x}: its send to node {y} fails on {y}\'s reset'))
        if word == 0:
            normal_steps(M, x, out)
        else:
            # Aborting: the job's own traffic stops; send the ABORTs, then exit.
            targets = abort_targets(variant, M, x)
            for y in targets:
                o = send_step(M, x, y, ABORT, word)
                if o is not None:
                    out.append(o)
            if not targets:
                upd = {ENDED(x): 2, ('$status', x): word}
                end_streams(M, x, upd)
                out.append((upd, {}, f'node {x}: its processes exit, status {word}'))
        for y in peers(x):
            if not M[CLOSED(x, y)] and (M[QLEN(y, x)] or (M[KFIN(y, x)] and not M[EOF(x, y)])):
                out.append(receive_step(M, x, y))
        checked = []
        for o in out:
            upd, loc, label = o
            bad = violation_index(variant, lambda k, upd=upd: upd[k] if k in upd else M[k])
            checked.append((upd, dict(loc, bad=bad), label))
        return checked

    return assemble([('STEP', ('nd', step, '@BAD?')),
                     ('BAD?', ('br', lambda L: L['bad'], '@FAIL', '@FINISHED?')),
                     ('FAIL', ('check', lambda M, L: MESSAGES[L['bad'] - 1], '@END')),
                     ('FINISHED?', ('br', lambda L: L['finished'], '@END', '@STEP')),
                     ('END', ('done',))])


def normal_steps(M, x, out):
    """close_model.py's lead, without the optional acknowledgements."""
    if not M[OWN(x)]:
        upd = {OWN(x): 1}
        close_epoch_if_complete(M, x, upd)
        out.append((upd, {}, f'node {x}: members arrive'))
    if M[LEFT(x)] and not M[EXITING(x)]:
        out.append(({EXITING(x): 1}, {}, f'node {x}: begins its exit'))
    for y in peers(x):
        queued = [m for m in (ARRIVE, RELEASE) if M[OWES(x, y, m)]]
        if M[EXITING(x)] and not M[BYE(x, y)] and not queued:
            o = send_step(M, x, y, GOODBYE, 0)
            if o:
                out.append(o)
        if M[EXITING(x)] and M[BYE(x, y)] and not M[FINNED(x, y)]:
            o = send_step(M, x, y, FIN, 0)
            if o:
                out.append(o)
        if queued and not M[BYE(x, y)]:
            o = send_step(M, x, y, queued[0], 0)
            if o:
                out.append(o)
        # A connection whose sends met the peer's reset needs no FIN of this
        # side's: the peer reads nothing more - it crashed during the close.
        if ((M[FINNED(x, y)] or M[SEND_FAILED(x, y)]) and M[EOF(x, y)]
                and not M[CLOSED(x, y)]):
            out.append(({CLOSED(x, y): 1}, {}, f'node {x}: closes its socket to node {y}'))


def send_step(M, x, y, m, status):
    n = M[QLEN(x, y)]
    if n >= CAPACITY or M[CLOSED(x, y)] or M[SEND_FAILED(x, y)]:
        return None
    upd = {Q(x, y, n): m * 8 + status, QLEN(x, y): n + 1}
    if m in (ARRIVE, RELEASE):
        upd[OWES(x, y, m)] = 0
    elif m == GOODBYE:
        upd[BYE(x, y)] = 1
    elif m == FIN:
        upd[FINNED(x, y)] = 1
    elif m == ABORT:
        upd[('$twice', x, y)] = 1 if M[SENT(x, y)] else 0
        upd[('$echo', x, y)] = 1 if M[FROM(x)] == y else 0
        upd[SENT(x, y)] = 1
    return (upd, {}, f'node {x} -> {y}: {NAME[m]}' + (f'({status})' if status else ''))


def end_streams(M, x, upd):
    """The processes are gone: the kernel ends every stream still open and closes
    every socket; what the peers have not read yet stays readable."""
    for y in peers(x):
        if not M[FINNED(x, y)]:
            upd[KFIN(x, y)] = 1
        upd[CLOSED(x, y)] = 1


def receive_step(M, x, y):
    n = M[QLEN(y, x)]
    upd = {}
    if n == 0:                          # only the kernel's FIN is left
        m, status = FIN, 0
    else:
        v = M[Q(y, x, 0)]
        m, status = v // 8, v % 8
        upd[QLEN(y, x)] = n - 1
        for i in range(n - 1):
            upd[Q(y, x, i)] = M[Q(y, x, i + 1)]
        upd[Q(y, x, n - 1)] = 0
    aborting = M[WORD(x)] != 0
    if m == ARRIVE and not aborting:
        upd[CHILD_IN(x, y)] = 1
        close_epoch_if_complete(M, x, upd)
    elif m == RELEASE and not aborting:
        upd[LEFT(x)] = 1
        for c in CHILDREN[x]:
            upd[OWES(x, c, RELEASE)] = 1
    elif m == GOODBYE:
        upd[HEARD_BYE(x, y)] = 1
    elif m == ABORT:
        upd[HEARD_BYE(x, y)] = 1        # an abortive goodbye: its FIN is not a crash
        learn(M, upd, x, status, y)
    elif m == FIN:
        upd[EOF(x, y)] = 1
        if not M[HEARD_BYE(x, y)]:      # §6.4: a peer that died
            learn(M, upd, x, FAIL, y)
    return (upd, {}, f'node {x} <- {y}: {NAME[m]}' + (f'({status})' if status else ''))


MESSAGES = []
for x in NODES:
    MESSAGES.append(f'STATUS: node {x}\'s processes exited with the failure status, '
                    f'where every abort was a global exit with a status of its own')
    for y in NODES:
        if x != y:
            MESSAGES.append(f'ONCE: node {x} sent node {y} a second ABORT')
            MESSAGES.append(f'ONCE: node {x} sent node {y} back the ABORT it came with')


def violation_index(variant, get):
    i = 0
    for x in NODES:
        if get(('$failure_ok',)) == 0 and get(ENDED(x)) == 2 and get(('$status', x)) == FAIL:
            return i + 1
        i += 1
        for y in peers(x):
            if get(('$twice', x, y)):
                return i + 1
            if variant == 'tree' and get(('$echo', x, y)):
                return i + 2
            i += 2
    return 0


def case(variant, starts):
    mem = {('$failure_ok',): int(any(s in ('crash', FAIL) for ss in starts.values()
                                      for s in ss))}
    for x in NODES:
        for k in (OWN, LEFT, EXITING, WORD, ENDED):
            mem[k(x)] = 0
        mem[FROM(x)] = -1
        mem[('$status', x)] = 0
        mem[STARTS(x)] = len(starts.get(x, ()))
        for c in CHILDREN[x]:
            mem[CHILD_IN(x, c)] = 0
        for y in peers(x):
            mem[QLEN(x, y)] = 0
            for i in range(CAPACITY):
                mem[Q(x, y, i)] = 0
            for m in (ARRIVE, RELEASE):
                mem[OWES(x, y, m)] = 0
            for k in (BYE, FINNED, HEARD_BYE, EOF, CLOSED, KFIN, SENT, SEND_FAILED):
                mem[k(x, y)] = 0
            mem[('$twice', x, y)] = 0
            mem[('$echo', x, y)] = 0
    threads = [lead(variant, x, tuple(starts.get(x, ()))) for x in NODES]
    label = ','.join(f'{x}:{"/".join(str(s) for s in ss)}' for x, ss in sorted(starts.items()))
    return f'abort/{variant} [{label}]', threads, mem


def run(variant, starts, out=sys.stdout):
    name, threads, mem = case(variant, starts)
    return explore(name, threads, mem, out=out,
                   done=lambda M: all(M[ENDED(x)] for x in NODES),
                   liveness=True, max_seconds=1800, liveness_max_states=12_000_000)


ONE_CALLER = {0: (1,)}                  # the root calls shmem_global_exit(1)
TWO_CALLERS = {0: (1,), 2: (2,)}        # the root and the leaf, statuses 1 and 2
INTERIOR = {1: (1,)}                    # the interior node calls it
FATAL = {2: (FAIL,)}                    # a fatal at the leaf
CRASH = {1: ('crash',)}                 # the interior node dies without a word

CASES = [
    ('goodbye', ONE_CALLER, True),
    ('goodbye', INTERIOR, True),
    ('goodbye', TWO_CALLERS, True),
    ('goodbye', FATAL, True),
    ('goodbye', CRASH, True),
    ('tree', ONE_CALLER, False),
    ('exit_first', ONE_CALLER, False),
    ('closing_drops', ONE_CALLER, True),
    ('resend', ONE_CALLER, False),
    ('no_abort', CRASH, True),
    ('no_abort', ONE_CALLER, False),
    ('send_error_ends', ONE_CALLER, False),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for variant, starts, expect in CASES:
        name, _, _ = case(variant, starts)
        if not keep(name):
            continue
        ok &= report(run(variant, starts), expect_ok=expect, tso=False)
    sys.exit(0 if ok else 1)
