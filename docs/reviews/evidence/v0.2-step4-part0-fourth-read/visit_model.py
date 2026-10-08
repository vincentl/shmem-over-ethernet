#!/usr/bin/env python3
"""A reviewer's model of one connection's visits: progress.c's flush() in its
step order, busy()/settle(), the pass's visit rule (dirty | readable | busy),
one writer that writes and then asks for an acknowledgement (quiet), the
kernel's completions and notifications, and a peer that acknowledges only a
segment that asked. Exhaustive breadth-first search; it reports a state in
which the writer's quiet still waits and nothing can happen any more:
no visit pending, no completion or notification owed, no acknowledgement on
its way - the hang, absent other traffic (the deadlock detector's sample is
such traffic: it marks the connection, progress.c's osoe_progress_quiet).

Not modelled: the reservation protocol (reservation_model.py), several
writers, replies, owe_ack, the goodbye. Each buffer holds one message.

  mode 'copy'     the send as it was: released at its last completion
  mode 'zc'       the zero-copy send as built (flush at progress.c:1013-1142,
                  busy at :1210-1219)
  mode 'zc_late'  as built, and busy() also true while a late request is
                  unsealed (one candidate fix)
  mode 'zc_reseal' as built, and flush seals again after an open in the same
                  visit (another)
"""
import sys
from collections import deque

FREE, OPEN, SEALED, SENDING, LENT = 'FREE', 'OPEN', 'SEALED', 'SENDING', 'LENT'


def initial(writes):
    return dict(
        st=(FREE, FREE),        # per buffer
        data=(False, False),    # a message committed in it (OPEN/SEALED/SENDING/LENT)
        asks=(False, False),    # the segment in it carries ACK_REQUESTED
        submitted=(False, False),   # its send is with the kernel
        done=(False, False),    # its completion has been reaped
        owed=(0, 0),            # notifications still to come
        writes_left=writes,
        asked=False,            # the writer's request (ack_wanted_through)
        carried=False,          # a sealed segment has carried the request (flagged)
        acked=False,            # the peer's acknowledgement has arrived
        ack_coming=False,       # the peer has the asking segment and will answer
        dirty=True, readable=False, busy=True,   # osoe_progress_attach marks busy
    )


def freeze(s):
    return tuple(sorted(s.items()))


def put(t, k, v):
    t = list(t)
    t[k] = v
    return tuple(t)


def find(s, what):
    return next((k for k in (0, 1) if s['st'][k] == what), None)


def busy(s, mode):
    open_ = find(s, OPEN)
    b = (open_ is None or find(s, SENDING) is not None or find(s, SEALED) is not None)
    if mode != 'copy':
        b = b or LENT in s['st']
    if mode == 'zc_late':
        b = b or (s['asked'] and not s['carried'] and not s['acked'])
    return b


def flush(s, mode):
    s = dict(s)
    zc = mode != 'copy'
    late = lambda: s['asked'] and not s['carried'] and not s['acked']
    # 1. release a lent buffer that owes nothing (progress.c:1020-1025)
    if zc:
        for k in (0, 1):
            if s['st'][k] == LENT and s['owed'][k] == 0:
                s['st'] = put(s['st'], k, FREE)
                s['data'] = put(s['data'], k, False)
                s['asks'] = put(s['asks'], k, False)
    # 2. finish the in-flight send (:1029-1058): submit on the first offer, take
    # the completion once reaped; release at once if nothing is owed, else LENT
    k = find(s, SENDING)
    if k is not None:
        if not s['submitted'][k]:
            s['submitted'] = put(s['submitted'], k, True)
        elif s['done'][k]:
            if s['asks'][k]:
                s['ack_coming'] = True          # the peer has the asking segment
            free = (not zc) or s['owed'][k] == 0
            s['st'] = put(s['st'], k, FREE if free else LENT)
            if free:
                s['data'] = put(s['data'], k, False)
                s['asks'] = put(s['asks'], k, False)
            s['submitted'] = put(s['submitted'], k, False)
            s['done'] = put(s['done'], k, False)
    # 3. promote the sealed buffer; finalize decides ACK_REQUESTED (:1063-1074)
    if find(s, SENDING) is None and find(s, SEALED) is not None:
        k = find(s, SEALED)
        s['st'] = put(s['st'], k, SENDING)
        if late():
            s['asks'] = put(s['asks'], k, True)
            s['carried'] = True

    def seal():
        # 4. seal the open buffer for data or a late request (:1083-1112)
        o = find(s, OPEN)
        if find(s, SEALED) is None and o is not None and (s['data'][o] or late()):
            s['st'] = put(s['st'], o, SEALED)
    seal()
    # 5. open a free buffer when none is open (:1116-1140)
    opened = False
    if find(s, OPEN) is None:
        for k in (0, 1):
            if s['st'][k] == FREE and k != find(s, SEALED) and k != find(s, SENDING):
                assert not zc or s['owed'][k] == 0, 'the open check would fire'
                s['st'] = put(s['st'], k, OPEN)
                s['data'] = put(s['data'], k, False)
                opened = True
                break
    if mode == 'zc_reseal' and opened:
        seal()
    return s


