#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""§3.5's reservation protocol and §3.8's pre-open drain, as connection.c and
progress.c implement them.

Written from the C by the second adversarial review (docs/reviews/evidence/
phase1-step2-slice1/protocols/) and promoted here in phase 1 step 2 slice 2,
which adds the pre-open drain (B2) and the variants that make ORDER and ONCE
each the first check to fail somewhere - the review found neither ever was.

The two first-round models (docs/reviews/evidence/transport/resv_model.py,
docs/reviews/evidence/refutation/refute_model.py) were written against the
design text, before the C existed. This one is written from the C, step for
step, and checks the properties the *callers* rely on rather than the
accounting alone:

  TORN     a segment goes out only when every reserved byte in it was copied
  ORDER    one writer's messages reach the peer in the order it issued them
           (invariant 5, fence, put_signal's FENCED rmw - all of them rest on
           this and on nothing else in the AM path)
  ONCE     every message is delivered exactly once
  SEQ      the sequence number a writer recorded after its CAS is the number of
           the segment its message left in (what quiet's dirty set stores)
  QUIET    when `acked` reaches the number a writer recorded, that writer's
           messages have been applied (transport-internode.c: acknowledged())
  SEGSEQ   segments leave in sequence order, without gaps
  OVERFLOW a reservation ends inside its buffer, which was open when taken
           (the `fill` cases)
  liveness no writer and no progress thread is left blocked (STUCK)

What is mirrored from the C
  writer    osoe_connection_reserve: scan head[0], head[1] for an unsealed
            buffer; FULL and NONE return to the caller, which waits and retries
            (reserve_waiting - the wait is abstracted to an `await` on the word
            changing); CAS head; THEN load seq; copy; fetch_add filled.
  progress  flush()'s four steps in the C's order each pass: finish the send and
            release (head = SEALED, then filled = 0, two release stores);
            promote the sealed buffer if sendable (load head, load filled,
            equality); seal the open buffer if it holds anything or an ack is
            owed (one fetch_or); open a free buffer if none is open (filled,
            seq, head - three release stores in that order). A send may take
            more than one pass (nondeterministic), so sealed + sending coexist.
  pre-open  (slice 2, design; absent in `current`) immediately before that
  drain     open, while the freed buffer is still sealed and so private to the
            progress thread, pending replies are written straight into it - no
            CAS, no writer can see it - capped at half the buffer (§3.8's
            DRAINING cap). The open then publishes filled and head at the
            drained extent, not at the header, seal bit cleared last. This is
            what guarantees a reply a place on the wire even when writers win
            every race for the open buffer.
  drain     with replies=N the progress thread is *also* a writer, as drain()
            is: it reserves through the same osoe_connection_reserve before
            flush() in the same pass, and gives up for this pass on FULL/NONE.

What is abstracted
  * The peer. Delivery applies the segment and publishes `acked` at once. The
    acknowledgement *request* protocol is ack_model.py's subject, and is not
    modelled here at all: with delivery publishing `acked`, a request could
    only ever cause a header-only seal, which changes nothing this model
    checks. (The review's copy took the old one-bool request; slice 2 replaced
    that protocol, so it is removed rather than kept stale.)
  * The wake protocol of the lead's agent (arrival_model.py covers its shape):
    the progress thread `await`s "a pass would do something".
  * Units: the header is 1, every message is 1, a buffer holds CAP messages.

Breakages (each must fail, or the property it guards is unchecked)
  release_unsealed  release stores head = 0: a freed buffer is reservable
  seq_at_seal       the sequence number is assigned at seal, not at open
  head_before_seq   open publishes head before seq
  two_open          a free buffer is opened whenever there is one
  seal_store        the seal is load + store instead of one fetch_or
  promote_unchecked a sealed buffer is sent without waiting for filled == used
                    (the only variant that reaches TORN)
  sendable_ge       (control) `filled >= used` instead of equality, on top of
                    release_unsealed - shows what the equality is for
  open_resets       the pre-open drain fills the buffer, then open publishes
                    head at the header, so writers reserve over the drained
                    replies
  filled_short      the pre-open drain advances head but not filled - the
                    accounting defect the review measured in
                    osoe_connection_drain_reserve (head=96 filled=64)
  drain_no_pop      the pre-open drain does not consume the reply it wrote, so
                    it is drained again: ONCE is the first check to fail
  drain_lifo        the pre-open drain takes the newest reply first: ORDER is
                    the first check to fail

`fill` (v0.2 step 4's first rung, FILL_CASES): writers of messages of several
units, each sent in parts that take as much of the open buffer's room as there
is - at least `least` units, or the whole remainder - the length decided from the
head the CAS compares (osoe_connection_reserve_up_to, remote_put); a message of
one unit after a put is its signal's fenced rmw. `whole` is today's whole
reservation with such messages. Their breakages, each of which must fail:
  fill_stale       the length is taken from one load of the head and the CAS
                   from another (OVERFLOW)
  fill_no_advance  a part does not advance the writer's position, so the next
                   copies the same units again (ONCE)
  fill_floor       the floor is not lowered to a remainder shorter than it,
                   which then never fits (STUCK)
Some `fill` cases put a stream beside a writer of one-unit messages - a
fetch-add, a get's request - which take whatever room the stream leaves.

`zero_copy` (v0.2 step 4, docs/decisions/2026-10-07.md item 7: the send behind
OSOE_WIRE_ZERO_COPY): a sealed buffer goes to the kernel by reference, in one
send or several - a short send's remainder is a send of its own - and each send
is a part the kernel holds until its notification. The kernel is a thread of
its own here: it takes the parts in the order they were sent and delivers each
by reading the buffer as it is *then* - a NIC reads the pages when it
transmits, and again when it retransmits, until the peer has acknowledged them
- and then posts that part's notification. A send's completion frees the wire
for the next buffer at once; the buffer it sent is LENT until every
notification of its parts has come, and only then released and free to open.
Its breakages, each of which must fail:
  zc_release_on_send     the buffer is released when its last send completes,
                         as the copying send releases it: a writer, or the
                         pre-open drain, writes into it before the kernel has
                         read it (TORN, ONCE, ORDER or SEQ)
  zc_first_notification  released at the first notification of a buffer sent
                         in parts: the later parts are read after the buffer
                         was reopened
**At most one buffer is lent** (the Pi, 2026-10-08): a part goes by reference
only while the connection's other buffer owes no notification, and otherwise by
copy - read once, at its send, and owing nothing - so that a connection always
has a buffer a copied send away from free. With both lent, an acknowledgement
the peer waits for found no buffer until a notification came, and that waited
for the peer's TCP to acknowledge, which it delays while it has nothing to send:
osoe-bench's 1 MiB lines stalled for seconds. BOTH LENT checks it:
  zc_both_lent           every part by reference, as first built: both buffers
                         lent at once (BOTH LENT)

`current` is the tree before slice 2: no pre-open drain. It passes every
safety check - starving a reply is a liveness property of the *policy*, and a
reply reaches the wire in this model whenever the open buffer has room, which
the scheduler here always eventually grants. Slice 2's reproduction of
starvation did not reproduce either (2-16 writers, the smallest buffer, the
zero-slack put size, CPU load). The pre-open drain is built because §3.8
specifies it as the guarantee, not because a test could fail first.

Usage: reservation_model.py [substring ...]
"""
import sys

from engine import DEL, assemble, explore, report, select

H = 1
SEALED = 64
# A message's identity: 1 + writer * PER_WRITER + its index among the writer's
# units; the progress thread's replies are writer 7.
PER_WRITER = 16
REPLY_BASE = 1 + 7 * PER_WRITER
# Designs with slice 2's pre-open drain; `current` and the review's breakages
# of the reservation protocol itself run without it, as the tree did.
PREOPEN = ('design', 'open_resets', 'filled_short', 'drain_no_pop', 'drain_lifo')
FREE, OPEN, SEALEDST, SENDING = 0, 1, 2, 3
LENT = 4                    # sent by reference, its notifications still owed
ZERO_COPY = ('zero_copy', 'zc_release_on_send', 'zc_first_notification', 'zc_both_lent')


def QCELL(j): return ('$q', j)          # the kernel's parts, oldest first; 0 is empty
def OWED(k): return ('$owed', k)        # buffer k's notifications still to come
def NOTES(k): return ('$notes', k)      # buffer k's notifications come (a breakage's)


def pack_part(k, last, first, end, seq, units=None):
    """A part the kernel holds: units [first, end) of buffer k, segment `seq`,
    `last` when it ends the segment; and, for a part sent by copy, the units as
    they were read at its send (`units`), since its buffer may be reused at once."""
    v = 1 + k + 2 * last + 4 * first + 64 * end + 4096 * seq
    if units is not None:
        v += 1 << 20
        for j, ident in enumerate(units):
            v += ident << (21 + 8 * j)
    return v


def unpack_part(v):
    """(k, last, first, end, seq, units or None)"""
    v -= 1
    k, last, first, end = v % 2, (v // 2) % 2, (v // 4) % 16, (v // 64) % 64
    seq = (v >> 12) & 0xff
    units = None
    if (v >> 20) & 1:
        units = [(v >> (21 + 8 * j)) & 0xff for j in range(max(0, end - max(first, H)))]
    return k, last, first, end, seq, units


def HEAD(k): return ('head', k)
def FILLED(k): return ('filled', k)
def SEQ(k): return ('seq', k)


ACKED = ('acked',)
NAPPLIED = ('$napplied',)
LASTSEQ = ('$lastseq',)


def reserve_blocks(p, ident, cap, on_fail, done, record_seq=True):
    """osoe_connection_reserve + copy + commit for message `ident`.

    on_fail: None -> wait and retry like reserve_waiting; a label -> go there on
    FULL or NONE, like drain().
    """
    B = [(p + 'S0', ('load', lambda L: HEAD(0), 'v', '@' + p + 'B0')),
         (p + 'B0', ('br', lambda L: not (L['v'] & SEALED), '@' + p + 'K0', '@' + p + 'S1')),
         (p + 'K0', ('set', 'k', lambda L: 0, '@' + p + 'KH')),
         (p + 'S1', ('load', lambda L: HEAD(1), 'v', '@' + p + 'B1')),
         (p + 'B1', ('br', lambda L: not (L['v'] & SEALED), '@' + p + 'K1', '@' + p + 'NW')),
         (p + 'K1', ('set', 'k', lambda L: 1, '@' + p + 'KH')),
         (p + 'KH', ('set', 'h', lambda L: L['v'], '@' + p + 'KV')),
         (p + 'KV', ('set', 'v', lambda L: DEL, '@' + p + 'CHK')),
         (p + 'CHK', ('br', lambda L, c=cap: L['h'] + 1 > H + c, '@' + p + 'FW', '@' + p + 'CAS'))]
    if on_fail is None:
        B += [(p + 'NW', ('set', 'v', lambda L: DEL, '@' + p + 'NW2')),
              (p + 'NW2', ('await', lambda M, L: not (M[HEAD(0)] & SEALED) or not (M[HEAD(1)] & SEALED),
                           '@' + p + 'S0')),
              (p + 'FW', ('await', lambda M, L: M[HEAD(L['k'])] != L['h'], '@' + p + 'FW2')),
              (p + 'FW2', ('set', 'h', lambda L: DEL, '@' + p + 'FW3')),
              (p + 'FW3', ('set', 'k', lambda L: DEL, '@' + p + 'S0'))]
    else:
        B += [(p + 'NW', ('set', 'v', lambda L: DEL, on_fail)),
              (p + 'FW', ('set', 'h', lambda L: DEL, '@' + p + 'FW3')),
              (p + 'FW3', ('set', 'k', lambda L: DEL, on_fail))]
    B += [(p + 'CAS', ('cas', lambda L: HEAD(L['k']), lambda L: L['h'], lambda L: L['h'] + 1, 'ok',
                       '@' + p + 'OK')),
          (p + 'OK', ('br', lambda L: L['ok'], '@' + p + 'OK2', '@' + p + 'RT')),
          (p + 'RT', ('set', 'ok', lambda L: DEL, '@' + p + 'FW2' if on_fail is None else '@' + p + 'RT2')),
          (p + 'OK2', ('set', 'ok', lambda L: DEL, '@' + p + 'RS')),
          # The sequence number, read after the reservation with no extra
          # synchronization (connection.c:77).
          (p + 'RS', ('load', lambda L: SEQ(L['k']), 's', '@' + p + 'CP')),
          # The memcpy: a step of its own, so a seal, a send or a recycle can
          # fall between the reservation and the commit.
          (p + 'CP', ('ghost', lambda M, L, i=ident: ({('$slot', L['k'], L['h']): i}, {}),
                      '@' + p + 'CM')),
          (p + 'CM', ('add', lambda L: FILLED(L['k']), lambda L: 1, '_', '@' + p + 'CG')),
          (p + 'CG', ('gpost', lambda M, L, i=ident: ({('$rseq', i): L['s']}, {}), '@' + p + 'CL')),
          (p + 'CL', ('set', 'h', lambda L: DEL, '@' + p + 'CL2')),
          (p + 'CL2', ('set', 'k', lambda L: DEL, done))]
    if on_fail is not None:
        # drain(): a lost CAS simply looks again, inside osoe_connection_reserve.
        B += [(p + 'RT2', ('set', 'h', lambda L: DEL, '@' + p + 'RT3')),
              (p + 'RT3', ('set', 'k', lambda L: DEL, '@' + p + 'S0'))]
    return B


def writer(w, msgs, cap, quiet):
    B = []
    for m in range(msgs):
        p = f'w{m}_'
        ident = 1 + w * PER_WRITER + m
        nxt = f'@w{m + 1}_S0' if m + 1 < msgs else '@Q0'
        B += reserve_blocks(p, ident, cap, None, nxt)
    if quiet:
        # transport-internode.c: wait for acked >= high_segment.
        B += [('Q0', ('set', 'q', lambda L: DEL, '@Q1')),
              ('Q1', ('await', lambda M, L: M[ACKED] >= L['s'], '@Q2')),
              ('Q2', ('check', lambda M, L, w=w, n=msgs: None if all(
                  M[('$applied', 1 + w * PER_WRITER + m)] == 1 for m in range(n)) else
                  f"QUIET: acked reached {L['s']}, the number writer {w} recorded, "
                  f"but not all of its messages have been applied", '@WEND'))]
    else:
        B += [('Q0', ('set', 's', lambda L: DEL, '@WEND'))]
    B += [('WEND', ('done',))]
    return assemble(B)


def part_blocks(p, w, cap, least, done, variant):
    """One message of a filling writer (v0.2 step 4's first rung,
    transport-internode.c's remote_put): each part reserves as much of the
    message's remaining units as the open buffer has room for - at least
    `least`, or the whole remainder when that is smaller - its length decided
    from the head the CAS then compares (osoe_connection_reserve_up_to). `u` is
    the writer's next unit, `rem` this message's units still to go; the parts of
    one message are successive messages of its writer, as a put's chunks are.

    variant 'whole': `least` is at least every message, so a message is
    reserved whole - today's reservation, with messages of several units.
    variant 'fill_stale': the length is taken from one load of the head and
    the CAS from another, so they can disagree.
    variant 'fill_no_advance': a part does not advance the writer's position,
    so the next part copies the same units again.
    variant 'fill_floor': the floor is not lowered to a remainder smaller than
    it, which then never fits.
    """
    stale = variant == 'fill_stale'
    B = [(p + 'S0', ('load', lambda L: HEAD(0), 'v', '@' + p + 'B0')),
         (p + 'B0', ('br', lambda L: not (L['v'] & SEALED), '@' + p + 'K0', '@' + p + 'S1')),
         (p + 'K0', ('set', 'k', lambda L: 0, '@' + p + 'KH')),
         (p + 'S1', ('load', lambda L: HEAD(1), 'v', '@' + p + 'B1')),
         (p + 'B1', ('br', lambda L: not (L['v'] & SEALED), '@' + p + 'K1', '@' + p + 'NW')),
         (p + 'K1', ('set', 'k', lambda L: 1, '@' + p + 'KH')),
         (p + 'KH', ('set', 'h', lambda L: L['v'], '@' + p + 'KV')),
         (p + 'KV', ('set', 'v', lambda L: DEL, '@' + p + 'T')),
         # The room after the head, and the part: as much of the remainder as fits.
         (p + 'T', ('set', 't', lambda L, c=cap: max(0, min(L['rem'], H + c - L['h'])),
                    '@' + p + 'CHK')),
         (p + 'CHK', ('br', (lambda L, f=least: L['t'] < f) if variant == 'fill_floor' else
                      (lambda L, f=least: L['t'] < min(L['rem'], f)), '@' + p + 'FW',
                      '@' + p + ('RL' if stale else 'CAS')))]
    if stale:
        B += [(p + 'RL', ('load', lambda L: HEAD(L['k']), 'h', '@' + p + 'CAS'))]
    B += [(p + 'CAS', ('cas', lambda L: HEAD(L['k']), lambda L: L['h'], lambda L: L['h'] + L['t'],
                       'ok', '@' + p + 'OK')),
          (p + 'OK', ('br', lambda L: L['ok'], '@' + p + 'OV', '@' + p + 'RT')),
          (p + 'RT', ('set', 'ok', lambda L: DEL, '@' + p + 'FW2')),
          # The reservation ends inside the buffer, which was open when taken.
          (p + 'OV', ('check', lambda M, L, c=cap, w=w: None if (
              not (L['h'] & SEALED) and L['h'] + L['t'] <= H + c) else
              f"OVERFLOW: writer {w} reserved {L['t']} units at {L['h']} of a buffer of "
              f"{c} after its header", '@' + p + 'OK2')),
          (p + 'OK2', ('set', 'ok', lambda L: DEL, '@' + p + 'RS')),
          (p + 'RS', ('load', lambda L: SEQ(L['k']), 's', '@' + p + 'CP')),
          # The memcpy of the part's units: a step of its own, as in reserve_blocks.
          (p + 'CP', ('ghost', lambda M, L, w=w: (
              {('$slot', L['k'], L['h'] + j): 1 + w * PER_WRITER + L['u'] + j
               for j in range(L['t'])}, {}), '@' + p + 'CM')),
          (p + 'CM', ('add', lambda L: FILLED(L['k']), lambda L: L['t'], '_', '@' + p + 'CG')),
          (p + 'CG', ('gpost', lambda M, L, w=w: (
              {('$rseq', 1 + w * PER_WRITER + L['u'] + j): L['s'] for j in range(L['t'])}, {}),
              '@' + p + 'AD')),
          (p + 'AD', ('set', 'u', (lambda L: L['u']) if variant == 'fill_no_advance' else
                      (lambda L: L['u'] + L['t']), '@' + p + 'AR')),
          (p + 'AR', ('set', 'rem', lambda L: L['rem'] - L['t'], '@' + p + 'CL')),
          (p + 'CL', ('set', 'h', lambda L: DEL, '@' + p + 'CL1')),
          (p + 'CL1', ('set', 't', lambda L: DEL, '@' + p + 'CL2')),
          (p + 'CL2', ('set', 'k', lambda L: DEL, '@' + p + 'NX')),
          (p + 'NX', ('br', lambda L: L['rem'] > 0, '@' + p + 'S0', done)),
          # No buffer open, or too little room: wait as reserve_waiting does.
          (p + 'NW', ('set', 'v', lambda L: DEL, '@' + p + 'NW2')),
          (p + 'NW2', ('await', lambda M, L: not (M[HEAD(0)] & SEALED) or not (M[HEAD(1)] & SEALED),
                       '@' + p + 'S0')),
          (p + 'FW', ('await', lambda M, L: M[HEAD(L['k'])] != L['h'], '@' + p + 'FW2')),
          (p + 'FW2', ('set', 'h', lambda L: DEL, '@' + p + 'FW3')),
          (p + 'FW3', ('set', 'k', lambda L: DEL, '@' + p + 'FW4')),
          (p + 'FW4', ('set', 't', lambda L: DEL, '@' + p + 'S0'))]
    return B


def filling_writer(w, sizes, cap, least, quiet, variant):
    """A writer of messages of `sizes` units - a put of several parts, then a
    put-with-signal's fenced rmw of one - each sent as part_blocks says."""
    B = [('U0', ('set', 'u', lambda L: 0, '@m0_M'))]
    for m, size in enumerate(sizes):
        p = f'm{m}_'
        nxt = f'@m{m + 1}_M' if m + 1 < len(sizes) else '@Q0'
        B += [(p + 'M', ('set', 'rem', lambda L, n=size: n, '@' + p + 'S0'))]
        B += part_blocks(p, w, cap, least, nxt, variant)
    units = sum(sizes)
    if quiet:
        B += [('Q0', ('set', 'rem', lambda L: DEL, '@Q1')),
              ('Q1', ('await', lambda M, L: M[ACKED] >= L['s'], '@Q2')),
              ('Q2', ('check', lambda M, L, w=w, n=units: None if all(
                  M[('$applied', 1 + w * PER_WRITER + u)] == 1 for u in range(n)) else
                  f"QUIET: acked reached {L['s']}, the number writer {w} recorded, "
                  f"but not all of its units have been applied", '@WEND'))]
    else:
        B += [('Q0', ('set', 's', lambda L: DEL, '@WEND'))]
    B += [('WEND', ('done',))]
    return assemble(B)


def deliver_check(M, L):
    k = L['k']
    used = M[HEAD(k)] & ~SEALED
    seq = M[SEQ(k)]
    if seq != M[LASTSEQ] + 1:
        return f'SEGSEQ: segment {seq} sent after segment {M[LASTSEQ]}'
    nxt, here = {}, set()
    for off in range(H, used):
        i = M[('$slot', k, off)]
        if i == 0:
            return f'TORN: segment {seq} sent with an uncopied reservation at offset {off}'
        # Twice in one segment is as much a duplicate as twice in two; the
        # review's copy saw only the second, so a same-segment duplicate
        # surfaced as ORDER instead.
        if M[('$applied', i)] or i in here:
            return f'ONCE: message {i} delivered twice'
        here.add(i)
        if M[('$rseq', i)] != seq:
            return (f"SEQ: message {i} left in segment {seq} but its writer recorded "
                    f"{M[('$rseq', i)]}")
        w, m = (i - 1) // PER_WRITER, (i - 1) % PER_WRITER
        want = nxt.get(w, M[('$next', w)])
        if m != want:
            return f'ORDER: writer {w} message {m} delivered before its message {want}'
        nxt[w] = m + 1
    return None


def deliver_ghost(M, L):
    k = L['k']
    used = M[HEAD(k)] & ~SEALED
    upd = {LASTSEQ: M[SEQ(k)]}
    n = 0
    for off in range(H, used):
        i = M[('$slot', k, off)]
        w = (i - 1) // PER_WRITER
        upd[('$applied', i)] = 1
        upd[('$next', w)] = upd.get(('$next', w), M[('$next', w)]) + 1
        upd[('$slot', k, off)] = 0
        n += 1
    upd[NAPPLIED] = M[NAPPLIED] + n
    return upd, {}


def kernel(design, total, qlen):
    """The zero-copy send's kernel: delivers each part as the buffer holds it
    when it does, in the order the parts were sent, then posts the part's
    notification; publishes `acked` at a segment's last part, as deliver_ghost
    does at the copying send's completion."""
    notes = design == 'zc_first_notification'

    def look(M, L):
        return [({}, {'more': 1 if M[QCELL(0)] else 0}, 'look')]

    def check(M, L):
        k, last, first, end, seq, units = unpack_part(M[QCELL(0)])
        if first == 0 and seq != M[LASTSEQ] + 1:
            return f'SEGSEQ: segment {seq} sent after segment {M[LASTSEQ]}'
        nxt, here = {}, set()
        for j, off in enumerate(range(max(first, H), end)):
            i = units[j] if units is not None else M[('$slot', k, off)]
            if i == 0:
                return (f'TORN: segment {seq} delivered with an uncopied or reused unit at '
                        f'offset {off} of buffer {k}')
            if M[('$applied', i)] or i in here:
                return f'ONCE: message {i} delivered twice'
            here.add(i)
            if M[('$rseq', i)] != seq:
                return (f"SEQ: message {i} left in segment {seq} but its writer recorded "
                        f"{M[('$rseq', i)]}")
            w, m = (i - 1) // PER_WRITER, (i - 1) % PER_WRITER
            want = nxt.get(w, M[('$next', w)])
            if m != want:
                return f'ORDER: writer {w} message {m} delivered before its message {want}'
            nxt[w] = m + 1
        return None

    def deliver(M, L):
        k, last, first, end, seq, units = unpack_part(M[QCELL(0)])
        upd, n = {}, 0
        for j, off in enumerate(range(max(first, H), end)):
            i = units[j] if units is not None else M[('$slot', k, off)]
            w = (i - 1) // PER_WRITER
            upd[('$applied', i)] = 1
            upd[('$next', w)] = upd.get(('$next', w), M[('$next', w)]) + 1
            if units is None:
                upd[('$slot', k, off)] = 0          # a copied part's were cleared at its send
            n += 1
        upd[NAPPLIED] = M[NAPPLIED] + n
        if last:
            upd[LASTSEQ] = seq
        for j in range(qlen - 1):
            upd[QCELL(j)] = M[QCELL(j + 1)]
        upd[QCELL(qlen - 1)] = 0
        if units is None:
            upd[OWED(k)] = M[OWED(k)] - 1               # the part's notification
            if notes:
                upd[NOTES(k)] = M[NOTES(k)] + 1
        return upd, {'ack': seq if last else DEL}

    B = [('K0', ('await', lambda M, L, t=total: M[QCELL(0)] != 0 or M[NAPPLIED] >= t, '@K1')),
         ('K1', ('nd', look, '@K2')),
         ('K2', ('br', lambda L: L['more'] == 1, '@K3', '@KEND')),
         ('K3', ('set', 'more', lambda L: DEL, '@K4')),
         # What the check reads is what is delivered: one read of the buffer, as a
         # NIC's transmission is, so the delivery folds into the check's step.
         ('K4', ('check', check, '@K5')),
         ('K5', ('gpost', deliver, '@K6')),
         ('K6', ('br', lambda L: L['ack'] is not None, '@K7', '@K0')),
         ('K7', ('store', lambda L: ACKED, lambda L: L['ack'], True, '@K8')),
         ('K8', ('set', 'ack', lambda L: DEL, '@K0')),
         ('KEND', ('done',))]
    return assemble(B)


def progress(design, total, cap, replies):
    st = lambda L, k: L[f'st{k}'] or 0
    find = lambda L, what: next((k for k in (0, 1) if st(L, k) == what), None)
    zc = design in ZERO_COPY
    qlen = 2 * (H + cap) + 2

    def releasable(M, k):
        """A lent buffer whose notifications have all come (zc_first_notification:
        any one of them)."""
        if design == 'zc_first_notification':
            return M[NOTES(k)] >= 1
        return M[OWED(k)] == 0

    def seen_releasable(L, k):
        """The same, as the pass's reap saw it (`ow`, `nt`): the C reads the wire's
        owed counts, which only the reap at a pass's start moves."""
        if design == 'zc_first_notification':
            return (L[f'nt{k}'] or 0) >= 1
        return (L[f'ow{k}'] or 0) == 0

    def todo(M, L):
        if M[NAPPLIED] >= total:
            return True
        if zc and any(st(L, k) == LENT and releasable(M, k) for k in (0, 1)):
            return True
        if find(L, SENDING) is not None or find(L, FREE) is not None and (
                design == 'two_open' or find(L, OPEN) is None):
            return True
        k = find(L, SEALEDST)
        if k is not None:
            if design == 'promote_unchecked':
                return True
            ge = design == 'sendable_ge'
            f, u = M[FILLED(k)], M[HEAD(k)] & ~SEALED
            return f >= u if ge else f == u
        k = find(L, OPEN)
        if k is not None and (M[HEAD(k)] & ~SEALED) > H:
            return True
        return bool(L['rleft']) and find(L, OPEN) is not None and (M[HEAD(find(L, OPEN))] & ~SEALED) < H + cap

    unsealed_release = design in ('release_unsealed', 'sendable_ge')
    B = [('P0', ('set', 'nseq', lambda L: 1, '@P1')),
         ('P1', ('set', 'rleft', lambda L, r=replies: r, '@TOP')),
         ('TOP', ('await', todo, '@FIN')),
         ('FIN', ('nd', lambda M, L: [({}, {'fin': 1 if M[NAPPLIED] >= total else 0}, 'pass')], '@FIN2')),
         ('FIN2', ('br', lambda L: L['fin'] == 1, '@PEND', '@FIN3')),
         ('FIN3', ('set', 'fin', lambda L: DEL, '@DR'))]

    # drain(): the progress thread as a writer, before flush() in the same pass.
    B += [('DR', ('br', lambda L: bool(L['rleft']), '@DRx', '@F0'))]
    for r in range(replies):
        p = f'd{r}_'
        B += reserve_blocks(p, REPLY_BASE + r, cap, '@F0', '@' + p + 'DN')
        B += [(p + 'DN', ('set', 'rleft', lambda L: L['rleft'] - 1, '@' + p + 'DN2')),
              (p + 'DN2', ('set', 's', lambda L: DEL, '@F0'))]
    # which reply is next: replies are sent in order
    B += [('DRx', ('br', lambda L, r=replies: L['rleft'] == r, '@d0_S0' if replies else '@F0',
                   '@d1_S0' if replies > 1 else '@F0'))]

    if not zc:
        B += [('F0', ('set', 'rq', lambda L: DEL, '@F1'))]
    else:
        # 0. release a lent buffer once the kernel has given it back: the same two
        # release stores as the copying send's, at its notification instead.
        def reap(M, L):
            return [({}, {'ow0': M[OWED(0)] or DEL, 'ow1': M[OWED(1)] or DEL,
                          'nt0': M[NOTES(0)] or DEL, 'nt1': M[NOTES(1)] or DEL}, 'reap')]

        def look_lent(M, L):
            k = next((k for k in (0, 1) if st(L, k) == LENT and seen_releasable(L, k)), None)
            return [({}, {'rk': k if k is not None else DEL}, 'look')]
        B += [('F0', ('nd', reap, '@F0l')),
              ('F0l', ('nd', look_lent, '@Z0')),
              ('Z0', ('br', lambda L: L['rk'] is not None, '@Z1', '@F1')),
              ('Z1', ('store', lambda L: HEAD(L['rk']), lambda L: SEALED, False, '@Z2')),
              ('Z2', ('store', lambda L: FILLED(L['rk']), lambda L: 0, False, '@Z3')),
              ('Z3', ('gpost', lambda M, L: ({NOTES(L['rk']): 0}, {}), '@Z4')),
              ('Z4', ('set', 'st0', lambda L: DEL if L['rk'] == 0 else L['st0'], '@Z5')),
              ('Z5', ('set', 'st1', lambda L: DEL if L['rk'] == 1 else L['st1'], '@Z6')),
              ('Z6', ('set', 'rk', lambda L: DEL, '@F1'))]

        # 1. the sending buffer to the kernel, a part a send: the rest of it, or a
        # short send's prefix; nothing yet, or the send pending. The last part's
        # completion frees the wire, and the buffer is LENT.
        # zero_copy: by reference only while the other buffer owes nothing.
        one_lent = design == 'zero_copy'

        def send_parts(M, L):
            k = find(L, SENDING)
            first, length, seq = L['from'] or 0, L['len'], L['sq']
            alts = [({}, {}, 'send pending')]
            cells = [M[QCELL(j)] for j in range(qlen)]
            if 0 not in cells:
                return alts
            free = cells.index(0)
            by_reference = not one_lent or (L[f'ow{1 - k}'] or 0) == 0
            for end in range(first + 1, length + 1):
                last = 1 if end == length else 0
                if by_reference:
                    mu = {QCELL(free): pack_part(k, last, first, end, seq),
                          OWED(k): M[OWED(k)] + 1}
                else:
                    offs = range(max(first, H), end)
                    mu = {QCELL(free): pack_part(k, last, first, end, seq,
                                                 [M[('$slot', k, off)] for off in offs])}
                    for off in offs:
                        mu[('$slot', k, off)] = 0
                owes = (L[f'ow{k}'] or 0) + (1 if by_reference else 0)
                # The last part: lent if the buffer owes notifications, and
                # otherwise released at once, as wire.c's owed count decides.
                lu = ({'from': DEL, 'len': DEL, 'sq': DEL,
                       'sentall': 1 if owes or not one_lent else 2} if last else {'from': end})
                alts.append((mu, lu, f'send units {first}..{end} of buffer {k}' +
                             ('' if by_reference else ' by copy')))
            return alts
        B += [('F1', ('br', lambda L: find(L, SENDING) is not None, '@Y1', '@F2')),
              ('Y1', ('nd', send_parts, '@Y2')),
              ('Y2', ('br', lambda L: L['sentall'] is not None, '@Y3', '@F2')),
              ('Y3', ('br', lambda L: L['sentall'] == 2, '@X0', '@Y3b')),
              ('Y3b', ('set', 'sentall', lambda L: DEL, '@Y4')),
              # Sent whole by copy: released at once, as a copied buffer always was.
              ('X0', ('set', 'sentall', lambda L: DEL, '@X1')),
              ('X1', ('set', 'rk', lambda L: find(L, SENDING), '@X2')),
              ('X2', ('store', lambda L: HEAD(L['rk']), lambda L: SEALED, False, '@X3')),
              ('X3', ('store', lambda L: FILLED(L['rk']), lambda L: 0, False, '@X4')),
              ('X4', ('set', 'st0', lambda L: DEL if L['rk'] == 0 else L['st0'], '@X5')),
              ('X5', ('set', 'st1', lambda L: DEL if L['rk'] == 1 else L['st1'], '@X6')),
              ('X6', ('set', 'rk', lambda L: DEL, '@F2'))]
        if design == 'zc_release_on_send':
            B += [('Y4', ('set', 'rk', lambda L: find(L, SENDING), '@Y5')),
                  ('Y5', ('store', lambda L: HEAD(L['rk']), lambda L: SEALED, False, '@Y6')),
                  ('Y6', ('store', lambda L: FILLED(L['rk']), lambda L: 0, False, '@Y7')),
                  ('Y7', ('set', 'st0', lambda L: DEL if L['rk'] == 0 else L['st0'], '@Y8')),
                  ('Y8', ('set', 'st1', lambda L: DEL if L['rk'] == 1 else L['st1'], '@Y9')),
                  ('Y9', ('set', 'rk', lambda L: DEL, '@F2'))]
        else:
            both = 'BOTH LENT: a connection lent both its buffers to the kernel'
            B += [('Y4', ('set', 'st0', lambda L: LENT if L['st0'] == SENDING else L['st0'], '@Y5')),
                  ('Y5', ('set', 'st1', lambda L: LENT if L['st1'] == SENDING else L['st1'],
                          '@Y6' if design in ('zero_copy', 'zc_both_lent') else '@F2')),
                  ('Y6', ('check', lambda M, L, m=both: m if (L['st0'] == LENT and
                                                              L['st1'] == LENT) else None,
                          '@F2'))]

    # 1. finish an in-flight send, then release.
    B += [('F1' if not zc else 'F1_copying', ('br', lambda L: find(L, SENDING) is not None, '@F1n', '@F2')),
          ('F1n', ('nd', lambda M, L: [({}, {'go': 1}, 'send completes'),
                                       ({}, {'go': 0}, 'short write')], '@F1b')),
          ('F1b', ('br', lambda L: L['go'] == 1, '@F1c', '@F1z')),
          ('F1z', ('set', 'go', lambda L: DEL, '@F2')),
          ('F1c', ('set', 'k', lambda L: find(L, SENDING), '@F1d')),
          ('F1d', ('check', deliver_check, '@F1e')),
          ('F1e', ('ghost', deliver_ghost, '@F1f')),
          ('F1f', ('load', lambda L: SEQ(L['k']), 'q', '@F1g')),
          ('F1g', ('store', lambda L: ACKED, lambda L: L['q'], False, '@F1h')),
          ('F1h', ('store', lambda L: HEAD(L['k']), lambda L: 0 if unsealed_release else SEALED,
                   False, '@F1i')),
          ('F1i', ('store', lambda L: FILLED(L['k']), lambda L: 0, False, '@F1j')),
          ('F1j', ('set', f'st0', lambda L: DEL if L['k'] == 0 else L['st0'], '@F1k')),
          ('F1k', ('set', f'st1', lambda L: DEL if L['k'] == 1 else L['st1'], '@F1l')),
          ('F1l', ('set', 'q', lambda L: DEL, '@F1m')),
          ('F1m', ('set', 'go', lambda L: DEL, '@F1o')),
          ('F1o', ('set', 'k', lambda L: DEL, '@F2'))]

    # 2. promote a sealed buffer once sendable: load head, load filled, equality.
    B += [('F2', ('br', lambda L: find(L, SENDING) is None and find(L, SEALEDST) is not None,
                  '@F2a', '@F3')),
          ('F2a', ('set', 'k', lambda L: find(L, SEALEDST), '@F2b')),
          ('F2b', ('load', lambda L: HEAD(L['k']), 'h', '@F2c')),
          ('F2c', ('load', lambda L: FILLED(L['k']), 'f', '@F2d')),
          ('F2d', ('br', (lambda L: True) if design == 'promote_unchecked' else
                   (lambda L: L['f'] >= (L['h'] & ~SEALED)) if design == 'sendable_ge' else
                   (lambda L: L['f'] == (L['h'] & ~SEALED)), '@F2e', '@F2z')),
          ('F2e', ('set', 'st0', lambda L: SENDING if L['k'] == 0 else L['st0'], '@F2f')),
          ('F2f', ('set', 'st1', lambda L: SENDING if L['k'] == 1 else L['st1'],
                   '@F2q' if zc else '@F2g')),
          # zero copy: the segment's length and number, which its parts carry.
          ('F2q', ('set', 'len', lambda L: L['h'] & ~SEALED, '@F2r')),
          ('F2r', ('load', lambda L: SEQ(L['k']), 'sq', '@F2g')),
          ('F2g', ('set', 'owe', lambda L: DEL, '@F2z')),
          ('F2z', ('set', 'h', lambda L: DEL, '@F2y')),
          ('F2y', ('set', 'f', lambda L: DEL, '@F2x')),
          ('F2x', ('set', 'k', lambda L: DEL, '@F3'))]

    # 3. seal the open buffer when it holds something or an ack is owed.
    B += [('F3', ('br', lambda L: find(L, SEALEDST) is None and find(L, OPEN) is not None,
                  '@F3a', '@F4')),
          ('F3a', ('set', 'k', lambda L: find(L, OPEN), '@F3b')),
          ('F3b', ('load', lambda L: HEAD(L['k']), 'h', '@F3c')),
          ('F3c', ('br', lambda L: (L['h'] & ~SEALED) > H, '@F3d', '@F3z'))]
    if design == 'seal_store':
        B += [('F3d', ('store', lambda L: HEAD(L['k']), lambda L: L['h'] | SEALED, False, '@F3e'))]
    else:
        B += [('F3d', ('or', lambda L: HEAD(L['k']), lambda L: SEALED, '_', '@F3e'))]
    if design == 'seq_at_seal':
        B += [('F3e', ('store', lambda L: SEQ(L['k']), lambda L: L['nseq'], False, '@F3e2')),
              ('F3e2', ('set', 'nseq', lambda L: L['nseq'] + 1, '@F3f'))]
    else:
        B += [('F3e', ('set', 'h', lambda L: DEL, '@F3f'))]
    B += [('F3f', ('set', 'st0', lambda L: SEALEDST if L['k'] == 0 else L['st0'], '@F3g')),
          ('F3g', ('set', 'st1', lambda L: SEALEDST if L['k'] == 1 else L['st1'], '@F3z')),
          ('F3z', ('set', 'h', lambda L: DEL, '@F3y')),
          ('F3y', ('set', 'k', lambda L: DEL, '@F4'))]

    # 4. open a free buffer only when none is open (§3.5's one-reservable rule).
    may_open = ((lambda L: find(L, FREE) is not None) if design == 'two_open' else
                (lambda L: find(L, OPEN) is None and find(L, FREE) is not None))
    preopen = design in PREOPEN
    # The pre-open drain's cap: half the buffer (§3.8), and at least one reply
    # when the buffer holds two or more, so the cap never makes it a no-op.
    half = max(cap // 2, 1) if cap >= 2 else 0

    def drain_private(M, L):
        """Write up to `half` pending replies into the still-sealed buffer k."""
        k, left = L['k'], L['rleft'] or 0
        n = min(left, half) if preopen else 0
        upd = {}
        for j in range(n):
            if design == 'drain_lifo':
                r = replies - 1 - j                     # newest first
            else:
                r = replies - left + j                  # oldest first, as queued
            ident = REPLY_BASE + r
            upd[('$slot', k, H + j)] = ident
            upd[('$rseq', ident)] = L['nseq']
        taken = 0 if design == 'drain_no_pop' else n
        return [(upd, {'n': n, 'rleft': left - taken if left - taken else DEL},
                 f'pre-open drain: {n} replies into buffer {k}')]

    filled_at = (lambda L: H) if design == 'filled_short' else (lambda L: H + (L['n'] or 0))
    head_at = (lambda L: H) if design == 'open_resets' else (lambda L: H + (L['n'] or 0))
    B += [('F4', ('br', may_open, '@F4a', '@TOP')),
          ('F4a', ('set', 'k', lambda L: find(L, FREE), '@F4p')),
          ('F4p', ('nd', drain_private, '@F4b')),
          ('F4b', ('store', lambda L: FILLED(L['k']), filled_at, False, '@F4c'))]
    if design == 'head_before_seq':
        B += [('F4c', ('store', lambda L: HEAD(L['k']), head_at, False, '@F4d')),
              ('F4d', ('store', lambda L: SEQ(L['k']), lambda L: L['nseq'], False, '@F4e'))]
    elif design == 'seq_at_seal':
        B += [('F4c', ('store', lambda L: HEAD(L['k']), head_at, False, '@F4f'))]
    else:
        B += [('F4c', ('store', lambda L: SEQ(L['k']), lambda L: L['nseq'], False, '@F4d')),
              ('F4d', ('store', lambda L: HEAD(L['k']), head_at, False, '@F4e'))]
    B += [('F4e', ('set', 'nseq', lambda L: L['nseq'] + 1, '@F4f')),
          ('F4f', ('set', 'st0', lambda L: OPEN if L['k'] == 0 else L['st0'], '@F4g')),
          ('F4g', ('set', 'st1', lambda L: OPEN if L['k'] == 1 else L['st1'], '@F4h')),
          ('F4h', ('set', 'k', lambda L: DEL, '@F4i')),
          ('F4i', ('set', 'n', lambda L: DEL, '@TOP')),
          ('PEND', ('done',))]
    if zc:
        B = [(('F2p' if lab == 'F2' else lab), ins) for lab, ins in B]
        B += [('F2', ('set', 'ow0', lambda L: DEL, '@F2c1')),
              ('F2c1', ('set', 'ow1', lambda L: DEL, '@F2c2')),
              ('F2c2', ('set', 'nt0', lambda L: DEL, '@F2c3')),
              ('F2c3', ('set', 'nt1', lambda L: DEL, '@F2p'))]
    return assemble(B)


def case(design, writers, msgs, cap, replies=0, quiet=True):
    total = writers * msgs + replies
    threads = [writer(w, msgs, cap, quiet) for w in range(writers)]
    threads.append(progress(design, total, cap, replies))
    if design in ZERO_COPY:
        threads.append(kernel(design, total, 2 * (H + cap) + 2))
    # osoe_connection_init: both buffers sealed and empty.
    mem = {HEAD(0): SEALED, HEAD(1): SEALED, FILLED(0): 0, FILLED(1): 0, SEQ(0): 0, SEQ(1): 0,
           ACKED: 0}
    name = (f'reservation/{design} writers={writers} msgs={msgs} cap={cap}'
            f"{f' replies={replies}' if replies else ''}{'' if quiet else ' noquiet'}")
    return name, threads, mem


def fill_case(variant, writers, sizes, cap, least, replies=0, quiet=True):
    """`sizes` is every writer's messages, or with `writers` None a tuple of
    each writer's own - a stream beside a writer of small messages."""
    each = [sizes] * writers if writers is not None else list(sizes)
    total = sum(sum(own) for own in each) + replies
    threads = [filling_writer(w, own, cap, least, quiet, variant)
               for w, own in enumerate(each)]
    threads.append(progress('design', total, cap, replies))
    mem = {HEAD(0): SEALED, HEAD(1): SEALED, FILLED(0): 0, FILLED(1): 0, SEQ(0): 0, SEQ(1): 0,
           ACKED: 0}
    shown = ' '.join(','.join(map(str, own)) for own in each)
    name = (f"reservation/{variant} writers={len(each)} sizes={shown} "
            f"cap={cap} least={least}{f' replies={replies}' if replies else ''}")
    return name, threads, mem


# (design, writers, msgs, cap, replies, tso, expect_ok)
CASES = [
    ('design', 1, 2, 1, 0, False, True),
    ('design', 2, 1, 1, 0, False, True),
    ('design', 2, 2, 1, 0, False, True),      # one message per buffer: every message recycles one
    ('design', 2, 2, 2, 0, False, True),
    ('design', 2, 2, 3, 0, False, True),
    ('design', 1, 2, 2, 1, False, True),      # the pre-open drain carries the reply
    ('design', 2, 1, 2, 1, False, True),
    ('design', 1, 1, 2, 2, False, True),      # two replies: one pre-opened, one either way
    ('design', 2, 1, 4, 2, False, True),      # a cap of two: both can go pre-open
    ('design', 2, 2, 2, 0, True, True),       # x86-TSO: the release stores are buffered
    ('design', 2, 1, 2, 1, True, True),
    ('design', 3, 1, 2, 0, False, True),

    ('current', 1, 2, 1, 1, False, True),     # the tree before slice 2: opportunistic drain only
    ('current', 2, 1, 2, 1, False, True),
    ('current', 2, 1, 1, 1, True, True),

    ('release_unsealed', 2, 2, 1, 0, False, False),
    ('release_unsealed', 2, 2, 2, 0, False, False),
    ('sendable_ge', 2, 2, 2, 0, False, False),
    ('seq_at_seal', 2, 2, 1, 0, False, False),
    ('head_before_seq', 2, 2, 1, 0, False, False),
    ('head_before_seq', 1, 2, 1, 0, False, False),
    ('two_open', 2, 2, 2, 0, False, False),
    ('two_open', 1, 3, 1, 0, False, False),
    ('seal_store', 2, 2, 2, 0, False, False),
    ('promote_unchecked', 2, 1, 2, 0, False, False),

    # v0.2 step 4's zero-copy send: a buffer lent to the kernel until every
    # notification of its sends has come
    ('zero_copy', 1, 2, 1, 0, False, True),
    ('zero_copy', 1, 2, 2, 0, False, True),
    ('zero_copy', 2, 1, 2, 0, False, True),
    ('zero_copy', 2, 2, 1, 0, False, True),
    ('zero_copy', 2, 2, 2, 0, False, True),
    ('zero_copy', 1, 2, 2, 1, False, True),   # beside the pre-open drain's reply
    ('zero_copy', 2, 1, 2, 1, False, True),
    ('zero_copy', 2, 2, 2, 0, True, True),    # x86-TSO
    ('zc_release_on_send', 2, 2, 1, 0, False, False),
    ('zc_release_on_send', 1, 2, 2, 1, False, False),
    ('zc_first_notification', 2, 2, 2, 0, False, False),
    ('zc_both_lent', 2, 2, 2, 0, False, False),

    ('open_resets', 1, 1, 2, 1, False, False),
    ('filled_short', 1, 1, 2, 1, False, False),
    ('drain_no_pop', 1, 1, 2, 1, False, False),
    ('drain_lifo', 0, 0, 4, 2, False, False),
]

# v0.2 step 4's first rung, filling the room: a message takes the room the open
# buffer has, down to `least` units. (variant, writers, sizes, cap, least,
# replies, tso, expect_ok)
FILL_CASES = [
    ('fill', 1, (2, 2), 3, 1, 0, False, True),     # the second message splits across buffers
    ('fill', 1, (3, 1), 2, 1, 0, False, True),     # a put in two parts, then its signal
    ('fill', 2, (2,), 3, 1, 0, False, True),       # two writers' parts interleave
    ('fill', 2, (3,), 2, 1, 0, False, True),       # every message splits
    ('fill', 2, (2, 1), 2, 2, 0, False, True),     # a floor of two: a part of one only as a remainder
    ('fill', 1, (3,), 2, 1, 1, False, True),       # beside the pre-open drain's reply
    ('fill', 2, (2,), 2, 1, 1, False, True),
    ('fill', 2, (2,), 3, 1, 0, True, True),        # x86-TSO
    ('whole', 2, (2,), 3, 99, 0, False, True),     # today's whole reservation, messages of two units
    ('fill', 3, (2,), 3, 1, 0, False, True),       # three writers
    ('fill', 2, (2, 2), 3, 1, 0, False, True),
    ('fill', 2, (3, 1), 3, 1, 1, False, True),     # puts and their signals beside a reply
    ('fill', 2, (3,), 2, 1, 0, True, True),        # x86-TSO, every message split
    # a stream beside a writer of one-unit messages - fetch-adds, get requests
    ('fill', None, ((3, 3), (1, 1)), 3, 1, 0, False, True),
    ('fill', None, ((3,), (1, 1)), 2, 1, 0, False, True),
    ('fill', None, ((3, 1), (1,)), 3, 1, 1, False, True),   # the stream's signal; a reply
    ('fill', None, ((2, 2), (1, 1)), 3, 1, 0, True, True),  # x86-TSO

    ('fill_stale', 2, (2,), 3, 1, 0, False, False),
    ('fill_no_advance', 1, (3,), 2, 1, 0, False, False),
    ('fill_floor', 1, (2, 1), 2, 2, 0, False, False),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, writers, msgs, cap, replies, tso, expect in CASES:
        name, threads, mem0 = case(design, writers, msgs, cap, replies)
        if not keep(name):
            continue
        zc = design in ZERO_COPY
        res = explore(name, threads, mem0, tso=tso, max_seconds=600 if zc else 240,
                      liveness_max_states=6_000_000 if zc else 400_000)
        ok &= report(res, expect_ok=expect, tso=tso)
    for variant, writers, sizes, cap, least, replies, tso, expect in FILL_CASES:
        name, threads, mem0 = fill_case(variant, writers, sizes, cap, least, replies)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, tso=tso, max_seconds=600,
                      liveness_max_states=3_000_000)
        ok &= report(res, expect_ok=expect, tso=tso)
    sys.exit(0 if ok else 1)
