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
              loop, whichever is slower, and a partial last piece its own
              bytes on the busiest link or the window's pace less what its
              shorter loop gives back, whichever is more (P4). Its choices
              sit between the two link models': choosing by it costs less,
              against either, than choosing by one and running under the
              other.
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
  shape_timing.py places    the default way of collect's places (P31) at each
                            node count, on the Pi's link and a loopback's
  shape_timing.py places-grid FILE, places-grid-compare FILE
                            the same for the places' choice and costs, for
                            shmem.shape.places-grid
"""
import heapq
import os
import statistics
import sys

import engine

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
# True for check_last_piece's broken variant alone: a partial last piece
# charged a whole period, as cost() did before P4.
LAST_PIECE_WHOLE = False


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
    # a partial last piece (P4): its own bytes on the busiest link, or the
    # window's pace less what its shorter loop gives back, whichever is more -
    # nearer what simulate() finds it adds than a period, a whole one's
    later, tail = pieces - 1, 0
    last = n - (pieces - 1) * s
    if not LAST_PIECE_WHOLE and pieces > 1 and last < s:
        t_last = (last * 1000000000 + bytes_per_second - 1) // bytes_per_second
        loop_last = max(a * h_ns + b * t_last + m * o_ns for a, b, m in loops)
        later = pieces - 2
        tail = max(beta * t_last, (loop + W - 1) // W - (loop - loop_last))
    return min(SATURATED, fill + later * period + tail)


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


# The most rows Träff's reduction takes, ceil(log2 nodes) of them: what the
# reduce-scatter's words hold (internal-region.h,
# OSOE_INTERNAL_CIRCULANT_ROWS_MOST), a team of more than 2**16 nodes taking the
# trees. A lane has no more than that for Träff's.
CIRCULANT_ROWS_MOST = 16


def lane_rows(ceiling, job_nodes):
    """The rows of a lane in a job of `job_nodes` nodes at a fan-out ceiling of
    `ceiling` - internal-region.h's osoe_internal_lane_rows: a row for each of
    a tree's child positions and one for what its parent sends down, ceiling +
    1; and, for Träff's, which gives each round of its reduce-scatter and
    allgather a row of the receiver's lane, as many as its rounds a half need,
    ceil(log2 job_nodes) at most CIRCULANT_ROWS_MOST - whichever is more. Both
    are every node's alike, so that no setting, no word at the rendezvous and no
    version of the layout is needed: the lane is larger than the ceiling's only
    past 512 nodes at the default ceiling of 8."""
    return max(ceiling + 1, min(len(circulant_skips(job_nodes)), CIRCULANT_ROWS_MOST))


def choose_shape(op, nodes, n, link, ceiling, capacity, small_bytes, s=PIECE, circulant=False,
                 lane=None):
    """osoe_shape_choose with its algorithm: (algorithm, k, W). The tree that
    choose() gives; and, where `circulant` allows it - the data path's word that
    it runs Träff's reduce-scatter - for a reduction past the small class whose
    rounds' rows, ceil(log2 nodes), fit a lane's `lane` rows, Träff's, where its
    closed form costs less than that tree's, ties to the tree. Its window is a
    round's slots: lane // rows of the lane's rows, the capacity each. `lane` is
    the lane's rows in the job (lane_rows(ceiling, the job's nodes)); None for a
    team that is the whole job, as the data path's world team is - osoe_shape_choose
    takes it always, from its caller."""
    if lane is None:
        lane = lane_rows(ceiling, nodes)
    k, W = choose(op, nodes, n, link, ceiling, capacity, small_bytes, s)
    rows = len(circulant_skips(nodes))
    if circulant and op == 'reduce' and nodes > 1 and n > small_bytes and rows <= lane:
        window = capacity * (lane // rows)
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


# ---- collect's places: four ways, and the choice among them (P31) ----------
#
# A collect of differing blocks on a team spanning nodes finds each member's
# place one of four ways (collectives.c, collect_varying): the members' vector,
# a reduction of 8 bytes a member; each node's total (P29), a reduction of 8
# bytes a node; the leads' scan of the nodes' totals (P30), a small pre-order
# tree at the ceiling - its widest message the down, 16 bytes - and a record
# more a step of the gather after, with the share at entry; and the leads'
# direct exchange of the totals (P33), a hop and a 16-byte message to every other
# lead. Where each node's members are one run of the team's ranks the last
# three apply - the scan only where the runs meet the node set's positions in
# order or in reverse, and the exchange only on the nodes its room holds -
# and otherwise the members' vector alone. Each way's cost, under the three
# numbers of the link, in the closed form above: a reduction as the data path
# runs it, its small class one piece through the ceiling's tree and a larger
# one the chooser's shape with a hop more for its grant; the scan the same
# small tree, a level-order tree's terms standing in for the pre-order one's,
# which is as deep; the exchange its N - 1 messages, each a message cost and a
# slot's bytes on the sender's link, and a hop. Provisional, all of it: H24 and
# H25 measure the four (docs/prompts/v0.2-open-items.md), and the choice
# follows them. collective-shape.c's osoe_places_cost and osoe_places_choose,
# line for line; shmem.shape.places-grid holds the two to the same answers.
#
# The default takes the exchange on no more than 16 nodes, however cheap these
# costs find it past them (P35, the author's decision of 2026-10-13): H24
# measured it the fastest of the four at 8 and 16 logical nodes and slower than
# the scan at 32, on one machine whose CPUs carry every lead's N - 1 messages at
# once - which overstates its cost, so a cluster can only move the crossover
# up (docs/design/evidence/v0.2-step4/2026-10-13-h24/). Past them each node's
# total or the scan, as the costs choose; provisional until H25.
# collective-shape.h's OSOE_PLACES_EXCHANGE_DEFAULT_NODES_MOST.

PLACES_WAYS = ('members', 'totals', 'scan', 'exchange')
PLACES_DOWN_BYTES = 16          # the leads' scan's down, the before and the total
EXCHANGE_SLOT_BYTES = 16        # the exchange's slot, the epoch and the total
EXCHANGE_DEFAULT_NODES_MOST = 16  # the most nodes the default takes the exchange on (P35)


def gather_steps(nodes):
    """The allgather's steps over `nodes` nodes, ceil(log2 nodes)."""
    steps = 0
    while (1 << steps) < nodes:
        steps += 1
    return steps


