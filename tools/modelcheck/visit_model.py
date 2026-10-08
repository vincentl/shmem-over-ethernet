#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""One connection's visits (progress.c: flush() in its step order, busy() and
settle(), the pass's visit rule dirty | readable | busy) under the zero-copy
send (v0.2 step 4; wire.h, wire-settings.h).

Written by the fourth adversarial read of v0.2 step 4's Part 0
(docs/reviews/archive/v0.2-step4-part0-fourth-adversarial-read.md, F1), and
promoted here with the one-lent rule: an exhaustive search, from the C's step
order, for a state in which a wait can no longer end - a writer's quiet still
waiting, or the close's goodbye unsent, and nothing that could happen: no visit
pending, no completion or notification owed, no acknowledgement on its way.

reservation_model.py cannot see this: its progress thread waits on "a pass
would do something" over the whole state, not on the C's busy bits, and it
models neither acknowledgement requests nor the goodbye.

What is modelled: one writer that writes messages and then asks for an
acknowledgement (shmem_quiet), each message a buffer's worth; the kernel's
completions and notifications - one TCP acknowledgement may free both buffers'
bytes, so one reap may take both notifications; a peer that acknowledges only a
segment that asked; the close, which seals the goodbye into an empty open
buffer once nothing is sending (flush_before_close and osoe_progress_close mark
every connection once).

Designs
  copy           the copying send: a buffer released at its last completion.
                 Baseline.
  zero_copy      as built: by reference only while the other buffer owes no
                 notification (so never both lent - checked), a lent buffer
                 released by the visit after its last notification, and busy()
                 true while a seal is owed - a late acknowledgement request, or
                 at the close a goodbye not yet sealed.
Breakages, each of which must be STUCK where it says
  zc_unmarked    one lent, but busy() without the seal still owed: both
                 buffers can come back in one visit - the lent one released at
                 its notification, the other at its copied completion - and the
                 visit opens one after the seal step has passed, with nothing
                 left to bring the connection back (F1). STUCK at a quiet. Not
                 at the close: with one lent, an idle connection always has a
                 buffer open when the close marks it, and the goodbye is sealed
                 at that visit - so busy()'s goodbye term is the rule's
                 statement there, not its rescue.
  zc_both_lent   every send by reference, busy() as before: the zero-copy send
                 as first built. STUCK at a quiet and at the close.

