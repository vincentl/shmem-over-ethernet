#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The lead's requests to a helper thread (v0.2 step 4, Part 3; the progress
helpers proposal's §3.5 and §3.6; docs/decisions/2026-10-08-helpers.md).

A helper is a thread of the lead's process that owns a ring and the data wires
on it (owner h, h > 0). The lead's agent asks it things - the deadlock
detector's "are you quiet", and at the end "stop" - through two words in the
process's memory and the helper's wake word in the connection area:

  request   the lead's alone to write: the latest request, by sequence.
  answer    the helper's alone to write: the sequence it answers, with the
            verdict a quiet request asks for.
  wake      the helper's wake word, which writers bump too (lead_wake_model.py's
            claimed wake): the helper sleeps in its ring on the value it loaded.

Lead:   store(request, q); add(wake, 1); futex_wake(wake); then wait for the
        answer to q.
Helper: w = load(wake); r = load(request); a pass - take every committed
        operation (the dirty bitmap), answer r if it is new; if it moved
        nothing, raise its flag, look at the bitmap again (the writers' half of
        the protocol), and sleep in its ring on w.

**Why the request needs no second look**: the lead stores the request before it
bumps the word, and the helper loads the word before it loads the request. A
helper that loaded the word after the bump finds the request; one that loaded it
before sleeps on a value the bump has changed, and its wait returns at once.
That is §4.9's rule, the word loaded before the pass, and it is the whole
protocol: the lead's wake is unconditional - requests are rare - so it needs
none of the flag's claim.

**A stop is acknowledged, then done.** On a stop the helper answers at once
(acknowledged), then closes its wires - a ring use the model counts - and then
answers done and returns. The lead waits for the acknowledgement under a
deadline, since a live helper answers within a pass and one that never does is
a defect to end the job by name; and for done without one, since a close waits
as long as its peers take to finalize (the no-progress rule bounds it, outside
this model). Only after done does the lead free the helper's ring and join it.

**A quiet answer is fresh**: it names the request it answers, so the lead takes
no earlier answer for a later request; and its idle verdict reads the work
committed - the open buffers' heads in the C - so an answer of idle for request
q means nothing committed before q was untaken.

Checked
  every request answered: no state where the lead waits on an answer and the
        helper sleeps with nothing to wake it (LOST WAKE / STUCK);
  RING     the helper never touches its ring after the lead freed it;
  FRESH    an answer of idle for q means everything committed before q taken;
  and, by the engine, AG EF finished: the lead ends - after joining the helper,
  or by the deadline, naming the helper that never answered.

Designs
  design              as above: a quiet request, then a stop.
  deaf_deadline       a helper that never answers a stop - the defect the
                      deadline is for: the job ends by name.
Breakages
  request_first       the helper loads the request before its wake word: a
                      request stored and bumped between the two loads is slept
                      through (LOST WAKE).
  no_bump             the lead wakes the helper without bumping its word: a
                      wake that lands before the helper's wait is lost.
  done_early          the helper answers done before its close's last ring use:
                      the lead frees the ring under it (RING).
  deaf_no_deadline    the deaf helper, the lead waiting for the acknowledgement
                      with no deadline: the job never ends (STUCK).
  stale_answer        the lead takes any answer as the one it asked for: an
                      answer of idle to an earlier request, computed before a
                      later commit, passes for the later request's (FRESH).

Usage: helper_request_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

REQUEST = ('request',)
ANSWER = ('answer',)            # the sequence answered
ANSWER_IDLE = ('answer_idle',)  # its verdict
ACKED = ('acked',)              # the stop's acknowledgement
DONE = ('done',)
WAKE = ('wake',)
SLEEPING = ('sleeping',)
DIRTY = ('dirty',)
DATA = ('data',)                # operations committed
TAKEN = ('$taken',)             # operations a pass has taken
FREED = ('$freed',)
BEFORE = lambda q: ('$before', q)   # operations committed when request q was posted
FINISHED = ('$finished',)
ENDED = ('ended',)             # the job ended by the deadline: the process, and the helper with it

QUIET1, QUIET2, STOP = 1, 2, 3     # request sequences, in the order the lead posts them


