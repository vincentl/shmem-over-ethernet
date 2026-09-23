#!/usr/bin/env python3
"""How the node leads meet at init: the rendezvous tree (design §6.2; phase 1
step 3 slice 1).

Node i's lead connects to its parent, (i - 1) / F, in a tree of fan-out F over
the node indices rooted at node 0. Each subtree's reports go up, every parent
checking its children's values against its own, and the root's tables come
down. No wait in it has a time limit (§3.15), so a lead waiting for a message
that will never come waits for ever. Beside agreement, the property that
matters is that this never happens: **a launch the leads disagree about ends
in a refusal - some lead's fatal, which the launcher turns into the end of the
job (`srun -K1`, osoe-launch) - and never in a job in which every lead
waits.**

The node count is the one value that shapes the waits, because a lead expects
children by its own count. A lead given too large a count waits for children
that do not exist. If it does that *before* its parent has heard from it,
nobody ever learns why: its parent waits for it, the root waits for the
parent, and every other lead waits for the root's tables. Hence the two rules
this model is for:

* **A lead connects to its parent before it accepts its children**, so a
  lead's parent hears from it before the lead waits by its own count;
* **the hello carries the node count**, and the lead that accepts a
  connection checks it, so a disagreement about the tree's shape is refused
  on the first edge that crosses it, before either end waits on it.

Every other value - the job's size, SHMEM_SYMMETRIC_SIZE, the handler
checksum, OSOE_COLLECTIVE_FANOUT - shapes no wait, and is checked in the
report, by each parent against its own. By induction every node agrees with
the root before the root builds, so no lead leaves the rendezvous with tables
from a job that disagrees.

Model: four leads at fan-out 2 - 0 the root, 1 and 2 its children, 3 the child
of 1 - so the tree has an interior node, a leaf at each depth, and leads whose
count can invent children: a count of 9 gives node 3 the children 7 and 8, and
node 1 the child 4. A connection is modelled as the listener makes one: the
caller's connection waits in the parent's backlog from the moment the parent
listens, and is answered only when the parent accepts it; a listener closed
with a caller waiting leaves that caller retrying for ever. A report is one
message, and so are the tables. A fatal ends the job.

Each case gives one lead a value, or a count, that differs from the others'.
Two groups of leads that each agree among themselves but disagree with the
other are left out on purpose. The root's group can then complete a smaller
job while the other group waits for a listener that has closed. Those are two
launches that share one job's ids, and no rendezvous can tell them apart from
one slow launch without a time limit. §3.15 rules a time limit out, and the
no-progress rule is its answer.

Variants
  design            as above.
  children_first    a lead accepts its children and reads their reports before
                    it connects to its parent - the order first written. Fails
                    TERMINATES when a lead's count invents a child.
  no_hello_count    the hello does not carry the count; only the report does.
                    Fails TERMINATES the same way, because the report never
                    comes.
  root_checks_only  only the root checks the values in a report. Fails AGREE
                    when a lead below the root's children disagrees.

Properties
  AGREE       no lead leaves the rendezvous while any lead disagrees with it.
  TERMINATES  every lead leaves the rendezvous, or some lead has refused the
              job (the trapped-state check; a state in which every lead waits
              is reported STUCK).

Usage: rendezvous_model.py [substring ...]
"""
import sys

from engine import assemble, explore, report, select

F = 2
NODES = (0, 1, 2, 3)
REAL_COUNT = len(NODES)


def parent(x):
    return None if x == 0 else (x - 1) // F


def children(x, count):
    """The children lead x expects, by its own count: some may not exist."""
    return tuple(c for c in range(F * x + 1, F * x + F + 1) if c < count)


def PHASE(x):   return ('$phase', x)       # index into the lead's phases
def LISTEN(x):  return ('$listen', x)      # x's listener is open
def PENDING(c): return ('$pending', c)     # c's connection waits in its parent's backlog
def JOINED(c):  return ('$joined', c)      # c's parent has accepted c's connection
def ACCEPTED(x):return ('$accepted', x)    # the children x has accepted, a bit mask
def UP(c):      return ('$up', c)          # c's report, sent and not yet read
def READ(x):    return ('$read', x)        # the children whose reports x has read, a bit mask
def SUBTREE(x): return ('$subtree', x)     # the nodes x's report covers, a bit mask
def DOWN(c):    return ('$down', c)        # the tables, delivered to c
def LEFT(x):    return ('$left', x)        # x has left the rendezvous with the tables
FATAL = ('$fatal',)                        # some lead has refused the job

LISTENING, CONNECTING, ACCEPTING, READING, SENDING, AWAITING, BUILDING, FORWARDING = range(8)


def phases(variant, x):
    if x == 0:
        return (LISTENING, ACCEPTING, READING, BUILDING, FORWARDING)
    if variant == 'children_first':
        return (LISTENING, ACCEPTING, READING, CONNECTING, SENDING, AWAITING, FORWARDING)
    return (LISTENING, CONNECTING, ACCEPTING, READING, SENDING, AWAITING, FORWARDING)


