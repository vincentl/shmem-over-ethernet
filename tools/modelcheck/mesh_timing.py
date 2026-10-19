#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Timing of the mesh's start-up (design §6.2, wire.c osoe_wires_open; H23):
the order osoe made its node-pair connections in before H23, against the
circulant schedule mesh_model.py checks (docs/design/mesh-schedule-proposal.md).

Each lead is one thread, a serial server. A connection: the caller dials (its
CPU), the kernels' handshake (a round trip), the hello (one way), the callee
reads and answers it (its CPU), the answer (one way), the caller confirms and
attaches its wire (its CPU), the confirmation (one way), the callee attaches
its end (its CPU). A lead that has not reached the mesh answers nothing; its
kernel still completes the handshakes, which wait in its backlog. Two wires a
pair, the control wire and the data wire, two calls.

  today    each lead accepts every lower node, then calls every higher node,
           one call at a time, in increasing order (wire.c before H23).
  slotted  the circulant: lead i calls (i + d) mod N at each offset d of the
           job's shuffled order (mesh_model.order), both wires of a pair in its
           slot; a call no earlier than its slot, Δ apart from the lead's
           entry, and at most W calls in flight; the listener answered
           throughout.

The link is tier3.sh's `pi`: 100 µs each way, 1 Gbit/s - about the Pi
cluster's. The CPU a connection costs each end is the proposal's estimate, 35
µs at the caller and 25 at the callee, one CPU a lead as on a cluster; `cores`
shares a few CPUs among every lead, as tier 3 runs them on the development
VM's four, and `wake` delays each piece of a lead's work as a loaded machine's
scheduler does. `tier3` sets the model at four CPUs and a wake of 50 µs beside
H23's runs there (docs/design/evidence/v0.2-step4/2026-10-19-h23/): within a sixth
at 16 and 32 nodes, both orders, and short of both at 64, where 64 namespaces
on four CPUs cost more than it has - 96 ms for the chain against 156 to 173
measured, 67 for the schedule against 82 to 105.