def writer(ops):
    """An application thread committing `ops` operations into the helper's
    connection, and waking it by lead_wake_model.py's claimed wake."""
    B = []
    for k in range(ops):
        p = f'w{k}_'
        nxt = f'@w{k + 1}_C' if k + 1 < ops else '@WEND'
        B += [(p + 'C', ('add', lambda L: DATA, lambda L: 1, '_', '@' + p + 'M')),
              (p + 'M', ('or', lambda L: DIRTY, lambda L: 1, '_', '@' + p + 'L')),
              (p + 'L', ('load', lambda L: SLEEPING, 'z', '@' + p + 'B')),
              (p + 'B', ('br', lambda L: L['z'] == 1, '@' + p + 'Q', '@' + p + 'X')),
              (p + 'Q', ('cas', lambda L: SLEEPING, lambda L: 1, lambda L: 0, 'z',
                         '@' + p + 'Q2')),
              (p + 'Q2', ('br', lambda L: L['z'], '@' + p + 'K', '@' + p + 'X')),
              (p + 'K', ('add', lambda L: WAKE, lambda L: 1, '_', '@' + p + 'F')),
              (p + 'F', ('fwake', lambda L: WAKE, '@' + p + 'X')),
              (p + 'X', ('set', 'z', lambda L: DEL, nxt))]
    B += [('WEND', ('done',))]
    return assemble(B)


def lead(design, quiets):
    """The lead's agent: `quiets` quiet requests, each answered, then a stop."""
    def post(q, at, nxt):
        """Store request q, then wake: bump and futex_wake (no_bump: only wake)."""
        B = [(at, ('store', lambda L, q=q: REQUEST, lambda L, q=q: q, True, f'@{at}_g')),
             (f'{at}_g', ('gpost', lambda M, L, q=q: ({BEFORE(q): M[DATA]}, {}), f'@{at}_b'))]
        if design == 'no_bump':
            B += [(f'{at}_b', ('fwake', lambda L: WAKE, nxt))]
        else:
            B += [(f'{at}_b', ('add', lambda L: WAKE, lambda L: 1, '_', f'@{at}_f')),
                  (f'{at}_f', ('fwake', lambda L: WAKE, nxt))]
        return B

    def fresh(q):
        def check(M, L):
            if M[ANSWER_IDLE] == 1 and M[TAKEN] < M[BEFORE(q)]:
                return (f'FRESH: an answer of idle taken for request {q} while '
                        f'{M[BEFORE(q)] - M[TAKEN]} operation(s) committed before it '
                        f'are untaken')
            return None
        return check

    B = []
    for k in range(quiets):
        q = QUIET1 + k
        B += post(q, f'Q{k}', f'@Q{k}w')
        # Wait for the answer: the C polls it on the detector's turns; the
        # wait's own wake is outside this model.
        if design == 'stale_answer':
            B += [(f'Q{k}w', ('await', lambda M, L: M[ANSWER] >= 1, f'@Q{k}c'))]
        else:
            B += [(f'Q{k}w', ('await', lambda M, L, q=q: M[ANSWER] >= q, f'@Q{k}c'))]
        B += [(f'Q{k}c', ('check', fresh(q), f'@Q{k + 1}' if k + 1 < quiets else '@S'))]
    B += post(STOP, 'S', '@SA')

    # The acknowledgement, under the deadline where the design has one.
    def acknowledged(M, L):
        out = []
        if M[ACKED] >= STOP:
            out.append(({}, {}, 'the stop acknowledged'))
        if design == 'deaf_deadline':
            out.append(({}, {'ended': 1}, 'the deadline: the job ends naming the helper'))
        return out

    B += [('SA', ('nd', acknowledged, '@SB')),
          ('SB', ('br', lambda L: L['ended'] == 1, '@X0', '@SD')),
          # The job's end takes the process, and the helper with it: in the model,
          # a word the helper reads at the top of its loop, and a wake.
          ('X0', ('store', lambda L: ENDED, lambda L: 1, True, '@X1')),
          ('X1', ('add', lambda L: WAKE, lambda L: 1, '_', '@X2')),
          ('X2', ('fwake', lambda L: WAKE, '@X3')),
          ('X3', ('ghost', lambda M, L: ({FINISHED: 1}, {}), '@END')),
          # Done, then free the ring and join the thread.
          ('SD', ('await', lambda M, L: M[DONE] >= STOP, '@SF')),
          ('SF', ('ghost', lambda M, L: ({FREED: 1, FINISHED: 1}, {}), '@END')),
          ('END', ('done',))]
    return assemble(B)


