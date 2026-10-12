#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The internode tree of a barrier (design §5.3, internode.h; phase 1 step 2 slice 3).

The AM backend's internode phase is a k-ary tree over the index of the
participating nodes: node index i has parent (i - 1) / k and children
k*i + 1 ... k*i + k. Each node's progress thread keeps, per team, the epoch in
progress, which children have arrived, and whether the node's own members have.
When both are complete it forwards ARRIVE to its parent - or, at the root,
starts the release. A node that receives RELEASE forwards it to its children
**first** and releases its own members afterwards, so the latency down the tree
is not serialized behind each level's local wake.

This model checks that protocol with every interleaving of the nodes' progress
threads, their local members, and the messages between them. It is the
successor of arrival_model.py, which checked one node's handoff against a root
abstracted into one thread; here every node is real and the root is one of them.

Properties
  P1  a node is released only after **every** participating node has arrived
      (ghost $arrived[n]: the epochs node n's members have all arrived for);
  P2  each node is released exactly once per epoch, in order (its release
      counter reads k when epoch k+1 is released);
  P3  no node ever holds two open epochs for one team, and none reopens an
      epoch it has already closed;
  and no trapped state: from every reachable state the job can still finish
  (the engine's AG EF done), with STUCK and LOST WAKE at dead ends.

Designs
  design         as described above, and as progress.c implements it. The
                 node's counts are reset at the moment it closes the epoch -
                 forwarding ARRIVE up, or at the root starting the release -
                 before anything is released.
Breakages, and the property each breaks (see FIRST below)
  forward_early  forward upward when all but one child has arrived. -> P1
  no_reset       count children across epochs without resetting: epoch 2
                 finds epoch 1's children still counted. -> P1
  release_first  the root-node hazard of §5.3 and §6.4: release locally before
                 forwarding down, and reset the node's counts afterwards, in the
                 later step that forwards. A member released early re-enters
                 and its node's next arrival lands in an epoch not yet closed.
                 -> P3
  wrong_epoch    a child's arrival is applied to the epoch the node's own
                 release counter implies, not the one the message carries.
                 Forwarding the release before releasing locally means a child
                 can be back with epoch k+1 while this node still reads k-1
                 released, so the arrival is booked against epoch k, which is
                 closed. -> P3
  release_twice  the local release applied twice. It exists because it is the
                 only variant that reaches P2's check, which no protocol variant
                 above fails first - a check no case can fail is the thing this
                 project keeps finding after the fact. -> P2

**Why forward-first needs the reset first.** A node that forwards the release
before its local wake lets its children re-enter at once, and their next
arrival can reach it before its own members have even been woken. That is
harmless exactly when the node closed its epoch - reset its counts - before
forwarding anything; `release_first` and `wrong_epoch` are the two ways of
getting that wrong, and both are silent at one epoch.

Shared words. The members of a node share its arrival counter, its release
counter and the handoff word (§5.3) with the node's progress thread; the tso
cases run them under x86-TSO, where the handoff is a plain release store
behind a locked add. The messages between nodes are abstracted as FIFO
channels, one per tree edge and direction; their wire, a socket, is the kernel's.

Usage: tree_barrier_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

EPOCHS = 2

# Violation codes an agent step leaves in its `err` local for the check after it.
P1, P2, P3_OPEN, P3_CLOSED = 1, 2, 3, 4


def parent_of(i, k):
    return (i - 1) // k if i > 0 else None


def children_of(i, k, n):
    return [c for c in range(k * i + 1, k * i + k + 1) if c < n]


def member(node, members, epochs):
    """One local member of `node`, running `epochs` barriers (§5.3, §5.4)."""
    B = []
    for k in range(epochs):
        e, p = k + 1, f'm{k}_'
        nxt = f'@m{k + 1}_A' if k + 1 < epochs else '@MEND'
        B += [(p + 'A', ('add', lambda L, n=node: ('arrived', n), lambda L: 1, 'n', '@' + p + 'T')),
              (p + 'T', ('br', lambda L, e=e, m=members: L['n'] == e * m, '@' + p + 'G', '@' + p + 'W')),
              # The last of the node's members in: record it for P1, then post.
              (p + 'G', ('gpre', lambda M, L, n=node, e=e: ({('$arrived', n): e}, {}), '@' + p + 'S')),
              # §3.16 single-writer publication: a release store, then the wake.
              (p + 'S', ('store', lambda L, n=node: ('handoff', n), lambda L, e=e: e, False,
                         '@' + p + 'W')),
              (p + 'W', ('set', 'n', lambda L: DEL, '@' + p + 'V')),
              # §5.4: every member waits for its node's release counter to equal
              # k+1. The futex protocol under it is epoch.c's, and the chained
              # wake has its own model.
              (p + 'V', ('await', lambda M, L, n=node, e=e: M[('released', n)] == e, nxt))]
    B += [('MEND', ('done',))]
    return assemble(B)


def agent(design, node, n, k, epochs, enabled):
    """The node's progress thread: the only thread that touches its tree state.
    `enabled` is the set of property checks that may fail; a disabled one is
    passed over and the step goes on as if it had not been checked."""
    parent = parent_of(node, k)
    kids = children_of(node, k, n)
    full = (1 << len(kids)) - 1

    def send(kind, to, e, M, mu):
        """Append a message to the (kind, to) channel: kind 'up' is ARRIVE from
        `to` toward its parent, 'down' is RELEASE toward `to`."""
        count = mu.get((kind + '#', to), M[(kind + '#', to)])
        mu[(kind, to, count)] = e
        mu[(kind + '#', to)] = count + 1

    def forward_down(e, M, mu):
        for c in kids:
            send('down', c, e, M, mu)

    def closed(L):
        return {'open': 0, 'own': 0} if design == 'no_reset' else {'open': 0, 'own': 0, 'mask': 0}

    def try_close(M, L, lu, mu):
        """Close the epoch if this node and every child have arrived."""
        own = lu.get('own', L['own'])
        mask = lu.get('mask', L['mask'])
        e = lu.get('open', L['open'])
        need = full
        if design == 'forward_early' and len(kids) > 0:
            need = full & ~(1 << (len(kids) - 1))       # all but the last child
        if not (own and (mask & need) == need and e):
            return
        lu['done_to'] = e
        if design == 'release_first':
            # Release locally now; forward and reset in a later step.
            if parent is None:
                lu['pend_local'] = e
                lu['pend_fwd'] = e
            else:
                send('up', node, e, M, mu)
            return
        lu.update(closed(L))
        if parent is None:
            forward_down(e, M, mu)
            lu['pend_local'] = e
        else:
            send('up', node, e, M, mu)

    def arrival(M, L, lu, mu, e, bit):
        """Book an arrival - this node's own (bit None) or a child's - for epoch e."""
        if e <= L['done_to'] and P3_CLOSED in enabled:
            lu['err'] = P3_CLOSED
            lu['e1'], lu['e2'] = e, L['done_to']
            return
        cur = L['open']
        if cur and cur != e and P3_OPEN in enabled:
            lu['err'] = P3_OPEN
            lu['e1'], lu['e2'] = cur, e
            return
        lu['open'] = e
        if bit is None:
            lu['own'] = 1
        else:
            lu['mask'] = L['mask'] | bit
        try_close(M, L, lu, mu)

    def local_release(M, L, lu, mu, e):
        for m in range(n):
            if M[('$arrived', m)] < e and P1 in enabled:
                lu['err'] = P1
                lu['e1'], lu['e2'] = e, m
                return
        have = mu.get(('released', node), M[('released', node)])
        if have != e - 1 and P2 in enabled:
            lu['err'] = P2
            lu['e1'], lu['e2'] = e, have
            return
        mu[('released', node)] = e

    def actions(M, L):
        out = []
        # This node's own members, through the handoff word (§5.3).
        h = M[('handoff', node)]
        if h != L['consumed']:
            lu, mu = {'consumed': h}, {}
            arrival(M, L, lu, mu, h, None)
            out.append((mu, lu, f'n{node}: own arrival {h}'))
        # A child's ARRIVE.
        for j, c in enumerate(kids):
            got = L[f'up{c}']
            if M[('up#', c)] > got:
                e = M[('up', c, got)]
                lu, mu = {f'up{c}': got + 1}, {}
                if design == 'wrong_epoch':
                    e = M[('released', node)] + 1
                arrival(M, L, lu, mu, e, 1 << j)
                out.append((mu, lu, f'n{node}: ARRIVE {M[("up", c, got)]} from n{c}'))
        # The parent's RELEASE: forward first, release locally after.
        if parent is not None:
            got = L['down']
            if M[('down#', node)] > got:
                e = M[('down', node, got)]
                lu, mu = {'down': got + 1}, {}
                if design == 'release_first':
                    lu['pend_local'] = e
                    lu['pend_fwd'] = e
                else:
                    forward_down(e, M, mu)
                    lu['pend_local'] = e
                out.append((mu, lu, f'n{node}: RELEASE {e}'))
        if design == 'release_first':
            if L['pend_local']:
                e = L['pend_local']
                lu, mu = {'pend_local': 0}, {}
                local_release(M, L, lu, mu, e)
                out.append((mu, lu, f'n{node}: local release {e}'))
            elif L['pend_fwd']:
                e = L['pend_fwd']
                lu, mu = {'pend_fwd': 0}, {}
                forward_down(e, M, mu)
                lu.update(closed(L))
                out.append((mu, lu, f'n{node}: forward and reset {e}'))
        elif L['pend_local']:
            e = L['pend_local']
            lu, mu = {'pend_local': 0}, {}
            local_release(M, L, lu, mu, e)
            if design == 'release_twice':
                lu['pend_local'] = e
            out.append((mu, lu, f'n{node}: local release {e}'))
        return out

    messages = {
        P1: lambda L: f'node {node} released epoch {L["e1"]} before node {L["e2"]} arrived',
        P2: lambda L: f'node {node} released epoch {L["e1"]} with its counter at {L["e2"]}: '
                      f'not exactly once, in order',
        P3_OPEN: lambda L: f'node {node} has epochs {L["e1"]} and {L["e2"]} open at once',
        P3_CLOSED: lambda L: f'node {node} booked an arrival for epoch {L["e1"]}, which it '
                             f'closed (it has closed through {L["e2"]})',
    }
    init = {'consumed': 0, 'open': 0, 'own': 0, 'mask': 0, 'done_to': 0, 'down': 0,
            'pend_local': 0, 'pend_fwd': 0}
    for c in kids:
        init[f'up{c}'] = 0
    # Locals start as None; give each its zero, one local step apiece.
    B = [(f'I{j}', ('set', name, lambda L, v=v: v, f'@I{j + 1}')) for j, (name, v) in
         enumerate(init.items())]
    B += [(f'I{len(init)}', ('set', 'err', lambda L: DEL, '@LOOP')),
          ('LOOP', ('nd', actions, '@CHK')),
          ('CHK', ('check', lambda M, L: messages[L['err']](L) if L['err'] else None, '@LOOP'))]
    return assemble(B)


ALL = frozenset((P1, P2, P3_OPEN, P3_CLOSED))
TARGET = {'forward_early': {P1}, 'no_reset': {P1}, 'release_first': {P3_OPEN},
          'wrong_epoch': {P3_CLOSED}, 'release_twice': {P2}}


def case(design, n, k, members, epochs=EPOCHS, targeted=False):
    """`members` maps a node to its member count; every other node has one."""
    threads, agents = [], []
    for node in range(n):
        m = members.get(node, 1)
        one = member(node, m, epochs)
        threads += [one] * m
    for node in range(n):
        agents.append(len(threads))
        threads.append(agent(design, node, n, k, epochs,
                             frozenset(TARGET[design]) if targeted else ALL))
    mem = {}
    for node in range(n):
        mem[('arrived', node)] = 0
        mem[('released', node)] = 0
        mem[('handoff', node)] = 0
        mem[('$arrived', node)] = 0
        mem[('up#', node)] = 0
        mem[('down#', node)] = 0
    sym, t = [], 0
    for node in range(n):
        m = members.get(node, 1)
        if m > 1:
            sym.append(list(range(t, t + m)))
        t += m
    agent_set = frozenset(agents)
    # A progress thread idles forever by design; the job is finished when every
    # member has left its last barrier.
    idle_ok = lambda i, M, L: i in agent_set
    label = ','.join(f'{node}x{c}' for node, c in sorted(members.items())) or '1 each'
    name = f'tree/{design} nodes={n} k={k} members={label} epochs={epochs}'
    if targeted:
        name += ' (its check only)'
    return name, threads, mem, sym, idle_ok


# (design, nodes, k, {node: members}, tso, expect_ok[, epochs]).
#
# Five nodes with k = 2 is the smallest tree of depth 2: the root, an interior
# node with two leaves, and a leaf of the root. Node 1, the interior one, has
# two members, so that one of them is released first and re-enters the next
# epoch early (§5.4's bounded skew) while its node is also forwarding. Seven
# nodes is the full binary tree of depth 2, at one epoch - at two it passes 30
# million states without finishing in half an hour; k = 4 at five nodes is a star of
# depth 1, which is what the tree degenerates to at small node counts.
CASES = [
    ('design', 5, 2, {1: 2}, False, True),
    ('design', 5, 2, {1: 2}, True, True),
    ('design', 5, 2, {3: 2}, False, True),
    ('design', 7, 2, {}, False, True, 1),   # two epochs pass 30 M states
    ('design', 5, 4, {0: 2}, False, True),
    ('design', 4, 1, {}, False, True),

    ('forward_early', 5, 2, {1: 2}, False, False),
    ('no_reset', 5, 2, {1: 2}, False, False),
    ('release_first', 5, 2, {1: 2}, False, False),
    ('release_first', 5, 2, {1: 2}, True, False),
    ('wrong_epoch', 5, 2, {1: 2}, False, False),
    ('release_twice', 5, 2, {1: 2}, False, False),
]

# The property each breakage breaks, by the message its check prints.
#
# **Which check a broken variant trips first depends on the search order, not
# on the variant**, so each is run twice. With every check on it must fail -
# some defence catches it. With only its own property's check on it must fail
# on that property, which is the evidence that it breaks what it is said to:
# forward_early, for one, is usually caught first by P3's refusal of a late
# child's arrival for an epoch already closed - the same fatal progress.c's
# book_arrival raises, and what the C mutation of it hit - while a root
# releasing before the late child arrived is equally reachable.
FIRST = {
    'forward_early': 'before node',
    'no_reset': 'before node',
    'release_first': 'open at once',
    'wrong_epoch': 'which it closed',
    'release_twice': 'not exactly once',
}

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    runs = []
    for design, n, k, members, tso, expect, *rest in CASES:
        runs.append((design, n, k, members, tso, expect, rest, False))
        if design in TARGET:
            runs.append((design, n, k, members, tso, expect, rest, True))
    for design, n, k, members, tso, expect, rest, targeted in runs:
        name, threads, mem0, sym, idle_ok = case(design, n, k, members, *rest,
                                                 targeted=targeted)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, tso=tso, symmetric=sym, idle_ok=idle_ok,
                      max_seconds=1800, liveness_max_states=4_000_000)
        good = report(res, expect_ok=expect, tso=tso)
        if targeted and res.complete and (res.message is None or
                                          FIRST[design] not in res.message):
            print(f'    <-- UNEXPECTED: {design} must fail on "{FIRST[design]}"')
            good = False
        ok &= good
    sys.exit(0 if ok else 1)
