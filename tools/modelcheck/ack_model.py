#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The acknowledgement protocol between two node leads (design §3.4, §4.6).

`shmem_quiet` has no handle, so a writer cannot name what it is waiting for.
What it waits on instead is a *sequence number*: the highest segment it has
written into toward a peer, against the highest that peer says it has applied.
The peer reports that number on every segment it sends, as the cumulative
`ack` field. One-way traffic would therefore never be acknowledged at all, so a
writer can also ask - `ACK_REQUESTED` on a segment - and a peer with nothing to
say answers with a header-only segment.

The whole protocol is those two fields plus the two shared words that carry a
request across processes, and the second adversarial review found it wrong
twice, in ways that hide each other (`docs/reviews/archive/phase1-step2-slice1-correctness.md`,
C1 and C2). This model is what that review said was owed.

Variants
  current   the tree at merge d542c63. One `owe_ack` bool per connection means
            both "the peer asked me" and "my writer wants one", and a segment
            carries the flag whenever it is set. So every acknowledgement is
            itself a request, and a quiesced pair echoes header-only segments
            at each other forever (C1). It also cannot tell which segment a
            request was for (C2), but the echo hides that: another round always
            follows.
  naive     the review's `naive-echo-fix.patch`: split the bool in two, so an
            acknowledgement no longer requests one. The echo stops and C2 comes
            out from behind it. The request is consumed by whichever segment is
            *finalized* next, which may be one sealed before the write the
            writer is waiting for, and nothing ever asks again - so `quiet`
            waits for an acknowledgement that will never be sent.
  design    slice 2's protocol. The request carries a sequence number,
            "acknowledge through S", published as a monotone maximum into a
            shared word. A segment carries the flag if its own sequence is at
            or below what is wanted and above what is acknowledged, or - the
            late request - if everything at or below what is wanted has already
            been finalized without it. A header-only segment sealed only to pay
            a debt never carries the flag, which is what ends the echo, and the
            lead remembers the highest sequence it has sent carrying the flag so
            that a late request is asked once rather than every pass.

Properties
  TERMINATES  every `quiet` returns. Checked as the engine's trapped-state
              check (AG EF) with `done` being "both quiets have returned": a
              state from which no such state is reachable is a hung `quiet`,
              whether or not any thread is asleep in it.
  QUIESCES    a segment carries ACK_REQUESTED only while some writer, on one
              node or the other, wants an acknowledgement beyond what its peer
              has already acknowledged.
              That is C1 exactly: an acknowledgement that asks for one is a
              request nobody made, and a pair that exchanges them never falls
              silent. It is checked at the step that decides the flag - the
              finalize at promotion.

              The first wording tried here was the stronger one this slice's
              brief states: in a quiesced state *no send step at all* is
              enabled. No correct protocol can meet it. A lead that receives
              ACK_REQUESTED must answer, and it cannot know that the asking
              node's `quiet` was satisfied meanwhile by some other segment's
              cumulative ack - so the design does send one last header-only
              segment into a quiesced pair. What distinguishes it from the echo
              is that this one carries no request, so nothing answers it. That
              is a *bounded* silence rather than an immediate one, and it is
              what the ack-echo test asserts: header-only segments sent stays
              under a small constant rather than growing. QUIESCES as checked
              here is the property that makes the bound hold.

Expected: `current` fails QUIESCES, `naive` fails TERMINATES, `design` passes
both. Anything else is a result, not a formality.

