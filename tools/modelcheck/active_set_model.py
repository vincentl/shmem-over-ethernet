#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""The deprecated collectives on an active set that is not the whole job (T28;
shmem/src/active-set.c, phase 1 step 5 slice 7).

shmem_barrier, shmem_sync, shmem_broadcast, fcollect, collect, the _to_all
reductions and alltoall, for a set of PEs that is not the world, run among the
set's members over the program's own pSync and the library's public RMA. The
slice's review checked them with a random model that is not in the tree
(docs/reviews/archive/phase1-step5-slice7-review.md, "Evidence"); this one is
exhaustive, on the engine, over small sets.

The protocol. **Every signal is a count**: a sender adds 1 to a pSync word at the
receiver, who waits for SHMEM_SYNC_VALUE plus the count it expects and takes the
count back with an add of the negative. A barrier is a dissemination: in round k a
member signals the member 2^k ranks on and waits for the member 2^k back. The rest
is a binomial tree over the ranks relative to a root: up, a member waits for each
child - a word a child - and signals its parent, stores first (a quiet); down, a
parent puts to each child with shmem_putmem_signal on the child's release word, the
largest subtree first. fcollect and the reductions get each child's range out of
the child's dest as the child arrives; collect first gets the children's byte totals
the same way, puts each child its offset and the whole, and then gathers like
fcollect; alltoall puts straight to each member, then a quiet and a barrier.

Members. A thread each, whose steps are active-set.c's in its order; `Member`'s
methods carry the C's names.
  osoe_active_set_barrier    [quiet], then a signal and an await a round
  arrive                     quiet, then the signal to the parent's word for this child
  up                         an await a child, then arrive
  down                       the await for the parent's bytes, then a put-with-signal
                             a child, the largest subtree first
  osoe_active_set_broadcast  up, then down from the root's source; the root's dest is
                             left alone
  gather_then_spread         per child an await and a get of its range, then arrive,
                             then down
  osoe_active_set_fcollect   the member's block into dest, gather_then_spread
  osoe_active_set_collect    per child an await and a get of its TOTAL; TOTAL set;
                             arrive; await the release; read OFFSET and GRAND and set
                             the three words back; per child puts of OFFSET and GRAND,
                             a fence, the release; the member's block into dest at its
                             offset; gather_then_spread over the second pass's words
  osoe_active_set_reduce     per child an await and a get combined into dest (a sum);
                             arrive; down
  osoe_active_set_alltoall   a put a member starting past itself, then the barrier
  do_putbarrier              not active-set.c's: the application puts to its right-hand
                             neighbour and calls shmem_barrier, where its quiet is held
A case gives the set's size, each member's node and the calls, which every member
makes in the same order on one pSync with no barrier between them - more than the
spec asks, which lets a pSync be reused once the earlier call has completed on every
PE; the slice's brief asked for it, and the review found it held - each call with a
dest of its own. Sources are distinct per call, member and cell, and a reduction's
are distinct bits, so a block missing, doubled, or from another call or rank shows in
the result's value. The cases run one to ten members and up to seven calls.

