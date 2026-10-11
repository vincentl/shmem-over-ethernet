#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The cost of a collective's shape on a link (v0.2 step 2): which fan-out k and
window W a large reduction or broadcast should run its internode tree with, from
three numbers measured at start-up - the hop h, the message cost o and the
bandwidth B (docs/prompts/done/v0.2-step2-tree-shape-prompt.md, Part 1; the record
docs/decisions/2026-10-06-tree-shape.md).

One rule, in three forms, in the form of grant_timing.py:

  simulate()  the loop of internode-generic.c's in_pieces over a k-ary tree of
              node leads, event by event. A lead sends piece `up` once every
              child's copy of it is in and fewer than W pieces are up without
              their release; it takes release `down` when it is in, copies it
              to each child, then counts it; it takes whichever is ready. A
              reduction's pieces go up with their bytes and come down with the
              result; a broadcast's go up empty - a credit, the child's slot is
              free - and come down with the data. A message costs its sender o,
              then its bytes cross the links, then h; one without bytes - a
              credit, an add - crosses at once. Two ways for the links to share:
                'sender'      each lead's link out carries one message at a
                              time, in the order its one progress thread sent
                              them; the senders into one lead share its link in
                              fairly, as a switch's port merges them. Fitted to
                              the Pi's table: the reference.
                'connection'  each connection is a stream, its messages in
                              order; connections share both links fairly. The
                              other end of what the hardware may do: on four
                              nodes the table cannot tell the two apart beyond
                              the noise, and on more they disagree.
  cost()      the closed form the chooser computes (collective-shape.c), in
              integer nanoseconds and bytes, the same arithmetic in both
              languages: the first piece's way through the tree, then every
              later piece at the pace of the busiest link or of the window's
              loop, whichever is slower. Its choices sit between the two link
              models': choosing by it costs less, against either, than choosing
              by one and running under the other.
  tree_terms()  what cost() needs of a tree, in closed form, as the C computes
              it, from the tree being full to its last level; checked against
              tree_terms_walk(), which walks every node.

Usage:
  shape_timing.py [check]   the test (the models preset): every check below,
                            then the broken variants, each of which must fail
  shape_timing.py fit       fit h, o and B to the Pi's table and report
  shape_timing.py grid FILE write the chooser's answers on a grid of inputs,
                            for collective-shape.c's test (shmem.shape.grid)
  shape_timing.py grid-compare FILE   check FILE is what `grid` writes now