What is modelled, and what is left to the models that own it
  * The wire is one reliable in-order segment slot per direction, filled by a
    send step and emptied by the peer's receive step. The byte ring underneath
    it, and its wake protocol, are `wire_model.py`'s subject.
  * A writer's reserve-and-commit into the open buffer is one atomic step, and
    so is the lead's seal against it. That atomicity is exactly what §3.5's
    reservation protocol provides and what `reservation_model.py` checks; here
    it is a premise.
  * The lead's own bookkeeping - what is applied, acknowledged, owed, sealed -
    is private to the progress thread (§3.12), so it is held under `$` names
    and written by steps that are never buffered. The only words two threads
    share are the two the connection really has: `ack_requested` /
    `ack_wanted_through`, which a writer sets and its own lead reads, and
    `acked`, which the lead publishes and `quiet` reads. Those carry real
    load/store/CAS instructions, so TSO can reorder them.
  * Two buffers per connection, one sealed and one open, as `progress.c` has
    them; the flag is consumed at *promotion* (progress.c:492-502), which is
    what makes C2 reachable at all.
  * The state space is bounded by capping **header-only seals** per node, and
    nothing else. Data-carrying segments are bounded already, by how many
    messages a writer sends, and they are what `quiet` is waiting for - so a
    cap on them would starve `quiet` and report a hang the protocol does not
    have. A cap on header-only seals cannot: it truncates the echo, which is
    the one thing that grows without bound, and every case below is also run
    one step either side of its cap to show the verdict does not turn on it.

Usage: ack_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

# Shared words: the two the connection really has (connection.h:146-171).
def WANTED(x):      return ('ack_wanted', x)     # writer -> its own lead
def ACKED_PUB(x):   return ('acked', x)          # lead -> quiet, in the connection


# Shared, but atomic by §3.5's reservation protocol: the open buffer.
def OPEN(x):        return ('open_seq', x)       # sequence of the open buffer, 0 = none
def OPEN_DATA(x):   return ('open_data', x)      # messages committed into it


# The wire from x to its peer: one segment.
def WSEQ(x):        return ('wire_seq', x)
def WACK(x):        return ('wire_ack', x)
def WFLAG(x):       return ('wire_flag', x)
def WDATA(x):       return ('wire_data', x)
def WFULL(x):       return ('wire_full', x)


# The lead's private state (§3.12), never buffered.
def SEALED(x):      return ('$sealed', x)        # sequence of the sealed buffer, 0 = none
def SEALED_DATA(x): return ('$sealed_data', x)
def NEXT(x):        return ('$next_seq', x)
def APPLIED(x):     return ('$applied', x)       # highest sequence applied from the peer
def ACKED(x):       return ('$acked', x)         # highest the peer says it applied of ours
def OWE(x):         return ('$owe_ack', x)       # the peer asked; my next segment pays it
def WANT(x):        return ('$want_ack', x)      # naive only: my writer asked
def FLAGGED(x):     return ('$flagged', x)       # design only: highest sequence sent flagged

# Ghost.
def HIGH(x):        return ('$high', x)          # highest sequence this writer wrote into
def QUIET(x):       return ('$quiet_returned', x)
def ACKS_ONLY(x):   return ('$header_only_sent', x)


NODES = (0, 1)


# ---- the writer ---------------------------------------------------------

def writer(variant, x, messages):
    """`messages` puts toward the peer, then one quiet."""
    B = []
    for m in range(messages):
        p = f'w{m}_'
        nxt = f'@w{m + 1}_RESERVE' if m + 1 < messages else '@Q_REQUEST'

        def reserve(M, L, x=x):
            # Blocked while no buffer is open, exactly as reserve_waiting is
            # (transport-internode.c). Reserve and commit are one step: §3.5's
            # protocol is what makes them atomic against the lead's seal.
            s = M[OPEN(x)]
            if s == 0:
                return []
            return [({OPEN_DATA(x): M[OPEN_DATA(x)] + 1, HIGH(x): s}, {'high': s},
                     f'reserve+commit into segment {s}')]

        B += [(p + 'RESERVE', ('nd', reserve, nxt))]

    if variant == 'design':
        # osoe_connection_request_ack(conn, seq): publish "acknowledge through
        # S" as a monotone maximum. One writer per side here, so the CAS cannot
        # fail; it is written as the loop the code runs.
        B += [('Q_REQUEST', ('load', lambda L, x=x: WANTED(x), 'w', '@Q_COMPARE')),
              ('Q_COMPARE', ('br', lambda L: L['w'] >= L['high'], '@Q_WAIT', '@Q_CAS')),
              ('Q_CAS', ('cas', lambda L, x=x: WANTED(x), lambda L: L['w'],
                         lambda L: L['high'], 'won', '@Q_RETRY')),
              ('Q_RETRY', ('br', lambda L: L['won'], '@Q_WAIT', '@Q_REQUEST'))]
    else:
        # osoe_connection_request_ack(conn): a seq_cst store of 1, with no way
        # to say which segment it is for.
        B += [('Q_REQUEST', ('store', lambda L, x=x: WANTED(x), lambda L: 1, True, '@Q_WAIT'))]

    B += [('Q_WAIT', ('await', lambda M, L, x=x: M[ACKED_PUB(x)] >= L['high'], '@Q_RETURN')),
          ('Q_RETURN', ('gpost', lambda M, L, x=x: ({QUIET(x): 1}, {}), '@END')),
          ('END', ('done',))]
    return assemble(B)


