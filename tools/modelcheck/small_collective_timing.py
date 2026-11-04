#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The cost of the small collectives' shapes between node leads (the large-cluster
playbook's §5 and C16; the author's decision of 2026-11-02, docs/decisions/2026-11-01.md
answer 15): what a barrier, a small reduction and a small broadcast cost on the k-ary
tree as built, against a binomial tree, a dissemination barrier, recursive doubling and
a broadcast without its up pass - from the three numbers the link's measurement gives
and the records measured, in the pattern of shape_timing.py. docs/design/small-collective-model.md
is the note.

The inputs, microseconds:
  h   the hop: one message's way from a lead's agent to another's, handling at both
      ends - half a small put's round trip, as link-measure.c reads it;
  o   the message cost: what a sender spends on each message before the next can
      leave, so that a parent's k sends to its children leave k serial costs apart;
  w   the lead's wake: what a data collective's hop costs beyond the barrier's - the
      message lands in the lead PE's internal region and the agent must wake the lead
      thread waiting on the counter, where a barrier's message is handled by the agent
      alone; 0 for a barrier;
  I   the intranode phase: the node's members' arrival at their counter, the post to the
      lead's agent and the release's chained wake - base + per_pe x PEs a node, fitted to
      H12's two-node barriers (docs/fast-link-results.md).

The shapes, each a closed form (cost) and an event simulation (simulate) of the loop:
  tree           the level-order k-ary tree of collective-tree.c, k from 2 to 32: a hop
                 and one message cost a level up - each child sends once, a parent
                 forwards when its last child's arrival is in - and down a hop and the
                 parent's serial sends, child j's release leaving j + 1 message costs
                 after the parent took its own; a small broadcast's credits go up as a
                 reduction's arrivals do, so it costs what the reduction costs
                 (internode-generic.c, in_pieces at one piece; progress.c's book_arrival
                 and release_down for the barrier).
  binomial       the binomial tree over the leads' indices: a lead's children are the
                 indices that add one bit below its lowest, sent largest subtree first;
                 ceil(log2 n) hops each way, the root's sends spread one a round, so that
                 a lead at index i is reached d hops and R - lowest_bit(i) message costs
                 after the root, d its bit count - R(h + o) each way on a full tree.
  dissemination  the dissemination barrier: ceil(log2 n) rounds, each lead sending to
                 the lead 2^r after it round the circle and waiting for the one 2^r
                 before; no root and no down pass: R(h + o).
  doubling       recursive doubling for a small allreduce: log2 rounds of one exchange
                 with the lead at i xor 2^r among the largest power of two of leads,
                 every one holding the result; the leads past that power of two fold
                 their vectors into a partner's first and take the result back after -
                 two rounds more at most, and fewer in effect, since a lead with no extra
                 sends its first round at once (doubling_cost says how).
  down           a small broadcast without its up pass: the root's data down the k-ary
                 tree as soon as it has it, no credits first - a single call's time
                 alone, since without the up pass nothing tells the root a child's slot
                 is free and a loop of such calls has no period of its own.

Two measures. `loop`: the period of a loop of calls with nothing between them, what
osoe-bench's barrier and collectives lines report - in steady state every lead enters
as it is released, so the root's next release comes max_i (down_i + up_i) + I after its
last, the leaf whose release comes last and whose arrival then has the longest way back
up; `call`: one call every lead enters at once, until the last is released - the slowest
way up, then the slowest way down. They differ where the leaf with the longest way down
is not the deepest: a level-order tree whose last level is partial, where the last
leaf of the level above, released after k serial sends at every level, has a shorter
way up than the deepest leaf. The simulation runs the loop event by event and gives
both; the closed form must agree with it to the microsecond, and does, on every shape
(SIMULATION) - it did not at first for recursive doubling past a power of two, where the
closed form charged two whole rounds for the extra leads and the loop showed them
overlapping the core's. What the closed form leaves out is what the simulation can add
and it cannot: a cost at the receiver for each message handled (`receive`), under which
a parent's k arrivals, landing together, queue at its agent, and the dissemination's one
message a round does not; `disagree` prints that table.

Fitted (FIT): with h and o from the records' own measurements, the tree at k = 8
reproduces the records' barriers, small reductions and small broadcasts on four nodes of
the Pi cluster and on two and eight nodes of Google Cloud within their spread. The
message cost the barriers imply is a few microseconds on both links - inside the 0 to
36 µs P38's least-less-least read on three cloud nodes, far under the 23 to 45 µs its
trimmed mean read - which the note discusses.

Usage:
  small_collective_timing.py [check]   the test (the models preset): every check, then the
                                       broken variants, each of which must fail
  small_collective_timing.py fit       fit o, I and w to the records and print the table
  small_collective_timing.py table     the fitted table, as `check` prints it
  small_collective_timing.py crossover the crossover tables at 8 to 32,768 nodes on the Pi's,
                                       the cloud's and two guessed links
  small_collective_timing.py disagree  where the simulation with a receiver's cost leaves
                                       the closed form
