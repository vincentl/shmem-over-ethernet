#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A team's node block: claimed from the pool, held, given back (step D; proposal §3, D3-D5).

A team's state on a node is a block of the node's pool, found by the team's
*name* on that node - (local id of its local lead, number) - through the map,
which holds block index + 1 under the name, 0 for none. The split's local lead
claims a free block from the pool's bitmap by compare-and-swap, writes it - a
raised generation, the name, the count of the team's local members as its
holders, the counters, the record - and publishes the map entry with release,
all before the split's sync. Members find the block through the map after the
sync and keep it. At destroy, after the sync over the team, each member leaves:
the team's local lead first clears the map entry under its name - the name is
its own, and nothing resolves it once any member here has left the team: no
message for the team arrives at the node after that - and then every member
brings holders down by one, and the one that brings it to zero retracts the
record, raises the generation and returns the block, in that order. So the
lead's name is free as soon as the lead has left, whoever holds the block
still, and the lead takes the number again at once (D4, as built: the first
design had the last holder clear the entry, and a lead whose members were slow
to leave found its own name taken).

The progress engine keys its state by block index. An arrival handoff carries
the generation beside the epoch, and a message from another node carries the
name, which resolves through the map; the engine learns a block's team afresh -
record and generation - whenever a handoff or a resolved block shows a
generation other than the one it learnt (D4, D5). A message for a team can
arrive only while the team lives: the destroy's sync completes only after every
message for the team has arrived (proposal §3, "A late message"), which the
model takes as given of the ARRIVEs. The RELEASEs the other way are not taken
as given (T16; the step D review, G1). The engine that closes the destroy's
epoch queues a RELEASE for the child node on a control queue (`forward`,
progress.c), from which it leaves only when the control wire has room
(`seal_control`: a slow or stopped peer keeps it there as long as it likes), and
lists the node's own release, which lets the members go (`release_down`). The
local release (`release_local`) is applied only once no RELEASE of the team is
queued - the hold. Without it the members leave, the block goes back to the
pool and is claimed by another team while the old team's RELEASE is still
queued, and the engine, learning the block afresh, ends the job (`known_team`).

A team of one PE never enters a block's life (P7, node layout 42): it claims,
names, maps and leaves nothing, its member finds nothing after the split's sync
and its epoch word names no block - nothing of the node's for these threads to
meet - so the teams here are of more than one PE. pool_dry_model.py's `one` has
the claim it skips.

