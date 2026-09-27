#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Landing-zone extents for concurrent blocking gets (design §4.11, §3.12).

Under SERIALIZED a PE lands a blocking get's reply in one slot, one caller at a
time. Under SHMEM_THREAD_MULTIPLE several threads of a PE can be in a blocking
get at once, so the author's decision of 2026-09-25 cuts the landing zone into E
fixed extents, claimed with a compare-and-swap over a bitmap: one bit per extent
in one word. A thread sets a clear bit before it sends its request, and the
request carries the extent. The progress thread lands the reply there. The
thread copies the reply out, clears its bit, and wakes the word. A thread that
finds every bit set sleeps on the word with the value it read, so a bit cleared
between its read and its sleep makes the sleep return at once.

Checked
  EXCLUSIVE  no two threads hold one extent at once.
  OWN        the reply a thread copies out is its own request's.
  and, by the engine: no lost wake, no stuck thread, and from every reachable
  state the run can still finish (AG EF done).

Threads
  issuers  T threads, G blocking gets each: claim, send, await the reply, copy
           out, release, wake. Waiting for the reply is abstract here: a
           blocking get's own completion is §4.6's, modelled in
           get_completion_model.py.
  lander   the reply path: lands any sent request, in any order.

Designs
  design         as above.
Breakages
  store_claim    the claim is a load and a store rather than a CAS: two threads
                 take one extent (EXCLUSIVE).
  release_early  the bit is cleared before the copy-out: another thread's reply
                 lands over this one's (OWN).
  wake_first     the release wakes the word before clearing the bit: a sleeper
                 wakes, finds the word still full, and sleeps for good.
  no_wake        the release clears the bit without waking anyone.

Every protocol step is a locked instruction, a CAS or a futex call, and the
plain stores to an extent's request and landed words are ordered by the bit's
CAS and clear, so x86-TSO adds no interleavings; runs are SC.

Usage: landing_extent_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

BITS = ('bits',)
REQ = lambda e: ('req', e)        # a request in flight for extent e: its tag, until it lands
DATA = lambda e: ('data', e)      # the tag of the reply landed in extent e
LANDED = lambda e: ('landed', e)  # 1 once extent e's reply has landed
OWNER = lambda e: ('$owner', e)   # the thread holding extent e, plus one
DONE = ('$done',)                 # gets completed


def lowest_clear(w, extents):
    for b in range(extents):
        if not w & (1 << b):
            return b
    return None