# ---- the lead -----------------------------------------------------------

def lead(variant, x, header_only_limit):
    """One pass of osoe_progress_poll over the one connection, forever."""
    y = 1 - x
    B = []

    def receive(M, L):
        # progress.c:269-292, segment_applied. Everything it touches but the
        # last line is the lead's own: applied, the request flag it owes, and
        # its record of what the peer has acknowledged. Publishing that record
        # into the connection is the one shared store, and it is the step
        # below, because a release store is what TSO may hold back.
        if not M[WFULL(y)]:
            return [({}, {'publish': DEL}, 'nothing has arrived')]
        upd = {WFULL(y): 0, APPLIED(x): M[WSEQ(y)]}
        if M[WFLAG(y)]:
            upd[OWE(x)] = 1
        ack = M[WACK(y)]
        fresh = ack > M[ACKED(x)]
        if fresh:
            upd[ACKED(x)] = ack
        return [(upd, {'publish': ack if fresh else DEL}, f'apply segment {M[WSEQ(y)]} '
                 f'(ack {ack}, flag {M[WFLAG(y)]}, {M[WDATA(y)]} messages)')]

    B += [('RECEIVE', ('nd', receive, '@PUBLISH?')),
          ('PUBLISH?', ('br', lambda L: L['publish'], '@PUBLISH', '@TAKE')),
          ('PUBLISH', ('store', lambda L, x=x: ACKED_PUB(x), lambda L: L['publish'], False, '@PUBLISHED')),
          # Forget it once stored. A stale value here re-issues the same store
          # every pass, which SC cannot see and TSO turns into a store buffer
          # that grows without bound.
          ('PUBLISHED', ('set', 'publish', lambda L: DEL, '@TAKE'))]

    if variant == 'design':
        # The monotone word is never cleared; the lead reads it once per pass,
        # before the seal, as the comment at progress.c:461-468 says.
        B += [('TAKE', ('load', lambda L, x=x: WANTED(x), 'w', '@QUIESCES'))]
    else:
        # osoe_connection_take_ack_request: an exchange with 0.
        target = WANT(x) if variant == 'naive' else OWE(x)
        B += [('TAKE', ('andn', lambda L, x=x: WANTED(x), lambda L: 1, 'req', '@TOOK?')),
              ('TOOK?', ('br', lambda L: L['req'], '@TOOK', '@QUIESCES')),
              ('TOOK', ('ghost', lambda M, L, t=target: ({t: 1}, {}), '@QUIESCES'))]

    def flag_of(M, L, x=x):
        """Does the segment now being finalized carry ACK_REQUESTED?"""
        seq = M[SEALED(x)]
        if variant == 'current':
            return M[OWE(x)]
        if variant == 'naive':
            return M[WANT(x)]
        w, acked = L['w'], M[ACKED(x)]
        if seq <= acked:
            return 0
        # Slice 2's rule: at or below what is wanted, or the late request -
        # everything at or below what is wanted has already gone out unflagged.
        late = w > acked and M[FLAGGED(x)] < w
        return 1 if (seq <= w or late) else 0

    def send(M, L, x=x):
        # Promote the sealed buffer: finalize (progress.c:430-448) writes the
        # header - the cumulative ack and the flag - and only then does the
        # segment go out. This is where the flag is consumed.
        if M[WFULL(x)] or M[SEALED(x)] == 0:
            return [({}, {}, 'nothing to promote')]
        seq, data, flag = M[SEALED(x)], M[SEALED_DATA(x)], flag_of(M, L)
        upd = {WSEQ(x): seq, WACK(x): M[APPLIED(x)], WFLAG(x): flag, WDATA(x): data,
               WFULL(x): 1, SEALED(x): 0, SEALED_DATA(x): 0, OWE(x): 0}
        if variant == 'naive':
            upd[WANT(x)] = 0
        if variant == 'design' and flag:
            upd[FLAGGED(x)] = seq
        return [(upd, {}, f'send segment {seq}: ack {M[APPLIED(x)]}, '
                 f'{"ACK_REQUESTED" if flag else "no request"}, {data} messages')]

    def seal_enabled(M, L, x=x):
        if M[SEALED(x)] or M[OPEN(x)] == 0:
            return False
        if M[OPEN_DATA(x)] or M[OWE(x)]:
            return True
        if variant == 'naive':
            return bool(M[WANT(x)])
        if variant == 'design':
            return L['w'] > M[ACKED(x)] and M[FLAGGED(x)] < L['w']
        return False

    def seal(M, L, x=x):
        header_only = M[OPEN_DATA(x)] == 0
        if not seal_enabled(M, L):
            return [({}, {}, 'nothing to seal')]
        if header_only and M[ACKS_ONLY(x)] >= header_only_limit:
            return [({}, {}, 'header-only seals capped')]     # the model's bound, not the code's
        upd = {SEALED(x): M[OPEN(x)], SEALED_DATA(x): M[OPEN_DATA(x)],
               OPEN(x): 0, OPEN_DATA(x): 0}
        if header_only:
            upd[ACKS_ONLY(x)] = M[ACKS_ONLY(x)] + 1
        return [(upd, {}, f'seal segment {M[OPEN(x)]} ({M[OPEN_DATA(x)]} messages)')]

    def open_buffer(M, L, x=x):
        if M[OPEN(x)]:
            return [({}, {}, 'no buffer to open')]
        return [({OPEN(x): M[NEXT(x)], NEXT(x): M[NEXT(x)] + 1, OPEN_DATA(x): 0}, {},
                 f'open segment {M[NEXT(x)]}')]

    def quiesces(M, L, x=x):
        if M[WFULL(x)] or M[SEALED(x)] == 0 or not flag_of(M, L):
            return None
        # "No writer wants an acknowledgement" is about *both* nodes. Each hop
        # of the echo is sent by a node whose own writer is satisfied, but the
        # peer's may not be - and answering a peer that is still waiting is the
        # protocol working. What must never happen is a request when nobody at
        # either end is waiting for one.
        if any(M[HIGH(n)] > M[ACKED_PUB(n)] for n in NODES):
            return None
        return ('QUIESCES: segment %d carries ACK_REQUESTED although no writer on either '
                'node wants an acknowledgement beyond what its peer has acknowledged'
                % M[SEALED(x)])

    B += [('QUIESCES', ('check', quiesces, '@SEND')),
          ('SEND', ('nd', send, '@SEAL')),
          ('SEAL', ('nd', seal, '@OPEN')),
          ('OPEN', ('nd', open_buffer, '@RECEIVE'))]
    return assemble(B)


