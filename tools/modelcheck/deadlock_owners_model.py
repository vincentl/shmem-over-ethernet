#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The deadlock detector with a helper thread (v0.2 step 4, Part 3; design
§3.15; the progress helpers proposal's §3.5; docs/decisions/2026-10-08-helpers.md).

deadlock_model.py checks the detector's proof with one engine a node, the
lead's agent. With helpers a node's engine is several threads, none writing
another's state: the agent, owner 0, keeps the control plane - the internode
tree's arrivals on the control wires, the detector's probes and reports - and a
helper, owner 1, the data wires - puts, gets and their replies. The agent
samples the helper through a quiet request, which the helper answers after a
whole pass begun after it (helper_request_model.py):

  first half    the agent reads the node's activity - the PE's waits, its own
                applied messages and the helper's, each count published by
                its owner - and whether the PE waits, and posts a quiet request
                to the helper.
  second half   once the agent has passed and the helper has answered the
                request: the PE waited at both halves, the agent is quiet, the
                helper answered quiet, and the activity is as it began.

The argument is deadlock_model.py's, owner by owner: every count only grows, so
an unchanged sum is every count unchanged; each owner's quiet covers the
messages it sent; and the helper's pass after the request takes what its PE
posted before waiting, as the agent's pass does on one engine.

Checked: as deadlock_model.py - SAFETY at a declaration, and a declaration
reachable from every state (every scenario ends deadlocked).

Designs
  design              as above.
Breakages
  helper_unseen       activity counts the agent's applied messages and not the
                      helper's: a get's request a helper applies between its
                      node's samples, its reply landing after the requester's
                      second (SAFETY; get_far).
  answer_unpassed     the helper answers the latest request without a pass
                      after it: a put its PE posted before waiting, not yet
                      taken, goes unseen (SAFETY; put_wakes).
  helper_unasked      the second half takes the agent's quiet for the node's,
                      without the helper's answer: a put in flight goes unseen
                      (SAFETY; put_silent).

Usage: deadlock_owners_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import assemble, explore, report, select

N = 3                                    # nodes; node 0 is the root
BLOCKED = lambda i: ('blocked', i)       # PE i: 0 running, 1 waiting for a wake, 2 for ever
NEED = lambda i: ('need', i)             # the wakes PE i's current wait needs, cumulative
WOKEN = lambda i: ('woken', i)           # wakes delivered to PE i, cumulative
WAITS = lambda i: ('waits', i)           # PE i's entries into and exits from waits
APPLIED_A = lambda i: ('applied_agent', i)    # application messages node i's agent applied
APPLIED_H = lambda i: ('applied_helper', i)   # ... and its helper
POSTED = lambda i, kind, j: ('posted', i, kind, j)   # posted at node i, no pass has taken it
OUT = lambda i, kind, j: ('out', i, kind, j)       # taken, unsent, for node j
CHAN = lambda i, j, kind: ('chan', i, j, kind)     # in flight from node i to node j
SENT = lambda i, j, w: ('sent', i, j, w)       # application messages node i sent node j on wire w
ACKED = lambda i, j, w: ('acked', i, j, w)     # of those, the ones node j applied, as i knows
ACKS = lambda i, j, w: ('acks', i, j, w)       # acknowledgements in flight from j back to i
HALF = lambda i: ('half', i)             # node i's sample: 0 none, 1 first half, 2 and a pass
START = lambda i: ('start', i)           # its activity at the first half, + 1; 0 if not blocked
FIRST = lambda i: ('first', i)           # its activity at its round's first sample, + 1
PROBED = lambda i: ('probed', i)         # the collection node i is answering: 0, 1 or 2
REQUEST = lambda i: ('request', i)       # the agent's latest quiet request to its helper:
                                         # 1 and 2 by turns, a sample at a time being
                                         # outstanding (0: none yet)
ANSWER = lambda i: ('answer', i)         # the request the helper answered last
ANSWER_QUIET = lambda i: ('answer_quiet', i)  # and its verdict
DECLARED = ('declared',)
VIOLATION = ('$violation',)

DATA_KINDS = ('put_wake', 'put_silent', 'get_request', 'get_reply')     # the helper's
CONTROL_APP = ('arrive',)                                                # the agent's
APP_KINDS = DATA_KINDS + CONTROL_APP
CONTROL_KINDS = ('probe1', 'probe2', 'report_ok', 'report_bad')
WIRES = ('data', 'control')


def wire_of(kind):
    return 'control' if kind in CONTROL_APP else 'data'


def waiting(M, i):
    """Whether PE i counts as waiting, with no wake that has come."""
    if M[BLOCKED(i)] == 0:
        return False
    return not (M[BLOCKED(i)] == 1 and M[WOKEN(i)] >= M[NEED(i)])   # woken: about to run