The network is a thread of its own. A put or a non-fetching atomic add is **in
flight** from the step that issues it until a delivery step lands it; a get reads at
one instant. To a PE of the issuer's own node, itself included, an operation is
applied before its call returns (transport-am.c, osoe_routes_in_process) and takes
no delivery step; to any other, between one issuer and one target, deliveries are
unordered except that
  a fence       places every operation before it before every one after it
                (transport.h; collect's fence between its puts and the release);
  a quiet       waits for every operation the issuer has made;
  a signal      of put-with-signal is placed after every write the issuer made earlier to
                that PE - OSOE_TRANSPORT_FENCED, signal.c - not after its own put alone.
A put lands whole, at one instant. Nothing reads its cells before the signal or the
quiet that follows it, so landing cell by cell would add states and no behaviour any
check sees: ENTERED is decided by the first cell to land and LANDED by the last, and a
whole put can land at either instant. Both of the library's backends place in issue
order (transport.h, OSOE_TRANSPORT_FENCED); the model does not use it, the contract
being what a second backend is held to - so no_fence and signal_unordered below break
the contract, and nothing today's backends do.

Steps merged, each exactly (the merged step reaches the states of the pair and no
others): an await and its take-back under SC - the add commutes with the adds that
land after the wait and only the owner reads the word (two steps under TSO, the add
a locked instruction); a put and its signal, or collect's two puts, fence and
release - the gap between the issues is seen by nobody; a get and the copy-out of
its bytes - nobody reads the local cells before the member arrives; the call's entry
(ghost ENTERED, and the member's block into dest) folded into its first step and its
return (RETURNED and the result's check) into its last, as the engine's gpre and
gpost. A store folded into the step after it - the member's block, collect's TOTAL and
memmove - is visible from that step, later than the C's; its readers are the parent's
gets, behind the arrival signal, a later step still. selftest.py's check, the same
verdicts with the reductions off, was run by hand over a sample of the cases; so was
the same with the TSO programs - an await and its add apart, a load and a store a cell
- under SC.

Checked (a check's message begins with its property's name)
  RESULT    every member's dest is the call's result, by value, when the call returns;
            the broadcast root's dest is untouched
  BARRIER   no member leaves a sync or barrier before every member has entered it
  ENTERED   every write into a call's buffer lands after its owner entered the call -
            alltoall's puts and the application's before shmem_barrier excepted: the
            spec has every dest ready before any member calls
  LANDED    and lands before its owner returned from it
  RETURNED  every read of a member's buffer - the parent's gets - is made before its
            owner returned
  PSYNC     every pSync word is back at SHMEM_SYNC_VALUE once every member has returned
            and every operation has landed
  and, by the engine, no hang: a member blocked with nothing left to land is STUCK,
  and the state graph has no cycle - every step advances a program counter or lands
  an operation - so that is the whole of it; AG EF done runs all the same under 2.5
  million states.

Designs
  design   as above.
Breakages, each failing the property it names; `only P` runs with P's check alone,
which is the evidence that the check can fail (tree_barrier_model.py's way)
  wrong_partner         the barrier signals the next rank in every round, not 2^k on:
                        four members, where a third round would be needed (BARRIER)
  no_take_back          an await leaves its count in the word (BARRIER at the next sync;
                        PSYNC alone)
  reset_not_subtract    the take-back stores SHMEM_SYNC_VALUE, losing an add that has
                        landed for the next call (STUCK)
  await_exact           the wait is for equality, not at least: an add for the next call
                        lands first and the word skips its target (STUCK)
  barrier_no_quiet      shmem_barrier, and alltoall's, without the quiet that starts it:
                        puts still in flight when the signals go (RESULT)
  no_fence              collect without the fence between its puts of OFFSET and GRAND
                        and the release (RESULT)
  offsets_early         a member reads OFFSET and GRAND before it is released (RESULT)
  read_total_early      a parent reads a child's TOTAL before the child arrives (RESULT)
  no_reset              collect leaves TOTAL, OFFSET and GRAND set (PSYNC)
  signal_unordered      put-with-signal's signal not placed after the bytes (RESULT;
                        LANDED alone: the bytes land after the owner has returned)
  shared_arrival        one arrival word for all of a parent's children, not one each
                        (RESULT)
  no_wait_release       down forwards before the parent's bytes have come (RESULT)
  no_up                 broadcast without its up phase: the root writes members that
                        have not entered (ENTERED)
  gather_after_release  a parent gets its children's ranges after it has released them
                        (RESULT; RETURNED alone)
  wrong_subtree         fcollect's length for a child is its span, not its subtree: only
                        where the set's size is not a power of two (RESULT)
  offsets_not_advanced  collect's offset for a child does not move past the one before
                        (RESULT)
  up_early              broadcast's up phase leaves out the wait for the last child: the
                        root releases a member that has not entered (PSYNC, the child's
                        signal never taken back; ENTERED alone)
  unfenced_arrive       arrive without its quiet, and its signal a publication that does
                        not drain the store buffer: the member's last stores to its dest
                        may still be buffered when its parent gets them. The one breakage
                        that needs x86-TSO: RESULT there, and under SC it holds.
  total_after_arrive    collect's TOTAL set after the signal its reader waits for, not before:
                        the parent reads SHMEM_SYNC_VALUE for the child's total (RESULT)
  alltoall_entered      not a defect: alltoall's puts held to ENTERED like every other
                        write. They land before their target has entered the call, as the
                        spec lets them, which is why the check excepts them (ENTERED)
  Four that hold - each a redundancy the model shows, not a flaw:
  arrive_no_quiet       arrive without its quiet. Every case holds under SC and under
                        x86-TSO: the signal that follows is itself a locked, seq_cst
                        instruction, ordered after the stores before it, and the quiet
                        orders nothing the protocol reads.
  await_exact,          the same variants as above, on tree collectives alone: no add of
  reset_not_subtract    a call's next arrives before the call has taken its own - no
                        member leaves a call before the root has every arrival. Only the
                        barrier's last round, whose sender may leave before its receiver
                        has taken the signal, sees an early add, and so needs the count
                        and the comparison.
  down_smallest_first   down puts to the smallest subtree first, as the slice was built
                        before its review: slower (the review's S1), as right.

Memory orders. Every case runs under SC; a set of them under x86-TSO too, the
stores as the library makes them (§3.16). A thread's buffered stores are the plain
ones to its own dest: the memmove of its block, the copy-out of a get, a reduction's
combine, collect's memmove; everything else is locked or seq_cst and drains the
buffer first:
  take-back of a count        a seq_cst add: `add`
  TOTAL's set, the three      pshmem_long_atomic_set: a seq_cst store
  resets
  OFFSET and GRAND fetched    atomic fetch: a load
  an issue - an add, or a     a locked rmw on the intranode route; on the AM route
  put with its signal         every put and add goes through osoe_connection_commit's
                              seq_cst add and osoe_connection_area_mark's seq_cst or: a
                              drain, then the step
  a bare put to a node-mate   a memcpy and notify's seq_cst load, no locked instruction;
                              alltoall's and the application's puts come where the
                              buffer is empty - a call's stores are drained by the time
                              it ends - so the drain the model gives them changes nothing
  quiet                       osoe_serializing_fence first (osoe_transport_wait): a drain
  the wait's load             osoe_transport_load, seq_cst: `await`
  fence                       a release fence: nothing on x86
A put to a node-mate is the issuer's own memcpy, buffered until its next locked
instruction - the quiet or the signal that follows; the model applies it at once, which
shows it earlier and nothing later. The TSO runs add flush steps and per-cell loads and
stores to the SC runs' steps and reach the same verdicts; aarch64, which does not keep a
thread's loads in order, is not an order the engine has.

Left out: the reduction's chunking (a child's partial is fetched 64 KiB at a time; one
chunk here, the gets and combines private to the reducer); alltoalls (the same puts by
iput and the same barrier) and the typed wrappers; validation
(osoe_active_set_is_whole_job); the stride and start of a set (members are ranks here;
where they are is `nodes`); the whole-job path, collectives.c's; how a wait sleeps
(wait_word_model.py's); more than one thread a PE; a pSync shared by sets that overlap.
Two things a word that is one atomic cell cannot show: OFFSET and GRAND are written by
a put and read and reset by atomics, TOTAL set by an atomic and read by a get, and
transport.h gives no exclusivity between an rmw and a put or a get - they are safe by
the release and the arrival that order them, which is what the cases check; and the
widths and signedness of the C (the wait is signed, the signal address a long cast to a
uint64_t, LP64).

Usage: active_set_model.py [substring ...]   runs the cases whose names match
"""
import sys

from engine import assemble, explore, report, select

SYNC = -1                              # SHMEM_SYNC_VALUE

# The words of pSync active-set.c uses (its enum).
SLOT_BARRIER, SLOT_RELEASED, SLOT_TOTAL, SLOT_OFFSET, SLOT_GRAND = 0, 65, 66, 67, 68
SLOT_RELEASED_DATA, SLOT_ARRIVED, SLOT_ARRIVED_DATA = 70, 72, 104

CELL_BITS = 16

MUTATIONS = frozenset('''design wrong_partner no_take_back reset_not_subtract await_exact
    barrier_no_quiet no_fence offsets_early read_total_early no_reset signal_unordered
    shared_arrival no_wait_release no_up gather_after_release arrive_no_quiet
    unfenced_arrive wrong_subtree offsets_not_advanced up_early down_smallest_first
    alltoall_entered total_after_arrive'''.split())


# ---- active-set.c's tree arithmetic -----------------------------------------------
def span_of(v, size):
    if v:
        return v & -v
    p = 1
    while p < size:
        p <<= 1
    return p


def subtree_size(v, size):
    span = span_of(v, size)
    return span if v + span <= size else size - v


def child_index(v):
    return (v & -v).bit_length() - 1


def child_steps(v, size):
    """The k of v's children v + k, smallest first: the loop of up, gather and reduce."""
    steps, k = [], 1
    while k < span_of(v, size) and v + k < size:
        steps.append(k)
        k <<= 1
    return steps


def check_tree(sizes):
    """The tree arithmetic, held for every size and every relative rank (the slice 7
    review's check, kept): children are v + 1, v + 2, v + 4 ...; each child's parent is
    v and its position is its index; the children's subtrees tile [v + 1, v + subtree)
    in rank order; no node has more than 31 children (offsets[32]); the root's subtree
    is the set. Returns the first message of a defect, or None."""
    for size in sizes:
        if subtree_size(0, size) != size:
            return f'TREE: size {size}: the root\'s subtree is {subtree_size(0, size)}'
        for v in range(size):
            at = v + 1
            steps = child_steps(v, size)
            if len(steps) > 31:
                return f'TREE: size {size}: rank {v} has {len(steps)} children'
            for j, k in enumerate(steps):
                child = v + k
                if child - (child & -child) != v or child_index(child) != j or child >= size:
                    return (f'TREE: size {size}: rank {v}\'s child {child} has parent '
                            f'{child - (child & -child)}')
                if child != at:
                    return f'TREE: size {size}: rank {v}\'s child {child} starts at {at}'
                at = child + subtree_size(child, size)
            if at != v + subtree_size(v, size):
                return (f'TREE: size {size}: rank {v}\'s children end at {at}, not '
                        f'{v + subtree_size(v, size)}')
    return None


# ---- memory -------------------------------------------------------------------------
def word(member, slot):
    return ('w', member, slot)


def cell(member, call, index):
    return ('d', member, call, index)


FLIGHT = ('F',)                        # bit i set: operation i is in flight
FINISHED = ('$fin',)                   # members that have left their last call
DRAIN = ('drain',)                     # only locked no-ops touch it (x86-TSO)


def pending(i):
    return ('v', i)                    # the payload of operation i while it is in flight


def entered(call):
    return ('$in', call)               # bit m set: member m has entered the call


def returned(call):
    return ('$out', call)


def pack(values):
    n = len(values)
    for i, v in enumerate(values):
        n |= v << (CELL_BITS * (i + 1))
    return n


def unpack(n):
    mask = (1 << CELL_BITS) - 1
    return [(n >> (CELL_BITS * (i + 1))) & mask for i in range(n & mask)]


# ---- what each call holds -------------------------------------------------------------
def tag(call, member, index):
    return 1 + index + 8 * (member + 8 * call)


def dest_length(call, size):
    kind = call[0]
    if kind in ('sync', 'barrier'):
        return 0
    if kind == 'broadcast':
        return call[2]
    if kind in ('fcollect', 'alltoall'):
        return size * call[1]
    if kind == 'collect':
        return sum(call[1])
    return call[1]                      # reduce, putbarrier


def source_cells(call_index, call, member, size):
    """What the member passes as source."""
    kind = call[0]
    if kind == 'broadcast':
        return [tag(call_index, member, t) for t in range(call[2])]
    if kind in ('fcollect', 'putbarrier'):
        return [tag(call_index, member, t) for t in range(call[1])]
    if kind == 'collect':
        return [tag(call_index, member, t) for t in range(call[1][member])]
    if kind == 'reduce':
        return [1 << (member + size * t) for t in range(call[1])]
    if kind == 'alltoall':
        return [tag(call_index, member, t) for t in range(size * call[1])]
    return []


def expected(call_index, call, member, size):
    """The member's dest when the call returns."""
    kind = call[0]
    if kind == 'broadcast':
        return [0] * call[2] if member == call[1] else source_cells(call_index, call, call[1], size)
    if kind in ('fcollect', 'collect'):
        result = []
        for r in range(size):
            result += source_cells(call_index, call, r, size)
        return result
    if kind == 'reduce':
        return [sum(source_cells(call_index, call, r, size)[t] for r in range(size))
                for t in range(call[1])]
    if kind == 'alltoall':
        n = call[1]
        result = []
        for r in range(size):
            result += source_cells(call_index, call, r, size)[member * n:(member + 1) * n]
        return result
    if kind == 'putbarrier':
        return source_cells(call_index, call, (member - 1) % size, size)
    return []


def describe(call):
    kind = call[0]
    if kind == 'broadcast':
        return f'broadcast.r{call[1]}' + (f'x{call[2]}' if call[2] != 1 else '')
    if kind == 'collect':
        return 'collect.' + ''.join(map(str, call[1]))
    if len(call) > 1 and call[1] != 1:
        return f'{kind}x{call[1]}'
    return kind


# ---- the network -----------------------------------------------------------------------
class Operation:
    """A put or a non-fetching atomic add, as one member issues it: applied before its
    call returns to a PE of the issuer's own node (transport-am.c,
    osoe_routes_in_process), in flight until a delivery step lands it to any other."""
    __slots__ = ('index', 'issuer', 'target', 'kind', 'slot', 'call', 'first', 'limit',
                 'immediate', 'blockers', 'epoch', 'label', 'check_entered', 'check_landed')


class Network:
    def __init__(self, case):
        self.case = case
        self.nodes = case.nodes
        self.flights = []              # the operations that can be in flight
        self.epoch = {}                # per issuer: fences made so far (generation order)
        self.since_quiet = {}          # per issuer: its flights since its last quiet
        self.words = set()
        self.issuer_mask = {}

    def start(self, member):
        self.epoch[member] = 0
        self.since_quiet[member] = []

    def fence(self, member):
        self.epoch[member] += 1

    def quiet(self, member):
        self.since_quiet[member] = []

    def make(self, issuer, target, kind, label, slot=None, call=None, first=0, limit=0,
             fenced=False, entry_exempt=False):
        """An operation of `kind`: 'add' (1 to a pSync word), 'word' (a put of one long to a
        pSync word) or 'cells' (a put into a call's dest). Its blockers are the
        operations to the same target that must have landed before it can: those a fence
        separates from it, or - `fenced`, put-with-signal's signal - all it follows."""
        operation = Operation()
        operation.issuer, operation.target, operation.kind = issuer, target, kind
        operation.slot, operation.call = slot, call
        operation.first, operation.limit, operation.label = first, limit, label
        operation.check_entered = self.case.on('ENTERED') and not entry_exempt
        operation.check_landed = self.case.on('LANDED')
        operation.immediate = self.nodes[issuer] == self.nodes[target]
        operation.index, operation.blockers = None, 0
        operation.epoch = self.epoch[issuer]
        if kind in ('add', 'word'):
            self.words.add((target, slot))
        if not operation.immediate:
            operation.index = len(self.flights)
            self.flights.append(operation)
            for earlier in self.since_quiet[issuer]:
                if earlier.target == target and (fenced or earlier.epoch < operation.epoch):
                    operation.blockers |= 1 << earlier.index
            self.since_quiet[issuer].append(operation)
            self.issuer_mask[issuer] = self.issuer_mask.get(issuer, 0) | (1 << operation.index)
        return operation


def land(operation, M, mu, payload):
    """Apply an operation at its target; returns the message of a property it breaks."""
    if operation.kind == 'add':
        key = word(operation.target, operation.slot)
        mu[key] = (mu[key] if key in mu else M[key]) + 1
        return None
    if operation.kind == 'word':
        mu[word(operation.target, operation.slot)] = payload
        return None
    values = unpack(payload)
    message = None
    if operation.check_entered and not (M[entered(operation.call)] >> operation.target) & 1:
        message = (f'ENTERED: member {operation.issuer} wrote member {operation.target}\'s '
                   f'buffer of call {operation.call} before it entered the call')
    elif operation.check_landed and (M[returned(operation.call)] >> operation.target) & 1:
        message = (f'LANDED: member {operation.issuer}\'s write to member {operation.target}\'s '
                   f'buffer of call {operation.call} landed after it returned')
    if operation.first < 0 or operation.first + len(values) > operation.limit:
        return message or (f'RESULT: member {operation.issuer} put {len(values)} cells at '
                           f'{operation.first} of member {operation.target}\'s buffer of call '
                           f'{operation.call}, which holds {operation.limit}')
    for i, v in enumerate(values):
        mu[cell(operation.target, operation.call, operation.first + i)] = v
    return message


class Member:
    """One member's steps, in the order active-set.c makes them."""

    def __init__(self, case, net, me):
        self.case, self.net, self.me = case, net, me
        self.size = case.size
        self.mutation = case.mutation
        self.tso = case.tso
        self.items = []
        net.start(me)

    # -- plumbing --
    def add(self, *instruction, fallible=False):
        """Append an engine instruction without its successor. A fallible one may leave
        a message in the local `bad`, and a local step after it ends the member on it."""
        self.items.append((instruction, fallible))

    def build(self):
        blocks, n = [], len(self.items)
        for i, (instruction, fallible) in enumerate(self.items):
            following = f'@s{i + 1}' if i + 1 < n else '@END'
            if fallible:
                blocks.append((f's{i}', instruction + (f'@b{i}',)))
                blocks.append((f'b{i}', ('br', lambda L: L['bad'] is not None, '@FAIL',
                                         following)))
            else:
                blocks.append((f's{i}', instruction + (following,)))
        blocks.append(('FAIL', ('check', lambda M, L: L['bad'], '@END')))
        blocks.append(('END', ('done',)))
        return assemble(blocks)

    def drain(self):
        """x86-TSO: a locked instruction empties this thread's store buffer first."""
        if self.tso:
            self.add('or', lambda L: DRAIN, lambda L: 0, '_')

    def own_reader(self, call_index, count, prefix):
        """Make the member's own dest cells [0, count) readable by the step that follows:
        straight from memory under SC, each loaded through the store buffer under TSO."""
        me = self.me
        if not self.tso:
            return lambda M, L, i: M[cell(me, call_index, i)]
        for i in range(count):
            self.add('load', lambda L, i=i: cell(me, call_index, i), f'{prefix}{i}')
        return lambda M, L, i: L[f'{prefix}{i}']

    # -- the library's primitives --
    def await_count(self, slot, count=1):
        """active-set.c await: wait for count adds to this member's word, then add the
        negative. One step under SC, two under TSO, the add a locked instruction."""
        key, need = word(self.me, slot), SYNC + count
        label = f'await pSync[{slot}]'
        mutation = self.mutation
        self.net.words.add((self.me, slot))

        def ready(w):
            return w == need if mutation == 'await_exact' else w >= need
        if self.tso:
            self.add('await', lambda M, L: ready(M[key]))
            if mutation == 'reset_not_subtract':
                self.add('store', lambda L: key, lambda L: SYNC, True)
            elif mutation != 'no_take_back':
                self.add('add', lambda L: key, lambda L: -count, '_')
            return

        def step(M, L):
            w = M[key]
            if not ready(w):
                return []
            if mutation == 'no_take_back':
                return [({}, {}, label)]
            if mutation == 'reset_not_subtract':
                return [({key: SYNC}, {}, label)]
            return [({key: w - count}, {}, label)]
        self.add('nd', step)

    def quiet(self):
        """pshmem_quiet: wait until every operation this member has issued has landed."""
        me, net = self.me, self.net
        self.drain()
        self.add('nd', lambda M, L: [] if M[FLIGHT] & net.issuer_mask.get(me, 0)
                 else [({}, {}, 'quiet')])
        net.quiet(me)

    def fence(self):
        if self.mutation != 'no_fence':
            self.net.fence(self.me)

    def issue(self, issues, label, drain=True):
        """One step that issues operations, in order: [(operation, payload function)]. A
        put or an atomic is a locked instruction (the intranode rmw) or is published
        through a connection's reservation (compare-and-swap) - serializing, under TSO."""
        if drain:
            self.drain()

        def step(M, L):
            mu, bad, flight = {}, None, M[FLIGHT]
            for operation, payload_fn in issues:
                payload = payload_fn(M, L) if payload_fn else None
                if operation.immediate:
                    bad = bad or land(operation, M, mu, payload)
                else:
                    flight |= 1 << operation.index
                    if payload_fn:
                        mu[pending(operation.index)] = payload
            if flight != M[FLIGHT]:
                mu[FLIGHT] = flight
            return [(mu, {'bad': bad} if bad else {}, label)]
        self.add('nd', step, fallible=True)

    def signal_operation(self, slot, target, fenced=False):
        return self.net.make(self.me, target, 'add',
                             f'deliver add to {target} pSync[{slot}] from {self.me}',
                             slot=slot, fenced=fenced)

    def signal(self, slot, target, drain=True):
        self.issue([(self.signal_operation(slot, target), None)],
                   f'add 1 to {target} pSync[{slot}]', drain)

    def arrival_slot(self, base, j):
        return base if self.mutation == 'shared_arrival' else base + j

    def arrive(self, root, v, base):
        """Signal v's parent on the word for v among its children, stores first (quiet)."""
        if self.mutation not in ('arrive_no_quiet', 'unfenced_arrive'):
            self.quiet()
        parent = (v - (v & -v) + root) % self.size
        self.signal(self.arrival_slot(base, child_index(v)), parent,
                    drain=self.mutation != 'unfenced_arrive')

    # -- the calls --
    def generate(self):
        calls = self.case.calls
        for call_index, call in enumerate(calls):
            self.enter(call_index, call)
            first = len(self.items)
            getattr(self, 'do_' + call[0])(call_index, *call[1:])
            if all(instruction[0] in ('set', 'gpre') for instruction, _ in self.items[first:]):
                # A set of one has nothing to wait for or to send; the engine commits a
                # folded ghost only with a real step of the same thread, so give it one.
                self.add('nd', lambda M, L: [({}, {}, 'a set of one: nothing to do')])
            self.leave(call_index, call, last=call_index == len(calls) - 1)
        return self

    def enter(self, call_index, call):
        """The call is entered: ghost ENTERED, folded into the member's first step, and
        its own block written into dest - folded as well under SC, a buffered store a
        cell under TSO."""
        me, size = self.me, self.size
        writes = {}
        if call[0] == 'fcollect':
            for t, v in enumerate(source_cells(call_index, call, me, size)):
                writes[cell(me, call_index, me * call[1] + t)] = v
        elif call[0] == 'reduce':
            for t, v in enumerate(source_cells(call_index, call, me, size)):
                writes[cell(me, call_index, t)] = v

        def step(M, L):
            mu = {} if self.tso else dict(writes)
            mu[entered(call_index)] = M[entered(call_index)] | (1 << me)
            return mu, {}
        self.add('gpre', step)
        if self.tso:
            for key, value in writes.items():
                self.add('store', lambda L, key=key: key, lambda L, value=value: value, False)

    def leave(self, call_index, call, last):
        """The call returns: ghost RETURNED, folded into the member's last step, with the
        checks of BARRIER and RESULT."""
        me, size = self.me, self.size
        want = expected(call_index, call, me, size)
        kind = call[0]
        check_barrier, check_result = self.case.on('BARRIER'), self.case.on('RESULT')
        read = self.own_reader(call_index, len(want), 'r') if check_result else None

        def step(M, L):
            mu = {returned(call_index): M[returned(call_index)] | (1 << me)}
            if last:
                mu[FINISHED] = M[FINISHED] + 1
            message = None
            if kind in ('sync', 'barrier'):
                for other in range(size):
                    if check_barrier and not (M[entered(call_index)] >> other) & 1:
                        message = (f'BARRIER: member {me} left call {call_index} ({kind}) before '
                                   f'member {other} entered it')
                        break
            elif check_result:
                got = [read(M, L, i) for i in range(len(want))]
                if got != want:
                    message = (f'RESULT: member {me} returned from call {call_index} ({kind}) with '
                               f'{got}, not {want}')
            return mu, ({'bad': message} if message else {})
        self.add('gpost', step, fallible=True)

    def do_sync(self, call_index):
        self.barrier(quiet=False)

    def do_barrier(self, call_index):
        self.barrier(quiet=True)

    def barrier(self, quiet):
        """osoe_active_set_barrier: a dissemination - in round k signal the member 2^k
        ranks on, wait for the member 2^k back. A barrier quiets first, a sync does not."""
        if quiet and self.mutation != 'barrier_no_quiet':
            self.quiet()
        rounds, k = 0, 1
        while k < self.size:
            partner = (self.me + (1 if self.mutation == 'wrong_partner' else k)) % self.size
            self.signal(SLOT_BARRIER + rounds, partner)
            self.await_count(SLOT_BARRIER + rounds)
            k <<= 1
            rounds += 1

    def do_putbarrier(self, call_index, n):
        """The application puts to its right-hand neighbour, then calls shmem_barrier."""
        me, size = self.me, self.size
        right = (me + 1) % size
        operation = self.net.make(me, right, 'cells',
                                  f'deliver put to {right} call {call_index} from {me}',
                                  call=call_index, first=0, limit=n, entry_exempt=True)
        block = pack(source_cells(call_index, ('putbarrier', n), me, size))
        self.issue([(operation, lambda M, L: block)], f'put to {right}')
        self.barrier(quiet=True)

    def up(self, root, base):
        v = (self.me - root) % self.size
        steps = child_steps(v, self.size)
        if self.mutation == 'up_early':
            steps = steps[:-1]
        for j, k in enumerate(steps):
            self.await_count(self.arrival_slot(base, j))
        if v:
            self.arrive(root, v, base)

    def put_signal(self, call_index, target, slot, count_fn, from_source, limit):
        """pshmem_putmem_signal: the bytes, then the signal, placed after them."""
        me = self.me
        data = self.net.make(me, target, 'cells',
                             f'deliver put to {target} call {call_index} from {me}',
                             call=call_index, first=0, limit=limit)
        signal = self.signal_operation(slot, target, fenced=self.mutation != 'signal_unordered')
        read = None if from_source is not None else self.own_reader(call_index, limit, 'p')

        def payload(M, L):
            n = count_fn(L)
            if from_source is not None:
                return pack(from_source[:n])
            return pack([read(M, L, i) for i in range(n)])
        self.issue([(data, payload), (signal, None)],
                   f'put to {target} then add 1 to pSync[{slot}]')

    def down(self, root, call_index, from_source, count_fn, slot, limit):
        v = (self.me - root) % self.size
        if v and self.mutation != 'no_wait_release':
            self.await_count(slot)
        steps = child_steps(v, self.size)
        for k in (steps if self.mutation == 'down_smallest_first' else reversed(steps)):
            self.put_signal(call_index, (v + k + root) % self.size, slot, count_fn, from_source,
                            limit)

    def do_broadcast(self, call_index, root, n):
        if self.mutation != 'no_up':
            self.up(root, SLOT_ARRIVED)
        source = None
        if self.me == root:
            source = source_cells(call_index, self.case.calls[call_index], root, self.size)
        self.down(root, call_index, source, lambda L: n, SLOT_RELEASED, n)

    def get_cells(self, target, call_index, first_fn, count_fn, limit, label, static,
                  combine=False):
        """pshmem_getmem into this member's own dest - or, for a reduction, the child's
        partial combined into it: the child's cells are sampled at one instant. Under
        TSO the copy-out is a buffered store a cell, `static` of them."""
        me = self.me
        check_returned = self.case.on('RETURNED')

        def step(M, L):
            first, count = first_fn(L), count_fn(L)
            if first is None or count is None or first < 0 or count < 0 or first + count > limit:
                return [({}, {'bad': f'RESULT: member {me} read cells {first}+{count} of a '
                                     f'buffer of {limit} (call {call_index})'}, label)]
            message = None
            if check_returned and (M[returned(call_index)] >> target) & 1:
                message = (f'RETURNED: member {me} read member {target}\'s buffer of call '
                           f'{call_index} after it returned')
            lu = {'bad': message} if message else {}
            if self.tso:
                lu.update({f'g{i}': M[cell(target, call_index, first + i)] for i in range(static)})
                return [({}, lu, label)]
            mu = {}
            for i in range(first, first + count):
                v = M[cell(target, call_index, i)]
                mu[cell(me, call_index, i)] = M[cell(me, call_index, i)] + v if combine else v
            return [(mu, lu, label)]
        self.add('nd', step, fallible=True)
        if self.tso:
            for i in range(static):
                key = lambda L, i=i: cell(me, call_index, first_fn(L) + i)
                if combine:
                    self.add('load', key, f'o{i}')
                    self.add('store', key, lambda L, i=i: L[f'o{i}'] + L[f'g{i}'], False)
                else:
                    self.add('store', key, lambda L, i=i: L[f'g{i}'], False)

    def gather_then_spread(self, call_index, arrived, released, whole_fn, limit, static, skip=()):
        """Gather into rank 0 - each parent gets its children's ranges out of their dests
        - then the whole of rank 0's dest down to everyone."""
        v = self.me
        steps = child_steps(v, self.size)

        def gets():
            for j, k in enumerate(steps):
                if not (skip and skip[j]):
                    self.get_cells(v + k, call_index, lambda L, j=j: L[f'off{j}'],
                                   lambda L, j=j: L[f'len{j}'], limit,
                                   f'get call {call_index} from {v + k}', static[j])
        if self.mutation == 'gather_after_release':
            for j in range(len(steps)):
                self.await_count(self.arrival_slot(arrived, j))
            if v:
                self.arrive(0, v, arrived)
            self.down(0, call_index, None, whole_fn, released, limit)
            gets()
            return
        for j, k in enumerate(steps):
            self.await_count(self.arrival_slot(arrived, j))
            if not (skip and skip[j]):
                self.get_cells(v + k, call_index, lambda L, j=j: L[f'off{j}'],
                               lambda L, j=j: L[f'len{j}'], limit,
                               f'get call {call_index} from {v + k}', static[j])
        if v:
            self.arrive(0, v, arrived)
        self.down(0, call_index, None, whole_fn, released, limit)

    def do_fcollect(self, call_index, n):
        v, size = self.me, self.size
        steps = child_steps(v, size)
        for j, k in enumerate(steps):
            self.add('set', f'off{j}', lambda L, o=(v + k) * n: o)
            extent = span_of(v + k, size) if self.mutation == 'wrong_subtree' \
                else subtree_size(v + k, size)
            self.add('set', f'len{j}', lambda L, o=extent * n: o)
        limit = size * n
        self.gather_then_spread(call_index, SLOT_ARRIVED, SLOT_RELEASED, lambda L: limit, limit,
                                [subtree_size(v + k, size) * n for k in steps],
                                skip=[n == 0 for k in steps])

    def do_reduce(self, call_index, n):
        v = self.me
        for j, k in enumerate(child_steps(v, self.size)):
            self.await_count(self.arrival_slot(SLOT_ARRIVED, j))
            if n:
                self.get_cells(v + k, call_index, lambda L: 0, lambda L: n, n,
                               f'get call {call_index} from {v + k}', n, combine=True)
        if v:
            self.arrive(0, v, SLOT_ARRIVED)
        self.down(0, call_index, None, lambda L: n, SLOT_RELEASED, n)

    def get_word(self, target, slot, local, label):
        """pshmem_long_g: one word of another member's pSync, read at one instant."""
        key = word(target, slot)
        self.net.words.add((target, slot))
        self.add('nd', lambda M, L: [({}, {local: M[key]}, label)])

    def read_places(self):
        """offset = fetch(OFFSET); whole = fetch(GRAND); then the three words back to
        SHMEM_SYNC_VALUE: atomics on this member's own PE, so locked or seq_cst."""
        me, mutation = self.me, self.mutation
        if self.tso:
            self.add('load', lambda L: word(me, SLOT_OFFSET), 'offset')
            self.add('load', lambda L: word(me, SLOT_GRAND), 'whole')
            if mutation != 'no_reset':
                for slot in (SLOT_TOTAL, SLOT_OFFSET, SLOT_GRAND):
                    self.add('store', lambda L, slot=slot: word(me, slot), lambda L: SYNC, True)
            return

        def step(M, L):
            mu = {}
            if mutation != 'no_reset':
                mu = {word(me, slot): SYNC for slot in (SLOT_TOTAL, SLOT_OFFSET, SLOT_GRAND)}
            return [(mu, {'offset': M[word(me, SLOT_OFFSET)], 'whole': M[word(me, SLOT_GRAND)]},
                     'read the offset and the whole, reset TOTAL, OFFSET and GRAND')]
        self.add('nd', step)

    def do_collect(self, call_index, counts):
        v, size, me = self.me, self.size, self.me
        steps = child_steps(v, size)
        limit = sum(counts)
        own = counts[me]

        def subtree_count(k):
            return sum(counts[v + k + i] for i in range(subtree_size(v + k, size)))
        for j, k in enumerate(steps):
            if self.mutation == 'read_total_early':
                self.get_word(v + k, SLOT_TOTAL, f'len{j}', f'read pSync[{SLOT_TOTAL}] of {v + k}')
                self.await_count(self.arrival_slot(SLOT_ARRIVED, j))
            else:
                self.await_count(self.arrival_slot(SLOT_ARRIVED, j))
                self.get_word(v + k, SLOT_TOTAL, f'len{j}', f'read pSync[{SLOT_TOTAL}] of {v + k}')
        self.add('set', 'total',
                 lambda L, n=len(steps): own + sum(L[f'len{j}'] for j in range(n)))
        if v:
            total_key = word(me, SLOT_TOTAL)
            self.net.words.add((me, SLOT_TOTAL))

            def set_total():
                if self.tso:
                    self.add('store', lambda L: total_key, lambda L: L['total'], True)
                else:
                    self.add('gpre', lambda M, L: ({total_key: L['total']}, {}))
            if self.mutation != 'total_after_arrive':
                set_total()                     # before the signal its reader waits for
            self.arrive(0, v, SLOT_ARRIVED)
            if self.mutation == 'total_after_arrive':
                set_total()
            if self.mutation == 'offsets_early':
                self.read_places()
                self.await_count(SLOT_RELEASED)
            else:
                self.await_count(SLOT_RELEASED)
                self.read_places()
        else:
            self.add('set', 'offset', lambda L: 0)
            self.add('set', 'whole', lambda L: L['total'])
        self.add('set', 'at', lambda L: L['offset'] + own)
        for j, k in enumerate(steps):
            child = v + k
            self.add('set', f'off{j}', lambda L: L['at'])
            offset_put = self.net.make(me, child, 'word', f'deliver OFFSET to {child} from {me}',
                                       slot=SLOT_OFFSET)
            grand_put = self.net.make(me, child, 'word', f'deliver GRAND to {child} from {me}',
                                      slot=SLOT_GRAND)
            self.fence()
            release = self.signal_operation(SLOT_RELEASED, child)
            self.issue([(offset_put, lambda M, L: L['at']), (grand_put, lambda M, L: L['whole']),
                        (release, None)], f'put OFFSET and GRAND to {child}, fence, release it')
            if self.mutation != 'offsets_not_advanced':
                self.add('set', 'at', lambda L, j=j: L['at'] + L[f'len{j}'])

        # memmove(dest + offset, source, bytes): plain stores to this member's own dest
        if self.tso:
            for t in range(own):
                self.add('store', lambda L, t=t: cell(me, call_index, L['offset'] + t),
                         lambda L, t=t: tag(call_index, me, t), False)
        else:
            def memmove(M, L):
                offset = L['offset']
                if offset is None or offset < 0 or offset + own > limit:
                    return {}, {'bad': f'RESULT: member {me} places its block of call '
                                       f'{call_index} at {offset}, in a buffer of {limit}'}
                return {cell(me, call_index, offset + t): tag(call_index, me, t)
                        for t in range(own)}, {}
            self.add('gpre', memmove, fallible=True)
        self.gather_then_spread(call_index, SLOT_ARRIVED_DATA, SLOT_RELEASED_DATA,
                                lambda L: L['whole'], limit, [subtree_count(k) for k in steps],
                                skip=[subtree_count(k) == 0 for k in steps])

    def do_alltoall(self, call_index, n):
        me, size = self.me, self.size
        for step in range(size):
            j = (me + 1 + step) % size
            operation = self.net.make(me, j, 'cells',
                                      f'deliver block to {j} call {call_index} from {me}',
                                      call=call_index, first=me * n, limit=size * n,
                                      entry_exempt=self.mutation != 'alltoall_entered')
            block = pack([tag(call_index, me, j * n + t) for t in range(n)])
            self.issue([(operation, lambda M, L, block=block: block)], f'put the block for {j}')
        self.barrier(quiet=self.mutation != 'barrier_no_quiet')


class Case:
    def __init__(self, mutation, size, nodes, calls, checks=None, tso=False):
        assert mutation in MUTATIONS, mutation
        self.mutation, self.size, self.calls, self.tso = mutation, size, calls, tso
        self.nodes = nodes or tuple(range(size))
        self.checks = checks

    def on(self, prop):
        return self.checks is None or prop in self.checks


def build(case):
    net = Network(case)
    members = [Member(case, net, m).generate() for m in range(case.size)]
    threads = [member.build() for member in members]

    def deliver(M, L):
        flight = M[FLIGHT]
        alternatives, rest = [], flight
        while rest:
            low = rest & -rest
            rest ^= low
            operation = net.flights[low.bit_length() - 1]
            if flight & operation.blockers:
                continue
            mu = {FLIGHT: flight ^ low}
            payload = None
            if operation.kind != 'add':
                payload = M[pending(operation.index)]
                mu[pending(operation.index)] = 0
            message = land(operation, M, mu, payload)
            alternatives.append((mu, {'bad': message} if message else {}, operation.label))
        if not alternatives and not flight and M[FINISHED] == case.size:
            alternatives.append(({}, {'finished': 1},
                                 'every member has returned and nothing is in flight'))
        return alternatives

    def final(M, L):
        if not case.on('PSYNC'):
            return None
        for member, slot in sorted(net.words):
            v = M[word(member, slot)]
            if v != SYNC:
                return (f'PSYNC: member {member}\'s pSync[{slot}] is {v} at the end, not '
                        f'SHMEM_SYNC_VALUE')
        return None

    threads.append(assemble([
        ('LOOP', ('nd', deliver, '@CHECK')),
        ('CHECK', ('br', lambda L: L['bad'] is not None, '@FAIL', '@FINISH')),
        ('FINISH', ('br', lambda L: L['finished'], '@FINAL', '@LOOP')),
        ('FINAL', ('check', final, '@END')),
        ('FAIL', ('check', lambda M, L: L['bad'], '@END')),
        ('END', ('done',))]))
    mem = {FLIGHT: 0, FINISHED: 0, DRAIN: 0}
    for member, slot in sorted(net.words):
        mem[word(member, slot)] = SYNC
    for call_index, call in enumerate(case.calls):
        mem[entered(call_index)] = mem[returned(call_index)] = 0
        for m in range(case.size):
            for i in range(dest_length(call, case.size)):
                mem[cell(m, call_index, i)] = 0
    for operation in net.flights:
        mem[pending(operation.index)] = 0
    return threads, mem


def name_of(case):
    nodes = '' if case.nodes == tuple(range(case.size)) else \
        f' nodes={"".join(map(str, case.nodes))}'
    only = '' if case.checks is None else f' (only {"+".join(sorted(case.checks))})'
    return (f'active_set/{case.mutation} size={case.size}{nodes} '
            f'calls: {" ".join(describe(call) for call in case.calls)}{only}')


def execute(case, max_seconds=1800):
    threads, mem = build(case)
    size = case.size
    return explore(name_of(case), threads, mem, tso=case.tso, idle_ok=lambda i, M, L: False,
                   done=lambda M: M[FINISHED] == size and not M[FLIGHT],
                   max_seconds=max_seconds, liveness_max_states=2_500_000)


sync, barrier = ('sync',), ('barrier',)


def broadcast(root, n=1):
    return ('broadcast', root, n)


def fcollect(n=1):
    return ('fcollect', n)


def reduce(n=1):
    return ('reduce', n)


def collect(*counts):
    return ('collect', counts)


def alltoall(n=1):
    return ('alltoall', n)


def put_barrier(n=1):
    return ('putbarrier', n)


def entry(mutation, size, calls, expect, nodes=None, only=None, tso=False):
    return Case(mutation, size, nodes, calls, only and frozenset(only.split()), tso), expect


# Every kind of call in a row, for a few sizes.
EVERY_KIND_FOUR = [fcollect(), broadcast(1), reduce(), collect(1, 0, 2, 1), broadcast(3),
                   sync, barrier]
MIXED_FIVE = [fcollect(), broadcast(3), reduce(), collect(1, 0, 2, 1, 1), sync]
MIXED_SIX = [fcollect(), broadcast(4), reduce(), collect(1, 0, 2, 1, 1, 0), sync]
MIXED_SEVEN = [fcollect(), broadcast(3), reduce(), collect(1, 0, 2, 1, 1, 0, 1), broadcast(5)]
EIGHT_COUNTS = (1, 0, 2, 1, 0, 1, 2, 1)

CASES = [
    # --- the design ----------------------------------------------------------------------
    # The smallest sets: one member, whose every call is the trivial one, and two.
    entry('design', 1, [put_barrier(), sync, barrier, broadcast(0), fcollect(), reduce(),
                        collect(2), alltoall(), sync], True),
    entry('design', 2, [sync, sync, sync, broadcast(0), broadcast(1), fcollect(), reduce(),
                        collect(1, 2), alltoall(), put_barrier(), barrier], True),
    # The dissemination barrier, where a member's signal for its next call can land before
    # this call's has been taken: three calls on four members, three rounds on five and six.
    entry('design', 3, [sync, barrier, sync, barrier], True),
    entry('design', 4, [sync, sync, sync], True),
    entry('design', 4, [barrier, sync, barrier], True),
    entry('design', 5, [sync, sync], True),
    entry('design', 5, [sync, broadcast(2), sync], True),
    entry('design', 6, [sync], True),
    # The application's puts, then shmem_barrier: the quiet that starts it.
    entry('design', 3, [put_barrier(), sync, put_barrier()], True),
    entry('design', 4, [put_barrier(), barrier], True),
    entry('design', 5, [put_barrier(), sync], True),
    # Broadcast from every root, the tree turned each time; from a root past the middle of
    # seven and eight members, where a node has two children and a grandchild.
    entry('design', 3, [broadcast(0), broadcast(1), broadcast(2)], True),
    entry('design', 4, [broadcast(0), broadcast(1), broadcast(2), broadcast(3)], True),
    entry('design', 7, [broadcast(0), broadcast(6), broadcast(3), broadcast(1)], True),
    entry('design', 8, [broadcast(7), fcollect(), broadcast(2)], True),
    # fcollect, collect and the reductions back to back on one pSync, their words shared.
    entry('design', 7, [fcollect(), fcollect(), fcollect()], True),
    entry('design', 7, [fcollect(2), broadcast(5, 2), reduce(2)], True),
    entry('design', 4, [collect(1, 0, 2, 1), collect(2, 1, 0, 1)], True),
    entry('design', 7, [collect(1, 0, 2, 1, 1, 0, 1), broadcast(4)], True),
    entry('design', 8, [collect(*EIGHT_COUNTS)], True),
    entry('design', 8, [collect(*EIGHT_COUNTS), reduce(), broadcast(6)], True),
    entry('design', 5, [fcollect(0), reduce(0), broadcast(2, 0), collect(0, 0, 0, 0, 0)], True),
    # The root with four children and a tree of depth three: nine and ten members.
    entry('design', 9, [fcollect(), broadcast(5)], True),
    entry('design', 10, [fcollect()], True),
    entry('design', 10, [reduce()], True),
    entry('design', 10, [broadcast(6)], True),
    # alltoall: puts that land in any order, then a quiet and a barrier.
    entry('design', 3, [alltoall(), alltoall(), sync], True),
    entry('design', 3, [alltoall(2), sync], True),
    entry('design', 3, [fcollect(), alltoall(), broadcast(1), alltoall(), reduce()], True),
    entry('design', 3, [fcollect(), alltoall(), broadcast(1), alltoall(), reduce()], True,
          nodes=(0, 0, 1)),
    entry('design', 3, [put_barrier(2), alltoall(), put_barrier()], True),
    entry('design', 4, [alltoall()], True),
    # Every kind in a row; on one node, on two, and on three or four.
    entry('design', 6, [fcollect(), broadcast(3), reduce(), collect(1, 0, 2, 1, 1, 0),
                        broadcast(1)], True),
    entry('design', 4, EVERY_KIND_FOUR, True),
    entry('design', 4, [fcollect(), alltoall(), broadcast(1), reduce(), collect(1, 0, 2, 1),
                        sync], True),
    entry('design', 5, MIXED_FIVE, True),
    entry('design', 5, MIXED_FIVE, True, nodes=(0, 0, 1, 1, 2)),
    entry('design', 5, MIXED_FIVE, True, nodes=(0, 1, 1, 0, 1)),
    entry('design', 6, MIXED_SIX, True, nodes=(0, 0, 0, 1, 1, 1)),
    entry('design', 6, MIXED_SIX, True, nodes=(0, 0, 0, 0, 0, 0)),
    entry('design', 4, [sync, sync, sync], True, nodes=(0, 0, 1, 1)),
    entry('design', 4, [sync, sync, sync], True, nodes=(0, 1, 0, 1)),

    # --- the design under x86-TSO: its plain stores buffered -----------------------------
    entry('design', 3, [sync, sync], True, tso=True),
    entry('design', 4, [sync, sync], True, tso=True),
    entry('design', 3, [broadcast(1), fcollect()], True, tso=True),
    entry('design', 4, [fcollect(), reduce()], True, tso=True),
    entry('design', 4, [broadcast(2), fcollect()], True, tso=True),
    entry('design', 3, [fcollect(), fcollect(), fcollect()], True, tso=True),
    entry('design', 3, [collect(1, 0, 2), collect(0, 2, 1)], True, tso=True),
    entry('design', 4, [collect(1, 0, 2, 1)], True, tso=True),
    entry('design', 3, [put_barrier(), sync], True, tso=True),
    entry('design', 3, [alltoall()], True, tso=True),
    entry('design', 4, [fcollect(), reduce()], True, nodes=(0, 0, 1, 1), tso=True),
    entry('design', 4, [fcollect(), broadcast(1), reduce(), collect(1, 0, 2, 1)], True, tso=True),
    entry('design', 4, [sync, fcollect(), sync], True, tso=True),
    entry('design', 4, [broadcast(1), broadcast(2), broadcast(3)], True, tso=True),
    entry('design', 4, [fcollect(), reduce(), collect(1, 0, 2, 1)], True, nodes=(0, 1, 0, 1),
          tso=True),
    entry('design', 3, [alltoall(), fcollect()], True, tso=True),
    entry('design', 2, [sync, broadcast(1), fcollect(), reduce(), collect(0, 2), alltoall(),
                        put_barrier()], True, tso=True),
    entry('design', 5, [broadcast(3), fcollect()], True, tso=True),
    entry('design', 5, [fcollect(), reduce()], True, tso=True),

    # --- breakages: each must fail the property it names ---------------------------------
    entry('wrong_partner', 4, [sync], 'BARRIER'),
    entry('no_take_back', 3, [sync, sync], 'BARRIER'),
    entry('no_take_back', 3, [sync, sync], 'PSYNC', only='PSYNC'),
    entry('reset_not_subtract', 4, [sync, sync], 'STUCK'),
    entry('await_exact', 4, [sync, sync], 'STUCK'),
    entry('barrier_no_quiet', 3, [put_barrier()], 'RESULT'),
    entry('barrier_no_quiet', 3, [alltoall()], 'RESULT'),
    entry('no_fence', 4, [collect(1, 1, 1, 1)], 'RESULT'),
    entry('offsets_early', 4, [collect(1, 1, 1, 1)], 'RESULT'),
    entry('read_total_early', 3, [collect(1, 1, 1)], 'RESULT'),
    entry('total_after_arrive', 3, [collect(1, 1, 1)], 'RESULT'),
    entry('no_reset', 3, [collect(1, 1, 1)], 'PSYNC'),
    entry('signal_unordered', 4, [fcollect()], 'RESULT'),
    entry('signal_unordered', 4, [fcollect()], 'LANDED', only='LANDED'),
    entry('shared_arrival', 4, [fcollect()], 'RESULT'),
    entry('shared_arrival', 4, [reduce()], 'RESULT'),
    entry('no_wait_release', 4, [fcollect()], 'RESULT'),
    entry('no_up', 4, [broadcast(0)], 'ENTERED'),
    entry('up_early', 4, [broadcast(0)], 'PSYNC'),
    entry('up_early', 4, [broadcast(0)], 'ENTERED', only='ENTERED'),
    entry('wrong_subtree', 6, [fcollect()], 'RESULT'),
    entry('offsets_not_advanced', 4, [collect(1, 1, 1, 1)], 'RESULT'),
    entry('gather_after_release', 4, [fcollect()], 'RESULT'),
    entry('gather_after_release', 4, [fcollect()], 'RETURNED', only='RETURNED'),
    entry('barrier_no_quiet', 3, [put_barrier()], 'RESULT', tso=True),
    entry('signal_unordered', 4, [fcollect()], 'RESULT', tso=True),
    entry('no_up', 4, [broadcast(0)], 'ENTERED', tso=True),
    entry('alltoall_entered', 3, [alltoall()], 'ENTERED'),
    entry('unfenced_arrive', 4, [fcollect()], 'RESULT', tso=True),
    entry('unfenced_arrive', 4, [reduce()], 'RESULT', tso=True),

    # --- variants that hold, each for a reason of its own --------------------------------
    entry('arrive_no_quiet', 4, EVERY_KIND_FOUR, True),
    entry('arrive_no_quiet', 8, [collect(*EIGHT_COUNTS), reduce(), broadcast(6)], True),
    entry('arrive_no_quiet', 4, [fcollect(), broadcast(1), reduce()], True, tso=True),
    entry('unfenced_arrive', 4, [fcollect(), broadcast(1), reduce()], True),
    entry('await_exact', 7, MIXED_SEVEN, True),
    entry('reset_not_subtract', 7, MIXED_SEVEN, True),
    entry('down_smallest_first', 7, [fcollect(), broadcast(3), reduce()], True),
]


def main():
    want = select(sys.argv)
    ok = True
    if want('active_set/tree arithmetic'):
        message = check_tree(range(1, 131))
        print(f'{"active_set/tree arithmetic sizes 1..130":<60} -> {message or "ok"}')
        ok &= message is None
    for case, expect in CASES:
        if want(name_of(case)):
            ok &= report(execute(case), expect=expect, tso=case.tso)
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
