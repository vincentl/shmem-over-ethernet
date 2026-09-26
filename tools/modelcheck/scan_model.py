#!/usr/bin/env python3
"""A prefix scan over a team's members, for shmem_sum_inscan and shmem_sum_exscan
(spec §9.10.10; design §5.8, "Prefix scans"; the proposal in
docs/prompts/done/phase1-step10-slice1-prompt.md).

Every member takes part on its own thread, at its team rank's place in a
pre-order k-ary tree over the ranks 0 .. M - 1: a place covering ranks
[a, b) is rank a, and its children split [a + 1, b) into at most k
consecutive blocks, so every subtree is a contiguous range of ranks. Up: a
member waits for each child's subtree total, adds its own source, and puts
the sum into its parent's slot for its position, then adds one to the
parent's counter for that position. Down: the root has nothing before it;
every other member waits for the sum of every rank before its subtree, from
its parent. Its own exclusive result is that sum, its inclusive result that
plus its own source, and child j gets that sum plus its own source plus the
totals of children 0 .. j - 1.

**The slots are per PE, not per team number**: k up-slots and one down-slot
in each PE's internal region, and a counter for each, cumulative and compared
as at least - the count a PE expects is its own count of the scans, of any
team, in which it had a child in that position (or a parent). What keeps a
scan on one team from writing into a PE still in a scan on another is:
  READY  a PE, entering a scan on team t, adds one to READY[t] at each child;
         a child puts its total up only once READY[t] shows its parent has
         entered this scan. Per team number: a single per-PE READY would let a
         parent's entry into a scan on another team satisfy the wait.
A parent writes a child's down-slot only after the child's total arrived, so
the child is in this scan and has read its last down-slot: no READY needed
there.

Checked
  SLOT     no slot is written while it holds a value not yet read.
  VALUE    a value read from a slot is this scan's: its (team, scan) matches.
  RESULT   every member's result is the exact sum the spec defines: the bits of
           the sources of the ranks before it (exclusive) - sources are
           distinct powers of two per (scan, rank), so a sum is a set.
  and, by the engine: no stuck member, and every state can still finish.

Teams: A is the members in world order; B the same members reversed (a
stride of -1), so the two trees differ. Each case gives the order of scans
every member makes, e.g. "AB": a scan on A then one on B.

Designs
  design         as above: per-PE slots shared by every team, and READY.
  per_team       **as built**: slots and counters per (PE, team number), each
                 PE counting per team, and no READY. The review of the slice
                 found the shared slots wrong under THREAD_MULTIPLE, where two
                 threads of a PE may scan two teams at once (spec §9.4); with a
                 team's own slots, consecutive scans on one team are safe by the
                 tree itself - a child reaches the next scan only after its
                 parent read its total and sent it the down - which the cases
                 on one team check, and scans on different teams touch
                 different memory.
Breakages
  no_ready       a child puts up without waiting for READY (SLOT or VALUE).
  shared_ready   one READY per PE rather than per team (SLOT or VALUE).
  no_left_sums   child j is sent the sum before its parent's subtree plus the
                 parent's source, without the totals of children before it
                 (RESULT).
  team_counts    a PE counts what it expects in its slots per team, not over
                 every team's scans: a scan on B reads what A's left (VALUE).
  per_team_pe_counts
                 per-team slots, but counted over every team's scans, as the
                 shared slots are: a scan on B waits for a count its own slots
                 never reach (STUCK).

Runs are SC; puts are fenced before the counter add that covers them, and
waits are abstract (the region wait's wake is wait_word_model.py's).

Usage: scan_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import assemble, explore, report, select

# Slots and counters are keyed by (PE, team) under the per_team design (as
# built) and by PE alone under the others, where `s` is always 0.
UP = lambda p, j, s=0: ('up', p, j, s)      # at PE p: child position j's subtree total
UPC = lambda p, j, s=0: ('upc', p, j, s)    # at PE p: totals put into position j, cumulative
DOWN = lambda p, s=0: ('down', p, s)        # at PE p: the sum before its subtree
DOWNC = lambda p, s=0: ('downc', p, s)
READY = lambda p, t: ('ready', p, t)    # at PE p: its parents' entries into scans on team t
UNREAD = lambda key: ('$unread',) + key  # ghost: the slot holds a value not yet read
TAG = lambda key: ('$tag',) + key        # ghost: (team, scan) + 1 of the value in the slot
RESULT = lambda p, x: ('$result', p, x)
FIN = ('$fin',)
# Each team's members in team-rank order, by world rank (PE). A is the world,
# B the world reversed (a stride of -1, so its tree differs), C a team of two
# whose root is PE 0 and whose one child is PE 3: a team PE 1 and PE 2 are not
# in, so they can reach a scan on another team before PE 0 has left C's.
#
# Z, X and Y are three teams of two, for per-team READY: PE 0 is Z's root with
# PE 3 its child, and X's root with PE 1 its child; PE 2 is Y's root with PE 1
# its child. With one READY per PE, PE 2's entry into Y - which it makes at
# once - could let PE 1 put up in X while PE 0 is still in Z, reading the
# same slot from PE 3.
TEAMS = {'A': [0, 1, 2, 3, 4, 5], 'B': [5, 4, 3, 2, 1, 0], 'C': [0, 3],
         'Z': [0, 3], 'X': [0, 1], 'Y': [2, 1]}
TEAM_NUMBER = {'A': 0, 'B': 1, 'C': 2, 'Z': 3, 'X': 4, 'Y': 5}


def team_members(team, members):
    if team in ('A', 'B'):
        return [p for p in TEAMS[team] if p < members]
    return TEAMS[team]


def children(a, b, k):
    """The children of the place covering [a, b): (first rank, end) of each."""
    rest = b - (a + 1)
    if rest <= 0:
        return []
    n = min(k, rest)
    out, start = [], a + 1
    for j in range(n):
        size = rest // n + (1 if j < rest % n else 0)
        out.append((start, start + size))
        start += size
    return out


def places(members, k):
    """rank -> (parent rank or None, position among its parent's children,
    [(child rank, end)])"""
    info = {}

    def walk(a, b, parent, position):
        kids = children(a, b, k)
        info[a] = (parent, position, kids)
        for j, (c, e) in enumerate(kids):
            walk(c, e, a, j)
    walk(0, members, None, None)
    return info


def pe_of(team, rank, members):
    return team_members(team, members)[rank]


def rank_of(team, pe, members):
    return team_members(team, members).index(pe)


def source(x, rank, members):
    return 1 << (x * members + rank)


def member(design, pe, members, k, order):
    """PE `pe`'s program: the scans of `order`, one after another."""
    B = []
    counts = {}                 # (team) -> scans made on it so far
    expected_up = {}            # (team or None) -> scans so far with a child in position j
    expected_down = {}          # (team or None) -> scans so far with a parent
    ready_expected = {}         # team -> scans so far on it (entries its parent made)
    shared_ready_expected = 0
    for x, team in enumerate(order):
        if pe not in team_members(team, members):
            continue
        size = len(team_members(team, members))
        tree = places(size, k)
        t = TEAM_NUMBER[team]
        n_on_team = counts.get(team, 0)
        counts[team] = n_on_team + 1
        rank = rank_of(team, pe, members)
        parent, position, kids = tree[rank]
        tag = 10 * t + n_on_team + 1          # (team, scan on it) + 1
        scope = team if design in ('team_counts', 'per_team') else None
        key = t if design in ('per_team', 'per_team_pe_counts') else 0
        ups = expected_up.setdefault(scope, [0] * k)
        p = f'x{x}_'
        later = [y for y in range(x + 1, len(order)) if pe in team_members(order[y], members)]
        nxt = f'@x{later[0]}_E' if later else '@DONE'
        # Entry: tell every child it may write here.
        steps = []
        for j, (c, _) in enumerate(kids):
            cpe = pe_of(team, c, members)
            addr = (lambda L, cpe=cpe: READY(cpe, 0)) if design == 'shared_ready' else \
                   (lambda L, cpe=cpe, t=t: READY(cpe, t))
            steps.append((f'{p}R{j}', ('add', addr, lambda L: 1, '_', None)))
        # Up: every child's total, in position order.
        for j, (c, _) in enumerate(kids):
            want = ups[j] + 1
            ups[j] += 1
            steps.append((f'{p}U{j}', ('await', lambda M, L, j=j, want=want, key=key:
                                       M[UPC(pe, j, key)] >= want, None)))
            steps.append((f'{p}V{j}', ('check', lambda M, L, j=j, tag=tag, key=key:
                                       None if M[TAG(UP(pe, j, key))] == tag else
                                       f'PE {pe} read up-slot {j} with tag {M[TAG(UP(pe, j, key))]}, '
                                       f'not this scan\'s {tag}', None)))
            steps.append((f'{p}T{j}', ('ghost', lambda M, L, j=j, key=key:
                                       ({UNREAD(UP(pe, j, key)): 0},
                                        {f'c{j}': M[UP(pe, j, key)]}), None)))
        own = source(x, rank, members)
        if parent is not None:
            ppe = pe_of(team, parent, members)
            if design in ('no_ready', 'per_team', 'per_team_pe_counts'):
                pass
            elif design == 'shared_ready':
                shared_ready_expected += 1
                want = shared_ready_expected
                steps.append((f'{p}W', ('await', lambda M, L, want=want:
                                        M[READY(pe, 0)] >= want, None)))
            else:
                want = ready_expected.get(team, 0) + 1
                ready_expected[team] = want
                steps.append((f'{p}W', ('await', lambda M, L, want=want, t=t:
                                        M[READY(pe, t)] >= want, None)))

            def put_up(M, L, ppe=ppe, position=position, own=own, n=len(kids), tag=tag,
                       slot=key):
                key = UP(ppe, position, slot)
                if M[UNREAD(key)]:
                    return [({('$overwrite',): 1}, {}, 'overwrite')]
                total = own + sum(L[f'c{j}'] for j in range(n))
                return [({key: total, UNREAD(key): 1, TAG(key): tag}, {},
                         f'PE {pe} puts its subtree total into PE {ppe}\'s up-slot {position}')]
            steps.append((f'{p}P', ('nd', put_up, None)))
            steps.append((f'{p}O', ('check', lambda M, L: None if not M[('$overwrite',)] else
                                    f'PE {pe} overwrote an unread slot', None)))
            steps.append((f'{p}A', ('add', lambda L, ppe=ppe, position=position, key=key:
                                    UPC(ppe, position, key), lambda L: 1, '_', None)))
            want = expected_down.get(scope, 0) + 1
            expected_down[scope] = want
            steps.append((f'{p}D', ('await', lambda M, L, want=want, key=key:
                                    M[DOWNC(pe, key)] >= want, None)))
            steps.append((f'{p}DV', ('check', lambda M, L, tag=tag, key=key:
                                     None if M[TAG(DOWN(pe, key))] == tag else
                                     f'PE {pe} read its down-slot with tag '
                                     f'{M[TAG(DOWN(pe, key))]}, not this scan\'s {tag}', None)))
            steps.append((f'{p}DT', ('ghost', lambda M, L, key=key: ({UNREAD(DOWN(pe, key)): 0},
                                                            {'before': M[DOWN(pe, key)]}), None)))
        else:
            steps.append((f'{p}D', ('set', 'before', lambda L: 0, None)))
        # The result, checked, then down to each child.
        expect = sum(source(x, r, members) for r in range(rank))  # ranks before, in this team
        steps.append((f'{p}X', ('check', lambda M, L, expect=expect, x=x:
                                None if L['before'] == expect else
                                f'PE {pe}\'s exclusive result in scan {x} is {L["before"]:b}, '
                                f'not {expect:b}', None)))
        for j, (c, _) in enumerate(kids):
            cpe = pe_of(team, c, members)

            def put_down(M, L, j=j, cpe=cpe, own=own, tag=tag, slot=key):
                key = DOWN(cpe, slot)
                if M[UNREAD(key)]:
                    return [({('$overwrite',): 1}, {}, 'overwrite')]
                left = 0 if design == 'no_left_sums' else sum(L[f'c{i}'] for i in range(j))
                return [({key: L['before'] + own + left, UNREAD(key): 1, TAG(key): tag}, {},
                         f'PE {pe} puts the sum before child {j} into PE {cpe}\'s down-slot')]
            steps.append((f'{p}Q{j}', ('nd', put_down, None)))
            steps.append((f'{p}QO{j}', ('check', lambda M, L: None if not M[('$overwrite',)] else
                                        f'PE {pe} overwrote an unread down-slot', None)))
            steps.append((f'{p}QA{j}', ('add', lambda L, cpe=cpe, key=key: DOWNC(cpe, key),
                                        lambda L: 1, '_', None)))
        # Chain the steps.
        chained = [(p + 'E', ('br', lambda L: True, None, None))] + steps
        for i, (lab, ins) in enumerate(chained):
            ins = list(ins)
            target = '@' + chained[i + 1][0] if i + 1 < len(chained) else '@' + p + 'Z'
            if ins[0] == 'br':
                ins[2] = ins[3] = target
            else:
                ins[-1] = target
            B.append((lab, tuple(ins)))
        B.append((p + 'Z', ('set', 'before', lambda L: None, nxt)))
    B += [('DONE', ('gpost', lambda M, L: ({FIN: M[FIN] + 1}, {}), '@END')),
          ('END', ('done',))]
    return assemble(B)


