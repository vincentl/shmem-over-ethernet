#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The internode arrival handoff (design §5.3, §3.12, §4.9).

§5.3 says whichever local member's increment reaches the epoch's arrival target
sends the node's arrival to the root. It cannot send it itself. The progress
state - the root's arrival counts, the per-connection queues, the send cursor -
is private to the local lead's *agent thread*, in the lead's process, and §3.12
makes that thread the only one that touches a connection's send path. The
arriver is an application thread of any local PE, usually another process
entirely.

So the arrival crosses a thread boundary by §3.12's stated mechanism, "a flag
plus a futex wake": the arriver leaves the epoch in a word of the node header
and wakes the lead's agent, which consumes it and performs the arrival.

**The question this model exists to answer is whether one word per team is
enough**, because that is what the data structure turns on. The argument is
§5.4's bounded skew: epoch k+2's arrival cannot be posted until every local
member has been released from k+1, and that release cannot happen until the
lead has already consumed and forwarded k+1. If that argument holds, a single
epoch word per team needs no queue, no sequence number and no second slot. If
it does not, a post can overwrite one the lead has not read, and a node stops
arriving - a hang on every *other* node, which is the worst place to debug it.
POSTED and CONSUMED below are that argument, checked rather than asserted:
OVERWRITE, no post over an epoch the lead has not consumed; and ORDER, each
epoch forwarded once, in order.

Designs
  design      the arriver publishes the epoch and then bumps the lead's wake
              word with a locked add; the lead's agent loads the wake word
              **first**, then the handoff, and records the epoch consumed - from
              the value it loaded - as part of forwarding it.
Breakages
  wake_after  the agent loads the handoff first and the wake word second. A post
              lands in the window between the two, and the agent then sleeps on
              a wake value that already counts the bump. Nothing wakes it.
  nowake      the arriver publishes the epoch and never bumps the wake word.
  consume_reread
              the agent records the consumed epoch by **re-reading the shared
              word** after forwarding, rather than from the value it forwarded:
              `arrive(...); consumed = hdr->handoff;`. On the root node the
              forward can release the team locally at once (§5.3: the root
              applies its own arrival directly), so a member can re-enter and
              post the next epoch into that word before the re-read. The agent
              then marks an epoch consumed that it never forwarded, and that
              epoch's arrival is lost.
  noconsume   the agent forwards and never records the epoch at all, so it finds
              the same word set on its next pass and forwards it twice. It is
              here because it is the only breakage that reaches the ordering
              check, which would otherwise assert nothing - a check no case can
              fail is exactly what this project keeps finding after the fact.

**The smallest configuration that can fail is not the same for all three.**
`wake_after` fails at one epoch: the very first arrival can land in the window
between the two loads. `consume_reread` and `noconsume` need two, because both
are defects in reusing the word, and at one epoch they are indistinguishable
from the design. A model run at one epoch would have called two of the three
correct.

The sweep is deliberately not modelled. Every wait in the library carries an
OSOE_SWEEP_NS deadline (§3.10), so in the running library a lost wake here is a
stall rather than a hang - and at 100 ms a barrier that relies on the sweep for
liveness is a hang as far as the program is concerned. The wake protocol is
what makes a barrier cost microseconds; the sweep is what keeps a lost one from
costing the job. This model checks the first, so a breakage earns STUCK or LOST
WAKE - whichever thread the terminal check reaches first, a member still waiting
on `released` or the agent asleep on a wake value that already counts the bump.

The internode round trip is abstracted into one thread: the root releases an
epoch once that epoch's arrival has been forwarded, at any later moment. It is
free to release immediately, which is the tightest race - a member is then back
posting the next epoch while the agent is still in the same pass - and it is
also what really happens on the root node itself.

Runs are SC except where noted. The handoff is a **plain release store**
followed by a locked add on the wake word (§3.16: single-writer publication
release, the two-sided bump seq_cst), so x86-TSO could in principle let the two
be seen out of order. The tso=True cases show that it does not break the design.
They do **not** show that the locked add is what prevents it: a bump done as a
plain load and store passes the same SC and TSO cases (second review,
evidence-audit F7, four configurations). The locked add is required for another
reason, outside this model - the wake word has many writers, and a plain
increment from two of them loses one.

Usage: arrival_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