def owner_quiet(M, i, kinds, wire):
    """An owner's quiet: nothing it has taken unsent, everything it sent acknowledged."""
    if any(M[OUT(i, k, j)] for k in kinds for j in range(N)):
        return False
    return all(M[ACKED(i, j, wire)] == M[SENT(i, j, wire)] for j in range(N) if j != i)


def activity(M, i, design):
    helper = 0 if design == 'helper_unseen' else M[APPLIED_H(i)]
    return M[WAITS(i)] + M[APPLIED_A(i)] + helper


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
                    return f'node {i} has a {k} for node {j} taken and unsent'
                if M[CHAN(i, j, k)]:
                    return f'a {k} from node {i} to node {j} is in flight'
    return None


def pe(i, script):
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
            # A routine that posts its arrival - the control plane's - and then
            # waits for good: the release never comes.
            j = step[1]
            B.append((p, ('add', lambda L, j=j: POSTED(i, 'arrive', j), lambda L: 1, '_',
                          '@FINAL')))
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


def traffic(i, kinds, applied):
    """An owner's sends, applies and acknowledgements for `kinds`, its wire's.
    `applied` is the count it publishes."""
    def steps(M):
        alts = []
        for j in range(N):
            for k in kinds:
                if M[OUT(i, k, j)]:
                    w = wire_of(k)
                    alts.append(({OUT(i, k, j): M[OUT(i, k, j)] - 1,
                                  CHAN(i, j, k): M[CHAN(i, j, k)] + 1,
                                  SENT(i, j, w): M[SENT(i, j, w)] + 1},
                                 {}, f'node {i} sends a {k} to node {j}'))
        for j in range(N):
            for k in kinds:
                if M[CHAN(j, i, k)]:
                    w = wire_of(k)
                    upd = {CHAN(j, i, k): M[CHAN(j, i, k)] - 1, applied: M[applied] + 1,
                           ACKS(j, i, w): M[ACKS(j, i, w)] + 1}
                    if k in ('put_wake', 'get_reply', 'arrive'):
                        upd[WOKEN(i)] = M[WOKEN(i)] + 1
                    if k == 'get_request':
                        upd[OUT(i, 'get_reply', j)] = M[OUT(i, 'get_reply', j)] + 1
                    alts.append((upd, {}, f'node {i} applies a {k} from node {j}'))
        w = wire_of(kinds[0])
        for j in range(N):
            if M[ACKS(i, j, w)]:
                alts.append(({ACKS(i, j, w): M[ACKS(i, j, w)] - 1,
                              ACKED(i, j, w): M[ACKED(i, j, w)] + 1},
                             {}, f'node {i} hears node {j} applied one on its {w} wire'))
        return alts
    return steps


def helper(i, design):
    """Node i's helper: the data wires, and its answers to the agent's quiet
    requests. `passed` is the request its latest pass began after."""
    moves = traffic(i, DATA_KINDS, APPLIED_H(i))

    def choose(M, L):
        alts = []
        passed = L['passed'] or 0
        posted = [(k, j) for k in DATA_KINDS for j in range(N) if M[POSTED(i, k, j)]]
        if posted or passed != M[REQUEST(i)]:
            upd = {}
            for k, j in posted:
                upd[POSTED(i, k, j)] = 0
                upd[OUT(i, k, j)] = M[OUT(i, k, j)] + M[POSTED(i, k, j)]
            alts.append((upd, {'passed': M[REQUEST(i)]}, f'helper {i} passes'))
        # Answer the request outstanding, once a pass has begun after it
        # (answer_unpassed: whether or not one has).
        due = M[REQUEST(i)] != M[ANSWER(i)] and (design == 'answer_unpassed' or
                                                 passed == M[REQUEST(i)])
        if due:
            alts.append(({ANSWER(i): M[REQUEST(i)],
                          ANSWER_QUIET(i): int(owner_quiet(M, i, DATA_KINDS, 'data'))},
                         {}, f'helper {i} answers request {M[REQUEST(i)]}'))
        return alts + moves(M)

    B = [('H0', ('set', 'passed', lambda L: 0, '@LOOP')),
         ('LOOP', ('nd', choose, '@LOOP'))]
    return assemble(B)