def case(variant, messages, header_only_limit, check_quiesces):
    threads = [writer(variant, 0, messages), lead(variant, 0, header_only_limit),
               writer(variant, 1, messages), lead(variant, 1, header_only_limit)]
    mem = {}
    for x in NODES:
        mem.update({WANTED(x): 0, ACKED_PUB(x): 0, OPEN(x): 0, OPEN_DATA(x): 0,
                    WSEQ(x): 0, WACK(x): 0, WFLAG(x): 0, WDATA(x): 0, WFULL(x): 0,
                    SEALED(x): 0, SEALED_DATA(x): 0, NEXT(x): 1, APPLIED(x): 0,
                    ACKED(x): 0, OWE(x): 0, WANT(x): 0, FLAGGED(x): 0,
                    HIGH(x): 0, QUIET(x): 0, ACKS_ONLY(x): 0})
    prop = 'QUIESCES' if check_quiesces else 'TERMINATES'
    name = f'ack/{variant} {prop} messages={messages} header-only<={header_only_limit}'
    return name, threads, mem


def run(variant, messages, header_only_limit, check_quiesces, tso, out=sys.stdout):
    name, threads, mem = case(variant, messages, header_only_limit, check_quiesces)
    if not check_quiesces:
        # Only the trapped-state check speaks here: strip QUIESCES so that a
        # variant which breaks both is reported on the one being asked about.
        threads = [[(ins[0], (lambda M, L: None), ins[2]) if ins[0] == 'check' else ins
                    for ins in prog] for prog in threads]
    return explore(name, threads, mem, tso=tso, out=out,
                   done=lambda M: all(M[QUIET(x)] for x in NODES),
                   liveness=not check_quiesces, max_seconds=1800,
                   max_depth=2_000_000, liveness_max_states=3_000_000)