def reduction_cost(nodes, n, link, ceiling, capacity, small_bytes, s=PIECE, circulant=True,
                   lane=None):
    """A reduction of `n` bytes among `nodes` leads as the data path runs it:
    the small class one piece through the ceiling's tree, and a larger one the
    chooser's shape, Träff's where `circulant` allows it, and a hop more for the
    grant a child waits for. `lane`, the lane's rows, as choose_shape's."""
    if nodes <= 1 or n == 0:
        return 0
    if n <= small_bytes:
        return cost('reduce', nodes, ceiling, 1, n, n, *link)
    shape = choose_shape('reduce', nodes, n, link, ceiling, capacity, small_bytes, s, circulant,
                         lane)
    return min(SATURATED, shape_cost('reduce', nodes, shape, n, link, s) + link[0])


def places_cost(way, nodes, members, link, ceiling, capacity, small_bytes, s=PIECE,
                circulant=True, lane=None):
    """What finding a collect's places costs one way (the section's comment),
    nanoseconds: osoe_places_cost. `lane`, the lane's rows, as choose_shape's."""
    h_ns, o_ns, bytes_per_second = link
    if nodes <= 1:
        return 0
    if bytes_per_second == 0:
        return SATURATED
    if way == 'members':
        return reduction_cost(nodes, 8 * members, link, ceiling, capacity, small_bytes, s,
                              circulant, lane)
    if way == 'totals':
        return reduction_cost(nodes, 8 * nodes, link, ceiling, capacity, small_bytes, s,
                              circulant, lane)
    if way == 'scan':
        tree = cost('reduce', nodes, ceiling, 1, PLACES_DOWN_BYTES, PLACES_DOWN_BYTES, *link)
        return min(SATURATED, tree + (gather_steps(nodes) + 1) * o_ns)
    t = (EXCHANGE_SLOT_BYTES * 1000000000 + bytes_per_second - 1) // bytes_per_second
    return min(SATURATED, h_ns + (nodes - 1) * (o_ns + t))


