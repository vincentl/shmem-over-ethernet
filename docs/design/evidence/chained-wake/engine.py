#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Exhaustive interleaving engine for osoe concurrency models (design §4.11).

Descends from the review's watch model
(docs/reviews/evidence/semantics/watch_model.py): threads are small programs
of instructions over shared memory, and every interleaving is explored. It
adds ghost state and assertions, so a model states what a routine guarantees
as well as how it runs, and it is built to reach tens of millions of states.

Memory models
  SC   sequential consistency: every store is visible at once.
  TSO  x86-TSO: each thread has a FIFO store buffer. A plain store is buffered
       and committed by a nondeterministic flush step. A serializing
       instruction (locked atomic, CAS, seq_cst store, futex call) runs only
       with the thread's own buffer empty. A thread reads its own buffer first.

Futex: futex_wait(addr, val) compares memory, not other threads' buffers, and
sleeps; futex_wake(addr) wakes every sleeper on addr.

Instructions (labels starting with '@' are resolved by assemble)
  ('set',   var, fn(L), nxt)                    local
  ('br',    cond(L), t, f)                      local
  ('load',  addr(L), var, nxt)
  ('store', addr(L), val(L), serial, nxt)
  ('add',   addr(L), delta(L), var, nxt)        locked; var <- new value
  ('cas',   addr(L), old(L), new(L), var, nxt)  locked; var <- success
  ('or',    addr(L), bits(L), var, nxt)         locked; var <- old value
  ('andn',  addr(L), bits(L), var, nxt)         locked; clears bits; var <- old value
  ('fwait', addr(L), val(L), nxt)
  ('fwake', addr(L), nxt)
  ('fwaitm', addr(L), val(L), mask(L), nxt)       futex_wait with a bitset (futex2's mask)
  ('fwaken', addr(L), n(L), mask(L), nxt)         wake up to n sleepers whose bitset meets
            mask; which ones is the kernel's choice, so one successor per choice.
            A plain fwait matches every mask, and a plain fwake wakes every
            sleeper on the address, masked or not, as FUTEX_BITSET_MATCH_ANY does.
  ('ghost', fn(M, L) -> (mem_upd, loc_upd), nxt)   a step of its own
  ('gpre',  fn(M, L) -> (mem_upd, loc_upd), nxt)   atomic with the thread's next step
  ('gpost', fn(M, L) -> (mem_upd, loc_upd), nxt)   atomic with the thread's previous step
  ('nd',    fn(M, L) -> [(mem_upd, loc_upd, label)], nxt)
            one successor per alternative; blocked while the list is empty
  ('await', cond(M, L), nxt)                    abstract blocking wait
  ('check', fn(M, L) -> None | message, nxt)
  ('done',)
A var of '_' discards the result.

M reads committed memory: M[key] and M.get(key, default); absent keys read 0.
L reads locals: L[name]; absent or deleted locals read None. A 'set' whose fn
returns DEL deletes the local, so states that differ only in dead locals merge.
Ghost state is memory under keys whose first element starts with '$'; it models
no instruction and is never buffered.

State-space reductions. Each keeps every violation, lost wake and stuck state
of the unreduced model:
  * Local steps ('set', 'br') run at once. They commute with every step of
    every other thread.
  * 'gpre' and 'gpost' fold a ghost into an adjacent real step of the same
    thread. Use them for the instant a call starts (gpre) or returns (gpost),
    and for ghost bookkeeping read only by checks and by enabling conditions
    that it can only make more permissive. For every schedule there is one in
    which that instant is adjacent to the thread's own step, and a check that
    fails in the first also fails in the second.
  * symmetric=[[i, j, ...], ...] declares threads with identical programs on
    which no state depends by index. States equal up to permuting them merge.

Storage. A state is a tuple of ints for memory (one fixed slot per address)
and one small tuple per thread. The visited set holds a 96-bit BLAKE2b
fingerprint of each state (hash compaction, as in SPIN). The chance that any
state is wrongly skipped is below n^2 / 2^97 for n states, and is printed
with each result. exact=True stores full states instead.

Liveness. LOST WAKE and STUCK are reported only at a state with no successor,
so a model in which one thread sleeps forever while another cycles forever has
no such state and used to be reported `ok`. After a complete search the engine
also checks AG EF done: from **every** reachable state some *finished* state
must still be reachable. A state from which none is is *trapped*, and the
verdict is TRAPPED with a trace to it. Finished means `done(M)` when a model
passes one, and otherwise that every thread is at 'done' or is awake at a
location its `idle_ok` allows. The check needs the whole state graph, which
costs two machine words per edge and three per state, so it runs only while the
model stays under `liveness_max_states`; past that the result line says the
check did not run. `liveness=False` turns it off.

Guards. A progress line on stderr every `progress` seconds (states, rate,
stack, depth, largest memory value), and TIME LIMIT, STATE LIMIT and DEPTH LIMIT verdicts
with the trace that reached them. A model with an unbounded loop or counter
reports itself instead of hanging: its depth or its largest value keeps
growing in the progress lines.
"""
import hashlib
import marshal
from array import array
import sys
import time

REDUCE = True     # False turns gpre/gpost into ordinary ghost steps and ignores symmetry (selftest.py)
DEL = object()
NONE = -(1 << 62)                     # stored value of an absent local
RUN, SLEEP = 0, 1
SERIAL = frozenset(('add', 'cas', 'or', 'andn', 'fwait', 'fwake', 'fwaitm', 'fwaken'))
MASKED = 1 << 20          # a masked sleeper's slot: MASKED * (addr + 1) + mask


def _sleeps_on(sl, a):
    """(does a sleeper with slot `sl` sleep on address a, its mask or None)"""
    if sl == a:
        return True, None
    if sl >= MASKED and sl // MASKED - 1 == a:
        return True, sl % MASKED
    return False, None
LOCAL = frozenset(('set', 'br', 'gpost'))


def assemble(blocks):
    idx = {}
    for n, (lab, _) in enumerate(blocks):
        if lab in idx:
            raise ValueError(f'duplicate label {lab}')
        idx[lab] = n
    out = []
    for lab, ins in blocks:
        ins = list(ins)
        for k, x in enumerate(ins):
            if isinstance(x, str) and x.startswith('@'):
                ins[k] = idx[x[1:]]
        out.append(tuple(ins))
    return out


class _M:
    __slots__ = ('ex', 'mem')

    def __init__(self, ex, mem):
        self.ex, self.mem = ex, mem

    def get(self, key, default=0):
        i = self.ex.amap.get(key)
        if i is None or i >= len(self.mem):
            return default
        return self.mem[i]

    __getitem__ = get


class _L:
    __slots__ = ('names', 'vals')

    def __init__(self, names, vals):
        self.names, self.vals = names, vals

    def __getitem__(self, k):
        i = self.names.get(k)
        if i is None or i >= len(self.vals):
            return None
        v = self.vals[i]
        return None if v == NONE else v

    get = __getitem__


class Result:
    def __init__(self, name):
        self.name = name
        self.states = 0
        self.terminals = 0
        self.max_depth = 0
        self.verdict = 'ok'
        self.message = None
        self.trace = None
        self.max_ghost = {}
        self.seconds = 0.0
        self.exact = False
        self.liveness = None

    @property
    def ok(self):
        return self.verdict == 'ok'

    @property
    def complete(self):
        return self.verdict not in ('TIME LIMIT', 'STATE LIMIT', 'DEPTH LIMIT')


class Explorer:
    def __init__(self, threads, mem0, tso=False, symmetric=()):
        if not REDUCE:
            threads = [[('ghost',) + ins[1:] if ins[0] in ('gpre', 'gpost') else ins for ins in prog]
                       for prog in threads]
            symmetric = ()
        self.progs = threads
        self.tso = tso
        self.amap = {}
        self.akeys = []
        self.lmaps = [{} for _ in threads]
        self.symmetric = [sorted(g) for g in symmetric]
        for g in self.symmetric:
            # threads that may be swapped must agree on where each local lives
            for j in g:
                if threads[j] is not threads[g[0]] and len(threads[j]) != len(threads[g[0]]):
                    raise ValueError(f'symmetric threads {g} run different programs')
                self.lmaps[j] = self.lmaps[g[0]]
        mem = ()
        for k, v in mem0.items():
            mem = self._mset(mem, self._addr(k), v)
        ths = tuple((0, (), RUN, -1, ()) for _ in threads)
        for i in range(len(ths)):
            mem, ths = self._norm(mem, ths, i)
        self.init = self._canon((mem, ths))

    # ---- memory and locals ---------------------------------------------
    def _addr(self, key):
        i = self.amap.get(key)
        if i is None:
            i = self.amap[key] = len(self.akeys)
            self.akeys.append(key)
        return i

    @staticmethod
    def _mget(mem, i):
        return mem[i] if i < len(mem) else 0

    @staticmethod
    def _mset(mem, i, v):
        n = len(mem)
        if i < n:
            return mem[:i] + (v,) + mem[i + 1:]
        return mem + (0,) * (i - n) + (v,)

    def _mupd(self, mem, upd):
        for k, v in upd.items():
            mem = self._mset(mem, self._addr(k), v)
        return mem

    def _lset(self, t, vals, name, v):
        if name == '_':
            return vals
        names = self.lmaps[t]
        i = names.get(name)
        if i is None:
            i = names[name] = len(names)
        if v is DEL or v is None:
            v = NONE
        n = len(vals)
        if i < n:
            return vals[:i] + (v,) + vals[i + 1:]
        return vals + (NONE,) * (i - n) + (v,)

    def _lupd(self, t, vals, upd):
        for k, v in upd.items():
            vals = self._lset(t, vals, k, v)
        return vals

    # ---- local steps ----------------------------------------------------
    def _norm(self, mem, ths, i):
        pc, loc, st, sl, buf = ths[i]
        if st != RUN:
            return mem, ths
        prog = self.progs[i]
        steps = 0
        while prog[pc][0] in LOCAL:
            ins = prog[pc]
            L = _L(self.lmaps[i], loc)
            if ins[0] == 'set':
                loc = self._lset(i, loc, ins[1], ins[2](L))
                pc = ins[3]
            elif ins[0] == 'br':
                pc = ins[2] if ins[1](L) else ins[3]
            else:
                mu, lu = ins[1](_M(self, mem), L)
                mem = self._mupd(mem, mu)
                loc = self._lupd(i, loc, lu)
                pc = ins[2]
            steps += 1
            if steps > 10000:
                raise RuntimeError(f'T{i}: local steps loop forever at pc {pc}')
        if not steps:
            return mem, ths
        return mem, ths[:i] + ((pc, loc, st, sl, buf),) + ths[i + 1:]

    def _canon(self, st):
        if not self.symmetric:
            return st
        mem, ths = st
        ths = list(ths)
        for g in self.symmetric:
            vals = sorted((ths[j] for j in g), key=lambda t: marshal.dumps(t, 2))
            for j, t in zip(g, vals):
                ths[j] = t
        return mem, tuple(ths)

    # ---- successors -----------------------------------------------------
    def successors(self, st, labels=False):
        """([(thread, label, next_state)], failed check as (thread, message) or None)."""
        mem0, ths0 = st
        out = []
        fail = None
        tso = self.tso
        for i in range(len(ths0)):
            pc, loc, status, sl, buf = ths0[i]
            if buf:
                a, v = buf[0]
                ths = ths0[:i] + ((pc, loc, status, sl, buf[1:]),) + ths0[i + 1:]
                out.append((i, labels and f'T{i}:flush {self.akeys[a]}={v}', (self._mset(mem0, a, v), ths)))
            if status != RUN:
                continue
            prog = self.progs[i]
            mem = mem0
            pre = ''
            while prog[pc][0] == 'gpre':
                ins = prog[pc]
                mu, lu = ins[1](_M(self, mem), _L(self.lmaps[i], loc))
                mem = self._mupd(mem, mu)
                loc = self._lupd(i, loc, lu)
                if labels:
                    pre += ('ghost ' + ','.join(f'{k}={v}' for k, v in mu.items()) if mu else '[call]') + ' + '
                tmp = ths0[:i] + ((ins[2], loc, status, sl, buf),) + ths0[i + 1:]
                mem, tmp = self._norm(mem, tmp, i)
                pc, loc = tmp[i][0], tmp[i][1]
            ins = prog[pc]
            op = ins[0]
            if op == 'done':
                continue
            if tso and buf and (op in SERIAL or (op == 'store' and ins[3])):
                continue
            L = _L(self.lmaps[i], loc)

            def emit(m, npc, nloc, lbl, nbuf=buf):
                t = ths0[:i] + ((npc, nloc, RUN, -1, nbuf),) + ths0[i + 1:]
                m, t = self._norm(m, t, i)
                out.append((i, labels and f'T{i}:{pre}{lbl()}', (m, t)))

            def rd(a):
                for aa, vv in reversed(buf):
                    if aa == a:
                        return vv
                return self._mget(mem, a)

            if op == 'load':
                _, af, var, nxt = ins
                a = self._addr(af(L))
                v = rd(a)
                emit(mem, nxt, self._lset(i, loc, var, v), lambda: f'load {self.akeys[a]}->{v}')
            elif op == 'store':
                _, af, vf, serial, nxt = ins
                a, v = self._addr(af(L)), vf(L)
                if tso and not serial:
                    emit(mem, nxt, loc, lambda: f'store(buf) {self.akeys[a]}={v}', nbuf=buf + ((a, v),))
                else:
                    emit(self._mset(mem, a, v), nxt, loc, lambda: f'store {self.akeys[a]}={v}')
            elif op == 'add':
                _, af, df, var, nxt = ins
                a = self._addr(af(L))
                v = self._mget(mem, a) + df(L)
                emit(self._mset(mem, a, v), nxt, self._lset(i, loc, var, v), lambda: f'add {self.akeys[a]}->{v}')
            elif op == 'cas':
                _, af, of, nf, var, nxt = ins
                a = self._addr(af(L))
                old, new = of(L), nf(L)
                ok = self._mget(mem, a) == old
                emit(self._mset(mem, a, new) if ok else mem, nxt, self._lset(i, loc, var, ok),
                     lambda: f'cas {self.akeys[a]} {old}->{new} {ok}')
            elif op == 'or' or op == 'andn':
                _, af, bf, var, nxt = ins
                a = self._addr(af(L))
                old = self._mget(mem, a)
                new = (old | bf(L)) if op == 'or' else (old & ~bf(L))
                emit(self._mset(mem, a, new), nxt, self._lset(i, loc, var, old),
                     lambda: f'{op} {self.akeys[a]} {old}->{new}')
            elif op == 'fwait':
                _, af, vf, nxt = ins
                a = self._addr(af(L))
                if self._mget(mem, a) != vf(L):
                    emit(mem, nxt, loc, lambda: f'futex_wait {self.akeys[a]} EAGAIN')
                else:
                    t = ths0[:i] + ((nxt, loc, SLEEP, a, buf),) + ths0[i + 1:]
                    out.append((i, labels and f'T{i}:{pre}futex_wait {self.akeys[a]} SLEEP', (mem, t)))
            elif op == 'fwaitm':
                _, af, vf, mf, nxt = ins
                a = self._addr(af(L))
                if self._mget(mem, a) != vf(L):
                    emit(mem, nxt, loc, lambda: f'futex_wait {self.akeys[a]} EAGAIN')
                else:
                    m = mf(L)
                    t = ths0[:i] + ((nxt, loc, SLEEP, MASKED * (a + 1) + m, buf),) + ths0[i + 1:]
                    out.append((i, labels and f'T{i}:{pre}futex_wait {self.akeys[a]} mask {m} SLEEP',
                                (mem, t)))
            elif op == 'fwaken':
                _, af, nf, mf, nxt = ins
                a = self._addr(af(L))
                n, m = nf(L), mf(L)
                match = []
                for j, tj in enumerate(ths0):
                    if tj[2] == SLEEP:
                        on, mask = _sleeps_on(tj[3], a)
                        if on and (mask is None or mask & m):
                            match.append(j)
                from itertools import combinations
                k = min(n, len(match))
                for chosen in combinations(match, k):
                    t = list(ths0)
                    for j in chosen:
                        tj = ths0[j]
                        t[j] = (tj[0], tj[1], RUN, -1, tj[4])
                    t[i] = (nxt, loc, RUN, -1, buf)
                    mm, t = self._norm(mem, tuple(t), i)
                    for j in chosen:
                        mm, t = self._norm(mm, t, j)
                    out.append((i, labels and f'T{i}:{pre}futex_wake {self.akeys[a]} n={n} mask {m} '
                                f'woke {list(chosen)}', (mm, t)))
            elif op == 'fwake':
                _, af, nxt = ins
                a = self._addr(af(L))
                t = list(ths0)
                woke = []
                for j, tj in enumerate(ths0):
                    if tj[2] == SLEEP and _sleeps_on(tj[3], a)[0]:
                        t[j] = (tj[0], tj[1], RUN, -1, tj[4])
                        woke.append(j)
                t[i] = (nxt, loc, RUN, -1, buf)
                m, t = self._norm(mem, tuple(t), i)
                for j in woke:
                    m, t = self._norm(m, t, j)
                out.append((i, labels and f'T{i}:{pre}futex_wake {self.akeys[a]}', (m, t)))
            elif op == 'ghost':
                _, fn, nxt = ins
                mu, lu = fn(_M(self, mem), L)
                emit(self._mupd(mem, mu), nxt, self._lupd(i, loc, lu),
                     lambda: 'ghost ' + ','.join(f'{k}={v}' for k, v in mu.items()))
            elif op == 'nd':
                _, fn, nxt = ins
                for mu, lu, lbl in fn(_M(self, mem), L):
                    emit(self._mupd(mem, mu), nxt, self._lupd(i, loc, lu), lambda lbl=lbl: lbl)
            elif op == 'await':
                _, cf, nxt = ins
                if cf(_M(self, mem), L):
                    emit(mem, nxt, loc, lambda: 'await ok')
            elif op == 'check':
                _, fn, nxt = ins
                msg = fn(_M(self, mem), L)
                if msg:
                    if fail is None:
                        fail = (i, msg)
                else:
                    emit(mem, nxt, loc, lambda: 'check ok')
            else:
                raise ValueError(op)
        return out, fail

    @staticmethod
    def fingerprint(st, exact):
        b = marshal.dumps(st, 2)
        return b if exact else hashlib.blake2b(b, digest_size=12).digest()

    def replay(self, path, tail=None):
        st = self.init
        out = []
        for i, alt in path:
            succ, _ = self.successors(st, labels=True)
            _, lbl, ns = [s for s in succ if s[0] == i][alt]
            out.append(lbl)
            st = self._canon(ns)
        if tail:
            out.append(tail)
        return out


def explore(name, threads, mem0, tso=False, idle_ok=lambda i, M, L: False, track=(),
            symmetric=(), exact=False, max_states=100_000_000, max_seconds=3600,
            max_depth=20_000, progress=15.0, stop_on_error=True, out=sys.stdout,
            done=None, liveness=True, liveness_max_states=400_000):
    ex = Explorer(threads, mem0, tso, symmetric)
    res = Result(name)
    res.exact = exact
    t0 = time.time()
    next_report = t0 + progress

    # Liveness (AG EF done): every reachable state must still be able to reach a
    # state the model calls finished. Needs the whole state graph, so it is kept
    # only while the model stays under liveness_max_states; past that the arrays
    # are dropped and the result says the check did not run.
    graph = liveness
    edge_from, edge_to = array('l'), array('l')
    parent = array('l', [-1])              # spanning tree of the search, per state id
    parent_step = array('l', [-1, -1])     # (thread, alt) that reached each state
    finished = bytearray(1)

    def is_finished(mem, ths):
        if done is not None:
            return bool(done(_M(ex, mem)))
        for i, (pc, _loc, status, _, _) in enumerate(ths):
            if ex.progs[i][pc][0] == 'done':
                continue
            if status == SLEEP or not idle_ok(i, _M(ex, mem), _L(ex.lmaps[i], _loc)):
                return False
        return True

    def drop_graph(why):
        nonlocal graph
        graph = False
        res.liveness = why
        del edge_from[:], edge_to[:], parent[:], parent_step[:], finished[:]

    seen = {ex.fingerprint(ex.init, exact): 0}
    stack = [(ex.init, 0, None, 0)]        # (state, depth, path node = (thread, alt, parent), state id)

    def fail(verdict, node, message, tail=None):
        res.verdict, res.message = verdict, message
        path = []
        while node is not None:
            path.append((node[0], node[1]))
            node = node[2]
        res.trace = ex.replay(path[::-1], tail)

    while stack:
        st, depth, node, sid = stack.pop()
        res.states += 1
        if depth > res.max_depth:
            res.max_depth = depth
        mem, ths = st
        if graph:
            if is_finished(mem, ths):
                finished[sid] = 1
        for key in track:
            v = _M(ex, mem).get(key)
            if v > res.max_ghost.get(key, NONE):
                res.max_ghost[key] = v
        if not res.states & 0x1fff:
            now = time.time()
            if now >= next_report:
                next_report = now + progress
                big = max(range(len(mem)), key=lambda k: mem[k], default=None)
                bigs = f'{ex.akeys[big]}={mem[big]}' if big is not None else '-'
                print(f'    .. {name}: {res.states:,} states, {res.states / (now - t0):,.0f}/s, '
                      f'stack {len(stack):,}, depth {depth} (max {res.max_depth}), '
                      f'largest {bigs}, {now - t0:.0f}s', file=sys.stderr, flush=True)
            if now - t0 > max_seconds:
                fail('TIME LIMIT', node, f'stopped after {max_seconds}s')
                break
        if res.states > max_states:
            fail('STATE LIMIT', node, f'stopped after {max_states:,} states')
            break
        if depth > max_depth:
            fail('DEPTH LIMIT', node, f'a path of {depth} steps: a loop that never repeats a state?')
            break
        succ, bad = ex.successors(st)
        if bad is not None and res.verdict == 'ok':
            fail('VIOLATION', node, bad[1], f'T{bad[0]}: CHECK FAILED: {bad[1]}')
            if stop_on_error:
                break
        if not succ:
            res.terminals += 1
            if res.verdict == 'ok':
                for i, (pc, loc, status, _, _) in enumerate(ths):
                    if status == SLEEP:
                        fail('LOST WAKE', node, f'T{i} asleep with nothing left to wake it')
                        break
                    if ex.progs[i][pc][0] != 'done' and not idle_ok(i, _M(ex, mem), _L(ex.lmaps[i], loc)):
                        fail('STUCK', node, f'T{i} blocked at pc {pc} ({ex.progs[i][pc][0]})')
                        break
                if res.verdict != 'ok' and stop_on_error:
                    break
            continue
        counts = {}
        for i, _, ns in succ:
            alt = counts.get(i, 0)
            counts[i] = alt + 1
            ns = ex._canon(ns)
            fp = ex.fingerprint(ns, exact)
            nid = seen.get(fp)
            if nid is None:
                nid = seen[fp] = len(seen)
                stack.append((ns, depth + 1, (i, alt, node), nid))
                if graph:
                    parent.append(sid)
                    parent_step.append(i)
                    parent_step.append(alt)
                    finished.append(0)
                    if len(seen) > liveness_max_states:
                        drop_graph(f'not checked: over {liveness_max_states:,} states')
            if graph:
                edge_from.append(sid)
                edge_to.append(nid)
    res.seconds = time.time() - t0
    if graph and res.verdict == 'ok' and res.complete:
        trapped = _trapped(len(seen), edge_from, edge_to, finished, parent)
        if trapped < 0:
            res.liveness = 'ok'
        else:
            path = []
            n = trapped
            while n > 0:
                path.append((parent_step[2 * n], parent_step[2 * n + 1]))
                n = parent[n]
            res.verdict = 'TRAPPED'
            res.message = ('no finished state is reachable at all' if trapped == 0 else
                           'this step enters a set of states from which no finished state is reachable: '
                           'from here the model can run forever without finishing')
            res.trace = ex.replay(path[::-1], 'TRAPPED: no finished state is reachable from here')
            res.liveness = 'trapped'
    elif graph:
        res.liveness = 'not checked: the search did not complete'
    return res


def _trapped(nstates, edge_from, edge_to, finished, parent):
    """A state that cannot reach a finished state, or -1 if every state can.

    Backward breadth-first search from the finished states over the reversed
    edges, held as a counting-sorted adjacency array so the whole graph costs
    two machine words per edge. Of the trapped states it returns one the search
    entered from a state that is *not* trapped, so that the trace ends on the
    step that gave up on finishing rather than somewhere deep inside the trap.
    """
    heads = array('l', bytes(8 * (nstates + 1)))
    for v in edge_to:
        heads[v + 1] += 1
    for v in range(nstates):
        heads[v + 1] += heads[v]
    fill = array('l', heads[:nstates])
    back = array('l', bytes(8 * len(edge_to)))
    for k in range(len(edge_to)):
        v = edge_to[k]
        back[fill[v]] = edge_from[k]
        fill[v] += 1
    can = bytearray(finished)
    work = [v for v in range(nstates) if can[v]]
    while work:
        v = work.pop()
        for k in range(heads[v], heads[v + 1]):
            u = back[k]
            if not can[u]:
                can[u] = 1
                work.append(u)
    if not can[0]:
        return 0                             # nothing finishes, from the start
    for v in range(1, nstates):
        if not can[v] and can[parent[v]]:    # the search tree's step into the trap
            return v
    for v in range(1, nstates):              # a trap entered only off the tree
        if not can[v]:
            return v
    return -1


def report(res, expect_ok=None, tso=False, extra='', out=sys.stdout):
    """Print one result line. Returns False for an unexpected or incomplete result."""
    unexpected = res.complete and expect_ok is not None and expect_ok != res.ok
    bound = '' if res.exact else f', miss<{max(res.states, 1) ** 2 / 2 ** 97:.0e}'
    live = '' if res.liveness is None or res.verdict != 'ok' else (
        ' +live' if res.liveness == 'ok' else f' [liveness {res.liveness}]')
    # Wall-clock only on a terminal: a recorded .out must reproduce by plain
    # diff, and everything else on this line is a function of the model.
    clock = f'{res.seconds:7.1f}s, ' if out.isatty() else ''
    print(f"{res.name:<60} {'TSO' if tso else 'SC '} states={res.states:>11,} depth={res.max_depth:>3} "
          f"({clock}{bound.lstrip(', ')}) -> {res.verdict}{live}{extra}"
          f"{'   <-- UNEXPECTED' if unexpected else ''}", file=out, flush=True)
    if res.verdict != 'ok':
        if res.message:
            print(f'    {res.message}', file=out)
        if res.trace:
            print('    trace: ' + ' ; '.join(res.trace), file=out, flush=True)
    return res.complete and not unexpected


def select(argv):
    """Case filter from the command line: any case whose name contains an argument."""
    pats = [a for a in argv[1:] if not a.startswith('-')]
    return lambda name: not pats or any(p in name for p in pats)