"""
import heapq
import statistics
import sys

import engine
from shape_timing import depth_of, max_digit_sum, tree

# ---- The links and nodes the records fit (fit, below) -----------------------------
#
# The Pi cluster, four nodes on gigabit Ethernet (docs/pi-cluster-results.md): the hop
# 86 µs, shape_timing.py's fit, inside link-measure.c's 86.8 to 94.0 and half the 169 to
# 175 µs 8-byte put round trip; the message cost 1 µs, held - its barriers are all on
# four nodes, a star, where 3o and the node's base are one number - between P4's
# measured 0 and shape_timing.py's fitted 4, inside the 0 to 21 link-measure.c read from
# start to start; its node 14 + 0.6 µs a PE, from the barriers at 1, 2 and 4 PEs a
# node; the wake 5.5 µs, the 8-byte reduce and broadcast 3 to 22 µs over the barrier.
# Google Cloud, eight c4-standard-16 nodes (docs/fast-link-results.md): the hop 43 µs,
# link-measure.c's 43 to 53 (H14) and half the 88.6 µs 8-byte put round trip of the
# two-PE job - the fastest round trip, 41.3 µs at 64 PEs, is the floor an awake agent
# gives, and an idle one's wake is the rest (P38's finding); the message cost 2 µs,
# fitted, where P38's least-less-least read 0 to 36 and the trimmed mean 23 to 45; its
# node 0 + 2.2 µs a PE, from H12's 125, 157, 221 and 672 µs at 16 to 256 PEs a node
# on two nodes and the two-PE job's 87.3; the wake 12 µs, the 8-byte reduce 24 and 44
# µs over the barrier in two sessions, the broadcast 13 in one.
PI_LINK = dict(h=86.0, o=1.0, w=5.5)
PI_NODE = dict(base=14.0, per_pe=0.6)
CLOUD_LINK = dict(h=43.0, o=2.0, w=12.0)
CLOUD_NODE = dict(base=0.0, per_pe=2.2)
# The two guessed fast links of the brief, for the crossover tables.
FAST_LINK = dict(h=10.0, o=2.0, w=5.0)
LOADED_LINK = dict(h=40.0, o=25.0, w=12.0)
# The k-ary tree's fan-out as built, OSOE_COLLECTIVE_FANOUT's default.
FANOUT = 8
FANOUT_MOST = 32
# True in a broken variant alone (check's planted errors): the closed form counts a
# parent's sends as one message cost a level, not j + 1 for child j.
SERIAL_SENDS = True


def intranode(node, pes):
    """The intranode phase's cost at `pes` PEs a node."""
    return node['base'] + node['per_pe'] * pes


def hop_of(link, op):
    """A hop of `op`: the barrier's h, a data collective's h + w."""
    return link['h'] + (0.0 if op == 'barrier' else link['w'])


def rounds_of(nodes):
    """ceil(log2 nodes): the binomial tree's depth, the dissemination's and recursive
    doubling's rounds."""
    r = 0
    while (1 << r) < nodes:
        r += 1
    return r


# ---- The k-ary tree, closed form and walk ----------------------------------------

