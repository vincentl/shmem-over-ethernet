#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Detecting a program's own deadlock, by proof (design §3.15; the author's
decision of 2026-09-27, "build it"; shmem/src/deadlock.h).

A job is deadlocked when every PE waits inside the library, nothing any node
sent is still in flight, and nothing is waiting to be sent: then nothing can
wake anyone, since only a library operation satisfies a SHMEM wait. The
detector proves that state and never guesses it from time.

A node's *sample* is taken by its lead's agent in two halves with a pass of
its engine between them. The first half reads the node's *activity* - its
PEs' entries into and exits from waits, and the application messages it
applied - and whether every PE waits. The pass consumes whatever the PEs
posted before they began to wait, which the engine's own state cannot show
until a pass has taken it (on AM, a collective's arrivals). The second half
asks again whether every PE waits, and whether the node is *quiet*: nothing
committed and unsent, and every message it sent that carried application work
acknowledged - applied - by its peer; and that the activity has not moved. A
PE counts as waiting only while it sleeps in a wait, never while it posts: a
wait whose routine posts first (a barrier's arrival) begins counting after.

The root runs rounds of two collections. It samples itself and asks every
other node for a sample; if every node was quiet, it samples itself again and
asks again; if every node was quiet again and its activity had not moved since
its first sample began, the job is deadlocked. The argument: take t* the end
of the first collection; every node's two samples bracket t*, and nothing
happened at any node from the start of its first to the end of its second. A
message in flight at t* was sent before its sender's first sample ended (the
sender sent nothing after: a send needs a PE that runs, or a message applied,
or a post, and the posts were all taken by the first sample's pass), so it was
acknowledged by then - applied before t*, a contradiction. So at t* every PE
waits, nothing is in flight and nothing queued: a deadlock, which lasts.

This model: three nodes, the root 0, each with a PE thread running a script -
a put that may wake its target's PE, a get whose reply wakes its issuer, a
spell of computing, a wait for a wake, a barrier-like step that posts and then
waits - and ending in a wait nobody satisfies, so every run ends deadlocked;
and an agent thread that takes what its PE posted (its pass), sends it,
applies what arrives (waking the PE, or queueing a get's reply of its own),
takes acknowledgements, samples when probed, and at the root runs the rounds.
Channels are counts per kind, not queues: the argument uses no order, so the
model allows every reordering the network does not.

Checked
  SAFETY  at a declaration, every PE is blocked with no wake coming, no
          application message is in any channel, nothing is posted, committed
          or unsent: the job is deadlocked.
  and, by the engine: from every state a declaration can still be reached -
  a deadlocked job is always found (every script ends deadlocked).

Designs
  design              as above.
  control_wire_acked  the barrier's arrival on a control wire of its own,
                      its segments acknowledged as a data wire's (v0.2 step 4
                      part 2; the progress helpers proposal's §3.1). This
                      assumes the acknowledgement; control_ack_model.py checks
                      the protocol that gives it.
Breakages
  waits_only          activity counts the PEs' waits but not the messages a
                      node applies: a get's request applied between the
                      replier's samples, its reply landing after the
                      requester's second (SAFETY; get_far, where neither is
                      the root, which samples first).
  no_acks             quiet does not ask that sent messages were applied: one
                      still in a channel goes unseen (SAFETY).
  no_idle             quiet ignores what is committed and unsent (SAFETY).
  no_pass             a sample's two halves with no pass between: a post the
                      engine has not taken goes unseen, and leaves after the
                      first sample (SAFETY; put_wakes).
  coarse_waits        a PE counts as waiting from the start of a routine that
                      posts and then waits, as the "blocked in" record does:
                      sampled before its post, which lands after its
                      target's second sample (SAFETY; barrier).
  one_collection      a round declares after its first collection (SAFETY).
  control_wire        the same arrival unacknowledged, as first built: in
                      flight through both of its receiver's samples, it goes
                      unseen (SAFETY; barrier; the adversarial read of v0.2
                      step 4 part 2, F1).
  no_blocked          quiet ignores whether the PE waits: a PE computing
                      through both samples, sending nothing (SAFETY).

Usage: deadlock_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import assemble, explore, report, select

N = 3                                    # nodes; node 0 is the root
BLOCKED = lambda i: ('blocked', i)       # PE i: 0 running, 1 waiting for a wake, 2 for ever
NEED = lambda i: ('need', i)             # the wakes PE i's current wait needs, cumulative
WOKEN = lambda i: ('woken', i)           # wakes delivered to PE i, cumulative
WAITS = lambda i: ('waits', i)           # PE i's entries into and exits from waits
APPLIED = lambda i: ('applied', i)       # application messages node i applied
POSTED = lambda i, kind, j: ('posted', i, kind, j)   # posted at node i, no pass has taken it
OUT = lambda i, kind, j: ('out', i, kind, j)       # taken or queued at node i, unsent, for node j
CHAN = lambda i, j, kind: ('chan', i, j, kind)     # in flight from node i to node j
SENT = lambda i, j: ('sent', i, j)       # application messages node i sent to node j
ACKED = lambda i, j: ('acked', i, j)     # of those, the ones node j has applied, as i knows
ACKS = lambda i, j: ('acks', i, j)       # acknowledgements in flight from j back to i
HALF = lambda i: ('half', i)             # node i's sample: 0 none, 1 first half, 2 and a pass
START = lambda i: ('start', i)           # its activity at the first half, + 1; 0 if not blocked
FIRST = lambda i: ('first', i)           # its activity at its round's first sample, + 1
PROBED = lambda i: ('probed', i)         # the collection node i is answering: 0, 1 or 2
DECLARED = ('declared',)
VIOLATION = ('$violation',)

APP_KINDS = ('put_wake', 'put_silent', 'get_request', 'get_reply', 'arrive')
CONTROL_KINDS = ('probe1', 'probe2', 'report_ok', 'report_bad')

# The internode tree's ARRIVE on a control wire of its own (v0.2 step 4 part
# 2; the progress helpers proposal's §3.1), counted like every application
# message in its receiver's activity (progress.c, complete). As first built,
# nothing on a control wire was acknowledged and quiet looked at the data wires
# alone, so a node whose ARRIVE was still in flight looked quiet (control_wire,
# the adversarial read's F1). control_wire_acked is the design: a control
# wire's segments acknowledged as a data wire's are, and quiet waiting for them.
UNACKED = ('control_wire',)


def acked_kind(k, design):
    return not (k == 'arrive' and design in UNACKED)


def waiting(M, i, design):
    """Whether PE i counts as waiting, with no wake that has come."""
    if design == 'no_blocked':
        return True
    if M[BLOCKED(i)] == 0:
        return False
    return not (M[BLOCKED(i)] == 1 and M[WOKEN(i)] >= M[NEED(i)])   # woken: about to run


def quiet(M, i, design):
    """The second half's look at the network: nothing unsent, everything acknowledged.
    What the PEs posted is not the engine's to see until a pass has taken it."""
    if design != 'no_idle' and any(M[OUT(i, k, j)] for k in APP_KINDS for j in range(N)):
        return False
    if design != 'no_acks' and any(M[ACKED(i, j)] != M[SENT(i, j)] for j in range(N) if j != i):
        return False
    return True


def activity(M, i, design):
    return M[WAITS(i)] + (0 if design == 'waits_only' else M[APPLIED(i)])


def deadlocked(M):
    """The global truth, for the check at a declaration."""
    for i in range(N):
        if M[BLOCKED(i)] == 0 or (M[BLOCKED(i)] == 1 and M[WOKEN(i)] >= M[NEED(i)]):
            return f'PE {i} is not blocked for good'
        for j in range(N):
            for k in APP_KINDS:
                if M[POSTED(i, k, j)]:
                    return f'node {i} has a {k} for node {j} posted and not taken'
                if M[OUT(i, k, j)]:
                    return f'node {i} has a {k} for node {j} committed and unsent'
                if M[CHAN(i, j, k)]:
                    return f'a {k} from node {i} to node {j} is in flight'
    return None


def pe(i, script, design):
    """PE i's program: its script, then a wait nobody satisfies."""
    B = []
    for n, step in enumerate(script):
        p = f'pe{i}s{n}_'
        nxt = f'@pe{i}s{n + 1}_' if n + 1 < len(script) else '@FINAL'
        if step[0] in ('put_wake', 'put_silent', 'get_request'):
            j = step[1]
            B.append((p, ('add', lambda L, k=step[0], j=j: POSTED(i, k, j), lambda L: 1, '_',
                          nxt if step[0] != 'get_request' else '@' + p + 'W')))
            if step[0] == 'get_request':
                B += wait(i, p + 'W', nxt)
        elif step[0] == 'wait':
            B += wait(i, p, nxt)
        elif step[0] == 'compute':
            B.append((p, ('set', 'x', lambda L: 1, '@' + p + 'C')))
            B.append((p + 'C', ('set', 'x', lambda L: 2, nxt)))
        elif step[0] == 'barrier':
            # A routine that posts and then waits for good: a barrier whose
            # arrival wakes node j and whose release never comes.
            j = step[1]
            if design == 'coarse_waits':
                # Counted as waiting from the routine's start, before its post.
                B.append((p, ('ghost', lambda M, L: ({BLOCKED(i): 2,
                                                      WAITS(i): M[WAITS(i)] + 1}, {}),
                              '@' + p + 'P')))
                B.append((p + 'P', ('add', lambda L, j=j: POSTED(i, 'put_wake', j), lambda L: 1,
                                    '_', '@END')))
            else:
                kind = 'arrive' if design.startswith('control_wire') else 'put_wake'
                B.append((p, ('add', lambda L, j=j, kind=kind: POSTED(i, kind, j), lambda L: 1,
                              '_', '@FINAL')))
    B += [('FINAL', ('ghost', lambda M, L: ({BLOCKED(i): 2, WAITS(i): M[WAITS(i)] + 1}, {}),
                     '@END')),
          ('END', ('done',))]
    return assemble(B)


def wait(i, label, nxt):
    """PE i enters a wait for the next wake, sleeps, leaves it: each an activity."""
    return [(label, ('ghost', lambda M, L: ({NEED(i): M[NEED(i)] + 1, BLOCKED(i): 1,
                                             WAITS(i): M[WAITS(i)] + 1}, {}),
                     '@' + label + 'S')),
            (label + 'S', ('await', lambda M, L: M[WOKEN(i)] >= M[NEED(i)], '@' + label + 'L')),
            (label + 'L', ('ghost', lambda M, L: ({BLOCKED(i): 0, WAITS(i): M[WAITS(i)] + 1}, {}),
                           nxt))]


def agent(i, design):
    """Node i's agent: whatever is ready, one step at a time."""
    root = i == 0

    def first_half(M):
        """Read the activity and whether the PE waits; the pass comes next."""
        start = activity(M, i, design) + 1 if waiting(M, i, design) else 0
        return {HALF(i): 1, START(i): start}

    def second_half(M):
        """Whether the sample holds: the PE waited at both halves, the network
        is quiet now, and nothing moved between. The activity it started at,
        + 1, or 0."""
        ok = (M[START(i)] != 0 and waiting(M, i, design) and quiet(M, i, design) and
              activity(M, i, design) + 1 == M[START(i)])
        return M[START(i)] if ok else 0

    def choose(M, L):
        alts = []
        # The pass: take everything the PE posted; a sample's first half is
        # followed by one before its second.
        posted = [(k, j) for k in APP_KINDS for j in range(N) if M[POSTED(i, k, j)]]
        if posted or M[HALF(i)] == 1:
            upd = {}
            for k, j in posted:
                upd[POSTED(i, k, j)] = 0
                upd[OUT(i, k, j)] = M[OUT(i, k, j)] + M[POSTED(i, k, j)]
            if M[HALF(i)] == 1:
                upd[HALF(i)] = 2
            alts.append((upd, {}, f'node {i} passes'))
        # Send one committed message.
        for j in range(N):
            for k in APP_KINDS:
                if M[OUT(i, k, j)]:
                    upd = {OUT(i, k, j): M[OUT(i, k, j)] - 1,
                           CHAN(i, j, k): M[CHAN(i, j, k)] + 1}
                    if acked_kind(k, design):
                        upd[SENT(i, j)] = M[SENT(i, j)] + 1
                    alts.append((upd, {}, f'node {i} sends a {k} to node {j}'))
        # Apply one arrived message, which the sender will hear of.
        for j in range(N):
            for k in APP_KINDS:
                if M[CHAN(j, i, k)]:
                    upd = {CHAN(j, i, k): M[CHAN(j, i, k)] - 1, APPLIED(i): M[APPLIED(i)] + 1}
                    if acked_kind(k, design):
                        upd[ACKS(j, i)] = M[ACKS(j, i)] + 1
                    if k in ('put_wake', 'get_reply', 'arrive'):
                        upd[WOKEN(i)] = M[WOKEN(i)] + 1
                    if k == 'get_request':
                        upd[OUT(i, 'get_reply', j)] = M[OUT(i, 'get_reply', j)] + 1
                    alts.append((upd, {}, f'node {i} applies a {k} from node {j}'))
        # An acknowledgement comes back.
        for j in range(N):
            if M[ACKS(i, j)]:
                alts.append(({ACKS(i, j): M[ACKS(i, j)] - 1, ACKED(i, j): M[ACKED(i, j)] + 1},
                             {}, f'node {i} hears node {j} applied one'))
        # A sample's second half, whole with the pass (or, broken, without).
        ready = M[HALF(i)] == 2 or (design == 'no_pass' and M[HALF(i)] == 1)
        if not root:
            # A probe: its first half now.
            for c in (1, 2):
                if M[CHAN(0, i, f'probe{c}')] and M[HALF(i)] == 0:
                    upd = first_half(M)
                    upd[CHAN(0, i, f'probe{c}')] = 0
                    upd[PROBED(i)] = c
                    alts.append((upd, {}, f'node {i} is probed ({c}) and begins a sample'))
            if ready:
                start = second_half(M)
                if M[PROBED(i)] == 1:
                    ok = start != 0
                    upd = {FIRST(i): start}
                else:
                    ok = start != 0 and start == M[FIRST(i)]
                    upd = {}
                kind = 'report_ok' if ok else 'report_bad'
                upd.update({HALF(i): 0, START(i): 0, PROBED(i): 0,
                            CHAN(i, 0, kind): M[CHAN(i, 0, kind)] + 1})
                alts.append((upd, {}, f'node {i} reports: {"quiet" if ok else "busy"}'))
        else:
            ph = L['phase']
            others = N - 1
            got = sum(M[CHAN(j, 0, 'report_ok')] + M[CHAN(j, 0, 'report_bad')]
                      for j in range(1, N))
            all_ok = all(M[CHAN(j, 0, 'report_ok')] for j in range(1, N))
            clear = {}
            for j in range(1, N):
                clear[CHAN(j, 0, 'report_ok')] = 0
                clear[CHAN(j, 0, 'report_bad')] = 0
            if ph in (0, 3) and M[HALF(0)] == 0 and not M[DECLARED]:
                alts.append((first_half(M), {'phase': ph + 1},
                             f'the root begins its {"first" if ph == 0 else "second"} sample'))
            if ph == 1 and ready:
                start = second_half(M)
                upd = {HALF(0): 0, START(0): 0, FIRST(0): start}
                if start != 0:
                    for j in range(1, N):
                        upd[CHAN(0, j, 'probe1')] = 1
                    alts.append((upd, {'phase': 2}, 'the root was quiet: it probes'))
                else:
                    alts.append((upd, {'phase': 0}, 'the root was busy'))
            if ph == 2 and got == others:
                if all_ok and design == 'one_collection':
                    clear[DECLARED] = 1
                    if deadlocked(M):
                        clear[VIOLATION] = 1
                    alts.append((clear, {'phase': 6}, 'the root declares after one'))
                elif all_ok:
                    alts.append((clear, {'phase': 3}, 'every node quiet: t*'))
                else:
                    alts.append((clear, {'phase': 0}, 'a node was busy: the round ends'))
            if ph == 4 and ready:
                start = second_half(M)
                upd = {HALF(0): 0, START(0): 0}
                if start != 0 and start == M[FIRST(0)]:
                    for j in range(1, N):
                        upd[CHAN(0, j, 'probe2')] = 1
                    alts.append((upd, {'phase': 5}, 'the root unmoved: it probes again'))
                else:
                    alts.append((upd, {'phase': 0}, 'the root moved: the round ends'))
            if ph == 5 and got == others:
                if all_ok:
                    clear[DECLARED] = 1
                    if deadlocked(M):
                        clear[VIOLATION] = 1
                    alts.append((clear, {'phase': 6}, 'nothing moved: the root declares'))
                else:
                    alts.append((clear, {'phase': 0}, 'something moved: the round ends'))
        return alts

    B = [('A0', ('set', 'phase', lambda L: 0, '@LOOP')),
         ('LOOP', ('nd', choose, '@CHECK')),
         ('CHECK', ('check', lambda M, L: None if not M[VIOLATION] else
                    f'SAFETY: declared, but {deadlocked(M) or "?"}', '@AFTER')),
         ('AFTER', ('br', lambda L: L.get('phase') == 6, '@END', '@LOOP')),
         ('END', ('done',))]
    return assemble(B)


def case(design, scripts):
    threads = []
    for i in range(N):
        threads.append(pe(i, scripts[i], design))
    for i in range(N):
        threads.append(agent(i, design))
    mem = {DECLARED: 0, VIOLATION: 0}
    for i in range(N):
        for key in (BLOCKED(i), NEED(i), WOKEN(i), WAITS(i), APPLIED(i), HALF(i), START(i),
                    FIRST(i), PROBED(i)):
            mem[key] = 0
        for j in range(N):
            mem[SENT(i, j)] = mem[ACKED(i, j)] = mem[ACKS(i, j)] = 0
            for k in APP_KINDS + CONTROL_KINDS:
                mem[CHAN(i, j, k)] = 0
            for k in APP_KINDS:
                mem[OUT(i, k, j)] = mem[POSTED(i, k, j)] = 0
    return threads, mem


# Each PE's script, then its wait for ever.
SCENARIOS = {
    'deadlock': ([], [], []),
    'put_wakes': ([], [('put_wake', 2)], [('wait',)]),
    'put_silent': ([], [('put_silent', 2)], []),
    'get': ([('get_request', 1)], [], []),
    # Between two nodes the root is not: the replier may be sampled before the
    # requester, the request applied between the replier's two samples.
    'get_far': ([], [], [('get_request', 1)]),
    'compute': ([], [('compute',)], []),
    'chain': ([('get_request', 1)], [('put_wake', 2)], [('wait',)]),
    # A barrier's arrival that wakes a node which waits for it.
    'barrier': ([], [('barrier', 2)], [('wait',)]),
}

# (design, scenario, expect): True, or the property the breakage must fail
CASES = [
    ('design', 'deadlock', True),
    ('design', 'put_wakes', True),
    ('design', 'put_silent', True),
    ('design', 'get', True),
    ('design', 'get_far', True),
    ('design', 'compute', True),
    ('design', 'chain', True),
    ('design', 'barrier', True),
    ('waits_only', 'get_far', 'SAFETY'),
    ('no_acks', 'put_silent', 'SAFETY'),
    ('no_idle', 'put_silent', 'SAFETY'),
    ('no_pass', 'put_wakes', 'SAFETY'),
    ('coarse_waits', 'barrier', 'SAFETY'),
    ('one_collection', 'put_wakes', 'SAFETY'),
    ('no_blocked', 'compute', 'SAFETY'),
    # The barrier's arrival on the control wire (v0.2 step 4 part 2).
    ('control_wire_acked', 'barrier', True),
    ('control_wire', 'barrier', 'SAFETY'),
]


def main():
    want = select(sys.argv)
    ok = True
    for design, scenario, expect in CASES:
        name = f'deadlock/{design} scenario={scenario}'
        if not want(name):
            continue
        threads, mem = case(design, SCENARIOS[scenario])
        # The agents are service loops, which may wait for ever with nothing to
        # do; whether the job is found is the check that a declaration stays
        # reachable from every state.
        res = explore(name, threads, mem, max_seconds=900, liveness_max_states=3_000_000,
                      idle_ok=lambda i, M, L: i >= N, done=lambda M: M[DECLARED] == 1)
        ok &= report(res, expect=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