Checked
  REWRITE  no block is written for a team while a member of another still holds
           it.
  OWN      every member's view of the block's counters is its own team's,
           straggler reads after the destroy's sync included.
  ENGINE   the engine never applies an arrival or a message of one generation
           to state learnt for another.
  NAMED    a message for a live team resolves, through the map, to that team's
           block.
  FREE     the lead finds its name unmapped once it has left its team, so that
           its number is in its free mask at its next split.
  FOUND    the member finds A's block mapped under A's name once the split's
           sync has returned.
  QUEUED   no block is learned again, or reused, while a RELEASE of its earlier
           use is queued: a block claimed again reaches the engine - its new team
           posts an arrival and receives a message - at another generation than
           the one the engine learnt, and the engine must not find the old team's
           RELEASEs still counted (`known_team`'s fatal: "holds another team ...
           RELEASEs are queued ... they left before its forwards"). Checked where
           the fatal is, at the relearn, which is where the engine would overwrite
           a team it still counts forwards of: a block claimed again while the old
           RELEASE is queued, and met by the engine only once it has left, costs
           nothing and is not flagged. The fatal also names an epoch still open
           and a release unapplied; the destroy's sync, which completes only once
           the release is applied, rules both out, and the model leaves them to it.
  and, by the engine: no stuck thread, and from every reachable state the run can
  still finish (AG EF done).

Threads (one node; a pool of two blocks)
  lead     local id 0: creates team A under name 0 with the member, posts an
           arrival, destroys A and leaves; then creates team B under the same
           name, alone on this node, and posts an arrival.
  member   local id 1: A's other member - finds A's block after the split's sync,
           reads the counters, reads them once more after the destroy's sync (a
           straggler), and leaves last or first.
  other    local id 2: creates team C under name 1, alone on this node, whenever
           a block is free, and posts an arrival.
  remote   another node's lead: one message for A while it lives, then one for C
           and one for B once each lives.
  engine   this node's progress thread: takes posted arrivals and messages in any
           order. The second of team A's two inputs - its members' arrival and
           the message of the child node, which the remote stands for - closes
           the epoch: the engine queues A's RELEASE for the child (one child; the
           RELEASEs of a team with k children are k) and lists the local release
           (progress.c, book_arrival and release_down, one step). A queued RELEASE
           leaves at any time (seal_control, which only the wire's room delays),
           and the local release is applied at any time once none is queued
           (release_local, which a pass runs at its start and its end; the
           model's other steps stand for the rest of the pass). B's and C's
           epochs close too in the library and queue theirs; nobody destroys
           them here, so nothing waits on their releases and they are left out.

Designs
  design           as above.
Breakages
  no_holders       no holders count: the lead gives the block back as it leaves,
                   whoever still holds it (REWRITE, OWN).
  no_relearn       the engine keeps the team it first learnt under a block index
                   (ENGINE).
  map_early        every member clears the map entry as it leaves, not the lead
                   alone: the lead takes the name again, and the slow member then
                   clears the new team's entry (NAMED).
  last_clears      the first design: the last holder clears the entry, with the
                   rest of the give-back, and the lead none - a lead whose member
                   is slow to leave finds its own name mapped (FREE).
  free_first       the last holder returns the block before raising its
                   generation: another team claims and writes it in between, and
                   the raise lands on that team's block (NAMED, OWN).
  no_hold          the local release is applied whatever the control queue holds -
                   the first build, `release_local` without its `releases_queued`
                   test: the members leave, the block is claimed again, and the
                   engine learns it afresh with the old RELEASE still queued
                   (QUEUED).

Memory orders (T29; the step D review's portability Q4). Every case runs under
SC and under x86-TSO, the stores as the library makes them (§3.16): a store
the library makes `release` is buffered (`serial=False`); a locked
instruction, a syscall or a `seq_cst` store drains the buffer. The map
entry's release and the members' acquire are the record's argument in node.h,
which the sync orders. The orders are node.c's, team.c's and internode-am.c's:
  claim        osoe_node_team_claim (node.c): a seq_cst compare-and-swap on the
               bitmap's word
  write_block  osoe_node_team_name (node.c): the generation, the counters
               (osoe_epoch_init, base/epoch.c, and collective_state_reset, node.c)
               and the holders; write_record (team.c): the record's `published`;
               finish (team.c): the map entry. All release stores, so buffered
  sync         sync_team (barrier.c): each member's arrival a seq_cst add
               (osoe_epoch_bump_eq), the model's `add` of the sync's word; and the
               wait for the release, osoe_epoch_wait_eq_chained, an acquire load of
               the block's `released`, which a later load of the member's cannot
               pass: the model's await, then its loads
  release      osoe_internode_released (barrier.c), on the engine: the block's
               `released` raised by osoe_epoch_release_bump_eq (base/epoch.c), a
               stamp stored with release and then a seq_cst add, a locked
               instruction that drains the engine's buffer - the stamp and
               publish_team's stores, for osoe-stat, are read by no thread here -
               so the raise is made at once, and it is the one word of the
               engine's a member reads. The queue and its count (`releases_queued`,
               progress.c: `forward` and `release_left_queue`) and the release
               listed (`release_pending`) are the engine's own plain variables,
               which no other thread reads: ghosts
  post         osoe_internode_arrive (internode-am.c): the handoff word, a release
               store, then the posted bitmap's two ors and the wake word's add
  engine       loads, all acquire: take_arrival's of the handoff (internode-am.c),
               team_by_name's of the map entry (osoe_node_team_find, node.h) and of
               the generation (progress-host.c)
  leave        osoe_node_team_leave (node.c): the lead's clear of the map entry
               and the last holder's retraction of the record are release stores,
               buffered; the holders' decrement, the generation's raise and the
               bit's or are seq_cst read-modify-writes, which drain them
  fence        under TSO only, a locked or of nothing where the library has a
               locked instruction the model leaves abstract: after the map entry,
               the split's sync arrival that follows finish in split (team.c),
               before which no message for the team can arrive, and after the
               handoff, the posted bitmap's ors and the wake word's add
               (posted_bitmap_model.py). Under SC it is no step.

Usage: team_block_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import DEL, assemble, explore, report, select

BLOCKS = 2
FREE = ('free',)                      # bit b set: block b is free
MAP = lambda n: ('map', n)            # name -> block index + 1, 0 for none
GEN = lambda b: ('gen', b)            # the block's generation
HOLDERS = lambda b: ('holders', b)
RECORD = lambda b: ('record', b)      # 1 while a team's record is published
COUNTERS = lambda b: ('counters', b)  # whose counters: the generation that wrote them
HANDOFF = lambda b: ('handoff', b)    # generation * 16 + posted epoch
SYNC = lambda k: ('sync', k)          # the arrivals at a sync over A's two members
HOLDER = lambda b, pe: ('$holder', b, pe)
LIVE = lambda n: ('$live', n)         # the team live under name n: 0, or A=1, B=2, C=3
TEAM_GEN = lambda t: ('$team_gen', t)
TEAM_BLOCK = lambda t: ('$team_block', t)
MESSAGE = ('$message',)               # a message in flight: name * 8 + team, or 0
A_TAKEN = ('$a_taken',)               # A's arrival and A's message, taken by the engine
QUEUED = lambda b: ('$queued', b)     # RELEASEs of the block's team in the control queue
PENDING = lambda b: ('$pending', b)   # 1 while the block's team's local release is unapplied
DONE = ('$done',)

RELEASED = ('released',)              # A's `released`: the engine raises it, the members wait on it

FENCE = ('fence',)                    # only the fences touch it, and it never changes

A, B, C = 1, 2, 3
TEAM = {A: 'A', B: 'B', C: 'C'}


def fence(p, then, tso):
    """Under TSO, a locked or of nothing: the seq_cst read-modify-write the
    library makes at this point, which drains the store buffer on x86. Under SC
    nothing: a local step that is no step."""
    if tso:
        return [(p, ('or', lambda L: FENCE, lambda L: 0, '_', then))]
    return [(p, ('br', lambda L: True, then, then))]


def lowest(w):
    for b in range(BLOCKS):
        if w & (1 << b):
            return b
    return None


def claim(p, then):
    """Take a free block by compare-and-swap over the bitmap (osoe_node_team_claim:
    seq_cst); wait while none is free (a dry pool fails the split in the library;
    here it only waits)."""
    return [(p + 'c', ('await', lambda M, L: M[FREE] != 0, '@' + p + 'l')),
            (p + 'l', ('load', lambda L: FREE, 'w', '@' + p + 'z')),
            (p + 'z', ('br', lambda L: L['w'] == 0, '@' + p + 'c', '@' + p + 'p')),
            (p + 'p', ('set', 'b', lambda L: lowest(L['w']), '@' + p + 'x')),
            (p + 'x', ('cas', lambda L: FREE, lambda L: L['w'], lambda L: L['w'] & ~(1 << L['b']),
                       'ok', '@' + p + 'k')),
            (p + 'k', ('br', lambda L: L['ok'], then, '@' + p + 'l'))]


def write_block(p, name, team, members, then, tso):
    """The split's local lead writes its block: no holder of another team may
    remain (REWRITE); then the generation, the counters, the holders, the record,
    and last the map entry - with release, before the sync. Each is a release
    store (osoe_node_team_name, write_record, finish), buffered under TSO; the
    sync arrival after the map entry drains them, and the team is live (LIVE) only
    from there: no message for it can come before the split's sync completes."""
    def check(M, L):
        held = [pe for pe in range(3) if M[HOLDER(L['b'], pe)]]
        return None if not held else \
            f'REWRITE: block {L["b"]} written for team {TEAM[team]} while PE {held[0]} holds it'
    return [(p + 'h', ('check', check, '@' + p + 'g')),
            (p + 'g', ('load', lambda L: GEN(L['b']), 'g', '@' + p + 'G')),
            (p + 'G', ('store', lambda L: GEN(L['b']), lambda L: L['g'] + 1, False,
                       '@' + p + 'C')),
            (p + 'C', ('store', lambda L: COUNTERS(L['b']), lambda L: L['g'] + 1, False,
                       '@' + p + 'H')),
            (p + 'H', ('store', lambda L: HOLDERS(L['b']), lambda L, k=len(members): k, False,
                       '@' + p + 'Hg')),
            (p + 'Hg', ('gpost', lambda M, L, ms=members: ({HOLDER(L['b'], pe): 1 for pe in ms}, {}),
                        '@' + p + 'R')),
            (p + 'R', ('store', lambda L: RECORD(L['b']), lambda L: 1, False, '@' + p + 'M')),
            (p + 'M', ('store', lambda L: MAP(name), lambda L: L['b'] + 1, False,
                       '@' + p + 'F'))] + fence(p + 'F', '@' + p + 'Mg', tso) + [
            (p + 'Mg', ('gpost', lambda M, L, n=name, t=team:
                        ({LIVE(n): t, TEAM_GEN(t): L['g'] + 1, TEAM_BLOCK(t): L['b']},
                         {'g': L['g'] + 1}), then))]


def post(p, then, tso):
    """The member that arrives last posts the team's arrival: the generation it
    holds beside epoch 1, a single-writer release store (osoe_internode_arrive),
    then the posted bitmap's ors and the wake word's add."""
    return [(p + 'a', ('store', lambda L: HANDOFF(L['b']), lambda L: L['g'] * 16 + 1, False,
                       '@' + p + 'f'))] + fence(p + 'f', then, tso)


def read_counters(p, team, then):
    return [(p + 'r', ('load', lambda L: COUNTERS(L['b']), 'c', '@' + p + 'rc')),
            (p + 'rc', ('check', lambda M, L, t=team: None if L['c'] == L['g'] else
                        f'OWN: a member of team {TEAM[t]} read block {L["b"]}\'s counters of '
                        f'generation {L["c"]}, its own being {L["g"]}', then))]


def leave(design, p, pe, name, then):
    """Forget the team: the lead (local id 0) clears the map entry under its name
    first; then holders down by one, and the last holder retracts the record,
    raises the generation and returns the block."""
    give_back = [(p + 'R', ('store', lambda L: RECORD(L['b']), lambda L: 0, False,
                            '@' + p + 'G')),
                 (p + 'G', ('add', lambda L: GEN(L['b']), lambda L: 1, '_', '@' + p + 'F')),
                 (p + 'F', ('or', lambda L: FREE, lambda L: 1 << L['b'], '_', then))]
    unheld = lambda M, L: ({HOLDER(L['b'], pe): 0}, {})
    # osoe_node_team_leave (node.c): the stores - the record's retraction, the map
    # entry's clear - are release; the decrement, the raise and the bit are seq_cst
    # read-modify-writes.
    if design == 'last_clears':
        # The first design: the map entry is the last holder's to clear, with the
        # rest of the give-back.
        give_back = [(p + 'R', ('store', lambda L: RECORD(L['b']), lambda L: 0, False,
                                '@' + p + 'G')),
                     (p + 'G', ('add', lambda L: GEN(L['b']), lambda L: 1, '_', '@' + p + 'M')),
                     (p + 'M', ('store', lambda L: MAP(name), lambda L: 0, False, '@' + p + 'F')),
                     (p + 'F', ('or', lambda L: FREE, lambda L: 1 << L['b'], '_', then))]
        return [(p + 'q', ('ghost', unheld, '@' + p + 'd')),
                (p + 'd', ('add', lambda L: HOLDERS(L['b']), lambda L: -1, 'h', '@' + p + 'z')),
                (p + 'z', ('br', lambda L: L['h'] == 0, '@' + p + 'R', then))] + give_back
    # The lead's first step: its name's entry cleared, before its decrement - so
    # before the last holder's, and before the bit.
    clear = [(p + 'e', ('store', lambda L: MAP(name), lambda L: 0, False, '@' + p + 'd'))] \
        if pe == 0 or design == 'map_early' else []
    first = '@' + p + 'e' if clear else '@' + p + 'd'
    if design == 'free_first':
        # The block back in the pool before its generation is raised: a claimer
        # can write it in between, and the raise lands on the new team's.
        give_back = [(p + 'F', ('or', lambda L: FREE, lambda L: 1 << L['b'], '_', '@' + p + 'R')),
                     (p + 'R', ('store', lambda L: RECORD(L['b']), lambda L: 0, False,
                                '@' + p + 'G')),
                     (p + 'G', ('add', lambda L: GEN(L['b']), lambda L: 1, '_', then))]
        return [(p + 'q', ('ghost', unheld, first))] + clear + [
                (p + 'd', ('add', lambda L: HOLDERS(L['b']), lambda L: -1, 'h', '@' + p + 'z')),
                (p + 'z', ('br', lambda L: L['h'] == 0, '@' + p + 'F', then))] + give_back
    if design == 'no_holders':
        # No count: the lead gives the block back as it leaves.
        if pe == 0:
            return [(p + 'q', ('ghost', unheld, '@' + p + 'e'))] + \
                [(p + 'e', ('store', lambda L: MAP(name), lambda L: 0, False, '@' + p + 'R'))] + \
                give_back
        return [(p + 'q', ('ghost', unheld, then))]
    return [(p + 'q', ('ghost', unheld, first))] + clear + [
            (p + 'd', ('add', lambda L: HOLDERS(L['b']), lambda L: -1, 'h', '@' + p + 'z')),
            (p + 'z', ('br', lambda L: L['h'] == 0, '@' + p + 'R', then))] + give_back


def sync(p, k, then, destroy=False):
    """A sync over A's two members: each arrives with a seq_cst add
    (osoe_epoch_bump_eq), which drains the store buffer; the wait for the others
    is abstract. The destroy's completes only once the engine has applied A's
    release: it has taken A's arrival and A's message, which closes the epoch
    (proposal §3, "A late message"), and - the hold - no RELEASE of A is still
    queued."""
    cond = (lambda M, L: M[SYNC(k)] == 2 and M[RELEASED] == 1) if destroy else \
           (lambda M, L: M[SYNC(k)] == 2)
    return [(p + 's', ('add', lambda L: SYNC(k), lambda L: 1, '_', '@' + p + 'w')),
            (p + 'w', ('await', cond, then))]


def finish(locals_):
    return [('E', ('gpost', lambda M, L: ({DONE: M[DONE] + 1}, {k: DEL for k in locals_}), '@END')),
            ('END', ('done',))]


def lead(design, tso):
    B_ = []
    B_ += claim('a', '@ah')
    B_ += write_block('a', 0, A, (0, 1), '@as', tso)
    B_ += sync('a', 1, '@aa')
    B_ += post('a', '@ar', tso)
    B_ += read_counters('a', A, '@ds')
    B_ += sync('d', 2, '@dl', destroy=True)
    B_ += [('dl', ('ghost', lambda M, L: ({LIVE(0): 0}, {}), '@lq'))]
    B_ += leave(design, 'l', 0, 0, '@bm')
    # Team B under the same name: the free mask leaves the number out while a
    # block is still mapped under (lead, number), and the lead finds it free as
    # soon as it has left A, whoever holds A's block still (FREE).
    B_ += [('bm', ('check', lambda M, L: None if M[MAP(0)] == 0 else
                   'FREE: the lead left team A and found its name still mapped, its number not '
                   'free for team B', '@bc'))]
    B_ += claim('b', '@bh')
    B_ += write_block('b', 0, B, (0,), '@ba', tso)
    B_ += post('b', '@br', tso)
    B_ += read_counters('b', B, '@E')
    B_ += finish(('w', 'ok', 'c', 'h', 'b', 'g'))
    return assemble(B_)


def member(design):
    B_ = []
    B_ += sync('a', 1, '@f')
    B_ += [('f', ('load', lambda L: MAP(0), 'v', '@fz')),
           ('fz', ('check', lambda M, L: None if L['v'] else
                   'FOUND: a member of team A found no block', '@fb')),
           ('fb', ('set', 'b', lambda L: L['v'] - 1, '@fg')),
           ('fg', ('load', lambda L: GEN(L['b']), 'g', '@fr'))]
    B_ += read_counters('f', A, '@ds')
    B_ += sync('d', 2, '@sr', destroy=True)
    B_ += read_counters('s', A, '@lq')                  # the straggler's read
    B_ += leave(design, 'l', 1, 0, '@E')
    B_ += finish(('v', 'c', 'h', 'b', 'g'))
    return assemble(B_)


def other(design, tso):
    B_ = []
    B_ += claim('c', '@ch')
    B_ += write_block('c', 1, C, (2,), '@ca', tso)
    B_ += post('c', '@cr', tso)
    B_ += read_counters('c', C, '@E')
    B_ += finish(('w', 'ok', 'c', 'b', 'g'))
    return assemble(B_)


def remote():
    """Another node: one message for A while it lives; then for C and for B, each
    once it lives. One message in flight at a time."""
    def send(name, team):
        return lambda M, L: ({MESSAGE: name * 8 + team}, {})
    return assemble([
        ('1', ('await', lambda M, L: M[LIVE(0)] == A and M[MESSAGE] == 0, '@1s')),
        ('1s', ('ghost', send(0, A), '@2')),
        ('2', ('await', lambda M, L: M[LIVE(1)] == C and M[MESSAGE] == 0, '@2s')),
        ('2s', ('ghost', send(1, C), '@3')),
        ('3', ('await', lambda M, L: M[LIVE(0)] == B and M[MESSAGE] == 0, '@3s')),
        ('3s', ('ghost', send(0, B), '@E')),
        ('E', ('ghost', lambda M, L: ({DONE: M[DONE] + 1}, {}), '@END')),
        ('END', ('done',))])


def engine(design):
    """Takes posted arrivals and messages, in any order, until it has taken the
    three arrivals and the three messages and has nothing queued or listed. Its
    state for a block is the generation it learnt the block's team at, and, for
    team A's block, the RELEASE it has queued and the release it has listed."""
    def events(M, L):
        alts = []
        for b in range(BLOCKS):
            h = M[HANDOFF(b)]
            if h and h != (L[f'taken{b}'] or 0):
                alts.append(({}, {'kind': 1, 'b': b, 'eg': h // 16, 'h': h}, f'arrival on block {b}'))
        if M[MESSAGE]:
            alts.append(({}, {'kind': 2, 'm': M[MESSAGE]}, 'a message'))
        for b in range(BLOCKS):
            if M[QUEUED(b)]:
                # seal_control: the RELEASE leaves the queue into the wire's segment,
                # which only the wire's room delays - so at any time.
                alts.append(({QUEUED(b): 0}, {'kind': 3},
                             f'block {b}\'s RELEASE leaves the control queue'))
            if M[PENDING(b)] and (design == 'no_hold' or not M[QUEUED(b)]):
                # release_local: the local release. The hold is its test of
                # `releases_queued`, which no_hold leaves out.
                alts.append(({PENDING(b): 0, RELEASED: 1}, {'kind': 3},
                             f'block {b}\'s local release applied'))
        if (L['n'] or 0) >= 6 and not any(M[QUEUED(b)] or M[PENDING(b)] for b in range(BLOCKS)):
            alts.append(({}, {'kind': 9}, 'nothing left to take, queued or release'))
        return alts

    def learn(M, L):
        # The record and the generation, afresh, when the event shows another
        # generation than the one learnt - or, broken, only the first time.
        b, g = L['b'], L['eg']
        known = L[f'learnt{b}']
        if known != g and (design != 'no_relearn' or not known):
            return {}, {f'learnt{b}': g}
        return {}, {}

    def relearning(M, L):
        # known_team (progress.c): a block shown at another generation than the one
        # learnt is learnt afresh, and its old team caught with a RELEASE queued is
        # the hold's argument broken - named rather than overwritten.
        b, g = L['b'], L['eg']
        known = L[f'learnt{b}']
        if known and known != g and design != 'no_relearn' and M[QUEUED(b)]:
            return (f'QUEUED: block {b} holds another team (generation {g}, learnt at {known}) '
                    f'while {M[QUEUED(b)]} of its RELEASEs are queued: its members left before '
                    f'its forwards')
        return None

    def apply(M, L):
        b, g = L['b'], L['eg']
        if L[f'learnt{b}'] != g:
            return (f'ENGINE: the engine applied '
                    f'{"an arrival" if L["kind"] == 1 else "a message"} of generation {g} to '
                    f'block {b}\'s state, learnt at generation {L[f"learnt{b}"]}')
        return None

    def own(M, L):
        name, team = divmod(L['m'], 8)
        if L['b'] != M[TEAM_BLOCK(team)] or L['eg'] != M[TEAM_GEN(team)]:
            return (f'NAMED: a message for team {TEAM[team]} under name {name} resolved to block '
                    f'{L["b"]} at generation {L["eg"]}, the team\'s being block '
                    f'{M[TEAM_BLOCK(team)]} at {M[TEAM_GEN(team)]}')
        return None

    def is_a(M, L):
        return L['b'] == M[TEAM_BLOCK(A)] and L['eg'] == M[TEAM_GEN(A)] and M[LIVE(0)] == A

    def taken(M, L, of_a):
        # A's second input closes its epoch (book_arrival), and release_down, in the
        # same step, queues the RELEASE for the child and lists the local release.
        mem = {A_TAKEN: M[A_TAKEN] + of_a}
        if of_a and M[A_TAKEN] == 1:
            mem[QUEUED(L['b'])] = 1
            mem[PENDING(L['b'])] = 1
        return mem

    return assemble([
        ('P', ('nd', events, '@K')),
        ('K', ('br', lambda L: L['kind'] == 1, '@HK', '@K2')),
        ('K2', ('br', lambda L: L['kind'] == 2, '@Q', '@K3')),
        # 3: a RELEASE left the queue or the release was applied, in the nd step
        # itself; 9: nothing is left.
        ('K3', ('br', lambda L: L['kind'] == 9, '@E', '@X')),
        # An arrival: the handoff's own generation.
        ('HK', ('check', relearning, '@HL')),
        ('HL', ('ghost', learn, '@HA')),
        ('HA', ('check', apply, '@HT')),
        ('HT', ('ghost', lambda M, L: (taken(M, L, is_a(M, L)),
                                       {f'taken{L["b"]}': L['h'], 'n': (L['n'] or 0) + 1}), '@X')),
        # A message: its name through the map, then the block's generation.
        ('Q', ('load', lambda L: MAP(L['m'] // 8), 'v', '@Qz')),
        ('Qz', ('check', lambda M, L: None if L['v'] else
                f'NAMED: a message for team {TEAM[L["m"] % 8]} under name {L["m"] // 8} found no '
                f'block in the map', '@Qb')),
        ('Qb', ('set', 'b', lambda L: L['v'] - 1, '@Qg')),
        ('Qg', ('load', lambda L: GEN(L['b']), 'eg', '@Qo')),
        ('Qo', ('check', own, '@QK')),
        ('QK', ('check', relearning, '@QL')),
        ('QL', ('ghost', learn, '@QA')),
        ('QA', ('check', apply, '@QT')),
        ('QT', ('ghost', lambda M, L: ({MESSAGE: 0, **taken(M, L, L['m'] % 8 == A)},
                                       {'n': (L['n'] or 0) + 1, 'v': DEL, 'm': DEL}), '@X')),
        ('X', ('set', 'kind', lambda L: DEL, '@X2')),
        ('X2', ('set', 'eg', lambda L: DEL, '@X3')),
        ('X3', ('set', 'h', lambda L: DEL, '@X4')),
        ('X4', ('set', 'b', lambda L: DEL, '@P')),
        ('E', ('ghost', lambda M, L: ({DONE: M[DONE] + 1}, {}), '@END')),
        ('END', ('done',))])


def case(design, tso=False):
    threads = [lead(design, tso), member(design), other(design, tso), remote(), engine(design)]
    mem = {FREE: (1 << BLOCKS) - 1, MESSAGE: 0, A_TAKEN: 0, DONE: 0, SYNC(1): 0, SYNC(2): 0,
           RELEASED: 0}
    for b in range(BLOCKS):
        mem[GEN(b)] = mem[HOLDERS(b)] = mem[RECORD(b)] = mem[COUNTERS(b)] = mem[HANDOFF(b)] = 0
        mem[QUEUED(b)] = mem[PENDING(b)] = 0
        for pe in range(3):
            mem[HOLDER(b, pe)] = 0
    for n in (0, 1):
        mem[MAP(n)] = mem[LIVE(n)] = 0
    for t in (A, B, C):
        mem[TEAM_GEN(t)] = 0
        mem[TEAM_BLOCK(t)] = 0
    return f'team_block/{design} blocks={BLOCKS}', threads, mem


# (design, expect): True, or the property it must fail. Each runs under SC and then under
# x86-TSO, with the same verdict.
CASES = [
    ('design', True),
    ('no_holders', 'REWRITE'),
    ('no_relearn', 'ENGINE'),
    ('map_early', 'NAMED'),
    ('last_clears', 'FREE'),
    ('free_first', 'NAMED'),
    ('no_hold', 'QUEUED'),
]


def main():
    want = select(sys.argv)
    ok = True
    for design, expect in CASES:
        if not want(case(design)[0]):
            continue
        for tso in (False, True):
            name, threads, mem = case(design, tso)
            res = explore(name, threads, mem, tso=tso, max_seconds=1800,
                          liveness_max_states=1_000_000, done=lambda M: M[DONE] == 5)
            ok &= report(res, expect=expect, tso=tso)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
