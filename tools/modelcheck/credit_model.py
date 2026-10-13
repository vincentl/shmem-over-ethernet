#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Reply credits and the queue bounds they enforce (design §3.8, §3.12, §4.6).

§3.8 bounds the target's deferred reply queues with a per-connection credit
rather than a per-PE outstanding-get cap, because the per-PE version grows with
*job* size: "The origin's lead takes a credit when one of its PEs issues a get
or fetching AMO toward that peer and returns it **when the reply lands**."

That sentence carries two bounds at once, at opposite ends of the connection,
and the model is here because they are easy to conflate:

  DESCRIPTORS  the target never holds more than C reply descriptors for this
               connection, so its worst case is connections x C and scales with
               node count as §1 requires (§3.8).
  LANDING      no more than C replies are ever in flight toward the origin, so
               they fit the landing zone the origin sized for them (§3.12: a
               reply cannot be written to a private `dest`, so it lands in the
               node segment first).
  CREDITS      and under both, the credit itself never goes below zero: no
               credit is handed out twice (race_take's failure).

The credit is per **connection**, not per PE: several PEs of a node share it, so
they contend on one counter. That contention is modelled, not abstracted.

This is also §3.8's distributed invariant - "**Enforced at the origin, not
locally**" - so the check that can fail is at the target while the code that
keeps it is at the origin. A peer's accounting bug overflows my queue and the
symptom appears on the wrong node, which is the reason to find it here.

Designs
  design    the credit returns when the reply lands at the origin.
Breakages, each the natural wrong moment to return it
  on_apply  returned when the target allocates the descriptor - handler time,
            which is when the target is "done" with the request. Overflows
            DESCRIPTORS: the origin reissues while the descriptor is alive.
  on_drain  returned when the target drains the reply onto the wire, rather than
            when it lands. Descriptors stay bounded, so this looks right - and it
            overflows LANDING instead, which is the bound at the *other* end and
            the reason the sentence says "lands" rather than "sent".
  none      no credit at all: §3.8's rejected unbounded queue.

Every counter here is a locked read-modify-write, so x86-TSO adds no
interleavings; runs are SC.

Usage: credit_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

ACC, DR = ('accepted',), ('drained',)
CRED = ('credits',)              # the connection's credits, shared by its PEs
SENT = lambda i: ('sent', i)     # requests on the wire from PE i
INUSE = lambda i: ('inuse', i)   # reply descriptors alive at the target, for PE i
LAND = lambda i: ('land', i)     # replies in flight toward PE i's landing zone
DONE = ('$done',)                # operations completed, for the terminal check

# §3.12 sizes a PE's landing zone by the outstanding-get resource, which is the
# credit C of §4.6. With blocking gets a PE never has more than one reply in
# flight whatever C is, so this bound has slack; with nonblocking gets it is the
# bound that binds, which is the `nbi` cases below.


def origin(design, pe, nops, C, nbi):
    """One PE issuing nops requests on this connection, and landing the replies.

    Issue and land are ordered per operation because a blocking get cannot issue
    its next request until its reply has landed: under SERIALIZED a PE has at
    most one such operation outstanding (§4.6). With `nbi` the PE issues every
    request before landing any, which is the shape a nonblocking get gives it -
    out of scope for this slice, and the only configuration in which the landing
    bound can bind at all.

    Only this PE decrements its own landing slot, so the test-and-take there
    needs no CAS. The credit is another matter: it is per *connection*, shared
    with the node's other PEs, and that is what the CAS below is for.
    """
    def issue(k, nxt):
        p = f'o{pe}_{k}_'
        B = []
        if design == 'none':
            B += [(p + 'T', ('br', lambda L: True, '@' + p + 'S', '@' + p + 'S'))]
        elif design == 'race_take':
            # Test then decrement: the two steps are not one, so two PEs of the
            # node can both pass the test and both decrement.
            B += [(p + 'T', ('await', lambda M, L: M[CRED] > 0, '@' + p + 'T2')),
                  (p + 'T2', ('add', lambda L: CRED, lambda L: -1, '_', '@' + p + 'T4'))]
        else:
            # A CAS loop, for the reason §3.5 gives for the reservation itself:
            # with a bare decrement an over-taking PE has already moved the
            # counter and must unwind. Blocking while no credit is free is legal
            # (§4.6: "A cap that causes blocking rather than failure is legal"),
            # and it is the issuing PE that blocks.
            B += [(p + 'T', ('load', lambda L: CRED, 'c', '@' + p + 'T1')),
                  (p + 'T1', ('br', lambda L: L['c'] > 0, '@' + p + 'T2', '@' + p + 'T0')),
                  (p + 'T0', ('await', lambda M, L: M[CRED] > 0, '@' + p + 'T')),
                  (p + 'T2', ('cas', lambda L: CRED, lambda L: L['c'],
                              lambda L: L['c'] - 1, 'ok', '@' + p + 'T3')),
                  (p + 'T3', ('br', lambda L: L['ok'], '@' + p + 'T4', '@' + p + 'T'))]
        if design != 'none':
            # A resource count that went negative has been handed out twice.
            B += [(p + 'T4', ('check', lambda M, L:
                              None if M[CRED] >= 0 else
                              f'CREDITS: credits went to {M[CRED]}: handed out twice',
                              '@' + p + 'S'))]
        B += [(p + 'S', ('add', lambda L, pe=pe: SENT(pe), lambda L: 1, '_', nxt))]
        return B

    def land(k, nxt):
        p = f'o{pe}_{k}_'
        B = [(p + 'W', ('await', lambda M, L, pe=pe: M[LAND(pe)] > 0, '@' + p + 'L')),
             (p + 'L', ('add', lambda L, pe=pe: LAND(pe), lambda L: -1, '_', '@' + p + 'D'))]
        # The credit returns when the reply lands, which is the design's claim.
        # race_take returns it here too: it differs from the design only in how
        # the credit is taken, so that the take is what the case is about.
        if design in ('design', 'race_take'):
            B += [(p + 'D', ('add', lambda L: CRED, lambda L: 1, '_', '@' + p + 'G'))]
        else:
            B += [(p + 'D', ('br', lambda L: True, '@' + p + 'G', '@' + p + 'G'))]
        B += [(p + 'G', ('gpost', lambda M, L: ({DONE: M[DONE] + 1},
                                                {'c': DEL, 'ok': DEL}), nxt))]
        return B

    B = []
    if nbi:
        # Issue only. The replies are landed by this PE's agent, a separate
        # thread (§3.12: a nonblocking get's reply is copied out of the landing
        # zone by that PE's agent, or by the application thread at quiet). They
        # must be separate here or the PE blocks issuing a request whose credit
        # its own later step would have returned - a deadlock of the model, not
        # of the design.
        for k in range(nops):
            B += issue(k, f'@o{pe}_{k + 1}_T' if k + 1 < nops else '@OEND')
    else:
        for k in range(nops):
            nxt = f'@o{pe}_{k + 1}_T' if k + 1 < nops else '@OEND'
            B += issue(k, '@' + f'o{pe}_{k}_' + 'W') + land(k, nxt)
    B += [('OEND', ('done',))]
    return assemble(B)


def lander(design, pe, nops, C):
    """A PE's agent, landing its nonblocking replies and returning credits."""
    B = []
    for k in range(nops):
        p = f'a{pe}_{k}_'
        nxt = f'@a{pe}_{k + 1}_W' if k + 1 < nops else '@AEND'
        B += [(p + 'W', ('await', lambda M, L, pe=pe: M[LAND(pe)] > 0, '@' + p + 'L')),
              (p + 'L', ('add', lambda L, pe=pe: LAND(pe), lambda L: -1, '_', '@' + p + 'D'))]
        if design in ('design', 'race_take'):
            B += [(p + 'D', ('add', lambda L: CRED, lambda L: 1, '_', '@' + p + 'G'))]
        else:
            B += [(p + 'D', ('br', lambda L: True, '@' + p + 'G', '@' + p + 'G'))]
        B += [(p + 'G', ('gpost', lambda M, L: ({DONE: M[DONE] + 1}, {}), nxt))]
    B += [('AEND', ('done',))]
    return assemble(B)


def target(design, npes, nreq, C, bound):
    """The peer's progress thread (§3.8).

    Accepting a request and draining a reply are **independent** activities, not
    a sequence: that is what a deferred reply queue is for. The thread holds
    descriptors for requests it has taken off the wire and drains them later, in
    its own order, round-robin across peers. Modelling them as one alloc-drain
    pair holds at most one descriptor alive at a time and makes the descriptor
    bound untestable - which is what an earlier version of this model did, and
    it reported every breakage as correct on that bound.

    Allocation is handler time and draining is later, which is also why a
    fetching AMO cannot be deferred: its descriptor carries the value, sampled
    when the handler ran, while a get reply carries a reference sampled at drain
    (§3.8 "Sampling semantics differ by type").
    """
    def alts(M, L):
        out = []
        if M[ACC] < nreq:
            for i in range(npes):
                if M[SENT(i)] > 0:
                    upd = {SENT(i): M[SENT(i)] - 1, INUSE(i): M[INUSE(i)] + 1,
                           ACC: M[ACC] + 1}
                    if design == 'on_apply':
                        upd[CRED] = M[CRED] + 1
                    live = sum(M[INUSE(j)] for j in range(npes)) + 1
                    out.append((upd, {'act': 0, 'n': live}, f'accept pe{i}'))
        for i in range(npes):
            if M[INUSE(i)] > 0:
                upd = {INUSE(i): M[INUSE(i)] - 1, LAND(i): M[LAND(i)] + 1,
                       DR: M[DR] + 1}
                if design == 'on_drain':
                    upd[CRED] = M[CRED] + 1
                out.append((upd, {'act': 1, 'l': M[LAND(i)] + 1}, f'drain pe{i}'))
        return out

    B = [('T', ('br', lambda L, n=nreq: L['dr'] is not None and L['dr'] >= n,
                '@TEND', '@STEP')),
         ('STEP', ('nd', alts, '@DISPATCH')),
         ('DISPATCH', ('br', lambda L: L['act'] == 0, '@CDESC', '@CLAND'))]

    # DESCRIPTORS: the per-connection bound the origin's credits keep. Checked on
    # its own so a breakage violating both bounds is not reported as violating
    # only whichever the explorer reaches first.
    if bound == 'descriptors':
        B += [('CDESC', ('check', lambda M, L, C=C:
                         None if L['n'] <= C else
                         f'DESCRIPTORS: {L["n"]} reply descriptors alive on the connection, '
                         f'credit is {C}',
                         '@ADV'))]
    else:
        B += [('CDESC', ('br', lambda L: True, '@ADV', '@ADV'))]

    # LANDING: a PE's replies in flight must fit its own landing zone (§3.12),
    # which is the bound at the *other* end of the connection - nothing at the
    # target can see it.
    if bound == 'landing':
        B += [('CLAND', ('check', lambda M, L, C=C:
                         None if L['l'] <= C else
                         f'LANDING: {L["l"]} replies in flight toward one PE, '
                         f'its landing zone holds {C}',
                         '@ADV'))]
    else:
        B += [('CLAND', ('br', lambda L: True, '@ADV', '@ADV'))]

    B += [('ADV', ('gpost', lambda M, L: ({}, {'dr': M[DR], 'n': DEL, 'l': DEL,
                                              'act': DEL}), '@T')),
          ('TEND', ('done',))]
    return assemble(B)


def case(design, npes, nops, C, bound, nbi=False):
    """npes PEs of one node, nops operations each, over one connection."""
    threads = [origin(design, i, nops, C, nbi) for i in range(npes)]
    if nbi:
        threads += [lander(design, i, nops, C) for i in range(npes)]
    threads.append(target(design, npes, npes * nops, C, bound))
    mem = {CRED: C, ACC: 0, DR: 0}
    for i in range(npes):
        mem[SENT(i)] = mem[INUSE(i)] = mem[LAND(i)] = 0
    # Not symmetric: the target's alternatives name PEs by index.
    tag = ' nbi' if nbi else ''
    return (f'credit/{design}{tag} pes={npes} ops={nops} C={C} [{bound}]', threads, mem, ())


# (design, pes, ops, C, bound, nbi, expect): True, or the property the breakage must
# fail. Each bound is checked on its own, so a breakage that violates both is not
# reported as violating only the one the explorer happened to reach first.
#
# Two results here are the reason the model was worth writing, and neither was
# the prediction it started from.
#
# 1. Every breakage passes at pes=1, and that is a result rather than a gap. The
#    credit is per *connection* and shared by the node's PEs, so only a node with
#    more than one PE can outrun it - which is every real node. A model run at
#    pes=1 would have called all four breakages correct.
#
# 2. The two bounds separate, and not where expected. The descriptor bound at the
#    target is broken by race_take, on_apply and none. The landing bound at the
#    origin cannot be broken at all while gets are blocking: a PE waits for its
#    own reply before issuing again (§4.6), so it never has two in flight
#    whatever the credit rule says. on_drain - returning the credit when the
#    reply goes on the wire rather than when it lands - is therefore
#    indistinguishable from the design in this slice, and only the `nbi` cases,
#    where a PE issues every request before landing any, show why §3.8 says
#    "lands". Recorded so that nobody simplifies it later on the evidence of the
#    blocking cases alone.
CASES = []
for _b in ('descriptors', 'landing'):
    CASES += [
        ('design', 1, 2, 1, _b, False, True), ('design', 2, 1, 1, _b, False, True),
        ('design', 2, 2, 1, _b, False, True), ('design', 2, 2, 2, _b, False, True),
        ('race_take', 1, 2, 1, _b, False, True),
        ('race_take', 2, 2, 1, _b, False, 'CREDITS'),
        ('on_apply', 1, 2, 1, _b, False, True),
        ('on_apply', 2, 2, 1, _b, False, 'DESCRIPTORS' if _b == 'descriptors' else True),
        # Blocking gets cannot show on_drain's defect; see note 2 above.
        ('on_drain', 1, 2, 1, _b, False, True),
        ('on_drain', 2, 2, 1, _b, False, True),
        ('none', 1, 2, 1, _b, False, True),
        ('none', 2, 2, 1, _b, False, 'DESCRIPTORS' if _b == 'descriptors' else True),
        # Nonblocking issue: one PE, several requests outstanding at once. This
        # is out of scope for this slice and is modelled because it is the only
        # configuration in which the landing bound binds.
        ('design', 1, 2, 1, _b, True, True), ('design', 1, 3, 2, _b, True, True),
        ('on_drain', 1, 2, 1, _b, True, 'LANDING' if _b == 'landing' else True),
        # on_apply returns the credit earliest of all and breaks both bounds.
        ('on_apply', 1, 2, 1, _b, True, 'DESCRIPTORS' if _b == 'descriptors' else 'LANDING'),
    ]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, npes, nops, C, bound, nbi, expect in CASES:
        name, threads, mem0, sym = case(design, npes, nops, C, bound, nbi)
        if not keep(name):
            continue
        res = explore(name, threads, mem0, symmetric=sym, max_seconds=120)
        ok &= report(res, expect=expect)
    sys.exit(0 if ok else 1)