def choose_places(nodes, members, runs, ordered, room, link, ceiling, capacity, small_bytes,
                  s=PIECE, circulant=True, lane=None):
    """osoe_places_choose: the way a collect's places take by default. The
    members' vector where some node's members are not one run (`runs` false) or
    on one node; otherwise the least cost of each node's total, the exchange -
    on no more nodes than its `room`, nor than EXCHANGE_DEFAULT_NODES_MOST - and
    the scan - where the runs are `ordered` - ties to that order. `lane`, the
    lane's rows, as choose_shape's."""
    if nodes <= 1 or not runs:
        return 'members'
    best, least = 'totals', places_cost('totals', nodes, members, link, ceiling, capacity,
                                        small_bytes, s, circulant, lane)
    exchange = nodes <= room and nodes <= EXCHANGE_DEFAULT_NODES_MOST
    for way, allowed in (('exchange', exchange), ('scan', ordered)):
        if not allowed:
            continue
        c = places_cost(way, nodes, members, link, ceiling, capacity, small_bytes, s, circulant,
                        lane)
        if c < least:
            best, least = way, c
    return best


def places_crossovers(link, members_a_node=1, ceiling=8, capacity=8, small=1024, room=64,
                      most=1024):
    """The default way at each node count from 2 to `most`, as runs of node
    counts taking one way: [(first, last, way)]."""
    runs = []
    for nodes in range(2, most + 1):
        way = choose_places(nodes, nodes * members_a_node, True, True, room, link, ceiling,
                            capacity, small)
        if runs and runs[-1][2] == way:
            runs[-1] = (runs[-1][0], nodes, way)
        else:
            runs.append((nodes, nodes, way))
    return runs


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
        failures.append(f'TABLE: the fit lands {w} of the Pi\'s {len(rows)} lines within the '
                        f'noise, fewer than the record\'s {PI_WITHIN}')
    r = table_regret(rows, h, o, B)
    if max(r) > 0.10:
        failures.append(f'TABLE: choosing by the model costs up to {max(r):.3f} against the '
                        'fastest shape the Pi measured, more than 10%')