def lead(variant, x, count, value):
    order = phases(variant, x)
    up = parent(x)
    expected = children(x, count[x])
    real_children = tuple(c for c in NODES if parent(c) == x)

    def step(M, L):
        if M[FATAL]:
            return []                   # the launcher ends the job (idle_ok)
        i = M[PHASE(x)]
        now = order[i]
        nxt = {PHASE(x): i + 1}
        out = []
        if now == LISTENING:
            out.append(({**nxt, LISTEN(x): 1}, {}, f'node {x}: listens'))
        elif now == CONNECTING:
            if M[JOINED(x)]:
                out.append((nxt, {}, f'node {x}: is node {up}\'s child'))
            elif not M[PENDING(x)] and M[LISTEN(up)]:
                out.append(({PENDING(x): 1}, {}, f'node {x}: calls node {up}'))
        elif now == ACCEPTING:
            accepted = M[ACCEPTED(x)]
            if all(accepted >> c & 1 for c in expected):
                upd = {**nxt, LISTEN(x): 0}
                for c in real_children:
                    if M[PENDING(c)]:
                        upd[PENDING(c)] = 0     # reset: it retries, and nobody listens
                out.append((upd, {}, f'node {x}: has its children; closes its listener'))
            else:
                for c in real_children:
                    if not M[PENDING(c)]:
                        continue
                    if c not in expected or accepted >> c & 1:
                        out.append(({FATAL: 1}, {},
                                    f'node {x}: refuses node {c}, which it does not expect'))
                    elif variant != 'no_hello_count' and count[c] != count[x]:
                        out.append(({FATAL: 1}, {},
                                    f'node {x}: refuses node {c}\'s hello ({count[c]} nodes, '
                                    f'not {count[x]})'))
                    else:
                        out.append(({PENDING(c): 0, JOINED(c): 1, ACCEPTED(x): accepted | 1 << c},
                                    {}, f'node {x}: accepts node {c}'))
        elif now == READING:
            read = M[READ(x)]
            unread = [c for c in expected if not read >> c & 1]
            if not unread:
                out.append((nxt, {}, f'node {x}: has every report'))
            elif M[UP(unread[0])]:      # in index order, as the code reads them
                c = unread[0]
                checks = variant != 'root_checks_only' or x == 0
                if checks and (count[c] != count[x] or value[c] != value[x]):
                    out.append(({FATAL: 1}, {}, f'node {x}: refuses node {c}\'s report'))
                else:
                    out.append(({UP(c): 0, READ(x): read | 1 << c,
                                 SUBTREE(x): M[SUBTREE(x)] | M[SUBTREE(c)]},
                                {}, f'node {x}: reads node {c}\'s report'))
        elif now == SENDING:
            out.append(({**nxt, UP(x): 1}, {}, f'node {x}: reports to node {up}'))
        elif now == AWAITING:
            if M[DOWN(x)]:
                out.append((nxt, {}, f'node {x}: has the tables'))
        elif now == BUILDING:
            heard = bin(M[SUBTREE(x)]).count('1')
            if heard != count[x]:
                out.append(({FATAL: 1}, {}, f'node 0: heard from {heard} nodes of {count[x]}'))
            else:
                out.append((nxt, {}, 'node 0: builds the tables'))
        elif now == FORWARDING:
            upd = {**nxt, LEFT(x): 1}
            for c in expected:
                upd[DOWN(c)] = 1
            loc = {'finished': 1}
            if any(count[y] != count[x] or value[y] != value[x] for y in NODES):
                loc['bad'] = 1
            out.append((upd, loc, f'node {x}: sends the tables down and leaves'))
        return out

    return assemble([('STEP', ('nd', step, '@BAD?')),
                     ('BAD?', ('br', lambda L: L['bad'], '@FAIL', '@FINISHED?')),
                     ('FAIL', ('check', lambda M, L: f'AGREE: node {x} left the rendezvous '
                                                     f'while a node disagrees with it', '@END')),
                     ('FINISHED?', ('br', lambda L: L['finished'], '@END', '@STEP')),
                     ('END', ('done',))])


def configuration(deviation):
    """Every lead's node count and one agreed value, with at most one lead different."""
    count = {x: REAL_COUNT for x in NODES}
    value = {x: 0 for x in NODES}
    if deviation.startswith('value@'):
        value[int(deviation[len('value@'):])] = 1
    elif deviation.startswith('count@'):
        node, n = deviation[len('count@'):].split('=')
        count[int(node)] = int(n)
    return count, value


def case(variant, deviation):
    count, value = configuration(deviation)
    mem = {FATAL: 0}
    for x in NODES:
        for k in (PHASE, LISTEN, PENDING, JOINED, ACCEPTED, UP, READ, DOWN, LEFT):
            mem[k(x)] = 0
        mem[SUBTREE(x)] = 1 << x
    threads = [lead(variant, x, count, value) for x in NODES]
    return f'rendezvous/{variant}/{deviation}', threads, mem


def run(variant, deviation, out=sys.stdout):
    name, threads, mem = case(variant, deviation)
    return explore(name, threads, mem, out=out,
                   done=lambda M: M[FATAL] or all(M[LEFT(x)] for x in NODES),
                   idle_ok=lambda i, M, L: bool(M[FATAL]),
                   liveness=True, max_seconds=600)


# (variant, deviation, the verdict it must reach)
CASES = [
    ('design', 'agree', 'ok'),
    ('design', 'value@3', 'ok'),
    ('design', 'count@3=9', 'ok'),
    ('design', 'count@1=9', 'ok'),
    ('design', 'count@2=3', 'ok'),
    ('design', 'count@0=9', 'ok'),
    ('design', 'count@0=2', 'ok'),
    ('children_first', 'agree', 'ok'),
    ('children_first', 'value@3', 'ok'),
    ('children_first', 'count@3=9', 'STUCK'),
    ('no_hello_count', 'count@3=9', 'STUCK'),
    ('root_checks_only', 'value@3', 'VIOLATION'),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for variant, deviation, verdict in CASES:
        name, _, _ = case(variant, deviation)
        if not keep(name):
            continue
        res = run(variant, deviation)
        right = res.complete and res.verdict == verdict
        ok &= report(res, expect_ok=verdict == 'ok', tso=False,
                     extra='' if right or not res.complete else
                     f'   <-- EXPECTED {verdict}')
        ok &= right
    sys.exit(0 if ok else 1)
