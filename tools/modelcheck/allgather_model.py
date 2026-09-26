#!/usr/bin/env python3
"""An allgather among node leads, for fcollect's internode phase (design §5.8;
internode.h, osoe_internode_collect).

The author's decision of 2026-09-25: a gather whose steps are log N, moving each
node's blocks once, in place of the reduction's tree running an or. Bruck's
algorithm among the team's N node leads: at step s, lead i sends every share it
holds - its own and those gathered so far, nodes i to i + c - 1 with
c = min(2^s, N - 2^s) - to lead i - 2^s, and receives nodes i + 2^s onwards
from lead i + 2^s; after ceil(log2 N) steps every lead holds every share. The
shares are written straight to their places in the receiver's buffer, the
lead's dest, so nothing is staged and nothing rotated.

Two rules make it safe, one per counter a lead keeps per step:
  READY  internode.h's rule: a peer's buffer is written only once that peer has
         entered this collective, since until then its members may still be
         copying the collective before's result out of it. At entry a lead adds
         one to READY[s] at each lead that will write to it, i + 2^s; the
         writer waits for it before its step-s put.
  DATA   a lead forwards at step s + 1 shares it received at step s, so it waits
         for the step-s writer's add, fenced after the put, before sending on;
         and it returns only after the last step's.
Counters are cumulative over the team's life and compared as at least: each
collective adds exactly one to each, so collective x waits for x + 1.

Checked
  DEST     a lead writes a peer's buffer only while the peer is in the same
           collective: never during the peer's members' copy-out of the one
           before.
  FORWARD  a share a lead sends carries this collective's value.
  RESULT   on return every share of the buffer carries this collective's value,
           and it still does when the members finish copying it out.
  and, by the engine: no stuck lead, and every state can still finish.

Designs
  design         as above.
Breakages
  no_ready       a lead writes without waiting for READY (DEST, RESULT).
  no_data_wait   a lead forwards without waiting for DATA (FORWARD).
  short_steps    floor(log2 N) steps rather than the ceiling: on a node count
                 that is not a power of two some shares never arrive (RESULT).

Sending the last step's full 2^s shares, past N - 2^s, is not a breakage: the
extra shares wrap onto ones the receiver already holds, with the same values.
The model found that before this line said it: the bound saves bytes, and
nothing depends on it.

Every counter add is fenced after the data it covers, and waits are abstract
(the region wait's wake is wait_word_model.py's); runs are SC.

Usage: allgather_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import assemble, explore, report, select

BUF = lambda n, k: ('buf', n, k)         # lead n's buffer: share k's value
READY = lambda n, s: ('ready', n, s)     # at lead n: step s's receiver has entered
DATA = lambda n, s: ('data', n, s)       # at lead n: step s's shares have landed
IN = lambda n: ('$in', n)                # ghost: the collective lead n is in, + 1; 0 between
FIN = ('$fin',)


def steps_of(nodes):
    s = 0
    while (1 << s) < nodes:
        s += 1
    return s


def value(x, k):
    """Share k's value in collective x: distinct across collectives and shares."""
    return 100 * (x + 1) + k