Entry: on schedule, every lead at once; `tree`, each lead when the
rendezvous's tables reach it - a parent sending them to its children over one
link, about 156 bytes a node each, so that at 4,097 nodes on 1 Gbit/s the
last level arrives some 0.16 s after the root (rendezvous.c's send_down).

Measured: the time until every wire is attached; the most SYNs reaching one
listener within one round trip; the most connections waiting at one lead
(handshake done, hello not yet answered).

Usage: mesh_timing.py [check] | table [COUNTS...] | tier3
"""
import heapq
import sys

import engine
from mesh_model import callee, order

JOB, STEP = 1678987, 0
ONE_WAY = 100e-6                 # tier3.sh's pi: netem's delay on each node's egress
BYTES_PER_SECOND = 125e6         # 1 Gbit/s
FANOUT = 8                       # the rendezvous's tree (OSOE_RENDEZVOUS_FANOUT)
TABLE_BYTES_A_NODE = 156         # a run (24), an address (4) and a blob (128)

# A connection's CPU at each end: the caller's dial, its hello, its
# confirmation with the attach; the callee's accept, read and answer, then its
# attach on the confirmation - the proposal's estimates.
COSTS = dict(dial=10e-6, hello=5e-6, confirm=20e-6, answer=10e-6, attach=15e-6)

TODAY, SLOTTED = 'today', 'slotted'
(ENTER, SLOT, DIALLED, CONNECTED, HELLO_SENT, HELLO_IN, ANSWERED, ANSWER_IN, CONFIRMED,
 CONFIRM_IN, ATTACHED) = range(11)


def tree_entry(n, one_way=ONE_WAY, bytes_per_second=BYTES_PER_SECOND):
    """When the rendezvous's tables reach each lead: a parent's children
    share its link, so each has them once all of them have."""
    each = TABLE_BYTES_A_NODE * n / bytes_per_second
    entry = [0.0] * n
    for p in range(n):
        children = [c for c in range(p * FANOUT + 1, p * FANOUT + FANOUT + 1) if c < n]
        for c in children:
            entry[c] = entry[p] + len(children) * each + one_way
    return entry


def plan(n, schedule, wires, slot_order):
    """Each lead's calls in order, (callee, slot)."""
    calls = []
    for i in range(n):
        if schedule == TODAY:
            mine = [(j, 0) for j in range(i + 1, n)]
        else:
            o = slot_order(i) if callable(slot_order) else slot_order
            mine = [(callee(i, d, n), s) for s, d in enumerate(o) if callee(i, d, n) is not None]
        calls.append([c for c in mine for _ in range(wires)])
    return calls


def simulate(n, schedule, window=8, slot=100e-6, wires=2, one_way=ONE_WAY, costs=COSTS,
             cores=None, entry=None, slot_order=None, wake=0.0):
    slot_order = slot_order if slot_order is not None else order(JOB, STEP, n)
    calls = plan(n, schedule, wires, slot_order)
    expected = [0] * n
    for mine in calls:
        for j, _ in mine:
            expected[j] += 1
    if schedule == TODAY:
        window = 1
    entry = entry or [0.0] * n
    round_trip = 2 * one_way
    dial, hello, confirm = costs['dial'], costs['hello'], costs['confirm']
    answer, attach = costs['answer'], costs['attach']

    events, sequence = [], 0
    push = heapq.heappush
    busy = list(entry)                   # a lead's thread, from its entry
    free_cores = [0.0] * cores if cores else None

    def work(lead, t, cost, kind, i, j):
        nonlocal sequence
        t += wake                       # the thread woken: a loaded machine's delay
        start = t if t > busy[lead] else busy[lead]
        if free_cores is not None:
            c = heapq.heappop(free_cores)
            if c > start:
                start = c
            heapq.heappush(free_cores, start + cost)
        busy[lead] = start + cost
        sequence += 1
        push(events, (start + cost, sequence, kind, i, j))

    def at(t, kind, i, j):
        nonlocal sequence
        sequence += 1
        push(events, (t, sequence, kind, i, j))

    entered = [False] * n
    waiting_hellos = [[] for _ in range(n)]
    answered_in, made, nxt, flying = [0] * n, [0] * n, [0] * n, [0] * n
    timer = [False] * n
    waiting, waiting_most = [0] * n, 0
    syns = [[] for _ in range(n)]
    done = [None] * n

    def call_more(i, t):
        if schedule == TODAY and answered_in[i] < expected[i]:
            return
        mine = calls[i]
        while nxt[i] < len(mine) and flying[i] < window:
            j, s = mine[nxt[i]]
            if schedule == SLOTTED:
                due = entry[i] + s * slot
                if due > t + 1e-12:
                    if not timer[i]:
                        timer[i] = True
                        at(due, SLOT, i, i)
                    return
            nxt[i] += 1
            flying[i] += 1
            work(i, t, dial, DIALLED, i, j)

    def finished(i, t):
        if done[i] is None and answered_in[i] == expected[i] and made[i] == len(calls[i]):
            done[i] = t

    for i in range(n):
        at(entry[i], ENTER, i, i)
    pop = heapq.heappop
    while events:
        t, _, kind, i, j = pop(events)
        if kind == DIALLED:
            syns[j].append(t + one_way)
            at(t + round_trip, CONNECTED, i, j)
        elif kind == CONNECTED:
            work(i, t, hello, HELLO_SENT, i, j)
        elif kind == HELLO_SENT:
            at(t + one_way, HELLO_IN, i, j)
        elif kind == HELLO_IN:
            waiting[j] += 1
            if waiting[j] > waiting_most:
                waiting_most = waiting[j]
            if entered[j]:
                work(j, t, answer, ANSWERED, i, j)
            else:
                waiting_hellos[j].append(i)
        elif kind == ANSWERED:
            waiting[j] -= 1
            at(t + one_way, ANSWER_IN, i, j)
        elif kind == ANSWER_IN:
            work(i, t, confirm, CONFIRMED, i, j)
        elif kind == CONFIRMED:
            made[i] += 1
            flying[i] -= 1
            at(t + one_way, CONFIRM_IN, i, j)
            call_more(i, t)
            finished(i, t)
        elif kind == CONFIRM_IN:
            work(j, t, attach, ATTACHED, i, j)
        elif kind == ATTACHED:
            answered_in[j] += 1
            if schedule == TODAY:
                call_more(j, t)
            finished(j, t)
        elif kind == SLOT:
            timer[i] = False
            call_more(i, t)
        elif kind == ENTER:
            entered[i] = True
            for caller in waiting_hellos[i]:
                work(i, t, answer, ANSWERED, caller, i)
            waiting_hellos[i] = []
            call_more(i, t)
            finished(i, t)
    assert all(d is not None for d in done), 'a lead never finished'
    syn_most = 0
    for arrivals in syns:
        arrivals.sort()
        low = 0
        for high in range(len(arrivals)):
            while arrivals[high] - arrivals[low] > round_trip:
                low += 1
            if high - low + 1 > syn_most:
                syn_most = high - low + 1
    return dict(done=max(done), syn_most=syn_most, waiting_most=waiting_most,
                start=min(entry), last_entry=max(entry))


def permutation_slots(n, slot_order):
    """For each slot, whether every listener is called at most once, and the
    opposite slot's upper half once each: the schedule's spread."""
    for s in range(len(slot_order(0) if callable(slot_order) else slot_order)):
        called = [0] * n
        for i in range(n):
            o = slot_order(i) if callable(slot_order) else slot_order
            j = callee(i, o[s], n)
            if j is not None:
                called[j] += 1
        if max(called) > 1:
            return f'PERMUTATION: slot {s} calls node {called.index(max(called))} {max(called)} times'
    return None


def per_lead(n):
    """Each lead its own shuffle, drawn from its node number - a breakage."""
    cache = {}

    def of(i):
        if i not in cache:
            cache[i] = order(JOB ^ (i + 1), STEP, n)
        return cache[i]
    return of


def in_order(n):
    o = list(range(1, (n - 1) // 2 + 1))
    if n % 2 == 0:
        o.append(n // 2)
    return o


def ms(x):
    return f'{x * 1e3:9.1f} ms'


def table(counts):
    print('The Pi-like link: 100 us each way, 1 Gbit/s; a connection 35 us of CPU at the caller')
    print('and 25 at the callee; two wires a pair; the slotted schedule at W = 8 calls in')
    print('flight - four pairs - and slots of 100 us. One CPU a lead, as on a cluster.')
    print('"SYN": the most reaching one listener within a round trip; "wait": the most')
    print('connections waiting at one lead for the answer to their hello.\n')
    for title, entry_of in (('Every lead enters at once', lambda n: None),
                            ("Each lead enters as the rendezvous's tables reach it", tree_entry)):
        print(title)
        print(f"  {'nodes':>6}{'today':>30}{'slotted':>30}{'faster':>9}")
        for n in counts:
            e = entry_of(n)
            a = simulate(n, TODAY, entry=e)
            b = simulate(n, SLOTTED, entry=e)
            cell = lambda r: f"{ms(r['done'])} {r['syn_most']:3d} SYN {r['waiting_most']:4d} wait"
            print(f"  {n:>6}{cell(a):>30}{cell(b):>30}{a['done'] / b['done']:8.1f}x", flush=True)
        print()


def check():
    """The schedule's spread, its backlog and the speed claim, each property
    with what fails it."""
    ok = True

    def judge(name, failure, expect):
        nonlocal ok
        held = failure is None
        wrong = engine.judge(held, engine.named(failure), expect)
        mark = '' if wrong is None else '   <-- UNEXPECTED' + wrong
        print(f'mesh_timing/{name}: {"holds" if held else "FAILS " + failure}{mark}', flush=True)
        ok &= wrong is None

    # PERMUTATION: each slot calls every listener at most once - the order the
    # same on every lead; each lead its own fails.
    failure = None
    for n in list(range(2, 130)) + [254, 255, 1024, 4097]:
        failure = failure or permutation_slots(n, order(JOB, STEP, n))
    judge('permutation/shared-order', failure, True)
    judge('permutation/per-lead-order', permutation_slots(64, per_lead(64)), 'PERMUTATION')

    # BACKLOG: the most connections that can wait at one lead - every caller
    # the rule gives it, both wires - within its listen backlog, min(N, 4096),
    # to the connection budget's ceiling of 4,097 nodes. Arithmetic, which
    # before H23 did not hold: the last lead had 2(N - 1) callers.
    failure = None
    for n in range(2, 4098):
        most = 2 * (n // 2)                 # an even N's upper half: N / 2 callers
        if most > min(n, 4096):
            failure = f'BACKLOG: {n} nodes: {most} callers, a backlog of {min(n, 4096)}'
            break
    judge('backlog', failure, True)

    # FASTER: the schedule at least four times today's order on the Pi-like
    # link, one CPU a lead, at 64 and 256 nodes, every lead entering at once
    # and as the tables arrive; today's order put through the same check fails
    # it, so the check can.
    def faster(schedule):
        for n in (64, 256):
            for entry in (None, tree_entry(n)):
                a, b = simulate(n, TODAY, entry=entry), simulate(n, schedule, entry=entry)
                if a['done'] < 4 * b['done']:
                    return (f"FASTER: {n} nodes: {schedule} {ms(b['done']).strip()}, today's "
                            f"order {ms(a['done']).strip()}")
        return None
    judge('faster/slotted', faster(SLOTTED), True)
    judge('faster/today', faster(TODAY), 'FASTER')

    # SPREAD: as the rendezvous's tables reach the leads, level by level, at
    # most 20 SYNs a round trip and 20 waiting connections at any listener, at
    # 256 nodes. In increasing order the callers of a late level call its
    # leads one after another, together.
    def spread(slot_order):
        r = simulate(256, SLOTTED, entry=tree_entry(256), slot_order=slot_order)
        if r['syn_most'] > 20 or r['waiting_most'] > 20:
            return (f"SPREAD: {r['syn_most']} SYNs a round trip and {r['waiting_most']} "
                    f"waiting at one listener")
        return None
    judge('spread/shuffled', spread(None), True)
    judge('spread/in-order', spread(in_order(256)), 'SPREAD')
    return ok


# Tier 3's runs of 2026-10-19 (H23): the slowest lead's mesh in ms over the
# Pi-like link, the chain's three rounds and the schedule's, alternating on one
# idle development VM of four CPUs (mesh-timing.txt in the evidence).
TIER3_MEASURED = {16: ((24.5, 22.0, 21.4), (5.0, 5.1, 4.9)),
                  32: ((51.6, 49.1, 49.7), (19.7, 19.4, 18.4)),
                  64: ((156.2, 173.0, 161.1), (87.7, 104.8, 81.6))}


def tier3():
    print('Tier 3 on the development VM: four CPUs for every lead, a wake of 50 us before each')
    print('piece of a lead\'s work; the model against the runs, ms.\n')
    print(f"  {'nodes':>6}{'chain':>10}{'measured':>22}{'schedule':>10}{'measured':>22}")
    for n, (chain, schedule) in TIER3_MEASURED.items():
        a = simulate(n, TODAY, cores=4, wake=50e-6)['done'] * 1e3
        b = simulate(n, SLOTTED, cores=4, wake=50e-6)['done'] * 1e3
        fmt = lambda xs: ' '.join(f'{x:6.1f}' for x in xs)
        print(f'  {n:>6}{a:10.1f}{fmt(chain):>22}{b:10.1f}{fmt(schedule):>22}')


def main(argv):
    if argv[:1] == ['table']:
        counts = [int(x) for x in argv[1:]] or [64, 128, 256, 512, 1024, 2048, 4097]
        table(counts)
        return 0
    if argv[:1] == ['tier3']:
        tier3()
        return 0
    if argv[:1] in (['check'], []):
        return 0 if check() else 1
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
