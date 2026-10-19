#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The mesh's start-up schedule: who calls whom, and that every lead's calls
and accepts end in a whole mesh (design §6.2; docs/design/mesh-schedule-proposal.md,
built by the author's decision A8 of 2026-10-11; H23).

The schedule. Lead i calls lead (i + d) mod N at each offset d = 1 ..
floor((N - 1) / 2), and for an even N the offset N / 2 from the lower lead of
each opposite pair: every pair has exactly one caller, and every lead makes
about N / 2 calls and answers the rest, where before it lead 0 made N - 1 and
the last lead none. One offset a slot, the same on every lead, so each slot is
a permutation - every listener is called once in it, but for the opposite
pairs' slot, where the upper half are called and the lower half call. The
slots' offsets in one shuffled order every lead computes alike from the job's
ids (`order`; `digest` writes the orders into
`shmem/tests/mesh-schedule-digest.txt`, which model.mesh.digest holds to what
the model writes now and shmem.mesh-schedule holds mesh-schedule.c to). Each
pair connects twice, the control wire and the data wire (§3.1), both calls in
the pair's slot. The C is mesh-schedule.c and wire.c's osoe_wires_open, its
calls listener.c's calls part-way.

The protocol. Each lead runs one loop that answers its listener and drives its
own calls, at most W in flight, a call begun no earlier than its slot - a slot
says "not before", and a call that waited for a place in the window is made
late, never skipped. A connection, as listener.c makes one:

  0  not begun
  1  dialled: the callee's kernel has completed the handshake, and the hello
     waits in its backlog - whether or not the callee has reached the mesh
  2  answered: the callee accepted it, read the hello, judged it by the
     schedule's rule, and answered; it holds one of the callee's places for
     callers part-way through the hello (P; 16 in listener.c)
  3  confirmed: the caller read the answer, confirmed and attached its wire
  4  attached: the callee read the confirmation and attached its own end

A lead reaches the mesh at any moment after its listener is listening (it
advertised its port before the rendezvous carried it), so callers wait in its
backlog until it does. It ends its loop and closes its listener once every call
of its own is confirmed and every caller the rule gives it has attached; a
connection refused by a closed listener, or dropped from a full backlog, is
dialled again by the kernel or the caller, which changes no state here. Each
step is a transition: the state graph has no cycle, so a lead never waits for
ever exactly when every terminal state is the whole mesh.

Checked:
  RULE   no lead refuses a caller: the callee's check is the caller's rule.
  ONCE   no wire attached twice at either end - the C's "connected to twice".
  STUCK  no terminal state with a lead still in its loop: a cycle of calls
         each waiting for an answer, or a caller whose callee has stopped
         answering, waits for ever.
  MESH   every terminal state has every pair's every wire at both ends.

Variants
  design            the schedule and the loop above.
  today             the order before H23: a lead accepts every lower node, then
                    calls every higher one, one blocking call at a time
                    (wire.c as it was) - which holds: its calls form no cycle.
  blocking          the circulant's calls made as today's are, one at a time,
                    answering nothing while a call waits: the offset-1 calls
                    are a cycle, each lead waiting for the next. Fails STUCK.
  lower_check       the hello's check left as it was, "not a lower node": the
                    calls that wrap past the last lead are refused. Fails RULE.
  opposite_both     both leads of an opposite pair call. Fails ONCE.
  opposite_neither  neither does. Fails MESH.
  close_early       a lead closes its listener once its own calls are done and
                    no caller holds a place, with callers still to come. Fails
                    STUCK.
  skip_late         a call still waiting for a place in the window when its
                    slot's time has passed by a whole slot is dropped, as a loop
                    that calls only the current slot's offset would. Fails
                    STUCK.

Every order of the offsets is checked at up to six leads, so the design holds
whatever the shuffle; the shuffle buys spread in time (mesh_timing.py), not
correctness. Usage: mesh_model.py [check] | digest | digest-compare FILE
"""
import sys

import engine

MASK = (1 << 64) - 1


def mix(x):
    """listener.c's mix, MurmurHash3's 64-bit finalizer."""
    x &= MASK
    x ^= x >> 33
    x = (x * 0xff51afd7ed558ccd) & MASK
    x ^= x >> 33
    x = (x * 0xc4ceb9fe1a85ec53) & MASK
    x ^= x >> 33
    return x


def offsets(n):
    """The offsets a lead calls at, one a slot, in increasing order."""
    out = list(range(1, (n - 1) // 2 + 1))
    if n % 2 == 0:
        out.append(n // 2)
    return out


def order(job, step, n):
    """The slots' offsets in the job's order, the same on every lead: a
    Fisher-Yates shuffle drawn from the job's ids by listener.c's mix."""
    d = offsets(n)
    seed = mix(job ^ mix(((step & 0xffffffff) << 32) | n))
    for k in range(len(d) - 1, 0, -1):
        r = mix(seed ^ k) % (k + 1)
        d[k], d[r] = d[r], d[k]
    return d


def calls(i, j, n):
    """Whether lead i calls lead j: the circulant's rule."""
    delta = (j - i) % n
    if 1 <= delta <= (n - 1) // 2:
        return True
    return n % 2 == 0 and delta == n // 2 and i < j


def callee(i, d, n):
    """Whom lead i calls in a slot of offset d, or None."""
    if n % 2 == 0 and d == n // 2 and i >= n // 2:
        return None
    return (i + d) % n


def run(variant, n, wires=1, window=2, places=1, slot_order=None, slots=False, trace=False):
    """Explore one case. Returns (held, failure message, states, trace)."""
    slot_order = slot_order if slot_order is not None else offsets(n)
    opposite = lambda i, j: n % 2 == 0 and (j - i) % n == n // 2
    if variant == 'today':
        rule = lambda i, j: i < j
    elif variant == 'opposite_both':
        rule = lambda i, j: calls(i, j, n) or opposite(i, j)
    elif variant == 'opposite_neither':
        rule = lambda i, j: calls(i, j, n) and not opposite(i, j)
    else:
        rule = lambda i, j: calls(i, j, n)
    check = (lambda i, j: i < j) if variant == 'lower_check' else rule
    blocking = variant in ('today', 'blocking')
    if blocking:
        window = 1
    # Connections: (caller, callee, wire); each lead's calls in its order with
    # the slot each belongs to.
    conns, mine, slot_of = [], [[] for _ in range(n)], []
    for i in range(n):
        if variant == 'today':
            plan = [(j, None) for j in range(i + 1, n)]
        else:
            plan = []
            for s, d in enumerate(slot_order):
                j = callee(i, d, n)
                if j is not None and rule(i, j):
                    plan.append((j, s))
            if variant == 'opposite_both' and n % 2 == 0 and i >= n // 2:
                s = slot_order.index(n // 2)
                plan.append(((i + n // 2) % n, s))
                plan.sort(key=lambda js: js[1])
        for j, s in plan:
            for w in range(wires):
                mine[i].append(len(conns))
                conns.append((i, j, w))
                slot_of.append(s)
    incoming = [[c for c, (a, b, w) in enumerate(conns) if b == j] for j in range(n)]
    expected = [sum(1 for i in range(n) if i != j and rule(i, j)) * wires for j in range(n)]
    total_slots = len(slot_order) if variant != 'today' else 0
    NOT_BEGUN, DIALLED, ANSWERED, CONFIRMED, ATTACHED, SKIPPED = 0, 1, 2, 3, 4, -1
    # Every wire's ends: the connections that attach lead's wire w toward peer.
    ends = {}
    for c, (a, b, w) in enumerate(conns):
        ends.setdefault((a, b, w), ([], []))[0].append(c)     # a's own call
        ends.setdefault((b, a, w), ([], []))[1].append(c)     # a caller of b

    # A state is one integer: each connection's phase + 1 in three bits, then
    # each lead's entered and closed bits, its next call (five bits) and the
    # slots due (four).
    C = len(conns)
    base = 3 * C
    ENTERED, CLOSED, NEXT, TICKS = 0, 1, 2, 7

    def lead_shift(i, field):
        return base + 11 * i + field

    def get(S, i, field, width):
        return (S >> lead_shift(i, field)) & ((1 << width) - 1)

    def put(S, i, field, width, v):
        k = lead_shift(i, field)
        return (S & ~(((1 << width) - 1) << k)) | (v << k)

    def phased(S, c, v):
        return (S & ~(7 << 3 * c)) | ((v + 1) << 3 * c)

    assert all(len(m) < 32 for m in mine) and total_slots < 16
    start = 0
    for c in range(C):
        start = phased(start, c, NOT_BEGUN)
    for i in range(n):
        start = put(start, i, TICKS, 4, 0 if slots else total_slots)

    def attached(phases, lead, peer, w):
        """How many of lead's ends of wire w toward peer are attached."""
        own, called = ends.get((lead, peer, w), ((), ()))
        return (sum(1 for c in own if phases[c] >= CONFIRMED) +
                sum(1 for c in called if phases[c] == ATTACHED))

    def successors(S):
        """[(state, label)], or a failure message for a refusal or a double."""
        phases = [((S >> 3 * c) & 7) - 1 for c in range(C)]
        out = []
        for i in range(n):
            if get(S, i, CLOSED, 1):
                continue
            if not get(S, i, ENTERED, 1):
                out.append((put(S, i, ENTERED, 1, 1), f'{i} reaches the mesh'))
                continue
            ticks, nxt = get(S, i, TICKS, 4), get(S, i, NEXT, 5)
            flying = sum(1 for c in mine[i] if phases[c] in (DIALLED, ANSWERED))
            in_call = blocking and flying > 0
            # The slot clock: a slot more due.
            if ticks < total_slots:
                t, x, T = ticks + 1, nxt, S
                if variant == 'skip_late':
                    while x < len(mine[i]) and slot_of[mine[i][x]] < t - 1:
                        T = phased(T, mine[i][x], SKIPPED)
                        x += 1
                out.append((put(put(T, i, TICKS, 4, t), i, NEXT, 5, x), f'{i}: slot {t - 1} due'))
            # Its next call, when due, with a place in the window.
            if nxt < len(mine[i]) and flying < window:
                c = mine[i][nxt]
                if variant == 'today':
                    due = all(phases[k] == ATTACHED for k in incoming[i])
                else:
                    due = slot_of[c] < ticks
                if due:
                    a, b, w = conns[c]
                    out.append((put(phased(S, c, DIALLED), i, NEXT, 5, nxt + 1),
                                f'{a} dials {b} for wire {w}'))
            # The listener: accept a caller in its backlog while a place is free.
            holding = sum(1 for c in incoming[i] if phases[c] == ANSWERED)
            serving = not in_call and (variant != 'today' or nxt == 0)
            if serving and holding < places:
                for c in incoming[i]:
                    if phases[c] == DIALLED:
                        a, b, w = conns[c]
                        if not check(a, b):
                            return f'RULE: node {b} refused node {a}, a caller its schedule gives it'
                        out.append((phased(S, c, ANSWERED), f'{b} answers {a} for wire {w}'))
            # Its own calls' answers: confirm and attach.
            for c in mine[i]:
                if phases[c] == ANSWERED:
                    a, b, w = conns[c]
                    if attached(phases, a, b, w):
                        return f'ONCE: node {a} attached a second wire {w} toward node {b}'
                    out.append((phased(S, c, CONFIRMED), f'{a} confirms to {b} and attaches wire {w}'))
            # Callers' confirmations: attach.
            if serving:
                for c in incoming[i]:
                    if phases[c] == CONFIRMED:
                        a, b, w = conns[c]
                        if attached(phases, b, a, w):
                            return f'ONCE: node {b} attached a second wire {w} toward node {a}'
                        out.append((phased(S, c, ATTACHED), f'{b} attaches wire {w} from {a}'))
            # The loop's end: every call of its own confirmed (or, skipped,
            # believed made) and every caller the rule gives it attached.
            own_done = nxt == len(mine[i]) and all(
                phases[c] in (CONFIRMED, ATTACHED, SKIPPED) for c in mine[i])
            if own_done:
                callers_in = sum(1 for c in incoming[i] if phases[c] == ATTACHED)
                if callers_in == expected[i]:
                    out.append((put(S, i, CLOSED, 1, 1), f'{i} closes its listener'))
                elif variant == 'close_early' and not any(
                        phases[c] in (ANSWERED, CONFIRMED) for c in incoming[i]):
                    out.append((put(S, i, CLOSED, 1, 1), f'{i} closes its listener early'))
        return out

    def terminal_failure(S):
        if not all(get(S, i, CLOSED, 1) for i in range(n)):
            waiting = [i for i in range(n) if not get(S, i, CLOSED, 1)]
            return f'STUCK: node {waiting[0]} waits for ever ({len(waiting)} of {n} still in the mesh)'
        phases = [((S >> 3 * c) & 7) - 1 for c in range(C)]
        for i in range(n):
            for j in range(n):
                if i != j:
                    for w in range(wires):
                        if attached(phases, i, j, w) != 1:
                            return f'MESH: node {i} has no wire {w} toward node {j}'
        return None

    # Depth first; the parents kept only for a trace, rerun when one is due.
    seen = {start: None} if trace else {start}
    stack = [start]
    while stack:
        S = stack.pop()
        nexts = successors(S)
        failure = nexts if isinstance(nexts, str) else (
            terminal_failure(S) if not nexts else None)
        if failure:
            if not trace:
                return run(variant, n, wires, window, places, slot_order, slots, trace=True)
            steps = []
            while seen[S]:
                S, label = seen[S]
                steps.append(label)
            return False, failure, len(seen), list(reversed(steps))
        for T, label in nexts:
            if T not in seen:
                if trace:
                    seen[T] = (S, label)
                else:
                    seen.add(T)
                stack.append(T)
    return True, None, len(seen), []


def permutations(items):
    if len(items) <= 1:
        yield list(items)
        return
    for k in range(len(items)):
        for rest in permutations(items[:k] + items[k + 1:]):
            yield [items[k]] + rest


def cases():
    """(name, variant, options, expect). Every order of the offsets at each
    count; two wires a pair at two and three leads, one beyond - the two calls
    of a pair are the same steps twice, and at three leads both are in flight
    at once - and the slot clock where a call can wait past its slot. Six
    leads are past what a search of every interleaving can hold (1.8 GB after
    nine minutes, unfinished, at one call in flight); five already have two
    offsets, each a cycle through every lead."""
    out = []
    for n in range(2, 6):
        wires = 2 if n <= 3 else 1
        for k, o in enumerate(permutations(offsets(n))):
            out.append((f'design/n{n}/order{k}/wires{wires}/window2/places1', 'design',
                        dict(n=n, wires=wires, window=2, places=1, slot_order=o), True))
    for n in (3, 4, 5):
        wires = 2 if n <= 3 else 1
        out.append((f'design/n{n}/wires{wires}/window1/places1', 'design',
                    dict(n=n, wires=wires, window=1, places=1), True))
    for n in (3, 4):
        wires = 2 if n <= 3 else 1
        out.append((f'design/n{n}/wires{wires}/window4/places16', 'design',
                    dict(n=n, wires=wires, window=4, places=16), True))
    out.append(('design/n3/wires2/window1/slots', 'design',
                dict(n=3, wires=2, window=1, places=1, slots=True), True))
    out.append(('design/n4/wires1/window1/slots', 'design',
                dict(n=4, wires=1, window=1, places=1, slots=True), True))
    out.append(('design/n5/wires1/window1/slots', 'design',
                dict(n=5, wires=1, window=1, places=1, slots=True), True))
    for n in (2, 3, 4, 5):
        out.append((f'today/n{n}/wires2', 'today', dict(n=n, wires=2, places=1), True))
    out += [
        ('blocking/n3', 'blocking', dict(n=3, wires=1, places=1), 'STUCK'),
        ('blocking/n5', 'blocking', dict(n=5, wires=1, places=1), 'STUCK'),
        ('lower_check/n3', 'lower_check', dict(n=3, wires=1, places=1), 'RULE'),
        ('opposite_both/n2', 'opposite_both', dict(n=2, wires=1, places=1), 'ONCE'),
        ('opposite_both/n4', 'opposite_both', dict(n=4, wires=1, places=1), 'ONCE'),
        ('opposite_neither/n4', 'opposite_neither', dict(n=4, wires=1, places=1), 'MESH'),
        ('close_early/n3', 'close_early', dict(n=3, wires=1, places=1), 'STUCK'),
        ('skip_late/n4', 'skip_late', dict(n=4, wires=2, window=1, places=1, slots=True),
         'STUCK'),
    ]
    return out


def check(names):
    ok = True
    for name, variant, options, expect in cases():
        if names and not any(name.startswith(x) for x in names):
            continue
        held, failure, states, trace = run(variant, **options)
        wrong = engine.judge(held, engine.named(failure), expect)
        mark = '' if wrong is None else '   <-- UNEXPECTED' + wrong
        print(f'mesh/{name}: {"holds" if held else "FAILS " + failure} ({states} states){mark}',
              flush=True)
        if trace and wrong is not None:
            print('   trace: ' + ' ; '.join(trace))
        elif trace:
            print('   trace: ' + ' ; '.join(trace[-12:]))
        ok &= wrong is None
    return ok


# The digest of the slots' orders: for each (job, step, node count), the
# number of slots, a hash of the order by listener.c's mix, and its first
# twelve offsets.
DIGEST_JOBS = ((0, 0), (1, 0), (1678987, 0), (0x123456789abcdef0, 7), (MASK, 0xffffffff))
DIGEST_COUNTS = tuple(range(1, 41)) + (63, 64, 65, 127, 128, 254, 255, 256, 1023, 1024, 1025,
                                       2048, 4095, 4096, 4097, 8192)


def digest_lines():
    lines = ['# job step nodes slots digest first: offsets (mesh_model.py digest; '
             'shmem.mesh-schedule.digest)']
    for job, step in DIGEST_JOBS:
        for n in DIGEST_COUNTS:
            o = order(job, step, n)
            h = 0
            for d in o:
                h = mix(h ^ d)
            head = ''.join(f' {d}' for d in o[:12])
            lines.append(f'{job} {step} {n} {len(o)} {h:016x}{head}')
    return lines


def main(argv):
    if argv[:1] == ['digest']:
        print('\n'.join(digest_lines()))
        return 0
    if argv[:1] == ['digest-compare']:
        with open(argv[1]) as f:
            have = f.read().splitlines()
        want = digest_lines()
        if have == want:
            print(f'mesh/digest: {argv[1]} is what mesh_model.py writes ({len(want) - 1} orders)')
            return 0
        for k, (a, b) in enumerate(zip(have, want)):
            if a != b:
                print(f'mesh/digest: line {k + 1} differs:\n  file:  {a}\n  model: {b}')
                break
        else:
            print(f'mesh/digest: {len(have)} lines in the file, {len(want)} from the model')
        return 1
    names = argv[1:] if argv[:1] == ['check'] else argv
    return 0 if check(names) else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