ARRIVED = ('arrived',)        # the team's §5.4 arrival counter, all local members
RELEASED = ('released',)      # the team's §5.4 release counter, waited on by members
HANDOFF = ('handoff',)        # the epoch the arriver leaves for the lead's agent
WAKE = ('wake',)              # the lead's agent_wake futex word (§3.12)
FORWARDED = ('forwarded',)    # arrivals the agent has put on the wire to the root
POSTED = ('$posted',)         # posts made, and posts the agent has taken: the
CONSUMED = ('$consumed',)     # one-word argument, in two ghost counters


def arriver(design, epochs, members):
    """One local member, running `epochs` barriers on a team of `members` here.

    Every member increments; whichever increment reaches (k+1) x members is the
    one that posts, and it does not wait for a designated PE to be scheduled
    (§5.3). Every member then waits for `released` to equal k+1 (§5.4).
    """
    B = []
    for k in range(epochs):
        e, p = k + 1, f'a{k}_'
        nxt = f'@a{k + 1}_A' if k + 1 < epochs else '@AEND'
        B += [(p + 'A', ('add', lambda L: ARRIVED, lambda L: 1, 'n', '@' + p + 'T')),
              (p + 'T', ('br', lambda L, e=e, m=members: L['n'] == e * m,
                         '@' + p + 'C', '@' + p + 'W'))]
        # The one-word argument, checked at the only moment it can fail: this
        # post is about to overwrite whatever is in the word.
        B += [(p + 'C', ('check', lambda M, L, e=e: None if M[POSTED] == M[CONSUMED] else
                         f'OVERWRITE: epoch {e} posted over epoch {M[POSTED]}, which the lead '
                         f'has not consumed: one word per team is not enough',
                         '@' + p + 'G')),
              (p + 'G', ('gpre', lambda M, L: ({POSTED: M[POSTED] + 1}, {}), '@' + p + 'S')),
              # §3.16 single-writer publication: a release store, not a locked one.
              (p + 'S', ('store', lambda L: HANDOFF, lambda L, e=e: e, False, '@' + p + 'K'))]
        if design == 'nowake':
            B += [(p + 'K', ('set', 'n', lambda L: DEL, '@' + p + 'W'))]
        else:
            B += [(p + 'K', ('add', lambda L: WAKE, lambda L: 1, '_', '@' + p + 'F')),
                  (p + 'F', ('fwake', lambda L: WAKE, '@' + p + 'W'))]
        B += [(p + 'W', ('set', 'n', lambda L: DEL, '@' + p + 'V')),
              # §5.4: equality, on the counter itself. Its wake protocol is
              # epoch.c's and is not what this model is about.
              (p + 'V', ('await', lambda M, L, e=e: M[RELEASED] == e, nxt))]
    B += [('AEND', ('done',))]
    return assemble(B)


def order(M, L):
    """Arrivals reach the root once each, in epoch order.

    Only a repeat can fail it here. An arrival skipped over - epoch k+2
    forwarded after k - cannot be reached by any variant: k+2 cannot be posted
    until k+1 is released, which needs k+1 forwarded. A lost arrival shows up as
    STUCK instead (`consume_reread`). The comparison still refuses a skip, in
    case that argument ever stops holding, but no case exercises it.
    """
    return (None if L['h'] == M[FORWARDED] + 1 else
            f"ORDER: epoch {L['h']} forwarded after {M[FORWARDED]}: an arrival was "
            f"{'repeated' if L['h'] <= M[FORWARDED] else 'skipped'}")


