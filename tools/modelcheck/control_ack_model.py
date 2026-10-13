#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A control wire's acknowledgements between two node leads (v0.2 step 4,
Part 2; design §3.1, §3.4, invariant 22).

The control wire's segments are acknowledged as a data wire's are, since the
deadlock detector's proof needs the tree's messages - application work their
receiver counts - acknowledged before their sender can look quiet (§3.15; the
first adversarial read of Part 2, F1, which deadlock_model.py's control_wire
case shows). This model, written by the second adversarial read of Part 2,
checks the acknowledgement protocol itself, which deadlock_model.py assumes:
one control wire each way, each side's state as progress.c keeps it in struct
osoe_progress_control, and each step as the C takes it:

  enqueue     enqueue_control: a tree message (application work) or a
              detector message; refused (tree: fatal) / dropped (detector)
              after this side's goodbye; marks the busy bit.
  visit       one visit of the pass's control loop: receive_control (any
              number of the segments that have arrived), flush_control,
              settle_control. Enabled only when the busy bit is set or a
              segment has arrived (the ring's readable bit).
  complete    the kernel's completion of the segment on the wire (not readable:
              the busy bit, which c->bytes keeps, brings the visit).
  quiet       osoe_progress_quiet's control-wire loop, after its idle check:
              raises detector_wanted and marks busy. Only while not closing
              (the detector runs in the agent's loop, not in the close).
  close       osoe_progress_close, once flush_before_close's idle holds and the
              barrier's premise does: neither side has a tree message left to
              send, and each has applied every tree message the other sealed.

Checked:
  SAFETY      quiet returns true only when every application message this side
              sealed has been applied by the peer (deadlock.h's proof step).
  QUIESCES    a segment carries ACK_REQUESTED only while its sender's detector
              wants an acknowledgement its peer has not given (the echo, C1).
  BOUNDED     sequence numbers stay under a cap: no endless exchange.
  STRANDED    in every reachable state, a side whose goodbye is not sealed and
              that owes a seal (queued messages, an acknowledgement owed, a
              request to carry, the goodbye to send) has its busy bit set, or
              a segment arrived to read.
  NO_LATE     no tree message is enqueued after this side's goodbye.
  APPLIED_MONOTONE  a segment arrives with a sequence number above the last one
              applied: none is applied twice or out of order.
  LIVE_QUIET  from every state before either side closes, a state where both
              sides' quiet holds is reachable (AG EF), the environment adding
              nothing.
  LIVE_QUIET_EACH  the same for each side alone, from every state where it is
              not closing, whether or not its peer has begun its close.
  LIVE_CLOSE  from every state, a state where both sides have closed (goodbye
              sent, the peer's goodbye applied, its stream ended) is reachable.

Variants: the design and its breakages, each expected to fail one check; and
busy_without_request, control_busy without its request_owed term, which holds:
the term is redundant - a request is sealed in the visit that finds the wire
free - and kept, as the data wire's busy() keeps its own. The environment
variables APP0, APP1 (tree messages a side), DET0, DET1 (detector messages),
SEQ_CAP and MAX_STATES size a run; the second read checked the design at up to
3.5 million states.

Usage: control_ack_model.py [variant ...]
"""
import sys
from collections import deque

import os
import engine
APP = {0: int(os.environ.get('APP0', '1')), 1: int(os.environ.get('APP1', '1'))}
DET = {0: int(os.environ.get('DET0', '1')), 1: int(os.environ.get('DET1', '0'))}
SEQ_CAP = int(os.environ.get('SEQ_CAP', '10'))
MAX_STATES = int(os.environ.get('MAX_STATES', '3000000'))

# node state fields
F = ['rem', 'drem', 'q', 'dq', 'bytes', 'comp', 'next_seq', 'applied', 'acked',
     'last_app', 'det_wanted', 'flagged', 'owe', 'goodbye', 'peer_goodbye', 'closing',
     'busy', 'ended', 'sent_app', 'got_app']
IX = {f: i for i, f in enumerate(F)}


def node0(x):
    return (APP[x], DET[x], 0, 0, False, False, 1, 0, 0, 0, 0, 0, False, 0, False, False,
            False, False, 0, 0)


def get(n, f):
    return n[IX[f]]


def put(n, **kw):
    n = list(n)
    for k, v in kw.items():
        n[IX[k]] = v
    return tuple(n)


def asks_for_ack(acked, flagged, seq, wanted):
    if seq <= acked:
        return False
    late = wanted > acked and flagged < wanted
    return seq <= wanted or late


def request_owed(n):
    return get(n, 'det_wanted') > get(n, 'acked') and get(n, 'flagged') < get(n, 'det_wanted')


def control_busy(n, v):
    if get(n, 'goodbye') != 0:
        return get(n, 'bytes')
    owed = request_owed(n) if v != 'busy_without_request' else False
    return bool(get(n, 'bytes') or get(n, 'q') or get(n, 'dq') or get(n, 'owe') or
                get(n, 'closing') or owed)


class Violation(Exception):
    pass


def seal(n, chan, v):
    """seal_control: returns the node and the channel x->y with the segment."""
    closing, q, dq = get(n, 'closing'), get(n, 'q'), get(n, 'dq')
    if v == 'goodbye_by_count':
        goodbye = q == 0 and dq == 0          # as first built: `count == 0`
    else:
        goodbye = closing and q == 0 and dq == 0
    napp, ndet = q, dq
    seq = get(n, 'next_seq')
    last_app = seq if (napp > 0 and v != 'unacked') else get(n, 'last_app')
    if v == 'answer_asks':
        ask = not goodbye and (get(n, 'owe') or
                               asks_for_ack(get(n, 'acked'), get(n, 'flagged'), seq,
                                            get(n, 'det_wanted')))
    else:
        ask = not goodbye and asks_for_ack(get(n, 'acked'), get(n, 'flagged'), seq,
                                           get(n, 'det_wanted'))
    if ask and not (get(n, 'det_wanted') > get(n, 'acked')):
        raise Violation(f'QUIESCES: segment {seq} asks with nothing wanted')
    flagged = seq if (ask and v != 'no_flagged') else get(n, 'flagged')
    owe = get(n, 'owe') if v == 'no_owe_clear' else False
    if seq >= SEQ_CAP:
        raise Violation(f'BOUNDED: sequence {seq} reached the cap')
    n = put(n, q=0, dq=0, next_seq=seq + 1, last_app=last_app,
            goodbye=1 if goodbye else get(n, 'goodbye'), flagged=flagged, owe=owe,
            bytes=True, comp=False, sent_app=get(n, 'sent_app') + napp)
    ack = 0 if (v == 'goodbye_no_ack' and goodbye) else get(n, 'applied')
    seg = (seq, ack, ask, goodbye, napp, ndet)
    return n, chan + (seg,)


def flush_control(n, chan, v):
    """flush_control, not aborting; the peer neither aborted nor reset."""
    if get(n, 'bytes'):
        if not get(n, 'comp'):
            return n, chan                    # the send still in flight
        n = put(n, bytes=False, comp=False)
        if get(n, 'goodbye') == 1:
            return put(n, goodbye=2), chan + ('EOF',)
    if get(n, 'goodbye') != 0:
        return n, chan
    owed = request_owed(n) if v != 'no_request_in_flush' else False
    closing = get(n, 'closing') if v != 'no_goodbye_seal' else False
    if get(n, 'q') or get(n, 'dq') or closing or get(n, 'owe') or owed:
        return seal(n, chan, v)
    return n, chan


def receive(n, seg, v):
    """segment_applied (and complete()'s count) for one segment of the peer's."""
    if seg == 'EOF':
        return put(n, ended=True)
    seq, ack, flag, gb, napp, ndet = seg
    if seq <= get(n, 'applied'):
        raise Violation(f'APPLIED_MONOTONE: applied regressed: {seq} after {get(n, "applied")}')
    n = put(n, applied=seq, got_app=get(n, 'got_app') + napp)
    if flag and v != 'no_owe':
        n = put(n, owe=True)
    if ack > get(n, 'acked'):
        n = put(n, acked=ack)
    if gb:
        n = put(n, peer_goodbye=True)
    return n


def idle(n):
    return not (get(n, 'bytes') or get(n, 'q') or get(n, 'dq'))


def quiet_holds(n):
    return idle(n) and get(n, 'acked') >= get(n, 'last_app')


def closed(n):
    return get(n, 'goodbye') == 2 and get(n, 'peer_goodbye') and get(n, 'ended')


def successors(S, v):
    nodes, chans = list(S[0]), list(S[1])     # chans[x]: x -> 1-x
    out = []
    for x in (0, 1):
        y = 1 - x
        n = nodes[x]

        def mk(nx, cx=None, cy=None, label=''):
            ns, cs = list(nodes), list(chans)
            ns[x] = nx
            if cx is not None:
                cs[x] = cx
            if cy is not None:
                cs[y] = cy
            out.append(((tuple(ns), tuple(cs)), label))

        # enqueue a tree message (application work), before the close
        if get(n, 'rem') > 0 and not get(n, 'closing'):
            if get(n, 'goodbye') != 0:
                raise Violation(f'NO_LATE: node {x} enqueues a tree message after its goodbye')
            mk(put(n, rem=get(n, 'rem') - 1, q=get(n, 'q') + 1, busy=True),
               label=f'{x} enqueues a tree message')
        # enqueue a detector message, at any time; dropped after the goodbye
        if get(n, 'drem') > 0:
            if get(n, 'goodbye') != 0:
                mk(put(n, drem=get(n, 'drem') - 1), label=f'{x} drops a detector message')
            else:
                mk(put(n, drem=get(n, 'drem') - 1, dq=get(n, 'dq') + 1, busy=True),
                   label=f'{x} enqueues a detector message')
        # the kernel completes the send
        if get(n, 'bytes') and not get(n, 'comp'):
            mk(put(n, comp=True), label=f'{x} reaps its send completion')
        # a visit: receive k of the arrived segments, flush, settle
        incoming = chans[y]
        if get(n, 'busy') or incoming:
            for k in range(0 if not incoming else 1, len(incoming) + 1):
                m = n
                for seg in incoming[:k]:
                    m = receive(m, seg, v)
                rest = incoming[k:]
                m, cx = flush_control(m, chans[x], v)
                m = put(m, busy=control_busy(m, v))
                mk(m, cx, rest, label=f'{x} visits, receiving {k}')
        # the detector's quiet, while the agent's loop runs
        if not get(n, 'closing') and idle(n):
            m = n
            holds = True
            if get(m, 'acked') < get(m, 'last_app'):
                holds = False
                if get(m, 'det_wanted') < get(m, 'last_app'):
                    m = put(m, det_wanted=get(m, 'last_app'))
                    if v != 'quiet_no_mark':
                        m = put(m, busy=True)
            if holds:
                if get(nodes[y], 'got_app') != get(n, 'sent_app') or get(n, 'q'):
                    raise Violation(f'SAFETY: node {x} quiet with {get(n, "sent_app")} tree '
                                    f'messages sealed and {get(nodes[y], "got_app")} applied')
            if m != n:
                mk(m, label=f'{x} asks quiet ({holds})')
            QUIET_SEEN[x] = QUIET_SEEN[x] or holds
        # the close, once the barrier's premise and flush_before_close's idle hold
        if (not get(n, 'closing') and get(n, 'rem') == 0 and idle(n) and
                get(nodes[y], 'rem') == 0 and get(nodes[y], 'q') == 0 and
                get(n, 'got_app') == get(nodes[y], 'sent_app') and
                get(nodes[y], 'got_app') == get(n, 'sent_app')):
            mk(put(n, closing=True, busy=True), label=f'{x} closes')
    return out


QUIET_SEEN = [False, False]


def stranded(S, v):
    nodes, chans = S
    for x in (0, 1):
        n = nodes[x]
        if get(n, 'goodbye') != 0:
            continue
        owes = (get(n, 'q') or get(n, 'dq') or get(n, 'owe') or get(n, 'closing') or
                request_owed(n) or get(n, 'bytes'))
        if owes and not get(n, 'busy') and not chans[1 - x]:
            return f'STRANDED: node {x} owes a seal, its busy bit clear, nothing to read'
    return None


def explore(v):
    start = ((node0(0), node0(1)), ((), ()))
    seen = {start: None}
    edges = {}
    frontier = deque([start])
    try:
        while frontier:
            S = frontier.popleft()
            s = stranded(S, v)
            if s:
                raise Violation(s)
            succ = successors(S, v)
            edges[S] = [T for T, _ in succ]
            for T, label in succ:
                if T not in seen:
                    seen[T] = (S, label)
                    frontier.append(T)
            if len(seen) > MAX_STATES:
                return None, f'gave up past {MAX_STATES} states', len(seen), []
    except Violation as e:
        return False, str(e), len(seen), trace(seen, S)
    # LIVE_QUIET and LIVE_CLOSE: backward reachability from the goal states
    rev = {}
    for S, ts in edges.items():
        for T in ts:
            rev.setdefault(T, []).append(S)

    def can_reach(goal, among):
        good = {S for S in edges if goal(S)}
        work = deque(good)
        while work:
            T = work.popleft()
            for S in rev.get(T, ()):
                if S not in good:
                    good.add(S)
                    work.append(S)
        bad = [S for S in among if S not in good]
        return bad

    def quiet_goal(S):
        return all(quiet_holds(n) for n in S[0]) and not any(get(n, 'closing') for n in S[0])

    pre_close = [S for S in edges if not any(get(n, 'closing') for n in S[0])]
    # the environment may still add messages, but a path that adds nothing must exist
    bad = can_reach(quiet_goal, pre_close)
    if bad:
        return False, 'LIVE_QUIET: a state from which both quiets never hold', len(seen), \
            trace(seen, bad[0])
    for x in (0, 1):
        bad = can_reach(lambda S, x=x: quiet_holds(S[0][x]) and not get(S[0][x], 'closing'),
                        [S for S in edges if not get(S[0][x], 'closing')])
        if bad:
            return False, f'LIVE_QUIET_EACH: node {x} not closing, its quiet never holds', \
                len(seen), trace(seen, bad[0])
    bad = can_reach(lambda S: all(closed(n) for n in S[0]), list(edges))
    if bad:
        return False, 'LIVE_CLOSE: a state from which the close never ends', len(seen), \
            trace(seen, bad[0])
    seqmax = max(get(n, 'next_seq') - 1 for S in edges for n in S[0])
    return True, f'holds; at most {seqmax} segments a side; quiet held at some state: {QUIET_SEEN}', len(seen), []


def trace(seen, S):
    steps = []
    while seen.get(S):
        S, label = seen[S]
        steps.append(label)
    return list(reversed(steps))


# (variant, expect): True for a case that must hold, else the property it must fail
VARIANTS = [
    ('design', True),
    ('busy_without_request', True),     # control_busy without request_owed: redundant
    ('unacked', 'SAFETY'),              # as first built: no last_application_seq on a control wire
    ('answer_asks', 'QUIESCES'),        # the echo: an answer asks
    ('no_owe_clear', 'BOUNDED'),        # a seal does not pay the debt
    ('no_request_in_flush', 'LIVE_QUIET'),   # a request is carried only by other traffic
    ('quiet_no_mark', 'STRANDED'),      # quiet raises the want without the busy bit
    ('no_owe', 'LIVE_QUIET'),           # a request is never owed an answer
    ('goodbye_by_count', 'NO_LATE'),    # 3d30d1c's goodbye: any empty segment
    ('no_flagged', 'BOUNDED'),          # the late request not remembered: endless asking
    ('goodbye_no_ack', 'LIVE_QUIET_EACH'),   # the goodbye pays no debt (LIVE_QUIET_EACH's breakage)
    ('no_goodbye_seal', 'LIVE_CLOSE'),  # closing seals nothing (LIVE_CLOSE's breakage)
]


def main():
    want = sys.argv[1:]
    ok = True
    for v, expect in VARIANTS:
        if want and v not in want:
            continue
        QUIET_SEEN[0] = QUIET_SEEN[1] = False
        held, why, states, tr = explore(v)
        verdict = 'holds' if held else ('FAILS' if held is False else 'NO VERDICT')
        if held is None:        # past MAX_STATES: no verdict, which no expectation takes
            wrong = None if expect is None else ': no verdict'
        else:
            wrong = engine.judge(held, engine.named(why), expect)
        exp = '' if expect is None else (' as expected' if wrong is None else
                                         '   <-- UNEXPECTED' + wrong)
        ok &= wrong is None
        print(f'control-ack/{v} APP={APP} DET={DET}: {verdict} ({states} states): {why}{exp}',
              flush=True)
        if tr:
            print('   trace: ' + ' ; '.join(tr[-14:]))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
