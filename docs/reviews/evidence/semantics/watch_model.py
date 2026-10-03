#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""Exhaustive interleaving model of the design §4.9 watch-table waiter/waker protocol.

Memory models:
  SC  - sequential consistency: every store is immediately visible.
  TSO - x86-TSO style: each thread has a FIFO store buffer; plain stores are
        buffered and committed by a nondeterministic flush step; a *serializing*
        instruction (lock-prefixed atomic, CAS, futex syscall) may only execute
        with the thread's own buffer empty and writes memory directly.
Futex: futex_wait(addr,val) atomically compares memory (not other threads'
buffers) and sleeps; futex_wake(addr) wakes all sleepers on addr.
A terminal state with any thread asleep is a LOST WAKE (no timeout modelled).
"""
import sys
EMPTY, TOMB = 0, -99
NSLOT = 2

def run(name, threads, mem0, tso=False, expect=None, maxstates=5_000_000):
    mem0 = dict(mem0)
    init = (tuple(sorted(mem0.items(), key=repr)),
            tuple((0, (), 'run', None, ()) for _ in threads))
    seen = set(); stack = [(init, ())]; bad = None; nstates = 0; terminals = 0
    while stack:
        st, trace = stack.pop()
        if st in seen: continue
        seen.add(st); nstates += 1
        if nstates > maxstates: print(name, 'STATE LIMIT'); break
        succ = list(successors(st, threads, tso))
        if not succ:
            terminals += 1
            asleep = [i for i, t in enumerate(st[1]) if t[2] == 'sleep']
            if asleep and bad is None:
                bad = (st, trace)
            continue
        for lbl, ns in succ:
            stack.append((ns, trace + (lbl,)))
    verdict = 'LOST WAKE' if bad else 'no lost wake'
    ok = (expect is None) or ((bad is not None) == expect)
    print(f"{name:<62} {'TSO' if tso else 'SC '} states={nstates:>7} terminals={terminals:>5}  -> {verdict}{'' if ok else '   <-- UNEXPECTED'}")
    if bad:
        st, trace = bad
        print('    counterexample:', ' ; '.join(trace))
        print('    final mem:', dict(st[0]))
    return bad is None

def successors(st, threads, tso):
    memt, ths = st
    mem = dict(memt)
    for i, (pc, loc, status, sleepaddr, buf) in enumerate(ths):
        if tso and buf:  # flush oldest buffered store
            a, v = buf[0]
            m2 = dict(mem); m2[a] = v
            yield (f"T{i}:flush {a}={v}", pack(m2, ths, i, (pc, loc, status, sleepaddr, buf[1:])))
        if status != 'run': continue
        prog = threads[i]
        ins = prog[pc]
        L = dict(loc)
        op = ins[0]
        def rd(a):
            for aa, vv in reversed(buf):
                if aa == a: return vv
            return mem.get(a, 0)
        def ser_ok(): return (not tso) or not buf
        if op == 'done': continue
        if op == 'set':
            _, var, fn, nxt = ins; L[var] = fn(L)
            yield (f"T{i}:{ins[-1] if False else 'set '+var}", pack(mem, ths, i, (nxt, fl(L), 'run', None, buf)))
        elif op == 'load':
            _, af, var, nxt = ins; a = af(L); L[var] = rd(a)
            yield (f"T{i}:load {a}->{L[var]}", pack(mem, ths, i, (nxt, fl(L), 'run', None, buf)))
        elif op == 'store':
            _, af, vf, serial, nxt = ins; a = af(L); v = vf(L)
            if tso and not serial:
                yield (f"T{i}:store(buf) {a}={v}", pack(mem, ths, i, (nxt, loc, 'run', None, buf + ((a, v),))))
            else:
                if not ser_ok(): continue
                m2 = dict(mem); m2[a] = v
                yield (f"T{i}:store {a}={v}", pack(m2, ths, i, (nxt, loc, 'run', None, buf)))
        elif op == 'add':
            _, af, d, var, nxt = ins
            if not ser_ok(): continue
            a = af(L); m2 = dict(mem); m2[a] = mem.get(a, 0) + d; L[var] = m2[a]
            yield (f"T{i}:add {a}{d:+}->{m2[a]}", pack(m2, ths, i, (nxt, fl(L), 'run', None, buf)))
        elif op == 'cas':
            _, af, of, nf, var, nxt = ins
            if not ser_ok(): continue
            a = af(L); m2 = dict(mem)
            if mem.get(a, 0) == of(L): m2[a] = nf(L); L[var] = True
            else: L[var] = False
            yield (f"T{i}:cas {a} {L[var]}", pack(m2, ths, i, (nxt, fl(L), 'run', None, buf)))
        elif op == 'br':
            _, cf, t, f = ins
            yield (f"T{i}:br", pack(mem, ths, i, (t if cf(L) else f, loc, 'run', None, buf)))
        elif op == 'fwait':
            _, af, vf, nxt = ins
            if not ser_ok(): continue
            a = af(L)
            if mem.get(a, 0) != vf(L):
                yield (f"T{i}:futex_wait {a} EAGAIN", pack(mem, ths, i, (nxt, loc, 'run', None, buf)))
            else:
                yield (f"T{i}:futex_wait {a} SLEEP", pack(mem, ths, i, (nxt, loc, 'sleep', a, buf)))
        elif op == 'fwake':
            _, af, nxt = ins
            if not ser_ok(): continue
            a = af(L)
            nths = list(ths)
            for j, t in enumerate(ths):
                if t[2] == 'sleep' and t[3] == a:
                    nths[j] = (t[0], t[1], 'run', None, t[4])
            nths[i] = (nxt, loc, 'run', None, buf)
            yield (f"T{i}:futex_wake {a}", (tuple(sorted(mem.items(), key=repr)), tuple(nths)))
        else:
            raise ValueError(op)

def fl(L): return tuple(sorted(L.items()))
def pack(mem, ths, i, t):
    n = list(ths); n[i] = t
    return (tuple(sorted(mem.items(), key=repr)), tuple(n))

# ---------------------------------------------------------------- programs
def assemble(blocks):
    """blocks: list of (label, instr-with-symbolic-labels). Returns list."""
    idx = {lab: n for n, (lab, _) in enumerate(blocks)}
    out = []
    for lab, ins in blocks:
        ins = list(ins)
        for k, x in enumerate(ins):
            if isinstance(x, str) and x.startswith('@'): ins[k] = idx[x[1:]]
        out.append(tuple(ins))
    return out

KEY = lambda s: ('key', s)
CNT = lambda s: ('slotcnt', s)
SEQ = lambda s: ('seq', s)

def waiter(key, pred, order='design', count_serial=True, remove='naive'):
    """order: design | check_then_insert | pred_before_seq"""
    B = []
    ins = [
        ('I0', ('set', 's', lambda L: 0, '@I1')),
        ('I1', ('load', lambda L: KEY(L['s']), 'k', '@I2')),
        ('I2', ('br', lambda L: L['k'] == key, '@J0', '@I3')),
        ('I3', ('br', lambda L: L['k'] == EMPTY, '@I4', '@I5')),
        ('I4', ('cas', lambda L: KEY(L['s']), lambda L: EMPTY, lambda L: key, 'ok', '@I4b')),
        ('I4b', ('br', lambda L: L['ok'], '@J0', '@I1')),
        ('I5', ('set', 's', lambda L: L['s'] + 1, '@I1')),   # table never fills in these runs
    ]
    if remove == 'naive':
        join = [
            ('J0', ('add', lambda L: CNT(L['s']), 1, '_', '@J1')),
        ]
    else:  # refcount with -1 teardown marker and key re-check
        join = [
            ('J0', ('load', lambda L: CNT(L['s']), 'c', '@J0a')),
            ('J0a', ('br', lambda L: L['c'] < 0, '@I1', '@J0b')),
            ('J0b', ('cas', lambda L: CNT(L['s']), lambda L: L['c'], lambda L: L['c'] + 1, 'ok', '@J0c')),
            ('J0c', ('br', lambda L: L['ok'], '@J0d', '@J0')),
            ('J0d', ('load', lambda L: KEY(L['s']), 'k2', '@J0e')),
            ('J0e', ('br', lambda L: L['k2'] == key, '@J1', '@J0f')),
            ('J0f', ('add', lambda L: CNT(L['s']), -1, '_', '@I0')),
        ]
    if count_serial:
        join += [('J1', ('add', lambda L: 'count', 1, '_', '@AFTER'))]
    else:  # plain (non-serializing) increment: load + store
        join += [('J1', ('load', lambda L: 'count', 'cc', '@J1b')),
                 ('J1b', ('store', lambda L: 'count', lambda L: L['cc'] + 1, False, '@AFTER'))]
    loop = [
        ('L1', ('load', lambda L: SEQ(L['s']), 'q', '@L2')),
        ('L2', ('load', lambda L: 'ivar', 'v', '@L3')),
        ('L3', ('br', lambda L: pred(L['v']), '@R0', '@L4')),
        ('L4', ('fwait', lambda L: SEQ(L['s']), lambda L: L['q'], '@L1')),
    ]
    if order == 'pred_before_seq':
        loop = [
            ('L1', ('load', lambda L: 'ivar', 'v', '@L2')),
            ('L2', ('load', lambda L: SEQ(L['s']), 'q', '@L3')),
            ('L3', ('br', lambda L: pred(L['v']), '@R0', '@L4')),
            ('L4', ('fwait', lambda L: SEQ(L['s']), lambda L: L['q'], '@L1')),
        ]
    if remove == 'naive':
        rem = [
            ('R0', ('add', lambda L: 'count', -1, '_', '@R1')),
            ('R1', ('add', lambda L: CNT(L['s']), -1, 'c', '@R2')),
            ('R2', ('br', lambda L: L['c'] == 0, '@R3', '@END')),
            ('R3', ('store', lambda L: KEY(L['s']), lambda L: EMPTY, True, '@END')),
        ]
    else:
        rem = [
            ('R0', ('add', lambda L: 'count', -1, '_', '@R1')),
            ('R1', ('add', lambda L: CNT(L['s']), -1, 'c', '@R2')),
            ('R2', ('br', lambda L: L['c'] == 0, '@R3', '@END')),
            ('R3', ('cas', lambda L: CNT(L['s']), lambda L: 0, lambda L: -1, 'ok', '@R4')),
            ('R4', ('br', lambda L: L['ok'], '@R5', '@END')),
            ('R5', ('store', lambda L: KEY(L['s']), lambda L: EMPTY, True, '@R6')),
            ('R6', ('store', lambda L: CNT(L['s']), lambda L: 0, True, '@END')),
        ]
    end = [('END', ('done',))]
    if order == 'check_then_insert':
        # predicate checked once BEFORE inserting; then design loop
        pre = [('P0', ('load', lambda L: 'ivar', 'v', '@P1')),
               ('P1', ('br', lambda L: pred(L['v']), '@END', '@I0'))]
        blocks = pre + ins + join + [('AFTER', ('set', 'x', lambda L: 0, '@L4x'))] + \
                 [('L4x', ('load', lambda L: SEQ(L['s']), 'q', '@L4y')),
                  ('L4y', ('fwait', lambda L: SEQ(L['s']), lambda L: L['q'], '@L1'))] + loop + rem + end
    else:
        blocks = ins + join + [('AFTER', ('set', 'x', lambda L: 0, '@L1'))] + loop + rem + end
    return assemble(blocks)

def writer(key, value=None, delta=None, order='design', ivar_serial=True, scan='first'):
    """Stores ivar (value) or atomically adds delta, then notifies."""
    if value is not None:
        storei = [('X1', ('store', lambda L: 'ivar', lambda L: value, ivar_serial, '@XN'))]
    else:
        storei = [('X1', ('add', lambda L: 'ivar', delta, '_', '@XN'))]
    notify = [
        ('N0', ('load', lambda L: 'count', 'c', '@N1')),
        ('N1', ('br', lambda L: L['c'] == 0, '@NEND', '@N2')),
        ('N2', ('set', 's', lambda L: 0, '@N3')),
        ('N3', ('br', lambda L: L['s'] >= NSLOT, '@NEND', '@N4')),
        ('N4', ('load', lambda L: KEY(L['s']), 'k', '@N5')),
        ('N5', ('br', lambda L: L['k'] == key, '@N6', '@N8')),
        ('N6', ('add', lambda L: SEQ(L['s']), 1, '_', '@N7')),
        ('N7', ('fwake', lambda L: SEQ(L['s']), '@N7b')),
        ('N7b', ('br', lambda L: scan == 'all', '@N9', '@NEND')),
        ('N8', ('br', lambda L: L['k'] == EMPTY and scan == 'first', '@NEND', '@N9')),
        ('N9', ('set', 's', lambda L: L['s'] + 1, '@N3')),
    ]
    if order == 'design':
        blocks = [(a, b if a != 'X1' else b) for a, b in storei]
        blocks = [(storei[0][0], tuple(x if x != '@XN' else '@N0' for x in storei[0][1]))] + notify + \
                 [('NEND', ('done',))]
    else:  # bump_before_store: notify first, then store
        notify2 = [(a, tuple(x if x != '@NEND' else '@X1' for x in b)) for a, b in notify]
        blocks = notify2 + [(storei[0][0], tuple(x if x != '@XN' else '@NEND' for x in storei[0][1]))] + \
                 [('NEND', ('done',))]
    return assemble(blocks)

def filler(key):
    """A thread that occupies a slot for `key` then leaves (a waiter whose predicate is already true)."""
    return waiter(key, lambda v: True)

A, Y = 1000, 2000   # key values (addresses)
M0 = {'ivar': 0, 'count': 0}
res = {}
print("== 1 waiter / 1 writer: design protocol and deliberate reversals")
res['design_sc'] = run("V0 design order", [waiter(A, lambda v: v == 1), writer(A, value=1)], M0, expect=False)
run("V1 check-then-insert (insert after first predicate check)", [waiter(A, lambda v: v == 1, order='check_then_insert'), writer(A, value=1)], M0, expect=True)
run("V2 writer bumps seq before storing ivar", [waiter(A, lambda v: v == 1), writer(A, value=1, order='bump_before_store')], M0, expect=True)
run("V3 waiter loads ivar before seq", [waiter(A, lambda v: v == 1, order='pred_before_seq'), writer(A, value=1)], M0, expect=True)
print("== memory model (Dekker pattern: waiter count++ / load ivar  vs  writer store ivar / load count)")
run("V4 TSO, count++ and ivar store both non-serializing", [waiter(A, lambda v: v == 1, count_serial=False), writer(A, value=1, ivar_serial=False)], M0, tso=True, expect=True)
run("V4b TSO, count++ serializing, ivar store plain", [waiter(A, lambda v: v == 1), writer(A, value=1, ivar_serial=False)], M0, tso=True, expect=True)
run("V4c TSO, count++ plain, ivar store serializing", [waiter(A, lambda v: v == 1, count_serial=False), writer(A, value=1)], M0, tso=True, expect=True)
run("V5 TSO, both serializing (lock-prefixed atomics)", [waiter(A, lambda v: v == 1), writer(A, value=1)], M0, tso=True, expect=False)
print("== keys")
run("V6 writer keys by its own VA of the slot (differs from waiter's)", [waiter(A, lambda v: v == 1), writer(A + 4096, value=1)], M0, expect=True)
print("== two writers (progress thread + local PE), atomic add, wait for ivar==2")
run("V7 two atomic adders, design", [waiter(A, lambda v: v == 2), writer(A, delta=1), writer(A, delta=1)], M0, expect=False)
print("== concurrent waiters on one address / slot reuse")
run("V8 naive refcount: remover clears key while joiner increments", [filler(A), waiter(A, lambda v: v == 1), writer(A, value=1)], M0, expect=True)
run("V8fix refcount CAS with -1 teardown + key recheck", [waiter(A, lambda v: True, remove='fixed'), waiter(A, lambda v: v == 1, remove='fixed'), writer(A, value=1)], M0, expect=False)
print("== linear-probe hole: slot 0 (key Y) freed while key A lives in slot 1")
M1 = dict(M0); M1.update({KEY(0): Y, CNT(0): 1, KEY(1): A, CNT(1): 1, 'count': 2})
# pre-state: some Y-waiter holds slot 0, an A-waiter holds slot 1 (modelled as already inserted & asleep-capable)
def preinserted_waiter(slot, key, pred):
    p = waiter(key, pred)
    return p
def y_leaver():
    return assemble([
        ('R0', ('add', lambda L: 'count', -1, '_', '@R1')),
        ('R1', ('add', lambda L: CNT(0), -1, 'c', '@R2')),
        ('R2', ('br', lambda L: L['c'] == 0, '@R3', '@END')),
        ('R3', ('store', lambda L: KEY(0), lambda L: EMPTY, True, '@END')),
        ('END', ('done',))])
def a_waiter_in_slot1(pred):
    return assemble([
        ('S', ('set', 's', lambda L: 1, '@L1')),
        ('L1', ('load', lambda L: SEQ(L['s']), 'q', '@L2')),
        ('L2', ('load', lambda L: 'ivar', 'v', '@L3')),
        ('L3', ('br', lambda L: pred(L['v']), '@END', '@L4')),
        ('L4', ('fwait', lambda L: SEQ(L['s']), lambda L: L['q'], '@L1')),
        ('END', ('done',))])
run("V9 hole -> second A slot claimed at 0; writer stops at first match", [y_leaver(), a_waiter_in_slot1(lambda v: v == 1), waiter(A, lambda v: v == 1), writer(A, value=1)], M1, expect=True)
run("V9fix writer bumps every matching slot", [y_leaver(), a_waiter_in_slot1(lambda v: v == 1), waiter(A, lambda v: v == 1), writer(A, value=1, scan='all')], M1, expect=False)
