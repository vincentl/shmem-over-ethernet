#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""What a control tree costs and saves against the mesh of control wires, in
sockets and in the time of the control plane's three operations
(docs/design/control-tree-proposal.md; the large-cluster playbook's C17 and
its §5 "Start-up" row). Not an interleaving model.

As built, a node's lead holds a control wire to every other node (design
§3.1), made by the start-up schedule (§6.2, mesh_timing.py); the deadlock
detector is a star from node 0 (§3.15), and an abort's goodbyes go from the
aborting node's lead to every other, each node then relaying. The control
tree keeps the control wires of the world team's tree alone - a lead's parent
and at most k children - and sends the detector's probes down it and their
answers up, and an abort's notices along it (control_tree_abort_model.py).

Costs, from the third cloud session's measurements as the playbook's C16 has
them: a hop `h` of 40 µs, a message's cost `o` at a lead - the sealing, the
send, the peer's read and parse - of 35 µs (23 to 45 by the trimmed mean);
`sensitivity` varies `o`. A lead is a serial server, as mesh_timing.py has it.

  sockets      a lead's control wires, the kernel's sockets a node with the
               data wires the tree needs eagerly (the collectives' at init),
               and the job's TCP flows.
  collection   one of the detector's two collections a round: the star, the
               root's n - 1 serial sends and n - 1 serial reads around a hop
               each way; the tree, k sends and a hop a level, down and up.
  abort        the time until the last node has heard: the star, the
               aborting lead's n - 1 serial sends and a hop; the tree, from a
               leaf: a send and a hop a level up, k sends and a hop a level
               down; and the ends gathering toward the carrier's node, a hop
               a level, which the mesh does not need.
  start-up     the control wires' making: the schedule (mesh_timing.py's
               slotted order, one wire a pair, the leads entering as the
               rendezvous's tables reach them) against the tree's, where each
               lead calls its children as the tables reach it - a connection's
               time, as mesh_timing.py has it, after the last lead's entry.

Usage: control_tree_timing.py [check] | table | sensitivity
"""
import math
import sys

import mesh_timing

HOP = 40e-6
MESSAGE = 35e-6
K = 8


def depth(n, k=K):
    """Levels below the root of a k-ary tree of n nodes in level order."""
    d, reach = 0, 1
    while reach < n:
        d += 1
        reach += k ** d
    return d


def sockets(n, k=K):
    mesh = 2 * (n - 1)                    # a control and a data wire a peer
    tree = 2 * (k + 1)                    # an interior lead's, the most
    return dict(mesh_control=n - 1, tree_control=k + 1, mesh_sockets=mesh, tree_sockets=tree,
                mesh_flows=n * (n - 1), tree_flows=2 * (n - 1))


def collection(n, k=K, h=HOP, o=MESSAGE):
    star = 2 * (n - 1) * o + 2 * h
    tree = 2 * depth(n, k) * (k * o + h)
    return star, tree


def abort(n, k=K, h=HOP, o=MESSAGE):
    d = depth(n, k)
    star = (n - 1) * o + h
    tree = d * (o + h) + d * (k * o + h)
    ends = 2 * d * h
    return star, tree, ends


def start_up(n, k=K):
    """The control wires made, by the schedule and by the tree, in seconds."""
    entry = mesh_timing.tree_entry(n)
    schedule = mesh_timing.simulate(n, mesh_timing.SLOTTED, wires=1, entry=entry)['done']
    costs = mesh_timing.COSTS
    one = (2 * mesh_timing.ONE_WAY + costs['dial']            # the handshake
           + mesh_timing.ONE_WAY + costs['hello'] + costs['answer']
           + mesh_timing.ONE_WAY + costs['confirm']
           + mesh_timing.ONE_WAY + costs['attach'])
    tree = max(entry) + one
    return schedule, tree


def us(x):
    return f'{x * 1e6:9.0f} us'


def table(counts=(8, 64, 512, 4096), k=K):
    print(f'fan-out {k}; hop {HOP * 1e6:.0f} us, message {MESSAGE * 1e6:.0f} us')
    print('  nodes  depth | control wires  sockets a node   TCP flows a job | collection       '
          '| abort heard        ends | control wires made')
    print('               |  mesh   tree    mesh    tree       mesh     tree |    star     tree '
          '|    star     tree    tree |  schedule     tree')
    for n in counts:
        s = sockets(n, k)
        cs, ct = collection(n, k)
        a_s, a_t, ends = abort(n, k)
        if n <= 1024:
            sch, tr = start_up(n, k)
            made = f'{sch * 1e3:7.1f} ms {tr * 1e3:7.1f} ms'
        else:
            made = '   (see mesh_timing.py)'
        print(f'{n:7d} {depth(n, k):6d} | {s["mesh_control"]:5d} {s["tree_control"]:6d} '
              f'{s["mesh_sockets"]:7d} {s["tree_sockets"]:7d} {s["mesh_flows"]:10d} {s["tree_flows"]:8d} '
              f'| {cs * 1e6:7.0f} {ct * 1e6:8.0f} | {a_s * 1e6:7.0f} {a_t * 1e6:8.0f} {ends * 1e6:7.0f} '
              f'| {made}')
    print('(times in microseconds but the last two columns)')


def sensitivity(n=4096, k=K):
    print(f'{n} nodes, fan-out {k}, hop {HOP * 1e6:.0f} us: the message cost varied')
    print('  message |  collection star   tree  | abort star   tree')
    for o in (5e-6, 15e-6, 35e-6, 45e-6):
        cs, ct = collection(n, k, o=o)
        a_s, a_t, _ = abort(n, k, o=o)
        print(f'  {o * 1e6:5.0f} us | {cs * 1e3:10.1f} ms {ct * 1e3:6.2f} ms | {a_s * 1e3:7.1f} ms {a_t * 1e3:6.2f} ms')


def check():
    ok = True

    def pin(name, holds, detail):
        nonlocal ok
        print(f'  {name}: {"ok" if holds else "FAILS"} ({detail})')
        ok &= holds

    s = sockets(4096)
    pin('sockets', s['tree_sockets'] == 18 and s['mesh_sockets'] == 8190,
        f'{s["tree_sockets"]} a node against {s["mesh_sockets"]} at 4,096 nodes')
    pin('flows', s['tree_flows'] * 1000 < s['mesh_flows'],
        f'{s["tree_flows"]} flows against {s["mesh_flows"]}')
    cs, ct = collection(4096)
    pin('collection', cs / ct > 50, f'star {cs * 1e3:.1f} ms, tree {ct * 1e3:.2f} ms, {cs / ct:.0f} times')
    cs, ct = collection(64)
    pin('collection at 64', cs > ct, f'star {cs * 1e6:.0f} us, tree {ct * 1e6:.0f} us')
    a_s, a_t, ends = abort(4096)
    pin('abort', a_s / a_t > 20, f'star {a_s * 1e3:.1f} ms, tree {a_t * 1e3:.2f} ms, {a_s / a_t:.0f} times')
    pin('ends', ends < 1e-3, f'the ends gather in {ends * 1e6:.0f} us at 4,096 nodes')
    a_s, a_t, _ = abort(8)
    pin('abort at 8', a_s < a_t, f'the star wins at 8 nodes: {a_s * 1e6:.0f} against {a_t * 1e6:.0f} us')
    a_s, a_t, _ = abort(64)
    pin('abort at 64', a_t < a_s, f'the tree wins from 64 nodes: {a_t * 1e6:.0f} against {a_s * 1e6:.0f} us')
    for o in (5e-6, 45e-6):
        cs, ct = collection(4096, o=o)
        pin(f'collection at {o * 1e6:.0f} us a message', ct < cs,
            f'tree {ct * 1e3:.2f} ms against star {cs * 1e3:.1f} ms')
    for n in (64, 256):
        sch, tr = start_up(n)
        pin(f'start-up at {n}', tr < sch, f'tree {tr * 1e3:.1f} ms against the schedule {sch * 1e3:.1f} ms')
    return ok


def main(argv):
    if len(argv) > 1 and argv[1] == 'check':
        return 0 if check() else 1
    if len(argv) > 1 and argv[1] == 'sensitivity':
        sensitivity()
        return 0
    table()
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