Usage: visit_model.py
"""
import sys
from collections import deque

FREE, OPEN, SEALED, SENDING, LENT = 'FREE', 'OPEN', 'SEALED', 'SENDING', 'LENT'
# (design, STUCK expected at a quiet, STUCK expected at the close)
DESIGNS = (('copy', False, False), ('zero_copy', False, False),
           ('zc_unmarked', True, False), ('zc_both_lent', True, True))


def initial(writes):
    return dict(
        st=(FREE, FREE),        # per buffer
        data=(False, False),    # a message committed in it
        asks=(False, False),    # the segment in it carries ACK_REQUESTED
        submitted=(False, False),   # its send is with the kernel
        done=(False, False),    # its completion has been reaped
        by_reference=(False, False),   # its send went by reference
        owed=(0, 0),            # notifications still to come
        writes_left=writes,
        asked=False,            # the writer's request (ack_wanted_through)
        carried=False,          # a sealed segment has carried it (flagged)
        acked=False,            # the peer's acknowledgement has arrived
        ack_coming=False,       # the peer has the asking segment and will answer
        dirty=True, readable=False, busy=True,   # osoe_progress_attach marks busy
        closing=False, goodbye='NONE', peer_ended=False,
    )


def freeze(s):
    return tuple(sorted(s.items()))


def put(t, k, v):
    t = list(t)
    t[k] = v
    return tuple(t)


def find(s, what):
    return next((k for k in (0, 1) if s['st'][k] == what), None)


def zero_copy(mode):
    return mode != 'copy'


def busy(s, mode):
    """progress.c's busy(), and the design's terms for a seal still owed."""
    open_ = find(s, OPEN)
    b = open_ is None or find(s, SENDING) is not None or find(s, SEALED) is not None
    if zero_copy(mode):
        b = b or LENT in s['st']
    if mode == 'zero_copy':
        b = b or (s['asked'] and not s['carried'] and not s['acked'])
        b = b or (s['closing'] and s['goodbye'] == 'NONE')
    return b


def check(s, mode):
    if mode == 'zero_copy' and s['st'] == (LENT, LENT):
        return 'BOTH LENT: a connection lent both its buffers to the kernel'
    for k in (0, 1):
        if s['st'][k] == OPEN and s['owed'][k]:
            return f'OPEN OWED: buffer {k} opened with notifications owed'
    return None


def flush(s, mode):
    s = dict(s)
    zc = zero_copy(mode)
    late = lambda: s['asked'] and not s['carried'] and not s['acked']
    # 0. release a lent buffer that owes nothing
    if zc:
        for k in (0, 1):
            if s['st'][k] == LENT and s['owed'][k] == 0:
                s['st'] = put(s['st'], k, FREE)
                s['data'] = put(s['data'], k, False)
                s['asks'] = put(s['asks'], k, False)
    # 1. finish the in-flight send: submit on the first offer - by reference
    # only while the other buffer owes nothing, under the one-lent rule - and
    # take the completion once reaped; release at once if nothing is owed
    k = find(s, SENDING)
    if k is not None:
        if not s['submitted'][k]:
            s['submitted'] = put(s['submitted'], k, True)
            reference = zc and (mode == 'zc_both_lent' or s['owed'][1 - k] == 0)
            s['by_reference'] = put(s['by_reference'], k, reference)
        elif s['done'][k]:
            if s['asks'][k]:
                s['ack_coming'] = True
            free = s['owed'][k] == 0
            s['st'] = put(s['st'], k, FREE if free else LENT)
            if free:
                s['data'] = put(s['data'], k, False)
                s['asks'] = put(s['asks'], k, False)
            s['submitted'] = put(s['submitted'], k, False)
            s['done'] = put(s['done'], k, False)
            s['by_reference'] = put(s['by_reference'], k, False)
            if s['goodbye'] == 'SEALED' and find(s, SEALED) is None:
                s['goodbye'] = 'SENT'
    # 2. promote the sealed buffer; finalize decides ACK_REQUESTED
    if find(s, SENDING) is None and find(s, SEALED) is not None:
        k = find(s, SEALED)
        s['st'] = put(s['st'], k, SENDING)
        if late():
            s['asks'] = put(s['asks'], k, True)
            s['carried'] = True
    # 3. seal the open buffer for data, a late request, or the goodbye
    o = find(s, OPEN)
    if find(s, SEALED) is None and o is not None and s['goodbye'] == 'NONE':
        bye = s['closing'] and find(s, SENDING) is None and not s['data'][o]
        if bye:
            s['goodbye'] = 'SEALED'
        if s['data'][o] or late() or bye:
            s['st'] = put(s['st'], o, SEALED)
    # 4. open a free buffer when none is open, never after the goodbye
    if find(s, OPEN) is None and s['goodbye'] == 'NONE':
        for k in (0, 1):
            if s['st'][k] == FREE and k != find(s, SEALED) and k != find(s, SENDING):
                s['st'] = put(s['st'], k, OPEN)
                s['data'] = put(s['data'], k, False)
                break
    return s


def successors(s, mode, closing):
    out = []
    if s['dirty'] or s['readable'] or s['busy']:
        t = flush(dict(s, dirty=False, readable=False), mode)
        t['busy'] = busy(t, mode)
        out.append(('visit', t))
    o = find(s, OPEN)
    if s['writes_left'] and o is not None and not s['data'][o]:
        out.append(('write', dict(s, data=put(s['data'], o, True),
                                  writes_left=s['writes_left'] - 1, dirty=True)))
    if not closing and not s['writes_left'] and not s['asked']:
        out.append(('ask', dict(s, asked=True, dirty=True)))
    if closing:
        # shmem_finalize: flush_before_close until osoe_progress_idle - nothing
        # sealed or sending, the open buffer empty or none open (a lent buffer is
        # idle) - then osoe_progress_close marks the connection busy once
        idle = (find(s, SENDING) is None and find(s, SEALED) is None and
                (o is None or not s['data'][o]))
        if not s['writes_left'] and not s['closing'] and idle:
            out.append(('close', dict(s, closing=True, busy=True)))
        if s['closing'] and not s['peer_ended']:
            out.append(('peer goodbye and end', dict(s, peer_ended=True, readable=True)))
    for k in (0, 1):
        if s['submitted'][k] and not s['done'][k]:
            owed = put(s['owed'], k, s['owed'][k] + 1) if s['by_reference'][k] else s['owed']
            out.append((f'complete {k}', dict(s, done=put(s['done'], k, True), owed=owed)))
        if s['owed'][k] and not (s['submitted'][k] and not s['done'][k]):
            out.append((f'notify {k}', dict(s, owed=put(s['owed'], k, s['owed'][k] - 1))))
    if s['owed'][0] and s['owed'][1] and not any(s['submitted'][k] and not s['done'][k]
                                                 for k in (0, 1)):
        # one TCP acknowledgement frees both buffers' bytes: one reap sees both
        out.append(('notify both', dict(s, owed=(s['owed'][0] - 1, s['owed'][1] - 1))))
    if not closing and s['ack_coming'] and not s['acked']:
        out.append(('peer acknowledges', dict(s, acked=True, readable=True)))
    return out


def explore(mode, writes, closing):
    """(states, failure, trace): STUCK, or a check's message."""
    start = initial(writes)
    seen = {freeze(start): None}
    queue = deque([start])

    def trace(s):
        path, f = [], freeze(s)
        while seen[f] is not None:
            f, label = seen[f]
            path.append(label)
        return path[::-1]

    while queue:
        s = queue.popleft()
        message = check(s, mode)
        if message:
            return len(seen), message, trace(s)
        succ = successors(s, mode, closing)
        waiting = (s['closing'] and s['goodbye'] != 'SENT') if closing else (
            s['asked'] and not s['acked'])
        if waiting and not succ:
            return len(seen), 'STUCK: ' + ', '.join(f'{k}={v}' for k, v in sorted(s.items())), trace(s)
        for label, t in succ:
            f = freeze(t)
            if f not in seen:
                seen[f] = (freeze(s), label)
                queue.append(t)
    return len(seen), None, None


if __name__ == '__main__':
    ok = True
    for closing in (False, True):
        for mode, at_quiet, at_close in DESIGNS:
            expect_failure = at_close if closing else at_quiet
            for writes in (1, 2, 3, 4):
                n, failure, path = explore(mode, writes, closing)
                verdict = failure.split(':')[0] if failure else 'ok'
                what = 'close' if closing else 'quiet'
                print(f'visit/{mode:<13} {what:<5} writes={writes} states={n:>6} -> {verdict}')
                if failure:
                    print('    ' + failure)
                    print('    trace: ' + ' ; '.join(path))
                # From two writes, every breakage must fail and every design hold.
                if writes >= 2:
                    ok &= (failure is not None) == expect_failure
                elif failure is not None and not expect_failure:
                    ok = False
    sys.exit(0 if ok else 1)