def successors(s, mode):
    out = []
    zc = mode != 'copy'
    # the agent's visit of this connection, when the pass would make one
    if s['dirty'] or s['readable'] or s['busy']:
        t = flush(dict(s, dirty=False, readable=False), mode)
        t['busy'] = busy(t, mode)
        out.append(('visit', t))
    # the writer: a message into the open buffer, then its quiet's request
    o = find(s, OPEN)
    if s['writes_left'] and o is not None and not s['data'][o]:
        out.append(('write', dict(s, data=put(s['data'], o, True),
                                  writes_left=s['writes_left'] - 1, dirty=True)))
    if not s['writes_left'] and not s['asked']:
        out.append(('ask', dict(s, asked=True, dirty=True)))
    # the kernel: a submitted send completes (a CQE wakes the agent; the
    # connection is busy while sending); a notification comes later
    for k in (0, 1):
        if s['submitted'][k] and not s['done'][k]:
            owed = put(s['owed'], k, s['owed'][k] + 1) if zc else s['owed']
            out.append((f'complete {k}', dict(s, done=put(s['done'], k, True), owed=owed)))
        if s['owed'][k] and not (s['submitted'][k] and not s['done'][k]):
            out.append((f'notify {k}', dict(s, owed=put(s['owed'], k, s['owed'][k] - 1))))
    if s['owed'][0] and s['owed'][1] and not any(s['submitted'][k] and not s['done'][k]
                                                 for k in (0, 1)):
        # one TCP acknowledgement frees both buffers' bytes: one reap sees both
        out.append(('notify both', dict(s, owed=(s['owed'][0] - 1, s['owed'][1] - 1))))
    # the peer's acknowledgement of the asking segment: its segment arrives
    if s['ack_coming'] and not s['acked']:
        out.append(('peer acknowledges', dict(s, acked=True, readable=True)))
    return out


def explore(mode, writes):
    start = initial(writes)
    seen = {freeze(start): None}
    queue = deque([start])
    while queue:
        s = queue.popleft()
        succ = successors(s, mode)
        waiting = s['asked'] and not s['acked']
        if waiting and not succ:
            path, f = [], freeze(s)
            while seen[f] is not None:
                f, label = seen[f]
                path.append(label)
            return len(seen), s, path[::-1]
        for label, t in succ:
            f = freeze(t)
            if f not in seen:
                seen[f] = (freeze(s), label)
                queue.append(t)
    return len(seen), None, None


def flush_closing(s, mode):
    """flush() during the close (progress.c:1094-1098): the open buffer is sealed
    as the goodbye once nothing is sending and it holds nothing; no buffer is
    opened after the goodbye; its last byte ends the stream."""
    s = dict(s)
    zc = mode != 'copy'
    if zc:
        for k in (0, 1):
            if s['st'][k] == LENT and s['owed'][k] == 0:
                s['st'] = put(s['st'], k, FREE)
                s['data'] = put(s['data'], k, False)
    k = find(s, SENDING)
    if k is not None:
        if not s['submitted'][k]:
            s['submitted'] = put(s['submitted'], k, True)
        elif s['done'][k]:
            free = (not zc) or s['owed'][k] == 0
            s['st'] = put(s['st'], k, FREE if free else LENT)
            if free:
                s['data'] = put(s['data'], k, False)
            s['submitted'] = put(s['submitted'], k, False)
            s['done'] = put(s['done'], k, False)
            if s['goodbye'] == 'SEALED' and find(s, SEALED) is None:
                s['goodbye'] = 'SENT'
    if find(s, SENDING) is None and find(s, SEALED) is not None:
        s['st'] = put(s['st'], find(s, SEALED), SENDING)

    def seal():
        o = find(s, OPEN)
        if find(s, SEALED) is None and o is not None and s['goodbye'] == 'NONE':
            bye = s['closing'] and find(s, SENDING) is None and not s['data'][o]
            if bye:
                s['goodbye'] = 'SEALED'
            if s['data'][o] or bye:
                s['st'] = put(s['st'], o, SEALED)
    seal()
    opened = False
    if find(s, OPEN) is None and s['goodbye'] == 'NONE':
        for k in (0, 1):
            if s['st'][k] == FREE:
                s['st'] = put(s['st'], k, OPEN)
                s['data'] = put(s['data'], k, False)
                opened = True
                break
    if mode == 'zc_reseal' and opened:
        seal()
    return s


