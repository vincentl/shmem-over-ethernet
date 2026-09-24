#!/usr/bin/env python3
"""The distributed lock of shmem_set_lock, shmem_test_lock and shmem_clear_lock
(spec §9.13; design §4.12; lock.c).

The spec's lock is first come, first served - "Waiting PEs are guaranteed to set
the lock in a first-come, first-served manner" - so it is a queue lock (MCS),
and the application's `long` lock variable is the implementation's on every PE,
which the spec allows ("Accessing an in-use lock variable using any method other
than the OpenSHMEM lock API ... results in undefined behavior"). One PE, the
lock's home, keeps the queue's tail in one 32-bit half of its copy; every PE
keeps its own queue node in the other half: the next waiter's PE + 1, and a
GRANTED bit.

  acquire(me)  store my node := 0                        (before anyone can see me)
               prev := swap(home's tail, me + 1)         (I am the last now)
               if prev == 0: the lock is mine
               else: or(prev's node, me + 1)             (link behind prev)
                     wait until my node has GRANTED       (a local wait)
                     and-not(my node, GRANTED)            (keep a successor's link)
  release(me)  (quiet: the critical region's writes complete)
               n := my node's successor
               if n == 0: if cas(home's tail, me + 1, 0): released, nobody waits
                          else: wait until my node != 0; n := its successor
               or(n's node, GRANTED)
  test(me)     store my node := 0; cas(home's tail, 0, me + 1): 0 if it succeeded

The release's wait is the code's: for the whole word to be nonzero, taking the
successor from the value the wait returned. That is the link only while GRANTED
is clear in a holder's node, which the acquire's and-not makes so; the model
checks it (a release that wakes to a word with no successor is a violation), so
the invariant the code's wait rests on is proved rather than assumed.

Every operation on another PE's word is one of the transport's rmw, which is
atomic and exclusive (§4.10), so each is one step here; the model is sequentially
consistent, since the protocol relies on nothing but each PE's program order and
that atomicity. The waits are abstract: the library waits on its own word with
the region wait (region-wait.h), whose wake-ups are checked elsewhere.

The link and the grant are not fetching, and across nodes they apply some time
after the call returns; here each applies at issue. That is sound because only
a node word's owner reads it, and the owner is either waiting for the write or
reads before it lands - a schedule the model also explores, the write simply
coming later.

Properties
  P1  mutual exclusion: never two PEs in the critical region;
  P2  first come, first served: a PE enters in the order its swap - or its
      successful test's compare-and-swap - took effect (ghost $queue);
  and no trapped state: every PE finishes all its rounds from every state (the
  engine's AG EF done, with STUCK at dead ends).

Breakages, each of which must fail
  reset_late     my node is zeroed after the swap: a successor that linked in
                 between loses its link. -> trapped
  no_wait_next   release frees the lock with a compare-and-swap that fails and
                 does not wait for the successor still linking. -> trapped
  store_clear    GRANTED is cleared by storing 0, not by and-not, wiping a link
                 a successor made meanwhile. -> trapped
  test_swaps     test uses the swap: a busy lock now has a tail nobody waits
                 for. -> P1 or trapped
  no_link        the waiter never links behind its predecessor. -> trapped
  grant_tail     release grants the queue's last waiter, not its next: a later
                 comer enters first. -> P2
  enter_early    a waiter enters once it has linked, without waiting for the
                 grant. -> P1
  no_andn        a waiter enters leaving GRANTED set in its node, so a release
                 whose compare-and-swap loses to a waiter not yet linked wakes at
                 once to a word with no successor. -> violation

Usage: lock_model.py [substring ...]
"""
import sys

from engine import DEL, assemble, explore, report, select

TAIL = ('tail',)
GRANTED = 0x100
SUCCESSOR = 0xff
IN = ('$in',)
QUEUE = ('$queue',)


def node(p):
    return ('node', p)


def enter_checks(me):
    """The critical region: P1 and P2, and the region's own ghost count."""
    return [
        ('CS', ('ghost', lambda M, L: ({IN: M[IN] + 1}, {}), '@P1')),
        ('P1', ('check', lambda M, L: None if M[IN] == 1 else
                'two PEs in the critical region', '@P2')),
        ('P2', ('check', lambda M, L, me=me: None if M.get(QUEUE, ()) and M[QUEUE][0] == me
                else f'PE {me} entered out of turn: queue {M.get(QUEUE, ())}', '@LEAVE')),
        ('LEAVE', ('ghost', lambda M, L: ({IN: M[IN] - 1, QUEUE: M[QUEUE][1:]}, {}), '@R')),
    ]


def release(design, me):
    B = [('R', ('load', lambda L, me=me: node(me), 'n', '@R1')),
         ('R1', ('br', lambda L: (L['n'] & SUCCESSOR) != 0, '@GRANT', '@FREE')),
         ('FREE', ('cas', lambda L: TAIL, lambda L, me=me: me + 1, lambda L: 0, 'ok', '@R2'))]
    if design == 'no_wait_next':
        B += [('R2', ('br', lambda L: L['ok'], '@ROUND', '@ROUND'))]
    else:
        # The code's wait: until the word is nonzero, the successor taken from
        # the value that ended it.
        B += [('R2', ('br', lambda L: L['ok'], '@ROUND', '@WAITNEXT')),
              ('WAITNEXT', ('nd', lambda M, L, me=me: [({}, {'n': M[node(me)]}, 'woke')]
                            if M[node(me)] != 0 else [], '@HAVENEXT')),
              ('HAVENEXT', ('check', lambda M, L: None if (L['n'] & SUCCESSOR) != 0 else
                            f"a release woke to node {L['n']:#x}, with no successor",
                            '@GRANT'))]
    if design == 'grant_tail':
        B += [('GRANT', ('load', lambda L: TAIL, 't', '@GRANT2')),
              ('GRANT2', ('or', lambda L: node(L['t'] - 1), lambda L: GRANTED, '_',
                          '@ROUND'))]
    else:
        B += [('GRANT', ('or', lambda L: node((L['n'] & SUCCESSOR) - 1), lambda L: GRANTED,
                         '_', '@ROUND'))]
    return B