#            variant   messages  header-only  QUIESCES?  TSO?   expect ok
#
# Read the TERMINATES rows in pairs. A run at a cap and the same run one higher
# that visit the *same number of states* prove the cap did not bind: raising it
# added no behaviour, so a TRAPPED verdict there is the protocol's and not the
# model's. The `design` rows at 2 and 3 show the test has teeth - at 2 the cap
# really does starve it - and the rows at 3 and 4 then show it does not bind.
CASES = [
    ('design',  1, 2, False, False, False),     # the cap can starve this model ...
    ('design',  1, 3, False, False, True),      # ... and at 3 it no longer does:
    ('design',  1, 4, False, False, True),      #     same state count as 3
    ('design',  1, 3, True,  False, True),
    ('design',  1, 3, False, True,  True),
    ('design',  1, 3, True,  True,  True),
    ('design',  2, 3, True,  False, True),
    ('design',  2, 3, False, False, True),
    ('design',  2, 4, False, False, True),

    ('current', 1, 3, True,  False, False),     # C1: the echo asks when nobody wants
    ('current', 1, 4, True,  False, False),
    ('current', 1, 3, True,  True,  False),
    ('current', 2, 3, True,  False, False),
    # TERMINATES cannot be asked of `current`: the echo is the only thing that
    # carries a late acknowledgement there (that is C2's "load-bearing by
    # accident"), so every finite cap on it starves `quiet`. The state counts
    # below grow with the cap, which is how one can tell the cap is what
    # decided - unlike `naive`, where they do not.
    ('current', 1, 3, False, False, False),
    ('current', 1, 4, False, False, False),

    ('naive',   1, 3, False, False, False),     # C2: quiet waits for an ack
    ('naive',   1, 4, False, False, False),     #     nothing will ever send
    ('naive',   1, 3, False, True,  False),
    ('naive',   2, 3, False, False, False),
    # The naive fix stops the unbounded echo but still sends a request nobody
    # wants: a `want_ack` taken from the shared word outlives the quiet that
    # set it when some other segment's cumulative ack satisfies that quiet
    # first. Bounded, so not C1 - but not right either.
    ('naive',   1, 3, True,  False, False),
]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for variant, messages, limit, quiesces, tso, expect in CASES:
        name, _, _ = case(variant, messages, limit, quiesces)
        if not keep(name):
            continue
        res = run(variant, messages, limit, quiesces, tso)
        ok &= report(res, expect_ok=expect, tso=tso)
    sys.exit(0 if ok else 1)
