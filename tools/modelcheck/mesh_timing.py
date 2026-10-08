#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Timing of the mesh's start-up (design §6.2, wire.c osoe_wires_open), for
docs/design/mesh-schedule-proposal.md: the order osoe makes its node-pair
connections in today, against a slotted schedule.

Each lead is one thread, a serial server. A call is: the caller dials (its
CPU), the kernel's handshake (a round trip), the hello (one way), the answerer
reads and answers it (its CPU), the answer (one way), the caller confirms and
attaches the wire (its CPU), the confirmation (one way), the answerer confirms
and attaches (its CPU). A lead that has not reached the mesh yet answers
nothing; its kernel still completes the handshakes, which wait in its backlog.

current:  a lead accepts every lower node, then calls every higher node, one
          call at a time, in increasing order.
rotation: lead i calls (i + d) mod N for d = 1 .. (N - 1) // 2, and N/2 from
          the lower lead when N is even - every lead about N/2 calls - its
          call d = s + 1 in slot s, a slot starting every `slot` seconds after
          the lead reaches the mesh, at most W calls in flight, the listener
          answered throughout.
shuffled: the rotation's slots with the offsets in one shuffled order, the
          same on every lead, so callers held up by a late lead do not all
          call the same next one.

Measured: the time until every wire is attached; the most SYNs reaching one
listener within one round trip; the most connections waiting at one lead
(handshake done, hello not yet answered).