def case(design, members, k, order):
    threads = [member(design, pe, members, k, order) for pe in range(members)]
    mem = {FIN: 0, ('$overwrite',): 0}
    for p in range(members):
        for s in [0] + sorted(TEAM_NUMBER.values()):
            for j in range(k):
                mem[UP(p, j, s)] = mem[UPC(p, j, s)] = 0
                mem[UNREAD(UP(p, j, s))] = mem[TAG(UP(p, j, s))] = 0
            mem[DOWN(p, s)] = mem[DOWNC(p, s)] = mem[UNREAD(DOWN(p, s))] = mem[TAG(DOWN(p, s))] = 0
        for t in TEAM_NUMBER.values():
            mem[READY(p, t)] = 0
    return (f'scan/{design} members={members} k={k} order={order}', threads, mem, members)


# (design, members, fan-out, order, expect_ok)
CASES = [
    ('design', 3, 2, 'AA', True),
    ('design', 4, 2, 'AB', True),
    ('design', 4, 2, 'ABA', True),
    ('design', 5, 2, 'AB', True),
    ('design', 5, 3, 'AB', True),
    ('design', 6, 2, 'AB', True),
    ('design', 4, 2, 'CA', True),
    ('design', 4, 2, 'CAB', True),
    ('design', 5, 2, 'CB', True),
    ('no_ready', 4, 2, 'CA', False),
    ('design', 4, 2, 'ZXY', True),
    ('shared_ready', 4, 2, 'ZXY', False),
    ('no_left_sums', 4, 2, 'A', False),
    ('team_counts', 4, 2, 'AB', False),
    ('per_team', 3, 2, 'AA', True),
    ('per_team', 4, 2, 'ABA', True),
    ('per_team', 5, 3, 'AB', True),
    ('per_team', 6, 2, 'AB', True),
    ('per_team', 4, 2, 'CAB', True),
    ('per_team', 4, 2, 'ZXY', True),
    ('per_team', 5, 2, 'AAA', True),
    ('per_team_pe_counts', 4, 2, 'AB', False),
]


def main():
    want = select(sys.argv)
    ok = True
    for design, members, k, order, expect in CASES:
        name, threads, mem, n = case(design, members, k, order)
        if not want(name):
            continue
        res = explore(name, threads, mem, max_seconds=900, liveness_max_states=3_000_000,
                      done=lambda M, n=n: M[FIN] == n)
        ok &= report(res, expect_ok=expect)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