def agent(i, design):
    """Node i's agent: the control plane, the samples, and at the root the rounds."""
    root = i == 0
    moves = traffic(i, CONTROL_APP, APPLIED_A(i))

    def first_half(M):
        """Read the activity and whether the PE waits, and ask the helper; the
        agent's pass, and the helper's answer, come next."""
        start = activity(M, i, design) + 1 if waiting(M, i) else 0
        return {HALF(i): 1, START(i): start, REQUEST(i): 2 if M[REQUEST(i)] == 1 else 1}

    def ready(M):
        answered = design == 'helper_unasked' or M[ANSWER(i)] == M[REQUEST(i)]
        return M[HALF(i)] == 2 and answered

    def second_half(M):
        helper_quiet = design == 'helper_unasked' or M[ANSWER_QUIET(i)] == 1
        ok = (M[START(i)] != 0 and waiting(M, i) and helper_quiet and
              owner_quiet(M, i, CONTROL_APP, 'control') and
              activity(M, i, design) + 1 == M[START(i)])
        return M[START(i)] if ok else 0

    def choose(M, L):
        alts = []
        # The agent's pass: take what the PE posted for the control plane.
        posted = [(k, j) for k in CONTROL_APP for j in range(N) if M[POSTED(i, k, j)]]
        if posted or M[HALF(i)] == 1:
            upd = {}
            for k, j in posted:
                upd[POSTED(i, k, j)] = 0
                upd[OUT(i, k, j)] = M[OUT(i, k, j)] + M[POSTED(i, k, j)]
            if M[HALF(i)] == 1:
                upd[HALF(i)] = 2
            alts.append((upd, {}, f'node {i} passes'))
        alts += moves(M)
        if not root:
            for c in (1, 2):
                if M[CHAN(0, i, f'probe{c}')] and M[HALF(i)] == 0:
                    upd = first_half(M)
                    upd[CHAN(0, i, f'probe{c}')] = 0
                    upd[PROBED(i)] = c
                    alts.append((upd, {}, f'node {i} is probed ({c}) and begins a sample'))
            if ready(M):
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
            if ph == 1 and ready(M):
                start = second_half(M)
                upd = {HALF(0): 0, START(0): 0, FIRST(0): start}
                if start != 0:
                    for j in range(1, N):
                        upd[CHAN(0, j, 'probe1')] = 1
                    alts.append((upd, {'phase': 2}, 'the root was quiet: it probes'))
                else:
                    alts.append((upd, {'phase': 0}, 'the root was busy'))
            if ph == 2 and got == others:
                if all_ok:
                    alts.append((clear, {'phase': 3}, 'every node quiet: t*'))
                else:
                    alts.append((clear, {'phase': 0}, 'a node was busy: the round ends'))
            if ph == 4 and ready(M):
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
    threads = [pe(i, scripts[i]) for i in range(N)]
    threads += [agent(i, design) for i in range(N)]
    threads += [helper(i, design) for i in range(N)]
    mem = {DECLARED: 0, VIOLATION: 0}
    for i in range(N):
        for key in (BLOCKED(i), NEED(i), WOKEN(i), WAITS(i), APPLIED_A(i), APPLIED_H(i),
                    HALF(i), START(i), FIRST(i), PROBED(i), REQUEST(i), ANSWER(i),
                    ANSWER_QUIET(i)):
            mem[key] = 0
        for j in range(N):
            for w in WIRES:
                mem[SENT(i, j, w)] = mem[ACKED(i, j, w)] = mem[ACKS(i, j, w)] = 0
            for k in APP_KINDS + CONTROL_KINDS:
                mem[CHAN(i, j, k)] = 0
            for k in APP_KINDS:
                mem[OUT(i, k, j)] = mem[POSTED(i, k, j)] = 0
    return threads, mem


SCENARIOS = {
    'deadlock': ([], [], []),
    'put_wakes': ([], [('put_wake', 2)], [('wait',)]),
    'put_silent': ([], [('put_silent', 2)], []),
    'get': ([('get_request', 1)], [], []),
    'get_far': ([], [], [('get_request', 1)]),
    'compute': ([], [('compute',)], []),
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
    ('design', 'barrier', True),
    ('helper_unseen', 'get_far', 'SAFETY'),
    ('answer_unpassed', 'put_wakes', 'SAFETY'),
    ('helper_unasked', 'put_silent', 'SAFETY'),
]


def main():
    want = select(sys.argv)
    ok = True
    for design, scenario, expect in CASES:
        name = f'deadlock_owners/{design} scenario={scenario}'
        if not want(name):
            continue
        threads, mem = case(design, SCENARIOS[scenario])
        # Agents and helpers are service loops, which may wait for ever with
        # nothing to do; that a declaration stays reachable from every state is
        # the check that the job is found.
        res = explore(name, threads, mem, max_seconds=1500, liveness_max_states=6_000_000,
                      idle_ok=lambda i, M, L: i >= N, done=lambda M: M[DECLARED] == 1)
        ok &= report(res, expect=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