def lead(design, i, nodes, collectives):
    S = steps_of(nodes)
    if design == 'short_steps':
        S = max(1, (nodes.bit_length() - 1))
    B = []
    for x in range(collectives):
        p = f'l{i}x{x}_'
        nxt = f'@l{i}x{x + 1}_E' if x + 1 < collectives else '@DONE'
        # Enter: the members' blocks are in (this node's share), then tell every
        # lead that will write here, one per step.
        B += [(p + 'E', ('ghost', lambda M, L, x=x: ({IN(i): x + 1}, {}), '@' + p + 'E0')),
              (p + 'E0', ('store', lambda L: BUF(i, i), lambda L, x=x: value(x, i), True,
                          '@' + p + 'R0'))]
        for s in range(S):
            writer = (i + (1 << s)) % nodes
            nx = f'@{p}R{s + 1}' if s + 1 < S else f'@{p}S0'
            B += [(f'{p}R{s}', ('add', lambda L, w=writer, s=s: READY(w, s), lambda L: 1, '_', nx))]
        # The steps.
        for s in range(S):
            to = (i - (1 << s)) % nodes
            c = min(1 << s, nodes - (1 << s))
            shares = [(i + j) % nodes for j in range(c)]
            q = f'{p}S{s}'
            # Wait for the receiver's entry, then write, then say so.
            if design == 'no_ready':
                B += [(q, ('br', lambda L: True, f'@{q}W0', f'@{q}W0'))]
            else:
                B += [(q, ('await', lambda M, L, s=s, x=x: M[READY(i, s)] >= x + 1, f'@{q}W0'))]
            for j, k in enumerate(shares):
                nx = f'@{q}W{j + 1}' if j + 1 < len(shares) else f'@{q}D'
                B += [(f'{q}W{j}', ('check', lambda M, L, to=to, x=x:
                                    None if M[IN(to)] == x + 1 else
                                    f'lead {i} wrote lead {to}\'s buffer in collective {x} while '
                                    f'lead {to} was ' + (f'in collective {M[IN(to)] - 1}' if M[IN(to)]
                                                         else 'outside it: not yet entered, or '
                                                              'its members copying out'),
                                    f'@{q}V{j}')),
                      (f'{q}V{j}', ('check', lambda M, L, k=k, x=x:
                                    None if M[BUF(i, k)] == value(x, k) else
                                    f'lead {i} forwarded share {k} as {M[BUF(i, k)]}, '
                                    f'not collective {x}\'s {value(x, k)}', f'@{q}P{j}')),
                      (f'{q}P{j}', ('store', lambda L, to=to, k=k: BUF(to, k),
                                    lambda L, k=k, x=x: value(x, k), True, nx))]
            B += [(f'{q}D', ('add', lambda L, to=to, s=s: DATA(to, s), lambda L: 1, '_', f'@{q}A'))]
            # Before the next step or the return: this step's shares are in.
            if design == 'no_data_wait' and s + 1 < S:
                B += [(f'{q}A', ('br', lambda L: True, f'@{p}S{s + 1}', f'@{p}S{s + 1}'))]
            else:
                nx = f'@{p}S{s + 1}' if s + 1 < S else f'@{p}RET'
                B += [(f'{q}A', ('await', lambda M, L, s=s, x=x: M[DATA(i, s)] >= x + 1, nx))]
        # Return: the members copy the result out, then the lead may enter the next.
        B += [(p + 'RET', ('ghost', lambda M, L: ({IN(i): 0}, {}), '@' + p + 'C0'))]
        for k in range(nodes):
            nx = f'@{p}C{k + 1}' if k + 1 < nodes else nxt
            B += [(f'{p}C{k}', ('check', lambda M, L, k=k, x=x:
                                None if M[BUF(i, k)] == value(x, k) else
                                f'lead {i}\'s members copied share {k} as {M[BUF(i, k)]}, not '
                                f'collective {x}\'s {value(x, k)}', nx))]
    B += [('DONE', ('gpost', lambda M, L: ({FIN: M[FIN] + 1}, {}), '@X')),
          ('X', ('done',))]
    return assemble(B)


def case(design, nodes, collectives):
    threads = [lead(design, i, nodes, collectives) for i in range(nodes)]
    mem = {FIN: 0}
    for n in range(nodes):
        mem[IN(n)] = 0
        for s in range(steps_of(nodes)):
            mem[READY(n, s)] = mem[DATA(n, s)] = 0
        for k in range(nodes):
            mem[BUF(n, k)] = 0
    return f'allgather/{design} nodes={nodes} collectives={collectives}', threads, mem, nodes


# (design, nodes, collectives, expect_ok)
CASES = [
    ('design', 2, 2, True),
    ('design', 3, 2, True),
    ('design', 4, 2, True),
    ('design', 5, 2, True),
    ('design', 3, 3, True),
    ('no_ready', 3, 2, False),
    ('no_data_wait', 4, 1, False),
    ('short_steps', 3, 1, False),
]


def main():
    want = select(sys.argv)
    ok = True
    for design, nodes, collectives, expect in CASES:
        name, threads, mem, n = case(design, nodes, collectives)
        if not want(name):
            continue
        res = explore(name, threads, mem, max_seconds=900, liveness_max_states=3_000_000,
                      done=lambda M, n=n: M[FIN] == n)
        ok &= report(res, expect_ok=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