def pe(design, me, rounds, tester=False):
    """A PE acquiring (or, as the tester, trying) the lock `rounds` times."""
    B = [('START', ('set', 'round', lambda L: 0, '@TOP')),
         ('TOP', ('br', lambda L, r=rounds: L['round'] >= r, '@END', '@BEGIN'))]
    if tester:
        if design == 'test_swaps':
            swap = lambda M, L, me=me: [({TAIL: me + 1, QUEUE: M.get(QUEUE, ()) + (me,)}
                                         if M[TAIL] == 0 else {TAIL: me + 1},
                                         {'ok': M[TAIL] == 0}, 'swap')]
        else:
            swap = lambda M, L, me=me: ([({TAIL: me + 1, QUEUE: M.get(QUEUE, ()) + (me,)},
                                          {'ok': True}, 'got')]
                                        if M[TAIL] == 0 else [({}, {'ok': False}, 'busy')])
        B += [('BEGIN', ('store', lambda L, me=me: node(me), lambda L: 0, True, '@TRY')),
              ('TRY', ('nd', swap, '@TRIED')),
              ('TRIED', ('br', lambda L: L['ok'], '@CS', '@ROUND'))]
    else:
        swap = [('SWAP', ('nd', lambda M, L, me=me: [({TAIL: me + 1,
                                                       QUEUE: M.get(QUEUE, ()) + (me,)},
                                                      {'prev': M[TAIL]}, 'swap')],
                          '@FIRST'))]
        reset = [('RESET', ('store', lambda L, me=me: node(me), lambda L: 0, True, '@NEXT'))]
        if design == 'reset_late':
            B += [('BEGIN', ('br', lambda L: True, '@SWAP', '@SWAP'))] + swap
            B += [('FIRST', ('br', lambda L: True, '@RESET', '@RESET'))]
            B += [('RESET', ('store', lambda L, me=me: node(me), lambda L: 0, True, '@DECIDE'))]
        else:
            B += [('BEGIN', ('br', lambda L: True, '@RESET', '@RESET'))] + reset
            B += [('NEXT', ('br', lambda L: True, '@SWAP', '@SWAP'))] + swap
            B += [('FIRST', ('br', lambda L: True, '@DECIDE', '@DECIDE'))]
        B += [('DECIDE', ('br', lambda L: L['prev'] == 0, '@CS', '@LINK'))]
        if design == 'no_link':
            B += [('LINK', ('br', lambda L: True, '@WAIT', '@WAIT'))]
        else:
            B += [('LINK', ('or', lambda L: node(L['prev'] - 1), lambda L, me=me: me + 1, '_',
                            '@WAIT'))]
        if design == 'enter_early':
            B += [('WAIT', ('br', lambda L: True, '@CLEAR', '@CLEAR'))]
        else:
            B += [('WAIT', ('await', lambda M, L, me=me: M[node(me)] & GRANTED, '@CLEAR'))]
        if design == 'store_clear':
            B += [('CLEAR', ('store', lambda L, me=me: node(me), lambda L: 0, True, '@CS'))]
        elif design == 'no_andn':
            B += [('CLEAR', ('br', lambda L: True, '@CS', '@CS'))]
        else:
            B += [('CLEAR', ('andn', lambda L, me=me: node(me), lambda L: GRANTED, '_',
                             '@CS'))]
    B += enter_checks(me) + release(design, me)
    B += [('ROUND', ('set', 'round', lambda L: L['round'] + 1, '@CLEAN')),
          ('CLEAN', ('set', 'prev', lambda L: DEL, '@CLEAN2')),
          ('CLEAN2', ('set', 'n', lambda L: DEL, '@CLEAN3')),
          ('CLEAN3', ('set', 'ok', lambda L: DEL, '@CLEAN4')),
          ('CLEAN4', ('set', 't', lambda L: DEL, '@TOP')),
          ('END', ('done',))]
    return assemble(B)


def case(design, pes, rounds, testers=0):
    threads = [pe(design, me, rounds, tester=me >= pes - testers) for me in range(pes)]
    mem = {TAIL: 0, IN: 0, QUEUE: ()}
    for me in range(pes):
        mem[node(me)] = 0
    return f'lock/{design} pes={pes} rounds={rounds} testers={testers}', threads, mem


# (design, PEs, rounds each, of which testers, expect_ok)
CASES = [
    ('design', 2, 2, 0, True),
    ('design', 3, 1, 0, True),
    ('design', 3, 2, 0, True),
    ('design', 3, 2, 1, True),
    ('design', 4, 1, 1, True),

    ('reset_late', 3, 1, 0, False),
    ('no_wait_next', 3, 1, 0, False),
    ('store_clear', 3, 2, 0, False),
    ('test_swaps', 3, 1, 1, False),
    ('no_link', 2, 1, 0, False),
    ('grant_tail', 3, 1, 0, False),
    ('enter_early', 2, 1, 0, False),
    ('no_andn', 3, 1, 0, False),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, pes, rounds, testers, expect in CASES:
        name, threads, mem0 = case(design, pes, rounds, testers)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, max_seconds=900, liveness_max_states=4_000_000)
        ok &= report(res, expect_ok=expect)
    sys.exit(0 if ok else 1)