def tree_ways(nodes, k):
    """The (depth, digit sum) pairs of the leaves that can be slowest: a level-order
    tree is full to level L - 1 and partial at L, and a leaf's positions on its way
    from the root are the base-k digits of its offset in its level (shape_timing.py's
    tree_terms), so the slowest way down ends at the level-L leaf of largest digit sum
    or at the last leaf of level L - 1 if that one has no children."""
    if nodes <= 1:
        return [(0, 0)]
    L = depth_of(nodes, k)
    full = (k ** L - 1) // (k - 1)
    last = nodes - full
    ways = [(L, max_digit_sum(last - 1, k, L))]
    if -(-last // k) < k ** (L - 1):
        ways.append((L - 1, (L - 1) * (k - 1)))
    return ways


def tree_way_cost(depth, digits, hop, o):
    """What a leaf's way down costs: a hop a level and, at each level, the parent's
    sends up to and including this child's - its position + 1 message costs, the digit
    sum plus the depth over the way; the broken variant one a level."""
    return depth * hop + (depth + digits if SERIAL_SENDS else depth) * o


def tree_cost(op, nodes, k, link, I, measure='loop'):
    """The k-ary tree's cost of `op`, closed form: `loop`, the period of a loop of
    calls, I + max over the leaves of the way down and d levels back up, a hop and a
    message cost each; `call`, I + L levels up then the slowest way down."""
    if nodes <= 1:
        return I
    hop, o = hop_of(link, op), link['o']
    ways = tree_ways(nodes, k)
    if measure == 'call':
        L = max(d for d, _ in ways)
        return I + L * (hop + o) + max(tree_way_cost(d, s, hop, o) for d, s in ways)
    return I + max(tree_way_cost(d, s, hop, o) + d * (hop + o) for d, s in ways)


def tree_cost_walk(op, nodes, k, link, I, measure='loop'):
    """The reference: every leaf walked."""
    if nodes <= 1:
        return I
    hop, o = hop_of(link, op), link['o']
    parent, children, depth = tree(nodes, k)
    position = [0] * nodes
    for i in range(nodes):
        for j, c in enumerate(children[i]):
            position[c] = j
    down = [0.0] * nodes
    for i in range(1, nodes):
        down[i] = down[parent[i]] + hop + (position[i] + 1) * o
    leaves = [i for i in range(nodes) if not children[i]]
    if measure == 'call':
        return I + max(depth) * (hop + o) + max(down[i] for i in leaves)
    return I + max(down[i] + depth[i] * (hop + o) for i in leaves)


# ---- The binomial tree -----------------------------------------------------------

def binomial_children(nodes, i):
    """Lead i's children in the binomial tree over the indices, largest subtree first:
    i + 2^m for m below i's lowest set bit (every bit for the root), under `nodes`."""
    R = rounds_of(nodes)
    lowest = R if i == 0 else (i & -i).bit_length() - 1
    return [i + (1 << m) for m in reversed(range(lowest)) if i + (1 << m) < nodes]


def binomial_tree(nodes):
    """parent, children (in send order) and depth of every lead."""
    children = [binomial_children(nodes, i) for i in range(nodes)]
    parent = [None] * nodes
    for i in range(nodes):
        for c in children[i]:
            parent[c] = i
    depth = [0] * nodes
    for i in range(1, nodes):
        depth[i] = depth[parent[i]] + 1
    return parent, children, depth


def binomial_cost(op, nodes, link, I, measure='loop'):
    """The binomial tree's cost: every lead's way down walked - a hop a level, and at
    each level its parent's sends to the larger subtrees first - and its levels back up;
    on a full tree R(h + o) each way, the serial sends spread one a round."""
    if nodes <= 1:
        return I
    hop, o = hop_of(link, op), link['o']
    parent, children, depth = binomial_tree(nodes)
    down = [0.0] * nodes
    for i in range(nodes):
        for j, c in enumerate(children[i]):
            down[c] = down[i] + hop + (j + 1) * o
    if measure == 'call':
        return I + max(depth) * (hop + o) + max(down)
    return I + max(down[i] + depth[i] * (hop + o) for i in range(nodes))


# ---- The dissemination barrier and recursive doubling ------------------------------

def dissemination_cost(nodes, link, I, measure='loop'):
    """R rounds, each a message cost and a hop, every lead in step; no down pass, so
    the loop's period is a call's time."""
    if nodes <= 1:
        return I
    return I + rounds_of(nodes) * (link['h'] + link['o'])


def doubling_cost(op, nodes, link, I, measure='loop'):
    """Recursive doubling's cost, every lead walked: a core lead's round r ends at its own
    send, o after its round r - 1 ended, or at its partner's message, a hop and a message
    cost after the partner's round r - 1 - on a power of two R(h + o), every lead in step;
    past one the extra leads' vectors reach their partners a round late, and the result
    goes back to them a round after, but a core lead with no extra sends its first round
    at once, so the two rounds overlap the core's and a call costs less than R + 2 rounds
    - a first closed form charged R + 2, and the simulation found it out. No down pass, so
    the loop's period is a call's time."""
    if nodes <= 1:
        return I
    hop, o = hop_of(link, op), link['o']
    core = 1 << (nodes.bit_length() - 1)
    extras = nodes - core
    done = [hop + o if i < extras else 0.0 for i in range(core)]
    for r in range(core.bit_length() - 1):
        done = [max(done[i] + o, done[i ^ (1 << r)] + o + hop) for i in range(core)]
    last = max(done)
    if extras:
        last = max(last, max(done[i] + o + hop for i in range(extras)))
    return I + last


def down_cost(nodes, k, link, I):
    """A broadcast without its up pass: one call, the slowest way down the k-ary tree."""
    if nodes <= 1:
        return I
    hop, o = hop_of(link, 'broadcast'), link['o']
    return I + max(tree_way_cost(d, s, hop, o) for d, s in tree_ways(nodes, k))


def cost(shape, op, nodes, link, I, k=FANOUT, measure='loop'):
    """One shape's closed form, by name."""
    if shape == 'tree':
        return tree_cost(op, nodes, k, link, I, measure)
    if shape == 'binomial':
        return binomial_cost(op, nodes, link, I, measure)
    if shape == 'dissemination':
        return dissemination_cost(nodes, link, I, measure)
    if shape == 'doubling':
        return doubling_cost(op, nodes, link, I, measure)
    if shape == 'down':
        return down_cost(nodes, k, link, I)
    raise ValueError(shape)


# ---- The event simulation -----------------------------------------------------------

class Simulation:
    """A loop of `op` over `nodes` leads in `shape`, event by event. Each lead's agent
    is one server: a send costs it o, after which the message is h (+ w for a data
    collective) from landing; with `receive` > 0 a message landing is handled only
    once the agent has handled those before it, `receive` each - the closed form's
    assumption is 0. A lead released at t has its members' next arrival at t + I;
    for `call`, every lead's members arrive at 0 and the answer is the last release
    plus I."""

    def __init__(self, shape, op, nodes, link, I, k=FANOUT, receive=0.0):
        self.shape, self.op, self.n, self.I, self.k = shape, op, nodes, I, k
        self.hop, self.o, self.receive = hop_of(link, op), link['o'], receive
        self.events, self.order = [], 0
        self.busy = [0.0] * nodes            # the agent's sends
        self.handled = [0.0] * nodes         # the agent's receives, with `receive`
        self.released = [[] for _ in range(nodes)]
        self.loop = False
        if shape in ('tree', 'down'):
            self.parent, self.children, _ = tree(nodes, k)
        elif shape == 'binomial':
            self.parent, self.children, _ = binomial_tree(nodes)
        self.R = rounds_of(nodes)
        self.core = 1 << (nodes.bit_length() - 1)
        self.state = {}

    def at(self, t, f, *a):
        heapq.heappush(self.events, (t, self.order, f, a))
        self.order += 1

    def send(self, i, j, now, f, *a):
        """Lead i sends to lead j: o of i's time, then the hop; handled at j."""
        self.busy[i] = max(self.busy[i], now) + self.o
        self.at(self.busy[i] + self.hop, self.land, j, f, a)
        return self.busy[i]

    def land(self, j, f, a, now):
        if self.receive > 0:
            self.handled[j] = max(self.handled[j], now) + self.receive
            now = self.handled[j]
        f(*a, now)

    def release(self, i, c, now):
        self.released[i].append(now)
        if self.loop:
            self.at(now + self.I, self.own, i, c + 1)

    # -- the trees: tree, binomial, down
    def own(self, i, c, now):
        if self.shape == 'down':
            if i == 0:
                self.release_down(i, c, now)
            return
        if self.shape in ('tree', 'binomial'):
            self.arrival(i, c, now)
        elif self.shape == 'dissemination':
            self.state.setdefault(('round', i), {})[c] = 0
            self.send(i, (i + 1) % self.n, now, self.dissemination_in, (i + 1) % self.n, c, 0)
            self.dissemination_step(i, c, now)
        elif self.shape == 'doubling':
            self.doubling_start(i, c, now)

    def arrival(self, i, c, now):
        key = ('arrived', i, c)
        self.state[key] = self.state.get(key, 0) + 1
        if self.state[key] < len(self.children[i]) + 1:
            return
        if self.parent[i] is None:
            self.release_down(i, c, now)
        else:
            self.send(i, self.parent[i], now, self.arrival, self.parent[i], c)

    def release_down(self, i, c, now):
        for child in self.children[i]:
            self.send(i, child, now, self.release_down, child, c)
        self.release(i, c, now)

    # -- the dissemination barrier
    def dissemination_in(self, i, c, r, now):
        key = ('got', i, c, r)
        self.state[key] = True
        self.dissemination_step(i, c, now)

    def dissemination_step(self, i, c, now):
        rounds = self.state.setdefault(('round', i), {})
        if c not in rounds:
            return                               # not entered yet; the message waits
        while self.state.get(('got', i, c, rounds[c])):
            rounds[c] += 1
            if rounds[c] == self.R:
                del rounds[c]
                self.release(i, c, now)
                return
            to = (i + (1 << rounds[c])) % self.n
            self.send(i, to, now, self.dissemination_in, to, c, rounds[c])

    # -- recursive doubling
    def doubling_start(self, i, c, now):
        if i >= self.core:                       # an extra lead: its vector to a partner
            partner = i - self.core
            self.send(i, partner, now, self.doubling_in, partner, c, -1)
            return
        self.state[('round', i, c)] = -1 if i + self.core < self.n else 0
        self.doubling_step(i, c, now)

    def doubling_in(self, i, c, r, now):
        self.state[('got', i, c, r)] = True
        if i >= self.core:
            self.release(i, c, now)              # the result came back
            return
        self.doubling_step(i, c, now)

    def doubling_step(self, i, c, now):
        """A core lead's rounds: -1 waits for its extra's vector; round r sends to the
        partner i xor 2^r once, and ends when the partner's message is in; after the
        core's log2 rounds the result goes back to the extra, and the lead is released."""
        key = ('round', i, c)
        if key not in self.state:
            return
        core_rounds = self.core.bit_length() - 1
        while True:
            r = self.state[key]
            if r == -1:
                if not self.state.get(('got', i, c, -1)):
                    return
                self.state[key] = 0
                continue
            if r == core_rounds:
                del self.state[key]
                if i + self.core < self.n:
                    self.send(i, i + self.core, now, self.doubling_in, i + self.core, c, self.R)
                self.release(i, c, now)
                return
            if not self.state.get(('sent', i, c, r)):
                self.state[('sent', i, c, r)] = True
                self.send(i, i ^ (1 << r), now, self.doubling_in, i ^ (1 << r), c, r)
            if not self.state.get(('got', i, c, r)):
                return
            self.state[key] = r + 1

    def run(self, measure='loop', rounds=4):
        self.loop = measure == 'loop'
        for i in range(self.n):
            self.at(0.0, self.own, i, 0)
        while self.events:
            t, _, f, a = heapq.heappop(self.events)
            f(*a, t)
            if self.loop and all(len(r) >= rounds for r in self.released):
                break
        if measure == 'call':
            return max(r[0] for r in self.released) + self.I
        periods = [self.released[0][j + 1] - self.released[0][j] for j in range(rounds - 1)]
        return periods[-1]


def simulate(shape, op, nodes, link, I, k=FANOUT, measure='loop', receive=0.0):
    """One shape's time by the simulation, microseconds; a one-node job is its
    intranode phase alone, as the closed form has it."""
    if nodes <= 1:
        return I
    if shape == 'down' and measure == 'loop':
        measure = 'call'
    return Simulation(shape, op, nodes, link, I, k, receive).run(measure)


# ---- The records ---------------------------------------------------------------------
#
# Each row: the operation, nodes, PEs a node, the microseconds the records measured -
# every session's median that measured that shape - and where. A row's spread is the
# range of its values, widened to a tenth either side of their median where the records
# show less: the same line moved by 7 to 14% between sessions on both clusters (the
# Pi's barrier 176 to 183 bound and 196 to 208 unbound; the cloud's 99, 117 and 132 at
# 8 x 8).
PI_ROWS = [
    ('barrier', 4, 1, [181.0], 'the first session, PEs per node'),
    ('barrier', 4, 2, [202.0], 'the first session, PEs per node'),
    ('barrier', 4, 4, [185.0, 176.0, 183.0, 196.0, 208.0, 175.6, 182.6, 180.6, 175.7],
     'the sessions of 2026-09-26 to 2026-10-30, bound and unbound'),
]
CLOUD_ROWS = [
    ('barrier', 2, 1, [87.3], 'the second session, the two-PE job'),
    ('barrier', 2, 16, [125.0], 'H12'),
    ('barrier', 2, 32, [157.0], 'H12'),
    ('barrier', 2, 64, [221.0], 'H12'),
    ('barrier', 2, 256, [672.0], 'H12, 16 PEs a vCPU'),
    ('barrier', 8, 8, [132.0, 117.0, 99.0], 'the second session, H11, H15'),
    ('barrier', 8, 16, [146.0, 139.0], 'the second session, H11'),
    ('barrier', 8, 32, [184.0], 'the second session'),
]
# The small data collectives are fitted as what each costs over the barrier of its own
# session, since a session's lines move together (the cloud's 8 x 8 barrier 99 to 132 µs
# across three) and the model has the two differ by the lead's wake alone, 2w for the
# two levels of a star: the op, nodes, PEs a node, the gaps measured and where. A
# reduction's and a broadcast's gaps are pooled: the model costs them alike.
PI_GAPS = [
    ('reduce', 4, 1, [3.0], 'the first session: 184 over 181'),
    ('reduce', 4, 4, [22.0, 11.0], '198 over 176 bound, 211 over 200 unbound'),
    ('broadcast', 4, 4, [19.0, 11.0], '195 over 176 bound, 211 over 200 unbound'),
]
CLOUD_GAPS = [
    ('reduce', 8, 8, [44.0, 24.0], 'H11: 161 over 117; H15: 123 over 99'),
    ('broadcast', 8, 8, [13.0], 'H15: 112 over 99'),
]


def spread(values, widen=0.1):
    m = statistics.median(values)
    return min(min(values), (1 - widen) * m), max(max(values), (1 + widen) * m)


def predicted(row, link, node):
    op, nodes, pes, _, _ = row
    return tree_cost(op, nodes, FANOUT, link, intranode(node, pes))


def predicted_gap(row, link, node):
    op, nodes, pes, _, _ = row
    I = intranode(node, pes)
    return tree_cost(op, nodes, FANOUT, link, I) - tree_cost('barrier', nodes, FANOUT, link, I)


def table_lines(name, rows, gaps, link, node):
    lines = [f'{name}: h {link["h"]:g}, o {link["o"]:g}, w {link["w"]:g} us; the node '
             f'{node["base"]:g} + {node["per_pe"]:g} us a PE',
             f'  {"op":<16}{"nodes":>6}{"PEs/node":>9}{"measured":>22}{"model":>8}  within']
    for row in rows:
        op, nodes, pes, values, _ = row
        low, high = spread(values)
        p = predicted(row, link, node)
        measured = f'{min(values):.0f} to {max(values):.0f}' if len(values) > 1 else \
            f'{values[0]:.0f}'
        lines.append(f'  {op:<16}{nodes:>6}{pes:>9}{measured:>22}{p:>8.0f}  '
                     f'{"yes" if low <= p <= high else "NO":<3} ({low:.0f} to {high:.0f})')
    pooled = [g for row in gaps for g in row[3]]
    low, high = spread(pooled)
    for row in gaps:
        op, nodes, pes, values, _ = row
        p = predicted_gap(row, link, node)
        measured = f'{min(values):.0f} to {max(values):.0f}' if len(values) > 1 else \
            f'{values[0]:.0f}'
        lines.append(f'  {op + " - barrier":<16}{nodes:>6}{pes:>9}{measured:>22}{p:>8.0f}  '
                     f'{"yes" if low <= p <= high else "NO":<3} ({low:.0f} to {high:.0f}, '
                     'the gaps pooled)')
    return lines


def fit_link(rows, gaps, h, o_range, base_range, per_pe_range, w_range):
    """The o, I and w that put every row nearest its measured median at the hop h: the
    least worst relative error over the barrier rows for o and I, then over the pooled
    gaps' median for w."""
    best = None
    for o in o_range:
        for base in base_range:
            for per_pe in per_pe_range:
                link, node = dict(h=h, o=o, w=0.0), dict(base=base, per_pe=per_pe)
                worst = max(abs(predicted(r, link, node) / statistics.median(r[3]) - 1)
                            for r in rows)
                if best is None or worst < best[0]:
                    best = (worst, o, base, per_pe)
    worst, o, base, per_pe = best
    node = dict(base=base, per_pe=per_pe)
    pooled = statistics.median([g for row in gaps for g in row[3]])
    best_w = None
    for w in w_range:
        link = dict(h=h, o=o, w=w)
        worst_w = max(abs(predicted_gap(r, link, node) / pooled - 1) for r in gaps)
        if best_w is None or worst_w < best_w[0]:
            best_w = (worst_w, w)
    return dict(h=h, o=o, w=best_w[1]), node, worst, best_w[0]


def steps(a, b, step):
    out, x = [], a
    while x <= b + 1e-9:
        out.append(round(x, 6))
        x += step
    return out


def fit_all():
    """The Pi's o is held at PI_LINK's: its three barrier rows are all on four nodes, a
    star, where 3o + base is one number and the two cannot be told apart."""
    pi = fit_link(PI_ROWS, PI_GAPS, PI_LINK['h'], [PI_LINK['o']], steps(0, 30, 1),
                  steps(0, 4, 0.1), steps(0, 40, 0.5))
    cloud = fit_link(CLOUD_ROWS, CLOUD_GAPS, CLOUD_LINK['h'], steps(0, 10, 0.5), steps(0, 30, 1),
                     steps(0, 4, 0.1), steps(0, 40, 0.5))
    return pi, cloud


# ---- The checks ------------------------------------------------------------------------

def check_terms(failures):
    """The k-ary closed form against its walk, both measures, every k to 32 and node
    count to 300."""
    link = dict(h=10.0, o=3.0, w=4.0)
    for op in ('barrier', 'reduce'):
        for k in range(2, FANOUT_MOST + 1):
            for nodes in range(1, 300 if k < 5 else 130):
                for measure in ('loop', 'call'):
                    a = tree_cost(op, nodes, k, link, 7.0, measure)
                    b = tree_cost_walk(op, nodes, k, link, 7.0, measure)
                    if abs(a - b) > 1e-6:
                        failures.append(f'TERMS: tree_cost({op}, {nodes}, {k}, {measure}) = '
                                        f'{a:.3f}, the walk says {b:.3f}')
                        return


def check_simulation(failures, shapes=('tree', 'binomial', 'dissemination', 'doubling', 'down')):
    """Every closed form against the simulation of its loop, both measures, on a grid of
    node counts, fan-outs and two links - a message cost below the hop and one above
    it - to the microsecond."""
    links = [dict(h=20.0, o=3.0, w=6.0), dict(h=10.0, o=25.0, w=0.0)]
    for shape in shapes:
        ops = ('barrier',) if shape == 'dissemination' else \
            ('reduce',) if shape == 'doubling' else ('broadcast',) if shape == 'down' else \
            ('barrier', 'reduce')
        for op in ops:
            for nodes in (1, 2, 3, 4, 5, 7, 8, 9, 13, 16, 17, 31, 33, 64, 65, 100):
                for k in ((2, 3, 8) if shape in ('tree', 'down') else (8,)):
                    for link in links:
                        measures = ('call',) if shape == 'down' else ('loop', 'call')
                        for measure in measures:
                            a = cost(shape, op, nodes, link, 5.0, k, measure)
                            b = simulate(shape, op, nodes, link, 5.0, k, measure)
                            if abs(a - b) > 1e-6:
                                failures.append(f'SIMULATION: {shape} {op} on {nodes} nodes, k '
                                                f'{k}, h {link["h"]:g} o {link["o"]:g}, '
                                                f'{measure}: the closed form says {a:.3f}, '
                                                f'the simulation {b:.3f}')
                                return


def check_fit(failures, pi_link=PI_LINK, pi_node=PI_NODE, cloud_link=CLOUD_LINK,
              cloud_node=CLOUD_NODE):
    """The tree at k = 8 on each record's rows, within their spread; and the message
    costs inside what each cluster's sessions measured - 0 to 21 µs on the Pi, 0 to 45
    on the cloud - and the hops what they read."""
    for name, rows, gaps, link, node in (('the Pi', PI_ROWS, PI_GAPS, pi_link, pi_node),
                                         ('the cloud', CLOUD_ROWS, CLOUD_GAPS, cloud_link,
                                          cloud_node)):
        for row in rows:
            low, high = spread(row[3])
            p = predicted(row, link, node)
            if not low <= p <= high:
                failures.append(f'FIT: on {name} the tree at k = 8 gives a {row[0]} of '
                                f'{row[1]} x {row[2]} {p:.0f} us, outside the records\' '
                                f'{low:.0f} to {high:.0f}')
        low, high = spread([g for row in gaps for g in row[3]])
        for row in gaps:
            p = predicted_gap(row, link, node)
            if not low <= p <= high:
                failures.append(f'FIT: on {name} a {row[0]} of {row[1]} x {row[2]} costs '
                                f'{p:.0f} us over the barrier, outside the records\' {low:.0f} '
                                f'to {high:.0f}')
    if not 0 <= pi_link['o'] <= 21 or not 86 <= pi_link['h'] <= 94:
        failures.append('FIT: the Pi\'s h and o are not what its sessions measured')
    if not 0 <= cloud_link['o'] <= 45 or not 20 <= cloud_link['h'] <= 53:
        failures.append('FIT: the cloud\'s h and o are not what its sessions measured')


def check_pins(failures):
    """What the shapes must say, whatever the fit moves: on every link the binomial
    tree and the dissemination cost R(h + o) each way or in all on a power of two; a
    broadcast costs what a reduction does on the tree; recursive doubling pays more than R
    rounds and at most R + 2 past a power of two; the loop's period is never more than a call; a wider tree
    is never slower at o = 0 and the star is slower than the binary tree at h = 0 from 16
    nodes; and the tree at 8 is the records' shape - on 4 and 8 nodes a star, two hops and
    the root's sends."""
    link = dict(h=30.0, o=4.0, w=5.0)
    for nodes in (2, 4, 8, 16, 64, 1024):
        R = rounds_of(nodes)
        if abs(binomial_cost('barrier', nodes, link, 0.0, 'call') - 2 * R * 34.0) > 1e-6:
            failures.append(f'PINS: the binomial barrier on {nodes} nodes is not 2R(h + o)')
        if abs(dissemination_cost(nodes, link, 0.0) - R * 34.0) > 1e-6:
            failures.append(f'PINS: the dissemination on {nodes} nodes is not R(h + o)')
        if abs(doubling_cost('reduce', nodes, link, 0.0) - R * 39.0) > 1e-6:
            failures.append(f'PINS: recursive doubling on {nodes} nodes is not R(h + w + o)')
        past = doubling_cost('reduce', nodes + 1, link, 0.0)
        if not R * 39.0 < past <= (R + 2) * 39.0:
            failures.append(f'PINS: recursive doubling on {nodes + 1} nodes costs {past:.0f}, '
                            'not between R and R + 2 rounds')
    for nodes in (3, 8, 9, 73, 100):
        if tree_cost('broadcast', nodes, 8, link, 0.0) != tree_cost('reduce', nodes, 8, link, 0.0):
            failures.append(f'PINS: a small broadcast on {nodes} nodes costs other than a '
                            'reduction')
        for k in (2, 8):
            if tree_cost('barrier', nodes, k, link, 0.0) > \
               tree_cost('barrier', nodes, k, link, 0.0, 'call') + 1e-9:
                failures.append(f'PINS: on {nodes} nodes at k = {k} the loop costs more than '
                                'a call')
    free = dict(h=30.0, o=0.0, w=0.0)
    instant = dict(h=0.0, o=4.0, w=0.0)
    for nodes in (9, 73, 585):
        costs = [tree_cost('barrier', nodes, k, free, 0.0) for k in range(2, 33)]
        if any(costs[j + 1] > costs[j] + 1e-9 for j in range(len(costs) - 1)):
            failures.append(f'PINS: at o = 0 a wider tree is slower on {nodes} nodes')
        if nodes >= 16 and tree_cost('barrier', nodes, FANOUT_MOST, instant, 0.0) <= \
           tree_cost('barrier', nodes, 2, instant, 0.0):
            failures.append(f'PINS: at h = 0 the widest tree is not slower than the binary '
                            f'tree on {nodes} nodes')
    for nodes in (4, 8):
        want = 2 * 30.0 + (nodes - 1) * 4.0 + 4.0
        if abs(tree_cost('barrier', nodes, 8, link, 0.0) - want) > 1e-6:
            failures.append(f'PINS: the tree at 8 on {nodes} nodes is not a star of two hops '
                            f'and the root\'s {nodes - 1} sends')
    if down_cost(8, 8, link, 0.0) >= tree_cost('broadcast', 8, 8, link, 0.0, 'call'):
        failures.append('PINS: a broadcast without its up pass is not cheaper than with it')


def checks(terms=True, simulation=True, fit=True, pins=True, **fit_args):
    failures = []
    if terms:
        check_terms(failures)
    if simulation:
        check_simulation(failures)
    if fit:
        check_fit(failures, **fit_args)
    if pins:
        check_pins(failures)
    return failures


def unexpected(failures, check):
    names = [engine.named(f) for f in failures]
    failed = check if check in names else (names[0] if names else None)
    wrong = engine.judge(not failures, failed, check)
    return '' if wrong is None else '   <-- UNEXPECTED' + wrong


# ---- The crossovers -----------------------------------------------------------------------

CROSSOVER_NODES = (8, 64, 512, 4096, 32768)
CROSSOVER_LINKS = (('the Pi', PI_LINK, PI_NODE),
                   ('the Pi at shape_timing.py\'s o of 4', dict(PI_LINK, o=4.0), PI_NODE),
                   ('the cloud', CLOUD_LINK, CLOUD_NODE),
                   ('fast, h 10 o 2', FAST_LINK, CLOUD_NODE),
                   ('loaded, h 40 o 25', LOADED_LINK, CLOUD_NODE))
CROSSOVER_PES = 8


def best_tree(op, nodes, link, I, measure='loop'):
    """The fan-out from 2 to 32 of least cost, ties to the smaller."""
    return min(range(2, FANOUT_MOST + 1), key=lambda k: (tree_cost(op, nodes, k, link, I, measure), k))


def crossover_lines():
    """For each link and node count, every shape's time at 8 PEs a node - the barrier's
    and the reduction's a loop's period, the broadcast's a call's, since without its up
    pass a loop has none - and which beat the tree at 8; then the message cost over the
    hop at which each alternative beats it, the intranode left out, so that the answer
    is a ratio and not a time."""
    lines = []
    for name, link, node in CROSSOVER_LINKS:
        I = intranode(node, CROSSOVER_PES)
        lines.append(f'{name}: h {link["h"]:g}, o {link["o"]:g}, w {link["w"]:g} us; the node '
                     f'{I:.0f} us at {CROSSOVER_PES} PEs a node. Microseconds: a loop\'s period, '
                     f'the broadcasts one call; * beats the tree at {FANOUT}')
        lines.append(f'  {"nodes":>6} | {"barrier: tree 8":>15} {"tree 16":>9} {"best k":>11} '
                     f'{"binomial":>9} {"dissem.":>9} | {"reduce: tree 8":>14} {"best k":>11} '
                     f'{"binomial":>9} {"doubling":>9} | {"bcast: tree 8":>13} {"binomial":>9} '
                     f'{"no up pass":>11}')
        for nodes in CROSSOVER_NODES:
            def cell(value, against):
                return f'{value:>8.0f}{"*" if value < against - 1e-9 else " "}'
            b8 = tree_cost('barrier', nodes, 8, link, I)
            kb = best_tree('barrier', nodes, link, I)
            r8 = tree_cost('reduce', nodes, 8, link, I)
            kr = best_tree('reduce', nodes, link, I)
            c8 = tree_cost('broadcast', nodes, 8, link, I, 'call')
            lines.append(
                f'  {nodes:>6} | {b8:>15.0f} {cell(tree_cost("barrier", nodes, 16, link, I), b8)} '
                f'{cell(tree_cost("barrier", nodes, kb, link, I), b8)}k{kb:<2} '
                f'{cell(binomial_cost("barrier", nodes, link, I), b8)} '
                f'{cell(dissemination_cost(nodes, link, I), b8)} | {r8:>14.0f} '
                f'{cell(tree_cost("reduce", nodes, kr, link, I), r8)}k{kr:<2} '
                f'{cell(binomial_cost("reduce", nodes, link, I), r8)} '
                f'{cell(doubling_cost("reduce", nodes, link, I), r8)} | {c8:>13.0f} '
                f'{cell(binomial_cost("broadcast", nodes, link, I, "call"), c8)} '
                f'{cell(down_cost(nodes, 8, link, I), c8):>11}')
        lines.append('')
    lines.append('The message cost over the hop, o / h, at which each alternative beats the '
                 f'tree at {FANOUT} (w = 0, no intranode), to o = 2h: a dash where it never does')
    lines.append(f'  {"nodes":>6} {"tree 16":>12} {"binomial":>12} {"dissem.":>12} '
                 f'{"doubling":>12}')
    for nodes in CROSSOVER_NODES:
        cells = []
        for shape, k in (('tree', 16), ('binomial', None), ('dissemination', None),
                         ('doubling', None)):
            wins = []
            for step in range(0, 201):
                ratio = step / 100
                link = dict(h=1.0, o=ratio, w=0.0)
                op = 'reduce' if shape == 'doubling' else 'barrier'
                against = tree_cost(op, nodes, FANOUT, link, 0.0)
                mine = tree_cost(op, nodes, k, link, 0.0) if shape == 'tree' else \
                    cost(shape, op, nodes, link, 0.0)
                wins.append(mine < against - 1e-9)
            if not any(wins):
                found = '-'
            else:
                first, last = wins.index(True), 200 - wins[::-1].index(True)
                found = f'{first / 100:.2f} to {last / 100:.2f}' if last < 200 else \
                    f'from {first / 100:.2f}'
            cells.append(f'{found:>12}')
        lines.append(f'  {nodes:>6} ' + ' '.join(cells))
    return lines


def disagree_lines():
    """Where the simulation leaves the closed form: a receiver's cost a message, on a
    barrier on the tree at 8, the binomial and the dissemination at 64 and 4,096 nodes
    on the cloud's link, the receiver charged 0, o and 3o."""
    lines = ['A receiver\'s cost a message handled, which the closed form has not '
             '(microseconds, the loop\'s period, the cloud\'s link, no intranode):',
             f'  {"nodes":>6} {"receive":>8} | {"tree 8 closed":>13} {"simulated":>10} | '
             f'{"binomial closed":>15} {"simulated":>10} | {"dissem. closed":>14} {"simulated":>10}']
    link = CLOUD_LINK
    for nodes in (64, 4096):
        for receive in (0.0, link['o'], 3 * link['o']):
            lines.append(
                f'  {nodes:>6} {receive:>8.0f} | '
                f'{tree_cost("barrier", nodes, 8, link, 0.0):>13.0f} '
                f'{simulate("tree", "barrier", nodes, link, 0.0, 8, receive=receive):>10.0f} | '
                f'{binomial_cost("barrier", nodes, link, 0.0):>15.0f} '
                f'{simulate("binomial", "barrier", nodes, link, 0.0, receive=receive):>10.0f} | '
                f'{dissemination_cost(nodes, link, 0.0):>14.0f} '
                f'{simulate("dissemination", "barrier", nodes, link, 0.0, receive=receive):>10.0f}')
    return lines


def main(argv):
    global SERIAL_SENDS
    mode = argv[1] if len(argv) > 1 else 'check'
    if mode == 'fit':
        (pi_link, pi_node, pi_worst, pi_w), (cloud_link, cloud_node, cloud_worst, cloud_w) = \
            fit_all()
        for name, link, node, worst, w_worst in (('the Pi', pi_link, pi_node, pi_worst, pi_w),
                                                 ('the cloud', cloud_link, cloud_node,
                                                  cloud_worst, cloud_w)):
            print(f'{name}: h {link["h"]:g} fixed; o {link["o"]:g}, the node {node["base"]:g} + '
                  f'{node["per_pe"]:g} us a PE, the barriers within {worst:.1%} of their medians;'
                  f' w {link["w"]:g}, the data rows within {w_worst:.1%}')
            print("\n".join(table_lines(name, PI_ROWS if name == "the Pi" else CLOUD_ROWS,
                                        PI_GAPS if name == 'the Pi' else CLOUD_GAPS, link, node)))
        return 0
    if mode == 'table':
        print('\n'.join(table_lines('the Pi', PI_ROWS, PI_GAPS, PI_LINK, PI_NODE)))
        print('\n'.join(table_lines('the cloud', CLOUD_ROWS, CLOUD_GAPS, CLOUD_LINK, CLOUD_NODE)))
        return 0
    if mode == 'crossover':
        print('\n'.join(crossover_lines()))
        return 0
    if mode == 'disagree':
        print('\n'.join(disagree_lines()))
        return 0
    if mode != 'check':
        print(__doc__)
        return 2
    failures = checks()
    print(f'design: {len(failures)} failure(s)')
    for f in failures:
        print('   ', f)
    ok = not failures
    print('\n'.join(table_lines('the Pi', PI_ROWS, PI_GAPS, PI_LINK, PI_NODE)))
    print('\n'.join(table_lines('the cloud', CLOUD_ROWS, CLOUD_GAPS, CLOUD_LINK, CLOUD_NODE)))
    # The broken variants, each of which must fail the check written for it.
    SERIAL_SENDS = False
    broken = checks(simulation=False, fit=False, pins=False)
    SERIAL_SENDS = True
    mark = unexpected(broken, 'TERMS')
    print(f'broken serial sends (a parent\'s k sends one message cost a level): '
          f'{"fails" if broken else "PASSES - the check cannot see it"}{mark}')
    for f in broken[:2]:
        print('   ', f)
    ok = ok and not mark
    # The binomial's sends in the wrong order - smallest subtree first - costs the
    # deepest leaf the root's R sends: the simulation, which sends as the children are
    # listed, disagrees with the closed form's walk, which lists them largest first.
    saved = globals()['binomial_children']
    globals()['binomial_children'] = lambda nodes, i: list(reversed(saved(nodes, i)))
    broken = []
    link = dict(h=20.0, o=3.0, w=0.0)
    for nodes in (8, 16, 64):
        a = binomial_cost('barrier', nodes, link, 0.0, 'call')
        b = 2 * rounds_of(nodes) * 23.0
        if abs(a - b) > 1e-6:
            broken.append(f'PINS: the binomial barrier on {nodes} nodes is not 2R(h + o): '
                          f'{a:.0f} against {b:.0f}')
            break
    globals()['binomial_children'] = saved
    mark = unexpected(broken, 'PINS')
    print(f'broken binomial order (the smallest subtree sent first): '
          f'{"fails" if broken else "PASSES - the check cannot see it"}{mark}')
    for f in broken[:2]:
        print('   ', f)
    ok = ok and not mark
    broken = checks(terms=False, simulation=False, pins=False,
                    cloud_link=dict(CLOUD_LINK, o=23.0))
    mark = unexpected(broken, 'FIT')
    print(f'broken message cost (the cloud\'s o the trimmed mean\'s 23 us): '
          f'{"fails" if broken else "PASSES - the check cannot see it"}{mark}')
    for f in broken[:2]:
        print('   ', f)
    ok = ok and not mark
    broken = checks(terms=False, simulation=False, pins=False,
                    cloud_node=dict(base=CLOUD_NODE['base'], per_pe=0.0))
    mark = unexpected(broken, 'FIT')
    print(f'broken intranode (nothing a PE): '
          f'{"fails" if broken else "PASSES - the check cannot see it"}{mark}')
    for f in broken[:2]:
        print('   ', f)
    ok = ok and not mark
    print('ok' if ok else 'FAILED')
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