def issuer(design, i, gets, extents):
    full = (1 << extents) - 1
    B = []
    for k in range(gets):
        tag = 1 + i * gets + k
        p = f'i{i}_{k}_'
        nxt = f'@i{i}_{k + 1}_C' if k + 1 < gets else '@END'
        # The claim: read the word, sleep while it is full, set a clear bit.
        B += [(p + 'C', ('load', lambda L: BITS, 'w', '@' + p + 'F')),
              (p + 'F', ('br', lambda L, full=full: L['w'] == full, '@' + p + 'W', '@' + p + 'P')),
              (p + 'W', ('fwait', lambda L: BITS, lambda L: L['w'], '@' + p + 'C')),
              (p + 'P', ('set', 'b', lambda L, e=extents: lowest_clear(L['w'], e), '@' + p + 'T'))]
        if design == 'store_claim':
            B += [(p + 'T', ('store', lambda L: BITS, lambda L: L['w'] | (1 << L['b']), True,
                             '@' + p + 'T1')),
                  (p + 'T1', ('set', 'ok', lambda L: True, '@' + p + 'K'))]
        else:
            B += [(p + 'T', ('cas', lambda L: BITS, lambda L: L['w'],
                             lambda L: L['w'] | (1 << L['b']), 'ok', '@' + p + 'K'))]
        B += [(p + 'K', ('br', lambda L: L['ok'], '@' + p + 'X', '@' + p + 'C')),
              (p + 'X', ('check', lambda M, L: None if M[OWNER(L['b'])] == 0 else
                         f'extent {L["b"]} held by thread {M[OWNER(L["b"])] - 1} and '
                         f'claimed again', '@' + p + 'O')),
              (p + 'O', ('ghost', lambda M, L, i=i: ({OWNER(L['b']): i + 1}, {}), '@' + p + 'S')),
              # Send: the request carries the extent; from here its reply may land.
              (p + 'S', ('store', lambda L: REQ(L['b']), lambda L, tag=tag: tag, True,
                         '@' + p + 'A')),
              (p + 'A', ('await', lambda M, L: M[LANDED(L['b'])] == 1, '@' + p + 'R0'))]

        def copy_out(label, then, tag=tag, p=p):
            return [(p + label, ('check', lambda M, L, tag=tag:
                                 None if M[DATA(L['b'])] == tag else
                                 f'thread copied reply {M[DATA(L["b"])]} out of extent {L["b"]}, '
                                 f'expected its own {tag}', '@' + p + label + 'a')),
                    # The extent is empty again before it is given back.
                    (p + label + 'a', ('store', lambda L: LANDED(L['b']), lambda L: 0, True,
                                       then))]

        def release(label, then, p=p):
            clear = [(p + label, ('gpre', lambda M, L: ({OWNER(L['b']): 0}, {}),
                                  '@' + p + label + 'c')),
                     (p + label + 'c', ('andn', lambda L: BITS, lambda L: 1 << L['b'], '_',
                                        '@' + p + label + 'w'))]
            wake = [(p + label + 'w', ('fwake', lambda L: BITS, then))]
            if design == 'no_wake':
                wake = [(p + label + 'w', ('br', lambda L: True, then, then))]
            if design == 'wake_first':
                return [(p + label, ('fwake', lambda L: BITS, '@' + p + label + 'g')),
                        (p + label + 'g', ('gpre', lambda M, L: ({OWNER(L['b']): 0}, {}),
                                           '@' + p + label + 'c')),
                        (p + label + 'c', ('andn', lambda L: BITS, lambda L: 1 << L['b'], '_',
                                           then))]
            return clear + wake

        finish = [(p + 'G', ('gpost', lambda M, L: ({DONE: M[DONE] + 1},
                                                    {'w': DEL, 'b': DEL, 'ok': DEL}), nxt))]
        if design == 'release_early':
            B += release('R0', '@' + p + 'Y0') + copy_out('Y0', '@' + p + 'G') + finish
        else:
            B += copy_out('R0', '@' + p + 'Z0') + release('Z0', '@' + p + 'G') + finish
    B += [('END', ('done',))]
    return assemble(B)


def lander(total, extents):
    """The progress thread: lands any request in flight, in any order. A request
    is a message, consumed when its reply lands."""
    def alts(M, L):
        return [({DATA(e): M[REQ(e)], LANDED(e): 1, REQ(e): 0}, {'n': (L['n'] or 0) + 1},
                 f'land e{e}')
                for e in range(extents) if M[REQ(e)] != 0]
    return assemble([('L', ('br', lambda L, t=total: (L['n'] or 0) >= t, '@LEND', '@S')),
                     ('S', ('nd', alts, '@L')),
                     ('LEND', ('done',))])


def case(design, threads_n, gets, extents):
    threads = [issuer(design, i, gets, extents) for i in range(threads_n)]
    threads.append(lander(threads_n * gets, extents))
    mem = {BITS: 0, DONE: 0}
    for e in range(extents):
        mem[REQ(e)] = mem[DATA(e)] = mem[LANDED(e)] = mem[OWNER(e)] = 0
    name = f'landing_extent/{design} threads={threads_n} gets={gets} extents={extents}'
    return name, threads, mem


# (design, threads, gets, extents, expect_ok)
CASES = [
    ('design', 2, 2, 1, True),
    ('design', 3, 1, 2, True),
    ('design', 3, 2, 2, True),
    ('store_claim', 2, 1, 2, False),
    ('release_early', 2, 2, 1, False),
    ('wake_first', 2, 2, 1, False),
    ('no_wake', 2, 2, 1, False),
]


def main():
    want = select(sys.argv)
    ok = True
    for design, threads_n, gets, extents, expect in CASES:
        name, threads, mem = case(design, threads_n, gets, extents)
        if not want(name):
            continue
        total = threads_n * gets
        res = explore(name, threads, mem, max_seconds=600,
                      done=lambda M, total=total: M[DONE] == total)
        ok &= report(res, expect_ok=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