def lead(design, epochs):
    """The local lead's agent: the only thread that may touch the send path."""
    B = [('L0', ('set', 'f', lambda L: 0, '@L1')),
         ('L1', ('set', 'c', lambda L: 0, '@LOOP')),
         ('LOOP', ('br', lambda L, e=epochs: L['f'] >= e, '@LEND', '@ORDER'))]
    if design == 'wake_after':
        B += [('ORDER', ('load', lambda L: HANDOFF, 'h', '@SECOND')),
              ('SECOND', ('load', lambda L: WAKE, 'w', '@HAVE'))]
    else:
        # §4.9's rule: the wake word BEFORE the state it guards.
        B += [('ORDER', ('load', lambda L: WAKE, 'w', '@SECOND')),
              ('SECOND', ('load', lambda L: HANDOFF, 'h', '@HAVE'))]
    B += [('HAVE', ('br', lambda L: L['h'] != L['c'], '@FWD', '@SLEEP')),
          # Nothing posted: sleep on the value loaded above. The kernel compares
          # at submission, so a bump since that load returns at once (§3.10).
          ('SLEEP', ('fwait', lambda L: WAKE, lambda L: L['w'], '@LOOP')),
          ('FWD', ('check', order, '@FWD2'))]
    if design == 'consume_reread':
        B += [('FWD2', ('add', lambda L: FORWARDED, lambda L: 1, 'f', '@FWD3')),
              ('FWD3', ('load', lambda L: HANDOFF, 'c', '@FWD4')),
              ('FWD4', ('gpost', lambda M, L: ({CONSUMED: M[CONSUMED] + 1}, {}), '@LOOP'))]
    elif design == 'noconsume':
        B += [('FWD2', ('gpre', lambda M, L: ({CONSUMED: M[CONSUMED] + 1}, {}), '@FWD3')),
              ('FWD3', ('add', lambda L: FORWARDED, lambda L: 1, 'f', '@LOOP'))]
    else:
        # Consumed from the value being forwarded, and recorded with the forward.
        B += [('FWD2', ('gpre', lambda M, L: ({CONSUMED: M[CONSUMED] + 1}, {'c': L['h']}),
                        '@FWD3')),
              ('FWD3', ('add', lambda L: FORWARDED, lambda L: 1, 'f', '@LOOP'))]
    B += [('LEND', ('done',))]
    return assemble(B)


def root(epochs):
    """The internode round trip, abstracted: release an epoch once it arrives.

    Free to release the instant the arrival is forwarded, which is the tightest
    race and also what the root node does to itself (§5.3).
    """
    return assemble([
        ('R0', ('set', 'r', lambda L: 0, '@R')),
        ('R', ('br', lambda L, e=epochs: L['r'] >= e, '@REND', '@RW')),
        ('RW', ('await', lambda M, L: M[FORWARDED] > L['r'], '@RA')),
        ('RA', ('add', lambda L: RELEASED, lambda L: 1, '_', '@RI')),
        ('RI', ('set', 'r', lambda L: L['r'] + 1, '@R')),
        ('REND', ('done',)),
    ])


def case(design, members, epochs):
    one = arriver(design, epochs, members)
    threads = [one] * members + [lead(design, epochs), root(epochs)]
    mem = {ARRIVED: 0, RELEASED: 0, HANDOFF: 0, WAKE: 0, FORWARDED: 0}
    # The members run the same program and nothing depends on which is which.
    sym = (list(range(members)),) if members > 1 else ()
    return (f'arrival/{design} members={members} epochs={epochs}', threads, mem, sym)


# (design, members, epochs, tso, expect): True, or the property the breakage must fail.
#
# The epochs=1 rows are the measurement behind the docstring's note: wake_after
# already fails there, while consume_reread and noconsume do not, because those
# two are defects in *reusing* the word and one epoch never reuses it. Two
# members is the smallest configuration with an arriver that is not also the one
# that posts.
#
# The TSO rows say the design holds under TSO: the arriver publishes the epoch
# with an ordinary store and then bumps the wake word with a locked add. They do
# not discriminate the locked add - a plain bump passes them too (see the
# docstring) - so they are a check of the design, not an argument for its
# instruction choice.
CASES = [
    ('design', 1, 2, False, True),
    ('design', 2, 2, False, True),
    ('design', 3, 2, False, True),
    ('design', 2, 3, False, True),
    ('design', 2, 2, True, True),
    ('design', 3, 2, True, True),

    ('wake_after', 2, 1, False, 'STUCK'),   # the first arrival is already enough
    ('wake_after', 2, 2, False, 'STUCK'),
    ('wake_after', 3, 2, False, 'STUCK'),

    ('nowake', 2, 1, False, 'STUCK'),
    ('nowake', 2, 2, False, 'STUCK'),

    ('consume_reread', 2, 1, False, True),  # one epoch cannot show this one
    ('consume_reread', 2, 2, False, 'OVERWRITE'),
    ('consume_reread', 3, 2, False, 'OVERWRITE'),

    ('noconsume', 2, 1, False, True),       # nor this one
    ('noconsume', 2, 2, False, 'ORDER'),
    ('noconsume', 3, 2, False, 'ORDER'),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, members, epochs, tso, expect in CASES:
        name, threads, mem0, sym = case(design, members, epochs)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, tso=tso, symmetric=sym, max_seconds=180)
        ok &= report(res, expect=expect, tso=tso)
    sys.exit(0 if ok else 1)