def check_terms(failures, interior=1, count_runs=True):
    for op in ('reduce', 'broadcast'):
        for k in range(2, 33):
            for nodes in range(2, 300 if k < 5 else 120):
                a = tree_terms_walk(op, nodes, k, interior)
                b = tree_terms(op, nodes, k, interior)
                if (a[0], a[1], a[2]) != (b[0], b[1], b[2]):
                    failures.append(f'TERMS: tree_terms({op}, {nodes}, {k}) = {b}, '
                                    f'the walk says {a}')
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
                    failures.append(f'TERMS: {op} on {nodes} nodes at k = {k}: the closed form '
                                    f'counts {beta} pieces on the busiest link, the loop '
                                    f'{busiest}')
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
            failures.append(f'CHOICES: against {links} links the closed form\'s choices cost p90 '
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
            failures.append(f'PINS: an 8-byte {op} is not the small class\'s ceiling tree')
        if choose(op, 4, 2048, PI_LINK, 8, 4, 1024)[0] < 3:
            failures.append(f'PINS: a 2 KiB {op} on four nodes of the Pi does not take the star')
        if choose(op, 4, 1 << 20, PI_LINK, 8, 4, 1024)[0] != 2:
            failures.append(f'PINS: a 1 MiB {op} on four nodes of the Pi does not take k = 2')
        c = crossover(op, 4, PI_LINK)
        if c is None or not 16384 <= c <= 65536:
            failures.append(f'PINS: the Pi\'s {op} crossover on four nodes is at {c} bytes, '
                            'not between 16 and 64 KiB as F2 measured')
        lc = crossover(op, 4, LOOPBACK_LINK)
        if c is not None and lc is not None and lc < 8 * c:
            failures.append(f'PINS: a loopback link\'s {op} crossover ({lc}) is not far above '
                            f'the Pi\'s ({c})')
    # Träff's (v0.2 step 3): on four nodes of the Pi a 1 MiB reduction takes
    # it where it is allowed, and nothing else does - not a 2 KiB reduction,
    # not a broadcast, not a reduction where the data path does not allow it,
    # not one whose rows a lane cannot hold.
    if choose_shape('reduce', 4, 1 << 20, PI_LINK, 8, 4, 1024, circulant=True)[0] != 'circulant':
        failures.append('PINS: a 1 MiB reduction on four nodes of the Pi does not take Träff\'s')
    if choose_shape('reduce', 4, 2048, PI_LINK, 8, 4, 1024, circulant=True)[0] != 'tree':
        failures.append('PINS: a 2 KiB reduction on four nodes of the Pi takes Träff\'s')
    if choose_shape('broadcast', 4, 1 << 20, PI_LINK, 8, 4, 1024, circulant=True)[0] != 'tree':
        failures.append('PINS: a broadcast takes Träff\'s')
    if choose_shape('reduce', 4, 1 << 20, PI_LINK, 8, 4, 1024)[0] != 'tree':
        failures.append('PINS: a reduction takes Träff\'s where the data path does not allow it')


def check_lane(failures, lane_of=lane_rows):
    """Träff's rows are a lane's, and a lane's rows are not the ceiling's alone
    (P3): a lane has max(ceiling + 1, ceil(log2 nodes)) rows, at most 16 for
    Träff's, so that a 1 MiB reduction takes Träff's wherever the cost model
    finds it cheaper - on the Pi's link at every node count to 65,536, at the
    default ceiling of 8 as at a ceiling of 2, where a lane has 3 rows to
    begin with - and a lane of the ceiling's rows leaves it to the trees past
    the nodes those hold (shmem.shape.choice and shmem.circulant.*.ceiling-2
    hold the C to these). `lane_of` is the lane's rows by the rule under test."""
    for ceiling, nodes, want in ((8, 1, 9), (8, 256, 9), (8, 257, 9), (8, 512, 9), (8, 513, 10),
                                 (8, 1024, 10), (8, 1025, 11), (8, 4096, 12), (8, 65536, 16),
                                 (8, 65537, 16), (8, 1 << 20, 16), (32, 65536, 33),
                                 (2, 4, 3), (2, 5, 3), (2, 8, 3), (2, 9, 4), (2, 17, 5),
                                 (2, 33, 6), (15, 65536, 16), (16, 65536, 17)):
        if lane_of(ceiling, nodes) != want:
            failures.append(f'LANE: a lane has {lane_of(ceiling, nodes)} rows in a job of '
                            f'{nodes} nodes at a ceiling of {ceiling}, not {want}')
    for ceiling, nodes in ((8, 512), (8, 513), (8, 1024), (8, 4096), (8, 65536), (4, 33),
                           (2, 5), (2, 9), (2, 17), (2, 33)):
        lane = lane_of(ceiling, nodes)
        shape = choose_shape('reduce', nodes, 1 << 20, PI_LINK, ceiling, 4, 1024, circulant=True,
                             lane=lane)
        if shape[0] != 'circulant' or shape[2] != 4 * (lane // len(circulant_skips(nodes))):
            failures.append(f'LANE: a 1 MiB reduction on {nodes} nodes of the Pi at a ceiling of '
                            f'{ceiling} takes {shape}, not Träff\'s with its lane\'s window')
    # a lane of the ceiling's rows holds no more than the trees' (the rule before P3)
    for ceiling, nodes in ((8, 513), (8, 1024), (2, 9), (2, 33)):
        shape = choose_shape('reduce', nodes, 1 << 20, PI_LINK, ceiling, 4, 1024, circulant=True,
                             lane=ceiling + 1)
        if shape[0] != 'tree':
            failures.append(f'LANE: {nodes} nodes take Träff\'s with a lane of the ceiling\'s '
                            f'{ceiling + 1} rows, whose {len(circulant_skips(nodes))} do not '
                            'fit')
    # and none past 65,536 nodes even where the caller allows it: the 17 rows
    # outgrow a lane of 16
    if choose_shape('reduce', 65537, 1 << 20, PI_LINK, 8, 4, 1024, circulant=True,
                    lane=lane_of(8, 65537))[0] != 'tree':
        failures.append('LANE: 65,537 nodes take Träff\'s with a lane of 16 rows, whose 17 '
                        'do not fit')
    # a team within a larger job has the lane the job gave it: of 12 rows in a job
    # of 4,096, four blocks of the capacity's slots for the 3 rows of 8 nodes
    shape = choose_shape('reduce', 8, 1 << 20, PI_LINK, 8, 4, 1024, circulant=True,
                         lane=lane_of(8, 4096))
    if shape != ('circulant', 8, 16):
        failures.append(f'LANE: 8 nodes of a job of 4,096 take {shape}, not Träff\'s with a '
                        'window of 16 pieces, the capacity for each of 4 blocks')


def check_circulant(failures):
    """Träff's closed form against its simulation, and the choice between it
    and the trees against each link model's best of them - within the bounds
    the record states."""
    grid = [(N, n) for N in (3, 4, 5, 7, 8, 16) for n in (16384, 65536, 1 << 20)]
    for links, error_bound, regret_bound in (('sender', 0.12, 0.08), ('connection', 0.18, 0.08)):
        errors, regrets = [], []
        for N, n in grid:
            window = 4 * (lane_rows(8, N) // len(circulant_skips(N)))   # the chooser's, at (8, 4)
            sim_c = simulate_circulant(N, n, PIECE, window, 86, 7, 116, links=links)
            errors.append(abs(circulant_cost(N, n, PIECE, window, *PI_LINK) / 1000 / sim_c - 1))
            trees = {k: simulate('reduce', N, k, 4, n, PIECE, 86, 7, 116, measure='completion',
                                 links=links) for k in range(2, 9)}
            algorithm, k, W = choose_shape('reduce', N, n, PI_LINK, 8, 4, 1024, circulant=True)
            picked = sim_c if algorithm == 'circulant' else trees[k]
            regrets.append(picked / min(sim_c, min(trees.values())) - 1)
        if max(errors) > error_bound:
            failures.append(f'CIRCULANT: against {links} links Träff\'s closed form errs by up to '
                            f'{max(errors):.3f}, beyond {error_bound}')
        if max(regrets) > regret_bound:
            failures.append(f'CIRCULANT: against {links} links the choice between the trees '
                            f'and Träff\'s costs up to {max(regrets):.3f}, beyond {regret_bound}')


def check_last_piece(failures):
    """A partial last piece is charged its bytes (P4): on two nodes of the
    Pi's link, as the data path asks, where the tree and Träff's send the same
    bytes, every 16 KiB k + 1 to 4 MiB takes what 16 KiB k takes - charged a
    whole period, the tree took 16 KiB k and Träff's 16 KiB k + 1, every k;
    and one byte past whole pieces adds to the closed form what it adds to the
    simulation, on a grid of trees, within 1% of the call at the median and
    15% at worst - charged a period, 18% and 82%."""
    flips = [k for k in range(1, 257)
             if choose_shape('reduce', 2, k * PIECE, PI_LINK, 8, 8, 1024, circulant=True)[0] !=
             choose_shape('reduce', 2, k * PIECE + 1, PI_LINK, 8, 8, 1024, circulant=True)[0]]
    if flips:
        failures.append(f'LAST: on two nodes of the Pi, 16 KiB k + 1 takes another algorithm '
                        f'than 16 KiB k for {len(flips)} k of 256, from k = {flips[0]}')
    errors = []
    for op in ('reduce', 'broadcast'):
        for nodes in (2, 3, 4, 7, 16):
            for k in (2, 4, 8):
                for j in (1, 4, 16):
                    n = j * PIECE
                    closed = (cost(op, nodes, k, 8, n + 1, PIECE, *PI_LINK) -
                              cost(op, nodes, k, 8, n, PIECE, *PI_LINK)) / 1000
                    before = simulate(op, nodes, k, 8, n, PIECE, 86, 7, 116, measure='completion')
                    after = simulate(op, nodes, k, 8, n + 1, PIECE, 86, 7, 116,
                                     measure='completion')
                    errors.append(abs(closed - (after - before)) / before)
    median, worst = statistics.median(errors), max(errors)
    if median > 0.01 or worst > 0.15:
        failures.append(f'LAST: a byte past whole pieces costs the closed form what it costs '
                        f'the simulation within {median:.3f} of the call at the median and '
                        f'{worst:.3f} at worst, beyond 0.01 and 0.15')


def check_circulant_wide(failures):
    """Träff's where the lane's rows now let it run (P3), past the 512 nodes the
    ceiling's lane held at the default: on 513 nodes, the first, a 1 MiB reduction
    by the simulation of the Pi's links takes Träff's at least twice less time
    than every tree to the ceiling. The closed form says 3.2 times: its trees
    cost about 1.3 times the simulation's at this size, where Träff's costs within
    15% of it, so the gain is 2.9 times by the simulation (51 ms against 18) and the
    choice the same. The sender links only: the other model's streams are too slow
    to run so many leads."""
    nodes, n = 513, 1 << 20
    algorithm, k, window = choose_shape('reduce', nodes, n, PI_LINK, 8, 4, 1024, circulant=True)
    if algorithm != 'circulant':
        failures.append('CIRCULANT: 513 nodes do not take Träff\'s at 1 MiB on the Pi\'s link')
        return
    sim_c = simulate_circulant(nodes, n, PIECE, window, 86, 7, 116, links='sender')
    sim_t = min(simulate('reduce', nodes, t, 4, n, PIECE, 86, 7, 116, measure='completion',
                         links='sender') for t in range(2, 9))
    closed = circulant_cost(nodes, n, PIECE, window, *PI_LINK) / 1000
    if sim_t < 2 * sim_c:
        failures.append(f'CIRCULANT: on {nodes} nodes the simulation has the best tree at '
                        f'{sim_t:.0f} us and Träff\'s at {sim_c:.0f}, under twice as fast')
    if abs(closed / sim_c - 1) > 0.15:
        failures.append(f'CIRCULANT: on {nodes} nodes Träff\'s closed form is {closed:.0f} us '
                        f'and its simulation {sim_c:.0f}, more than 15% apart')


def checks(rows, h, o, B, interior=1, count_runs=True, choices=True):
    failures = []
    check_table(rows, h, o, B, failures)
    check_terms(failures, interior, count_runs)
    if choices:
        check_choices(failures)
        check_pins(failures)
        check_lane(failures)
        check_circulant(failures)
        check_circulant_wide(failures)
        check_last_piece(failures)
    return failures


def unexpected(failures, check):
    """A broken variant must fail the check written for it, `check`, at least once:
    one failure's message begins with its name. The mark for the variant's result
    line: empty when it does, else why not."""
    names = [engine.named(f) for f in failures]
    failed = check if check in names else (names[0] if names else None)
    wrong = engine.judge(not failures, failed, check)
    return '' if wrong is None else '   <-- UNEXPECTED' + wrong


# ---- The grid collective-shape.c's test reads -------------------------------

def grid_lines():
    """Every input of a grid, and the shape and cost the model gives it: a
    reduction both with Träff's allowed and without, as the data path asks. The
    team is the job, so a lane has lane_rows(ceiling, nodes) rows (the C's
    osoe_internal_lane_rows): the node counts either side of where that passes
    the ceiling's, 5 and 9 at a ceiling of 2, 17 and 33 at 4, 257 and 513 at 8,
    and 65,536, the most Träff's takes."""
    lines = ['# op nodes bytes hop_ns message_ns bytes_per_second ceiling capacity small'
             ' circulant -> algorithm fanout window cost_ns (tools/modelcheck/shape_timing.py'
             ' grid)']
    links = [PI_LINK, LOOPBACK_LINK, (1000000, 50000, 12000000), (5000, 500, 25000000000)]
    for op in ('reduce', 'broadcast'):
        for link in links:
            for nodes in (1, 2, 3, 4, 5, 7, 8, 9, 13, 16, 17, 31, 33, 64, 255, 257, 512, 513,
                          1000, 1024, 4096, 65536):
                for ceiling, capacity in ((2, 4), (4, 2), (8, 4), (8, 8), (32, 8)):
                    for n in (8, 1024, 1025, 4096, 16384, 16385, 65536, 1 << 20, 1 << 30):
                        for allowed in ((0, 1) if op == 'reduce' else (0,)):
                            shape = choose_shape(op, nodes, n, link, ceiling, capacity, 1024,
                                                 circulant=bool(allowed))
                            c = shape_cost(op, nodes, shape, n, link) if n > 1024 else 0
                            lines.append(f'{op} {nodes} {n} {link[0]} {link[1]} {link[2]} '
                                         f'{ceiling} {capacity} 1024 {allowed} {shape[0]} '
                                         f'{shape[1]} {shape[2]} {c}')
    return lines


def places_grid_lines():
    """Every input of the places' grid, and the way and the four costs the
    model gives it, for collective-shape.c's test (shmem.shape.places-grid)."""
    lines = ['# nodes members runs ordered room hop_ns message_ns bytes_per_second ceiling'
             ' capacity small circulant -> way members_ns totals_ns scan_ns exchange_ns'
             ' (tools/modelcheck/shape_timing.py places-grid)']
    links = [PI_LINK, LOOPBACK_LINK, (1000000, 50000, 12000000), (5000, 500, 25000000000),
             (50000, 3000, 2700000000)]
    for link in links:
        for nodes in (1, 2, 3, 4, 7, 8, 9, 16, 17, 31, 46, 47, 60, 61, 64, 65, 100, 128, 129,
                      200, 254, 512, 513, 1000, 1024, 4096):
            for per_node in (1, 4, 16):
                for runs, ordered in ((False, False), (True, False), (True, True)):
                    for ceiling, capacity, small in ((8, 4, 1024), (8, 8, 1024), (4, 2, 128),
                                                     (32, 8, 16384)):
                        for circulant in (0, 1):
                            members = nodes * per_node
                            way = choose_places(nodes, members, runs, ordered, 64, link, ceiling,
                                                capacity, small, circulant=bool(circulant))
                            costs = [places_cost(w, nodes, members, link, ceiling, capacity,
                                                 small, circulant=bool(circulant))
                                     for w in PLACES_WAYS]
                            lines.append(f'{nodes} {members} {int(runs)} {int(ordered)} 64 '
                                         f'{link[0]} {link[1]} {link[2]} {ceiling} {capacity} '
                                         f'{small} {circulant} {way} ' +
                                         ' '.join(str(c) for c in costs))
    return lines


def check_places(failures):
    """What the places' choice must say, whatever H24 and H25 move: the
    members' vector without runs and on one node; on the Pi's link and a
    loopback's, the exchange on few nodes, never past its room nor past the
    default's cap (P35) - which the costs alone would cross on both links - and
    past 128 - where each node's total becomes a large reduction, with its
    grant - the scan where the runs are ordered, each node's total where they
    are not; and none of the costs zero past one node."""
    for name, link in (('the Pi', PI_LINK), ('loopback', LOOPBACK_LINK)):
        if choose_places(8, 8, False, False, 64, link, 8, 4, 1024) != 'members':
            failures.append(f'PLACES: on {name} a team with no runs does not take the members')
        if choose_places(1, 4, True, True, 64, link, 8, 4, 1024) != 'members':
            failures.append(f'PLACES: on {name} a team of one node does not take the members')
        if choose_places(4, 16, True, True, 64, link, 8, 4, 1024) != 'exchange':
            failures.append(f'PLACES: on {name} four nodes do not take the exchange')
        if choose_places(200, 200, True, True, 64, link, 8, 4, 1024) != 'scan':
            failures.append(f'PLACES: on {name} 200 ordered nodes do not take the scan')
        if choose_places(200, 200, True, False, 64, link, 8, 4, 1024) != 'totals':
            failures.append(f'PLACES: on {name} 200 unordered nodes do not take each node\'s '
                            'total')
        if choose_places(100, 100, True, True, 64, link, 8, 4, 1024) == 'exchange':
            failures.append(f'PLACES: on {name} 100 nodes take the exchange past its room')
        past = EXCHANGE_DEFAULT_NODES_MOST + 1
        for ordered in (True, False):
            if choose_places(past, past, True, ordered, 64, link, 8, 4, 1024) == 'exchange':
                failures.append(f'PLACES: on {name} {past} nodes take the exchange past the '
                                f'default\'s {EXCHANGE_DEFAULT_NODES_MOST}')
        for w in PLACES_WAYS:
            if places_cost(w, 8, 8, link, 8, 4, 1024) == 0:
                failures.append(f'PLACES: on {name} the {w} costs nothing on 8 nodes')


def main(argv):
    mode = argv[1] if len(argv) > 1 else 'check'
    if mode == 'places':
        for name, link in (('the Pi', PI_LINK), ('a loopback', LOOPBACK_LINK)):
            for per_node in (1, 16):
                runs = places_crossovers(link, per_node)
                print(f'{name}, {per_node} PE{"s" if per_node > 1 else ""} a node: ' +
                      ', '.join(f'{a}-{b} {w}' for a, b, w in runs))
        return 0
    if mode == 'places-grid':
        with open(argv[2], 'w') as f:
            f.write('\n'.join(places_grid_lines()) + '\n')
        return 0
    if mode == 'places-grid-compare':
        have = open(argv[2]).read().splitlines()
        if have != places_grid_lines():
            print(f'{argv[2]} is not what shape_timing.py places-grid writes now: regenerate it')
            return 1
        print(f'{argv[2]}: {len(have) - 1} lines, as the model writes them')
        return 0
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
    check_places(failures)
    print(f'design: {len(failures)} failure(s)')
    for f in failures:
        print('   ', f)
    ok = not failures
    # The broken variants, each of which must fail the check written for it.
    w, h, o, B = fit(rows, o_range=[0])
    broken = checks(rows, h, o, B, choices=False, count_runs=False)
    mark = unexpected(broken, 'TABLE')
    print(f'broken message_cost (o = 0, h {h} and B {B} refitted): '
          f'{"fails" if broken else "PASSES - the check cannot see it"}{mark}')
    for f in broken[:2]:
        print('   ', f)
    ok = ok and not mark
    broken = []
    check_terms(broken, interior=0)
    mark = unexpected(broken, 'TERMS')
    print(f'broken interior (an interior lead counted k, not k + 1): '
          f'{"fails" if broken else "PASSES - the check cannot see it"}{mark}')
    for f in broken[:2]:
        print('   ', f)
    ok = ok and not mark
    broken = []
    check_lane(broken, lane_of=lambda ceiling, job_nodes: ceiling + 1)
    mark = unexpected(broken, 'LANE')
    print(f'broken lane (its rows the ceiling\'s, as before P3): '
          f'{"fails" if broken else "PASSES - the check cannot see it"}{mark}')
    for f in broken[:2]:
        print('   ', f)
    ok = ok and not mark
    global LAST_PIECE_WHOLE
    LAST_PIECE_WHOLE = True
    broken = []
    check_last_piece(broken)
    LAST_PIECE_WHOLE = False
    mark = unexpected(broken, 'LAST')
    print(f'broken last piece (charged a whole period, as before P4): '
          f'{"fails" if broken else "PASSES - the check cannot see it"}{mark}')
    for f in broken[:2]:
        print('   ', f)
    ok = ok and not mark
    print('ok' if ok else 'FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