"""
import heapq
import os
import statistics
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
EVIDENCE = os.path.join(HERE, '..', '..', 'docs', 'design', 'evidence', 'pi-cluster')

# The record's constants: the fit of simulate() to the Pi's table of
# 2026-09-26, in microseconds and bytes a microsecond, and what that fit
# lands within the noise of that table and of step 2's sweep of 1 to 16 KiB
# (2026-10-06; 253 and 73 lines) - refitted to both, the best is h 89, o 4,
# B 116, six lines more of 558 and the same choice on 854 of the grid's 858
# large calls (Part 2's record), so the fit stays. The piece then carried
# 16,368 bytes, 16 KiB less its header; today's carries 16 KiB
# (internal-region.h, OSOE_INTERNAL_PIECE_BYTES).
PI_HOP, PI_MESSAGE, PI_BANDWIDTH = 86.0, 7.0, 116.0
PI_PIECE = 16368
PI_WITHIN = 326
PIECE = 16384


def tree(nodes, k):
    """The k-ary tree over `nodes` leads in level order, as collective-tree.c
    builds it: parent, children and depth of each."""
    parent = [None] + [(i - 1) // k for i in range(1, nodes)]
    children = [[j for j in range(k * i + 1, min(k * i + k + 1, nodes))] for i in range(nodes)]
    depth = [0] * nodes
    for i in range(1, nodes):
        depth[i] = depth[parent[i]] + 1
    return parent, children, depth


class Network:
    """The events and the links both simulations run on. A message of no bytes
    - a credit, an add - crosses at once and arrives h later; one with bytes
    waits its turn and shares the links as `links` says, then h:
      'sender'      each lead's link out carries one message at a time, in the
                    order its one progress thread sent them; the senders into
                    one lead share its link in fairly, as a switch's port
                    merges them.
      'connection'  each connection is a stream, its messages in order;
                    connections share both links fairly, max-min.
    count_links, a dict, gets the messages with bytes each link carried."""

    def __init__(self, h, B, links='sender', count_links=None):
        self.h, self.B, self.links, self.count_links = h, B, links, count_links
        self.events, self.order = [], 0
        self.flight, self.waiting = {}, {}      # queue -> [remaining, sender, receiver, (f, a)]
        self.clock, self.generation = 0.0, 0

    def at(self, t, f, *a):
        heapq.heappush(self.events, (t, self.order, f, a))
        self.order += 1

    def run(self):
        while self.events:
            t, _, f, a = heapq.heappop(self.events)
            f(*a, t)

    def queue_of(self, u, v):
        return u if self.links == 'sender' else (u, v)

    def rates(self):
        flight, B = self.flight, self.B
        if self.links == 'sender':
            into = {}
            for x in flight.values():
                into[x[2]] = into.get(x[2], 0) + 1
            return {q: B / into[x[2]] for q, x in flight.items()}
        load = {}
        for q, x in flight.items():
            load.setdefault(('out', x[1]), []).append(q)
            load.setdefault(('in', x[2]), []).append(q)
        rate = {q: 0.0 for q in flight}
        frozen, spare = set(), {link: B for link in load}
        while len(frozen) < len(flight):
            share, link = min((spare[l] / len([q for q in qs if q not in frozen]), l)
                              for l, qs in load.items() if any(q not in frozen for q in qs))
            for l, qs in load.items():
                spare[l] -= share * len([q for q in qs if q not in frozen])
            for q in rate:
                if q not in frozen:
                    rate[q] += share
            frozen.update(load[link])
        return rate

    def advance(self, now):
        dt = now - self.clock
        if dt > 0 and self.flight:
            r = self.rates()
            for q, x in self.flight.items():
                x[0] -= r[q] * dt
        self.clock = now

    def plan(self, now):
        self.generation += 1
        if self.flight:
            r = self.rates()
            due, q = min((x[0] / r[q], q) for q, x in self.flight.items())
            self.at(now + max(0.0, due), self.landed, self.generation, q)

    def landed(self, g, due, now):
        if g != self.generation:
            return
        self.advance(now)
        # The transfer this event was planned for lands whatever is left of it:
        # far into a run, what is left can be a fraction of a byte too small
        # for the clock to tell from now, and planned again would never land.
        self.flight[due][0] = 0.0
        for q in [q for q, x in self.flight.items() if x[0] <= 1e-9]:
            _, u, v, (f, a) = self.flight.pop(q)
            self.at(now + self.h, f, *a)
            if self.waiting.get(q):
                self.flight[q] = self.waiting[q].pop(0)
        self.plan(now)

    def send(self, u, v, nbytes, now, f, *a):
        if self.count_links is not None and nbytes:
            for key in (('out', u), ('in', v)):
                self.count_links[key] = self.count_links.get(key, 0) + 1
        if not nbytes:
            self.at(now + self.h, f, *a)
            return
        self.advance(now)
        q = self.queue_of(u, v)
        transfer = [float(nbytes), u, v, (f, a)]
        if q in self.flight:
            self.waiting.setdefault(q, []).append(transfer)
            return
        self.flight[q] = transfer
        self.plan(now)


def simulate(op, nodes, k, W, n, s, h, o, B, measure='osu', links='sender', count_links=None):
    """One collective of n bytes in pieces of s; microseconds. measure 'osu':
    the mean per-PE call time after a barrier whose release reaches depth d of
    the tree d hops after the root, as OSU's latency tests report
    (osu_oshm_reduce.c: a timer around the call, a barrier between iterations,
    the per-PE means summed and divided by the PEs); 'completion': every lead
    entering at once, the time the last one finishes. count_links, a dict,
    gets the messages with bytes each lead's link out and in carried."""
    parent, children, depth = tree(nodes, k)
    pieces = max(1, -(-n // s))
    last = n - (pieces - 1) * s if n > 0 else 0

    def piece_bytes(j):
        return s if j < pieces - 1 else last

    net = Network(h, B, links, count_links)
    at, send = net.at, net.send

    entry = [depth[i] * h if measure == 'osu' else 0.0 for i in range(nodes)]
    arrived = [[0] * len(children[i]) for i in range(nodes)]
    released = [0] * nodes
    up = [0] * nodes
    down = [0] * nodes
    busy = [0.0] * nodes
    done = [None] * nodes
    started = [False] * nodes

    def step(i, now):
        if not started[i]:
            return
        now = max(now, busy[i])
        root = parent[i] is None
        while down[i] < pieces:
            sending = (not root) and up[i] < pieces and up[i] < down[i] + W
            if sending and all(c > up[i] for c in arrived[i]):
                now += o
                p = parent[i]
                send(i, p, piece_bytes(up[i]) if op == 'reduce' else 0, now,
                     child_in, p, children[p].index(i))
                up[i] += 1
                continue
            have = all(c > down[i] for c in arrived[i]) if root else released[i] > down[i]
            if have:
                for c in children[i]:
                    now += o
                    send(i, c, piece_bytes(down[i]), now, release_in, c)
                down[i] += 1
                continue
            break
        busy[i] = now
        if down[i] == pieces and done[i] is None:
            done[i] = now

    def child_in(p, position, now):
        arrived[p][position] += 1
        step(p, now)

    def release_in(i, now):
        released[i] += 1
        step(i, now)

    def enter(i, now):
        started[i] = True
        busy[i] = now
        step(i, now)

    for i in range(nodes):
        at(entry[i], enter, i)
    net.run()
    if measure == 'completion':
        return max(done)
    return sum(done[i] - entry[i] for i in range(nodes)) / nodes


# ---- What the closed form needs of a tree ----------------------------------

def frontier(paths):
    """The (hops, piece times, messages) triples no other triple dominates in
    all three: the only paths that can be the slowest whatever h, t and o are."""
    paths = set(paths)
    return sorted(p for p in paths
                  if not any(q != p and q[0] >= p[0] and q[1] >= p[1] and q[2] >= p[2]
                             for q in paths))


def tree_terms_walk(op, nodes, k, interior=1):
    """The reference: every node walked. The frontier of the first piece's way
    (up, then down, for a reduction; credits up, then the data down, for a
    broadcast), the frontier of the window's loop, and beta, the most messages
    with bytes any lead's link carries for one piece of the vector. `interior`
    is what an interior lead's parent adds to its children's count: 1, or 0
    in the broken variant."""
    parent, children, depth = tree(nodes, k)
    down = [[(0, 0, 0)] for _ in range(nodes)]
    for i in range(nodes):
        for j, c in enumerate(children[i]):
            down[c] = frontier([(a + 1, b + j + 1, m + j + 1) for a, b, m in down[i]])
    leaves = [i for i in range(nodes) if not children[i]]
    down_root = frontier([p for i in leaves for p in down[i]])
    if op == 'reduce':
        # up: a parent holds its children's pieces once the slowest child's is
        # sent and all of them have crossed its one link in, then a hop
        up = [None] * nodes
        for i in reversed(range(nodes)):
            if not children[i]:
                up[i] = [(0, 0, 0)]
                continue
            c = len(children[i])
            up[i] = frontier([(a + 1, b + c, m + 1) for x in children[i] for a, b, m in up[x]])
        first = frontier([(a + a2, b + b2, m + m2) for a, b, m in up[0] for a2, b2, m2 in down_root])
        beta = max(len(children[i]) + (interior if parent[i] is not None else 0)
                   for i in range(nodes))
    else:
        top = max(depth)
        first = frontier([(a + top, b, m) for a, b, m in down_root])
        beta = max(len(children[i]) for i in range(nodes))
    # The window's loop at a leaf: at each level of its way, a hop up and one
    # down, its piece up (a reduction's) and its copy down, behind the copies
    # to the siblings before it.
    position = [0] * nodes
    for i in range(nodes):
        for j, c in enumerate(children[i]):
            position[c] = j
    loops = []
    for leaf in leaves:
        a = b = m = 0
        i = leaf
        while parent[i] is not None:
            a += 2
            b += (2 if op == 'reduce' else 1) + position[i]
            m += 2 if op == 'reduce' else 1
            i = parent[i]
        loops.append((a, b, m))
    return first, frontier(loops), beta


def depth_of(nodes, k):
    """collective-tree.c's osoe_collective_tree_depth."""
    d, span, total = 0, 1, 1
    while total < nodes:
        span *= k
        total += span
        d += 1
    return d


def max_digit_sum(limit, k, digits):
    """The largest base-k digit sum of a number 0 .. limit, `digits` digits:
    limit's own, or one of limit's digits one less and every lower one k - 1."""
    ds = [(limit // k ** i) % k for i in range(digits)]       # least significant first
    best = sum(ds)
    for i in range(digits):
        if ds[i] > 0:
            best = max(best, sum(ds[i + 1:]) + ds[i] - 1 + (k - 1) * i)
    return best


def tree_terms(op, nodes, k, interior=1):
    """tree_terms_walk()'s answer without the walk, as collective-shape.c
    computes it. A level-order k-ary tree is full to its last level but one,
    L - 1, and partial at L; a node's positions on its way from the root are
    the base-k digits of its offset in its level. So the slowest way down
    ends at the level-L leaf of largest digit sum, or at the last leaf of
    level L - 1 if that one has no children; the slowest way up starts under
    the first parent of level L - 1, which has the most children; and only a
    parent at a level below L - 1, or that first parent, can be the busiest."""
    L = depth_of(nodes, k)
    full = (k ** L - 1) // (k - 1)          # the nodes of levels 0 .. L-1
    last = nodes - full                     # the nodes of level L, 1 .. k^L
    ways = [(L, L + max_digit_sum(last - 1, k, L))]
    if -(-last // k) < k ** (L - 1):        # the last node of level L-1 is a leaf
        ways.append((L - 1, (L - 1) * k))
    down = frontier([(a, s, s) for a, s in ways])
    c = 2 if op == 'reduce' else 1
    loops = frontier([(2 * a, a * c + (s - a), a * c) for a, s, _ in down])
    beta = min(k, nodes - 1)
    if op == 'reduce':
        up = (L, min(k, last) + (L - 1) * k, L)
        first = frontier([(up[0] + a, up[1] + b, up[2] + m) for a, b, m in down])
        if L >= 3:
            beta = max(beta, k + interior)
        if L >= 2:
            beta = max(beta, min(k, last) + interior)
    else:
        first = frontier([(L + a, b, m) for a, b, m in down])
    return first, loops, beta


# ---- The closed form and the chooser ---------------------------------------

SATURATED = (1 << 64) - 1


def cost(op, nodes, k, W, n, s, h_ns, o_ns, bytes_per_second, interior=1):
    """The closed form, nanoseconds, integers alone - collective-shape.c's
    osoe_shape_cost, line for line, saturating at 2**64 - 1 as the C does."""
    if nodes <= 1 or n == 0:
        return 0
    if k < 2 or W == 0 or s == 0 or bytes_per_second == 0:
        return SATURATED
    first, loops, beta = tree_terms(op, nodes, k, interior)
    pieces = (n + s - 1) // s
    t = (min(n, s) * 1000000000 + bytes_per_second - 1) // bytes_per_second
    fill = max(a * h_ns + b * t + m * o_ns for a, b, m in first)
    loop = max(a * h_ns + b * t + m * o_ns for a, b, m in loops)
    period = max(beta * t, (loop + W - 1) // W)
    return min(SATURATED, fill + (pieces - 1) * period)


def choose(op, nodes, n, link, ceiling, capacity, small_bytes, s=PIECE):
    """osoe_shape_choose: (k, W) for one call. A job of one node, a barrier,
    a gather and the small class keep the ceiling's tree, one piece. Otherwise
    W is the capacity - more pieces in flight never cost a large collective
    time in either simulation, and the closed form's window term is its least
    exact - and k the least cost from 2 to the ceiling, ties to the smaller."""
    h_ns, o_ns, bytes_per_second = link
    if nodes <= 1 or n <= small_bytes or op not in ('reduce', 'broadcast'):
        return ceiling, 1
    best = None
    for k in range(2, ceiling + 1):
        c = cost(op, nodes, k, capacity, n, s, h_ns, o_ns, bytes_per_second)
        if best is None or c < best[0]:
            best = (c, k)
    return best[1], capacity


def choose_shape(op, nodes, n, link, ceiling, capacity, small_bytes, s=PIECE, circulant=False):
    """osoe_shape_choose with its algorithm: (algorithm, k, W). The tree that
    choose() gives; and, where `circulant` allows it - the data path's word that
    it runs Träff's reduce-scatter - for a reduction past the small class whose
    rounds' rows, ceil(log2 nodes), fit a lane's ceiling + 1 rows, Träff's,
    where its closed form costs less than that tree's, ties to the tree. Its
    window is a round's slots: (ceiling + 1) // rows of the lane's rows, the
    capacity each."""
    k, W = choose(op, nodes, n, link, ceiling, capacity, small_bytes, s)
    rows = len(circulant_skips(nodes))
    if circulant and op == 'reduce' and nodes > 1 and n > small_bytes and rows <= ceiling + 1:
        window = capacity * ((ceiling + 1) // rows)
        if circulant_cost(nodes, n, s, window, *link) < cost(op, nodes, k, W, n, s, *link):
            return 'circulant', ceiling, window
    return 'tree', k, W


def shape_cost(op, nodes, shape, n, link, s=PIECE):
    """What a chosen shape costs, by its algorithm."""
    algorithm, k, W = shape
    if algorithm == 'circulant':
        return circulant_cost(nodes, n, s, W, *link)
    return cost(op, nodes, k, W, n, s, *link)


# ---- Träff's circulant reduce-scatter and allgather (v0.2 step 3) ---------

def circulant_skips(nodes):
    """The rounds' (s', s): s halving the node count, rounding up, to 1
    (circulant_model.py)."""
    out, s = [], nodes
    while s > 1:
        out.append((s, (s + 1) // 2))
        s = (s + 1) // 2
    return out


def circulant_bytes(nodes, n, first, a, b):
    """The bytes of blocks (first + i) mod nodes for i in [a, b), block j
    being bytes [j n / nodes, (j + 1) n / nodes)."""
    total = 0
    for i in range(a, b):
        j = (first + i) % nodes
        total += (j + 1) * n // nodes - j * n // nodes
    return total


def circulant_rounds(nodes, n):
    """Each round's largest message, in bytes, in the order the rounds run:
    the reduce-scatter's s' - s blocks, then the allgather's in reverse - at
    most ceil((s' - s) n / nodes) bytes, as the C computes it."""
    rs = [((sp - s) * n + nodes - 1) // nodes for sp, s in circulant_skips(nodes)]
    return rs + rs[::-1]


def circulant_cost(nodes, n, s, W, h_ns, o_ns, bytes_per_second):
    """The closed form of Träff's reduction, nanoseconds, integers -
    collective-shape.c's, line for line. Every lead grants its reduce-scatter
    senders as it enters and starts once it holds all of its own grants: a
    message cost a grant, then a hop. The rounds run one after another, each a
    hop and a message cost, then its pieces at the pace of the link, of the
    lead's own messages - a piece and a credit each - or of its credits: the
    first W pieces go at once, and each W after them wait for a piece, two
    hops and a message; at the link's pace the round's bytes over the link.
    Between the halves the last allgather grant takes a message and a hop.
    Saturates at 2**64 - 1."""
    if nodes <= 1 or n == 0:
        return 0
    rounds = circulant_rounds(nodes, n)
    q = len(rounds) // 2
    total = q * o_ns + h_ns + o_ns + h_ns
    for m in rounds:
        pieces = m // s + (m % s != 0)
        t = (min(m, s) * 1000000000 + bytes_per_second - 1) // bytes_per_second
        wire = SATURATED if m > SATURATED // 1000000000 else \
            (m * 1000000000 + bytes_per_second - 1) // bytes_per_second
        credited = (t + 2 * h_ns + o_ns + W - 1) // W
        total += h_ns + o_ns + max(wire, t + (pieces - 1) * 2 * o_ns,
                                   t + max(0, pieces - W) * credited)
    return min(SATURATED, total)


def simulate_circulant(nodes, n, s, W, h, o, B, links='sender'):
    """Träff's reduction, event by event, every lead entering at once: the
    rounds of circulant_model.py, one after another on each lead, their pieces
    through the links of Network with W credits a round, a message cost o for
    every message a lead sends - pieces, credits and grants. A lead grants its
    reduce-scatter senders as it enters and starts its reduce-scatter once it
    holds every grant of its own; it grants the sender of its row k's
    allgather as its reduce-scatter round k ends, and starts its allgather
    once it holds every such grant. Microseconds, the time the last lead
    finishes."""
    skips = circulant_skips(nodes)
    q = len(skips)
    net = Network(h, B, links)
    plan = []
    for i in range(nodes):
        lead = []
        for half, order in ((0, skips), (1, skips[::-1])):
            for sp, sk in order:
                k = skips.index((sp, sk))
                if half == 0:
                    to, frm = (i + sk) % nodes, (i - sk) % nodes
                    out = circulant_bytes(nodes, n, i, sk, sp)
                    into = circulant_bytes(nodes, n, i, 0, sp - sk)
                else:
                    to, frm = (i - sk) % nodes, (i + sk) % nodes
                    out = circulant_bytes(nodes, n, i, 0, sp - sk)
                    into = circulant_bytes(nodes, n, i, sk, sp)
                lead.append((half, k, to, frm, out, into))
        plan.append(lead)
    at, sent, taken = [0] * nodes, [0] * nodes, [0] * nodes
    arrived, credits = {}, {}
    rs_grants, ag_grants = [0] * nodes, [0] * nodes
    busy, done = [0.0] * nodes, [None] * nodes

    def pieces(m):
        return (m + s - 1) // s

    def run(i, now):
        now = max(now, busy[i])
        while at[i] < len(plan[i]):
            half, k, to, frm, out, into = plan[i][at[i]]
            if half == 0 and rs_grants[i] < q or half == 1 and ag_grants[i] < q:
                break
            if taken[i] < pieces(into) and arrived.get((i, half, k), 0) > taken[i]:
                taken[i] += 1
                now += o
                net.send(i, frm, 0, now, credit_in, frm, half, k)
                continue
            if sent[i] < pieces(out):
                j = sent[i]
                if j < W or credits.get((i, half, k), 0) >= j - W + 1:
                    now += o
                    net.send(i, to, min(s, out - j * s), now, piece_in, to, half, k)
                    sent[i] += 1
                    continue
            if sent[i] == pieces(out) and taken[i] == pieces(into):
                if half == 0:
                    now += o
                    net.send(i, to, 0, now, ag_grant_in, to)
                at[i] += 1
                sent[i] = taken[i] = 0
                continue
            break
        busy[i] = now
        if at[i] == len(plan[i]) and done[i] is None:
            done[i] = now

    def piece_in(j, half, k, now):
        arrived[(j, half, k)] = arrived.get((j, half, k), 0) + 1
        run(j, now)

    def credit_in(j, half, k, now):
        credits[(j, half, k)] = credits.get((j, half, k), 0) + 1
        run(j, now)

    def rs_grant_in(j, now):
        rs_grants[j] += 1
        run(j, now)

    def ag_grant_in(j, now):
        ag_grants[j] += 1
        run(j, now)

    def enter(i, now):
        for sp, sk in skips:
            now += o
            net.send(i, (i - sk) % nodes, 0, now, rs_grant_in, (i - sk) % nodes)
        busy[i] = now
        run(i, now)

    for i in range(nodes):
        net.at(0.0, enter, i)
    net.run()
    return max(done)


# The links the tests pin: the Pi's, from the fit, and a loopback's.
PI_LINK = (86000, 7000, 116000000)
LOOPBACK_LINK = (20000, 2000, 4000000000)


# ---- The Pi's table --------------------------------------------------------

def pi_table():
    """2026-09-26's OSU lines, and step 2's sweep of 1 to 16 KiB at k 2, 3 and
    4 and D 2 and 4 (2026-10-06): (op, k, D, bytes, microseconds, file)."""
    rows = []
    day = os.path.join(EVIDENCE, '2026-09-26')
    late = os.path.join(EVIDENCE, '2026-09-26-late')
    sweep = os.path.join(EVIDENCE, '2026-10-06-v0.2-step2', 'comparison')
    files = [(os.path.join(day, f), f) for f in sorted(os.listdir(day))
             if f.startswith(('osu-reduce-k', 'osu-broadcast-k'))]
    files += [(os.path.join(late, f), f) for f in sorted(os.listdir(late)) if f.startswith('f2-')]
    files += [(os.path.join(sweep, f), f) for f in sorted(os.listdir(sweep))
              if f.startswith('sweep-')]
    for path, name in files:
        stem = name[:-4].split('-')
        op = 'reduce' if 'reduce' in stem else 'broadcast'
        k = int(next(x for x in stem if x.startswith('k'))[1:])
        ds = [x for x in stem if x.startswith('d') and x[1:].isdigit()]
        D = int(ds[0][1:]) if ds else 2         # the f2 sweeps ran at the default, 2
        for line in open(path):
            parts = line.split()
            if len(parts) == 2 and parts[0].isdigit():
                rows.append((op, k, D, int(parts[0]), float(parts[1]), name))
    return rows


def noise(n):
    """The table's run-to-run noise: the lines two sessions share agree within
    1% to 16 KiB and within 3 to 6% at 32 and 64 KiB (F1's losses, once 44%),
    and the large lines have no repeat."""
    return 0.02 if n <= 16384 else (0.08 if n <= 131072 else 0.04)


def within_noise(rows, h, o, B):
    return sum(1 for op, k, D, n, us, name in rows
               if abs(simulate(op, 4, k, D, n, PI_PIECE, h, o, B) / us - 1) <= noise(n))


def table_regret(rows, h, o, B):
    """For every call a session measured under several shapes: the measured
    time of the shape the model picks, the window at the largest the session
    measured that call at - 8 in 2026-09-26's lines, 4 in the sweep's -
    against the fastest that session measured. Repeated lines pooled by their
    mean within a session, never across two: the sweep ran ten days of code
    later, and its star at 16 KiB took 369 us where the table's took 454."""
    measured = {}
    for op, k, D, n, us, name in rows:
        session = 'sweep' if name.startswith('sweep-') else 'table'
        measured.setdefault((session, op, n, k, D), []).append(us)
    measured = {key: statistics.mean(v) for key, v in measured.items()}
    out = []
    for session, op, n in sorted({(s, a, b) for s, a, b, c, d in measured}):
        shapes = [(k, D, us) for (s2, o2, n2, k, D), us in measured.items()
                  if s2 == session and o2 == op and n2 == n]
        widest = max(x[1] for x in shapes)
        at_capacity = [x for x in shapes if x[1] == widest]
        if len(at_capacity) < 2:
            continue
        pick = min(at_capacity,
                   key=lambda x: (simulate(op, 4, x[0], widest, n, PI_PIECE, h, o, B), x[0]))
        best = min(shapes, key=lambda x: x[2])
        out.append(pick[2] / best[2] - 1)
    return out


def fit(rows, o_range=range(0, 11)):
    best = None
    for h in range(80, 100):
        for o in o_range:
            for B in range(108, 128, 2):
                w = within_noise(rows, h, o, B)
                if best is None or w > best[0]:
                    best = (w, h, o, B)
    return best


# ---- The checks ------------------------------------------------------------

def check_table(rows, h, o, B, failures):
    w = within_noise(rows, h, o, B)
    if w < PI_WITHIN:
        failures.append(f'the fit lands {w} of the Pi\'s {len(rows)} lines within the noise, '
                        f'fewer than the record\'s {PI_WITHIN}')
    r = table_regret(rows, h, o, B)
    if max(r) > 0.10:
        failures.append(f'choosing by the model costs up to {max(r):.3f} against the fastest '
                        'shape the Pi measured, more than 10%')


def check_terms(failures, interior=1, count_runs=True):
    for op in ('reduce', 'broadcast'):
        for k in range(2, 33):
            for nodes in range(2, 300 if k < 5 else 120):
                a = tree_terms_walk(op, nodes, k, interior)
                b = tree_terms(op, nodes, k, interior)
                if (a[0], a[1], a[2]) != (b[0], b[1], b[2]):
                    failures.append(f'tree_terms({op}, {nodes}, {k}) = {b}, the walk says {a}')
                    return
    if not count_runs:
        return
    # beta is what the simulation counts on the busiest link, for each piece
    for op in ('reduce', 'broadcast'):
        for nodes in range(2, 40):
            for k in range(2, 9):
                counts = {}
                pieces = 3
                simulate(op, nodes, k, 2, pieces * PIECE, PIECE, 86, 7, 116,
                         measure='completion', count_links=counts)
                busiest = max(counts.values()) // pieces
                beta = tree_terms(op, nodes, k, interior)[2]
                if beta != busiest:
                    failures.append(f'{op} on {nodes} nodes at k = {k}: the closed form counts '
                                    f'{beta} pieces on the busiest link, the loop {busiest}')
                    return


def check_choices(failures):
    """The closed form's choice of k at W = capacity against each link model's
    best k at the same W: within the bound the record states, on a grid."""
    grid = [(op, N, n) for op in ('reduce', 'broadcast') for N in (2, 3, 4, 5, 7, 8, 13, 16, 31)
            for n in (2048, 8192, 32768, 131072, 1 << 20)]
    for links, p90_bound, max_bound in (('sender', 0.08, 0.20), ('connection', 0.05, 0.30)):
        regrets = []
        for op, N, n in grid:
            ks = range(2, 9)
            sim = {k: simulate(op, N, k, 4, n, PIECE, 86, 7, 116, measure='completion', links=links)
                   for k in ks}
            pick = choose(op, N, n, PI_LINK, 8, 4, 1024)[0]
            regrets.append(sim[pick] / min(sim.values()) - 1)
        regrets.sort()
        p90, worst = regrets[int(len(regrets) * 0.9)], regrets[-1]
        if p90 > p90_bound or worst > max_bound:
            failures.append(f'against {links} links the closed form\'s choices cost p90 '
                            f'{p90:.3f}, worst {worst:.3f}, beyond {p90_bound} and {max_bound}')


def crossover(op, nodes, link, ceiling=8, capacity=4, small=1024):
    """The smallest power-of-two size from 2 KiB at which the chooser leaves
    the star for a deeper tree, or None up to 1 GiB."""
    star = max(2, nodes - 1)
    n = 2048
    while n <= 1 << 30:
        if choose(op, nodes, n, link, ceiling, capacity, small)[0] < min(star, ceiling):
            return n
        n *= 2
    return None


def check_pins(failures):
    """What collective-shape.c's test pins too (shmem.shape.choice)."""
    for op in ('reduce', 'broadcast'):
        if choose(op, 4, 8, PI_LINK, 8, 4, 1024) != (8, 1):
            failures.append(f'an 8-byte {op} is not the small class\'s ceiling tree')
        if choose(op, 4, 2048, PI_LINK, 8, 4, 1024)[0] < 3:
            failures.append(f'a 2 KiB {op} on four nodes of the Pi does not take the star')
        if choose(op, 4, 1 << 20, PI_LINK, 8, 4, 1024)[0] != 2:
            failures.append(f'a 1 MiB {op} on four nodes of the Pi does not take k = 2')
        c = crossover(op, 4, PI_LINK)
        if c is None or not 16384 <= c <= 65536:
            failures.append(f'the Pi\'s {op} crossover on four nodes is at {c} bytes, '
                            'not between 16 and 64 KiB as F2 measured')
        lc = crossover(op, 4, LOOPBACK_LINK)
        if c is not None and lc is not None and lc < 8 * c:
            failures.append(f'a loopback link\'s {op} crossover ({lc}) is not far above the Pi\'s ({c})')
    # Träff's (v0.2 step 3): on four nodes of the Pi a 1 MiB reduction takes
    # it where it is allowed, and nothing else does - not a 2 KiB reduction,
    # not a broadcast, not a reduction where the data path does not allow it,
    # not one whose rows a lane cannot hold.
    if choose_shape('reduce', 4, 1 << 20, PI_LINK, 8, 4, 1024, circulant=True)[0] != 'circulant':
        failures.append('a 1 MiB reduction on four nodes of the Pi does not take Träff\'s')
    if choose_shape('reduce', 4, 2048, PI_LINK, 8, 4, 1024, circulant=True)[0] != 'tree':
        failures.append('a 2 KiB reduction on four nodes of the Pi takes Träff\'s')
    if choose_shape('broadcast', 4, 1 << 20, PI_LINK, 8, 4, 1024, circulant=True)[0] != 'tree':
        failures.append('a broadcast takes Träff\'s')
    if choose_shape('reduce', 4, 1 << 20, PI_LINK, 8, 4, 1024)[0] != 'tree':
        failures.append('a reduction takes Träff\'s where the data path does not allow it')
    if choose_shape('reduce', 16, 1 << 20, PI_LINK, 2, 4, 1024, circulant=True)[0] != 'tree':
        failures.append('Träff\'s taken on 16 nodes with a ceiling of 2, whose lanes hold 3 rows')


def check_circulant(failures):
    """Träff's closed form against its simulation, and the choice between it
    and the trees against each link model's best of them - within the bounds
    the record states."""
    grid = [(N, n) for N in (3, 4, 5, 7, 8, 16) for n in (16384, 65536, 1 << 20)]
    for links, error_bound, regret_bound in (('sender', 0.12, 0.08), ('connection', 0.18, 0.08)):
        errors, regrets = [], []
        for N, n in grid:
            window = 4 * (9 // len(circulant_skips(N)))     # the chooser's, at (8, 4)
            sim_c = simulate_circulant(N, n, PIECE, window, 86, 7, 116, links=links)
            errors.append(abs(circulant_cost(N, n, PIECE, window, *PI_LINK) / 1000 / sim_c - 1))
            trees = {k: simulate('reduce', N, k, 4, n, PIECE, 86, 7, 116, measure='completion',
                                 links=links) for k in range(2, 9)}
            algorithm, k, W = choose_shape('reduce', N, n, PI_LINK, 8, 4, 1024, circulant=True)
            picked = sim_c if algorithm == 'circulant' else trees[k]
            regrets.append(picked / min(sim_c, min(trees.values())) - 1)
        if max(errors) > error_bound:
            failures.append(f'against {links} links Träff\'s closed form errs by up to '
                            f'{max(errors):.3f}, beyond {error_bound}')
        if max(regrets) > regret_bound:
            failures.append(f'against {links} links the choice between the trees and Träff\'s '
                            f'costs up to {max(regrets):.3f}, beyond {regret_bound}')


def checks(rows, h, o, B, interior=1, count_runs=True, choices=True):
    failures = []
    check_table(rows, h, o, B, failures)
    check_terms(failures, interior, count_runs)
    if choices:
        check_choices(failures)
        check_pins(failures)
        check_circulant(failures)
    return failures


# ---- The grid collective-shape.c's test reads -------------------------------

def grid_lines():
    """Every input of a grid, and the shape and cost the model gives it: a
    reduction both with Träff's allowed and without, as the data path asks."""
    lines = ['# op nodes bytes hop_ns message_ns bytes_per_second ceiling capacity small'
             ' circulant -> algorithm fanout window cost_ns (tools/modelcheck/shape_timing.py'
             ' grid)']
    links = [PI_LINK, LOOPBACK_LINK, (1000000, 50000, 12000000), (5000, 500, 25000000000)]
    for op in ('reduce', 'broadcast'):
        for link in links:
            for nodes in (1, 2, 3, 4, 5, 7, 8, 13, 16, 31, 64, 255, 1000, 4096):
                for ceiling, capacity in ((4, 2), (8, 4), (32, 8)):
                    for n in (8, 1024, 1025, 4096, 16384, 16385, 65536, 1 << 20, 1 << 30):
                        for allowed in ((0, 1) if op == 'reduce' else (0,)):
                            shape = choose_shape(op, nodes, n, link, ceiling, capacity, 1024,
                                                 circulant=bool(allowed))
                            c = shape_cost(op, nodes, shape, n, link) if n > 1024 else 0
                            lines.append(f'{op} {nodes} {n} {link[0]} {link[1]} {link[2]} '
                                         f'{ceiling} {capacity} 1024 {allowed} {shape[0]} '
                                         f'{shape[1]} {shape[2]} {c}')
    return lines


def main(argv):
    mode = argv[1] if len(argv) > 1 else 'check'
    if mode == 'fit':
        rows = pi_table()
        w, h, o, B = fit(rows)
        print(f'fit: h {h} us, o {o} us, B {B} bytes/us: {w} of {len(rows)} lines within the noise')
        return 0
    if mode == 'grid':
        with open(argv[2], 'w') as f:
            f.write('\n'.join(grid_lines()) + '\n')
        return 0
    if mode == 'grid-compare':
        have = open(argv[2]).read().splitlines()
        if have != grid_lines():
            print(f'{argv[2]} is not what shape_timing.py grid writes now: regenerate it')
            return 1
        print(f'{argv[2]}: {len(have) - 1} lines, as the model writes them')
        return 0
    if mode != 'check':
        print(__doc__)
        return 2
    rows = pi_table()
    failures = checks(rows, PI_HOP, PI_MESSAGE, PI_BANDWIDTH)
    print(f'design: {len(failures)} failure(s)')
    for f in failures:
        print('   ', f)
    ok = not failures
    # The broken variants, each of which must fail.
    w, h, o, B = fit(rows, o_range=[0])
    broken = checks(rows, h, o, B, choices=False, count_runs=False)
    print(f'broken message_cost (o = 0, h {h} and B {B} refitted): '
          f'{"fails" if broken else "PASSES - the check cannot see it"}')
    for f in broken[:2]:
        print('   ', f)
    ok = ok and bool(broken)
    broken = []
    check_terms(broken, interior=0)
    print(f'broken interior (an interior lead counted k, not k + 1): '
          f'{"fails" if broken else "PASSES - the check cannot see it"}')
    for f in broken[:2]:
        print('   ', f)
    ok = ok and bool(broken)
    print('ok' if ok else 'FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