def helper(design, deaf):
    """The helper's loop over its own wires: take its requests, pass, sleep."""
    def ring_use(M, L):
        return 'RING: the helper used its ring after the lead freed it' if M[FREED] else None

    def take(M, L):
        """The pass: every committed operation taken (exchange of the bitmap and
        the buffers' bytes, folded into one step - lead_wake_model.py checks the
        exchange); `t` what it has taken."""
        return ({TAKEN: M[DATA], DIRTY: 0}, {'moved': int(M[DIRTY] != 0), 't': M[DATA]})

    if design == 'request_first':
        B = [('TOP', ('load', lambda L: REQUEST, 'r', '@TOP2')),
             ('TOP2', ('load', lambda L: WAKE, 'w', '@TOP3'))]
    else:
        B = [('TOP', ('load', lambda L: WAKE, 'w', '@TOP2')),
             ('TOP2', ('load', lambda L: REQUEST, 'r', '@TOP3'))]
    B += [('TOP3', ('load', lambda L: ENDED, 'x', '@TOP4')),
          ('TOP4', ('br', lambda L: L['x'] == 1, '@HEND', '@P')),
          ('P', ('ghost', take, '@PU')),
          ('PU', ('check', ring_use, '@R')),
          # A new request: answer it after the pass.
          ('R', ('br', lambda L: (L['r'] or 0) > (L['seen'] or 0), '@RN', '@IDLE')),
          ('RN', ('set', 'seen', lambda L: L['r'], '@RK')),
          ('RK', ('br', lambda L: L['r'] == STOP, '@STOP', '@QA'))]
    # A quiet request: the verdict (idle: nothing committed untaken), then the
    # sequence it answers - a release store after its verdict.
    B += [('QA', ('load', lambda L: DATA, 'd', '@QA2')),
          ('QA2', ('store', lambda L: ANSWER_IDLE, lambda L: int(L['d'] == L['t']), True, '@QA3')),
          ('QA3', ('store', lambda L: ANSWER, lambda L: L['r'], True, '@TOP'))]
    if deaf:
        B += [('STOP', ('set', 'd', lambda L: DEL, '@IDLE'))]     # never answers
    else:
        B += [('STOP', ('store', lambda L: ACKED, lambda L: STOP, True, '@C0'))]
        if design == 'done_early':
            B += [('C0', ('store', lambda L: DONE, lambda L: STOP, True, '@C1')),
                  ('C1', ('check', ring_use, '@C2')),
                  ('C2', ('check', ring_use, '@HEND'))]
        else:
            # The close: its wires' last bytes and goodbyes through the ring.
            B += [('C0', ('check', ring_use, '@C1')),
                  ('C1', ('check', ring_use, '@C2')),
                  ('C2', ('store', lambda L: DONE, lambda L: STOP, True, '@HEND'))]
    # Nothing new: raise the flag, look at the bitmap again, sleep on w.
    B += [('IDLE', ('br', lambda L: L['moved'] == 1, '@TOP', '@RAISE')),
          ('RAISE', ('store', lambda L: SLEEPING, lambda L: 1, True, '@LOOK')),
          ('LOOK', ('load', lambda L: DIRTY, 'again', '@DEC')),
          ('DEC', ('br', lambda L: L['again'] != 0, '@AWAKE', '@SLEEP')),
          ('SLEEP', ('fwait', lambda L: WAKE, lambda L: L['w'], '@AWAKE')),
          ('AWAKE', ('store', lambda L: SLEEPING, lambda L: 0, True, '@TOP')),
          ('HEND', ('done',))]
    return assemble(B)


def case(design, quiets, ops):
    deaf = design in ('deaf_deadline', 'deaf_no_deadline')
    threads = [lead(design, quiets), helper(design, deaf)]
    if ops:
        threads.append(writer(ops))
    mem = {REQUEST: 0, ANSWER: 0, ANSWER_IDLE: 0, ACKED: 0, DONE: 0, WAKE: 0, SLEEPING: 0,
           DIRTY: 0, DATA: 0, TAKEN: 0, FREED: 0, FINISHED: 0, ENDED: 0}
    for q in (QUIET1, QUIET2, STOP):
        mem[BEFORE(q)] = 0
    name = f'helper_request/{design} quiets={quiets} writer_ops={ops}'
    return name, threads, mem


def finished(M):
    return M[FINISHED] == 1


# (design, quiet requests, writer operations, tso, expect_ok)
CASES = [
    ('design', 1, 0, False, True),
    ('design', 1, 1, False, True),
    ('design', 2, 1, False, True),
    ('design', 2, 1, True, True),
    ('deaf_deadline', 1, 0, False, True),
    ('deaf_deadline', 1, 1, True, True),
    ('request_first', 1, 0, False, False),
    ('no_bump', 1, 0, False, False),
    ('done_early', 1, 0, False, False),
    ('deaf_no_deadline', 1, 0, False, False),
    ('stale_answer', 2, 1, False, False),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, quiets, ops, tso, expect in CASES:
        name, threads, mem0 = case(design, quiets, ops)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, tso=tso, max_seconds=900, done=finished,
                      liveness_max_states=4_000_000)
        ok &= report(res, expect_ok=expect, tso=tso)
    sys.exit(0 if ok else 1)
