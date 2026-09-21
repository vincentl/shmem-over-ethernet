#!/usr/bin/env python3
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
           messages have been applied (transport-am.c: acknowledged())
  SEGSEQ   segments leave in sequence order, without gaps
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
# Designs with slice 2's pre-open drain; `current` and the review's breakages
# of the reservation protocol itself run without it, as the tree did.
PREOPEN = ('design', 'open_resets', 'filled_short', 'drain_no_pop', 'drain_lifo')
FREE, OPEN, SEALEDST, SENDING = 0, 1, 2, 3


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
        ident = 1 + w * 8 + m
        nxt = f'@w{m + 1}_S0' if m + 1 < msgs else '@Q0'
        B += reserve_blocks(p, ident, cap, None, nxt)
    if quiet:
        # transport-am.c: wait for acked >= high_segment.
        B += [('Q0', ('set', 'q', lambda L: DEL, '@Q1')),
              ('Q1', ('await', lambda M, L: M[ACKED] >= L['s'], '@Q2')),
              ('Q2', ('check', lambda M, L, w=w, n=msgs: None if all(
                  M[('$applied', 1 + w * 8 + m)] == 1 for m in range(n)) else
                  f"QUIET: acked reached {L['s']}, the number writer {w} recorded, "
                  f"but not all of its messages have been applied", '@WEND'))]
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
        w, m = (i - 1) // 8, (i - 1) % 8
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
        w = (i - 1) // 8
        upd[('$applied', i)] = 1
        upd[('$next', w)] = upd.get(('$next', w), M[('$next', w)]) + 1
        upd[('$slot', k, off)] = 0
        n += 1
    upd[NAPPLIED] = M[NAPPLIED] + n
    return upd, {}


def progress(design, total, cap, replies):
    st = lambda L, k: L[f'st{k}'] or 0
    find = lambda L, what: next((k for k in (0, 1) if st(L, k) == what), None)

    def todo(M, L):
        if M[NAPPLIED] >= total:
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
        B += reserve_blocks(p, 1 + 7 * 8 + r, cap, '@F0', '@' + p + 'DN')
        B += [(p + 'DN', ('set', 'rleft', lambda L: L['rleft'] - 1, '@' + p + 'DN2')),
              (p + 'DN2', ('set', 's', lambda L: DEL, '@F0'))]
    # which reply is next: replies are sent in order
    B += [('DRx', ('br', lambda L, r=replies: L['rleft'] == r, '@d0_S0' if replies else '@F0',
                   '@d1_S0' if replies > 1 else '@F0'))]

    B += [('F0', ('set', 'rq', lambda L: DEL, '@F1'))]

    # 1. finish an in-flight send, then release.
    B += [('F1', ('br', lambda L: find(L, SENDING) is not None, '@F1n', '@F2')),
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
          ('F2f', ('set', 'st1', lambda L: SENDING if L['k'] == 1 else L['st1'], '@F2g')),
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
            ident = 1 + 7 * 8 + r
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
    return assemble(B)


def case(design, writers, msgs, cap, replies=0, quiet=True):
    total = writers * msgs + replies
    threads = [writer(w, msgs, cap, quiet) for w in range(writers)]
    threads.append(progress(design, total, cap, replies))
    # osoe_connection_init: both buffers sealed and empty.
    mem = {HEAD(0): SEALED, HEAD(1): SEALED, FILLED(0): 0, FILLED(1): 0, SEQ(0): 0, SEQ(1): 0,
           ACKED: 0}
    name = (f'reservation/{design} writers={writers} msgs={msgs} cap={cap}'
            f"{f' replies={replies}' if replies else ''}{'' if quiet else ' noquiet'}")
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

    ('open_resets', 1, 1, 2, 1, False, False),
    ('filled_short', 1, 1, 2, 1, False, False),
    ('drain_no_pop', 1, 1, 2, 1, False, False),
    ('drain_lifo', 0, 0, 4, 2, False, False),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, writers, msgs, cap, replies, tso, expect in CASES:
        name, threads, mem0 = case(design, writers, msgs, cap, replies)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, tso=tso, max_seconds=240)
        ok &= report(res, expect_ok=expect, tso=tso)
    sys.exit(0 if ok else 1)