Two wires a pair (v0.2 step 4, Part 2; docs/design/progress-helpers-proposal.md
§3.1): `wires=2` makes every call twice in a row - the control wire, then the
data wire - so a lead accepts and makes twice the calls, in the same order.
"""
import heapq, random

def run(n, schedule, in_flight_most=4, slot=100e-6, round_trip=100e-6, answer_cpu=(10e-6, 15e-6),
        call_cpu=(10e-6, 5e-6, 20e-6), entry=None, wires=1):
    one_way = round_trip / 2
    hello_cpu, confirm_cpu = answer_cpu
    dial_cpu, send_cpu, attach_cpu = call_cpu
    calls = []
    if schedule == 'current':
        calls = [list(range(i + 1, n)) for i in range(n)]
    else:
        offsets = list(range(1, (n - 1) // 2 + 1))
        if schedule == 'shuffled':
            random.Random(n).shuffle(offsets)          # a fixed seed: every lead the same order
        for i in range(n):
            mine = [(i + d) % n for d in offsets]
            if n % 2 == 0 and i < n // 2:
                mine.append(i + n // 2)
            calls.append(mine)
    calls = [[j for j in mine for _ in range(wires)] for mine in calls]
    expected = [0] * n
    for i in range(n):
        for j in calls[i]:
            expected[j] += 1
    entry = entry or [0.0] * n
    events, sequence = [], 0
    def at(t, kind, *args):
        nonlocal sequence
        heapq.heappush(events, (t, sequence, kind, args))
        sequence += 1
    busy_until = list(entry)                # a lead answers nothing before it enters
    def work(lead, t, cost, kind, *args):
        start = max(t, busy_until[lead])
        busy_until[lead] = start + cost
        at(start + cost, kind, *args)
    answered_in, made, next_call, flying = [0] * n, [0] * n, [0] * n, [0] * n
    waiting, waiting_most = [0] * n, 0
    syn_arrivals = [[] for _ in range(n)]
    woken = [False] * n
    done = [None] * n
    def call_more(i, t):
        if schedule == 'current' and answered_in[i] < expected[i]:
            return
        most = 1 if schedule == 'current' else in_flight_most
        while next_call[i] < len(calls[i]) and flying[i] < most:
            if schedule != 'current':
                due = entry[i] + next_call[i] * slot
                if due > t + 1e-12:
                    if not woken[i]:
                        woken[i] = True
                        at(due, 'slot', i)
                    return
            j = calls[i][next_call[i]]
            next_call[i] += 1
            flying[i] += 1
            work(i, t, dial_cpu, 'dialled', i, j)
    def finished(i, t):
        if done[i] is None and answered_in[i] == expected[i] and made[i] == len(calls[i]):
            done[i] = t
    for i in range(n):
        at(entry[i], 'enter', i)
    while events:
        t, _, kind, args = heapq.heappop(events)
        if kind == 'slot':
            woken[args[0]] = False
            call_more(args[0], t)
        elif kind == 'enter':
            call_more(args[0], t)
            finished(args[0], t)
        elif kind == 'dialled':
            i, j = args
            waiting[j] += 1
            waiting_most = max(waiting_most, waiting[j])
            syn_arrivals[j].append(t + one_way)
            at(t + round_trip, 'connected', i, j)
        elif kind == 'connected':
            work(args[0], t, send_cpu, 'hello sent', *args)
        elif kind == 'hello sent':
            at(t + one_way, 'hello in', *args)
        elif kind == 'hello in':
            work(args[1], t, hello_cpu, 'answered', *args)
        elif kind == 'answered':
            waiting[args[1]] -= 1
            at(t + one_way, 'answer in', *args)
        elif kind == 'answer in':
            work(args[0], t, send_cpu + attach_cpu, 'confirmed', *args)
        elif kind == 'confirmed':
            i, j = args
            made[i] += 1
            flying[i] -= 1
            at(t + one_way, 'confirmation in', i, j)
            call_more(i, t)
            finished(i, t)
        elif kind == 'confirmation in':
            work(args[1], t, confirm_cpu, 'attached', *args)
        elif kind == 'attached':
            j = args[1]
            answered_in[j] += 1
            call_more(j, t)
            finished(j, t)
    assert all(d is not None for d in done)
    syn_most = 0
    for arrivals in syn_arrivals:
        arrivals.sort()
        low = 0
        for high in range(len(arrivals)):
            while arrivals[high] - arrivals[low] > round_trip:
                low += 1
            syn_most = max(syn_most, high - low + 1)
    return max(done), syn_most, waiting_most

def table(title, cases):
    print(title)
    print(f"  {'':<26}{'current':>28}{'rotation':>28}{'shuffled':>28}")
    for label, n, options in cases:
        cells = []
        for schedule in ('current', 'rotation', 'shuffled'):
            t, syn, wait = run(n, schedule, **options)
            cells.append(f"{t * 1e3:8.1f} ms {syn:3d} SYN {wait:4d} wait")
        print(f"  {label:<26}" + "".join(f"{c:>28}" for c in cells))
    print()

if __name__ == '__main__':
    print("A slot of 100 us, 4 calls in flight; a round trip of 100 us; a call's CPU 25 us at the")
    print("answerer, 35 us at the caller. 'SYN': the most reaching one listener within a round")
    print("trip; 'wait': the most connections waiting at one lead for their hello's answer.\n")
    table("On schedule: every lead enters the mesh at once",
          [(f"{n} nodes", n, {}) for n in (4, 16, 64, 256, 1024, 2048)])
    def skewed(n, seed):
        draw = random.Random(seed)
        return [draw.uniform(0, 10e-3) for _ in range(n)]
    table("Entry skew: leads enter uniformly within 10 ms (two draws)",
          [(f"{n} nodes, draw {seed}", n, {'entry': skewed(n, seed)})
           for n in (64, 1024) for seed in (7, 8)])
    def late(n, which):
        entry = [0.0] * n
        entry[which] = 20e-3
        return entry
    table("One lead 20 ms late",
          [(f"{n} nodes, lead {w}", n, {'entry': late(n, w)}) for n in (256, 1024) for w in (1, n // 4)])
    table("A round trip of 1 ms (a slow network), on schedule",
          [(f"{n} nodes", n, {'round_trip': 1e-3}) for n in (64, 1024)])
    table("Two wires a pair - the control wire, then the data wire - on schedule",
          [(f"{n} nodes, {w} wire{'s' if w > 1 else ''}", n, {'wires': w})
           for n in (4, 16, 64, 256, 1024) for w in (1, 2)])