def busy_closing(s, mode):
    b = busy(s, mode)
    if mode == 'zc_late':       # the fix's other half: a goodbye still to seal
        b = b or (s['closing'] and s['goodbye'] == 'NONE')
    return b


def successors_closing(s, mode):
    out = []
    zc = mode != 'copy'
    if s['dirty'] or s['readable'] or s['busy']:
        t = flush_closing(dict(s, dirty=False, readable=False), mode)
        t['busy'] = busy_closing(t, mode)
        out.append(('visit', t))
    o = find(s, OPEN)
    if s['writes_left'] and o is not None and not s['data'][o]:
        out.append(('write', dict(s, data=put(s['data'], o, True),
                                  writes_left=s['writes_left'] - 1, dirty=True)))
    # shmem_finalize: flush_before_close polls until osoe_progress_idle (nothing
    # sealed or sending, the open buffer empty or none open - a lent buffer is
    # idle), then osoe_progress_close marks every connection busy once
    idle = (find(s, SENDING) is None and find(s, SEALED) is None and
            (o is None or not s['data'][o]))
    if not s['writes_left'] and not s['closing'] and idle:
        out.append(('close', dict(s, closing=True, busy=True)))
    for k in (0, 1):
        if s['submitted'][k] and not s['done'][k]:
            owed = put(s['owed'], k, s['owed'][k] + 1) if zc else s['owed']
            out.append((f'complete {k}', dict(s, done=put(s['done'], k, True), owed=owed)))
        if s['owed'][k] and not (s['submitted'][k] and not s['done'][k]):
            out.append((f'notify {k}', dict(s, owed=put(s['owed'], k, s['owed'][k] - 1))))
    if s['owed'][0] and s['owed'][1] and not any(s['submitted'][k] and not s['done'][k]
                                                 for k in (0, 1)):
        out.append(('notify both', dict(s, owed=(s['owed'][0] - 1, s['owed'][1] - 1))))
    if s['closing'] and not s['peer_ended']:
        # the peer's goodbye and end of stream: its last receive completions
        out.append(('peer goodbye and end', dict(s, peer_ended=True, readable=True)))
    return out


def explore_closing(mode, writes):
    start = dict(initial(writes), closing=False, goodbye='NONE', peer_ended=False)
    seen = {freeze(start): None}
    queue = deque([start])
    while queue:
        s = queue.popleft()
        succ = successors_closing(s, mode)
        if s['closing'] and s['goodbye'] != 'SENT' and not succ:
            path, f = [], freeze(s)
            while seen[f] is not None:
                f, label = seen[f]
                path.append(label)
            return len(seen), s, path[::-1]
        for label, t in succ:
            f = freeze(t)
            if f not in seen:
                seen[f] = (freeze(s), label)
                queue.append(t)
    return len(seen), None, None


if __name__ == '__main__':
    ok = True
    for mode, expect_stuck in (('copy', False), ('zc', True), ('zc_late', False),
                               ('zc_reseal', False)):
        for writes in (1, 2, 3):
            n, stuck, path = explore_closing(mode, writes)
            verdict = 'STUCK' if stuck else 'ok'
            print(f'close {mode:<10} writes={writes} states={n:>6} -> {verdict}')
            if stuck:
                print('    ' + ' ; '.join(path))
                print('    final: ' + ', '.join(f'{k}={v}' for k, v in sorted(stuck.items())))
            if writes >= 2:
                ok &= (stuck is not None) == expect_stuck
    for mode, expect_stuck in (('copy', False), ('zc', True), ('zc_late', False),
                               ('zc_reseal', False)):
        for writes in (1, 2, 3, 4):
            n, stuck, path = explore(mode, writes)
            verdict = 'STUCK' if stuck else 'ok'
            print(f'{mode:<10} writes={writes} states={n:>6} -> {verdict}')
            if stuck:
                print('    ' + ' ; '.join(path))
                print('    final: ' + ', '.join(f'{k}={v}' for k, v in sorted(stuck.items())))
            if writes >= 2:
                ok &= (stuck is not None) == expect_stuck
    sys.exit(0 if ok else 1)
