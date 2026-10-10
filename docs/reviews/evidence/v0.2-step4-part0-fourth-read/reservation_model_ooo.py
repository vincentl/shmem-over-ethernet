#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A reviewer's variant of tools/modelcheck/reservation_model.py's zero-copy kernel.

The tree's kernel thread delivers each part in send order and posts its
notification in the same step, reading the buffer once, then. Here the two are
apart, as on a real NIC:

  deliver  in stream order (TCP): the part's units are read, checked and applied
           exactly as the tree's kernel does, and the part joins a set of parts
           whose notification is pending;
  notify   any pending part, in any order (an earlier send's notification may
           come after a later one's: an skb clone still held by a driver or a
           qdisc, notification stacking), after a last read of its units - a
           retransmission - which must find every unit as delivery left it
           (zeroed): a unit written since is a write into bytes the kernel still
           held (REUSED). Only then does the part's owed count fall.

The progress thread, the writers and the breakages are the tree's, unchanged;
only `kernel` is replaced. Usage: reservation_model_ooo.py [substring ...]
"""
import os
import sys

TREE = '<repo>/build/scratch/wt/zero-copy-send/tools/modelcheck'
sys.path.insert(0, TREE)

import reservation_model as rm                       # noqa: E402
from engine import DEL, assemble, explore, report, select   # noqa: E402


def PCELL(j): return ('$pending', j)     # delivered parts whose notification is pending


def kernel_ooo(design, total, qlen):
    notes = design == 'zc_first_notification'
    plen = qlen

    def pending(M):
        return [M[PCELL(j)] for j in range(plen) if M[PCELL(j)]]

    def canonical(parts):
        """The pending set as cells, largest first, zeros after: order carries
        no meaning, so states differing only in it merge."""
        parts = sorted(parts, reverse=True)
        return {PCELL(j): (parts[j] if j < len(parts) else 0) for j in range(plen)}

    def look(M, L):
        alts = []
        if M[rm.QCELL(0)]:
            alts.append(({}, {'act': 1}, 'deliver the oldest part'))
        for v in sorted(set(pending(M))):
            alts.append(({}, {'act': 2, 'pv': v}, f'notify part {rm.unpack_part(v)}'))
        if not alts:
            alts.append(({}, {'act': 0}, 'nothing queued or pending'))
        return alts

    def check_deliver(M, L):
        k, last, first, end, seq = rm.unpack_part(M[rm.QCELL(0)])
        if first == 0 and seq != M[rm.LASTSEQ] + 1:
            return f'SEGSEQ: segment {seq} sent after segment {M[rm.LASTSEQ]}'
        nxt, here = {}, set()
        for off in range(max(first, rm.H), end):
            i = M[('$slot', k, off)]
            if i == 0:
                return (f'TORN: segment {seq} delivered with an uncopied or reused unit at '
                        f'offset {off} of buffer {k}')
            if M[('$applied', i)] or i in here:
                return f'ONCE: message {i} delivered twice'
            here.add(i)
            if M[('$rseq', i)] != seq:
                return (f"SEQ: message {i} left in segment {seq} but its writer recorded "
                        f"{M[('$rseq', i)]}")
            w, m = (i - 1) // rm.PER_WRITER, (i - 1) % rm.PER_WRITER
            want = nxt.get(w, M[('$next', w)])
            if m != want:
                return f'ORDER: writer {w} message {m} delivered before its message {want}'
            nxt[w] = m + 1
        return None

    def deliver(M, L):
        v = M[rm.QCELL(0)]
        k, last, first, end, seq = rm.unpack_part(v)
        upd, n = {}, 0
        for off in range(max(first, rm.H), end):
            i = M[('$slot', k, off)]
            w = (i - 1) // rm.PER_WRITER
            upd[('$applied', i)] = 1
            upd[('$next', w)] = upd.get(('$next', w), M[('$next', w)]) + 1
            upd[('$slot', k, off)] = 0
            n += 1
        upd[rm.NAPPLIED] = M[rm.NAPPLIED] + n
        if last:
            upd[rm.LASTSEQ] = seq
        for j in range(qlen - 1):
            upd[rm.QCELL(j)] = M[rm.QCELL(j + 1)]
        upd[rm.QCELL(qlen - 1)] = 0
        upd.update(canonical(pending(M) + [v]))
        return upd, {'ack': seq if last else DEL}

    def check_notify(M, L):
        k, last, first, end, seq = rm.unpack_part(L['pv'])
        for off in range(max(first, rm.H), end):
            if M[('$slot', k, off)] != 0:
                return (f'REUSED: unit {off} of buffer {k} (segment {seq}, part {first}..{end}) '
                        f'was written while the kernel still held it: message '
                        f"{M[('$slot', k, off)]}")
        return None

    def notify(M, L):
        v = L['pv']
        k = rm.unpack_part(v)[0]
        rest = pending(M)
        rest.remove(v)
        upd = canonical(rest)
        upd[rm.OWED(k)] = M[rm.OWED(k)] - 1
        if notes:
            upd[rm.NOTES(k)] = M[rm.NOTES(k)] + 1
        return upd, {'pv': DEL}

    B = [('K0', ('await', lambda M, L, t=total: M[rm.QCELL(0)] != 0 or pending(M) or
                 M[rm.NAPPLIED] >= t, '@K1')),
         ('K1', ('nd', look, '@K2')),
         ('K2', ('br', lambda L: L['act'] == 1, '@D0', '@K3')),
         ('K3', ('br', lambda L: L['act'] == 2, '@N0', '@KEND')),
         ('D0', ('set', 'act', lambda L: DEL, '@D1')),
         ('D1', ('check', check_deliver, '@D2')),
         ('D2', ('gpost', deliver, '@D3')),
         ('D3', ('br', lambda L: L['ack'] is not None, '@D4', '@K0')),
         ('D4', ('store', lambda L: rm.ACKED, lambda L: L['ack'], True, '@D5')),
         ('D5', ('set', 'ack', lambda L: DEL, '@K0')),
         ('N0', ('set', 'act', lambda L: DEL, '@N1')),
         ('N1', ('check', check_notify, '@N2')),
         ('N2', ('gpost', notify, '@K0')),
         ('KEND', ('done',))]
    return assemble(B)


rm.kernel = kernel_ooo          # case() looks the kernel up by name

CASES = [c for c in rm.CASES if c[0] in rm.ZERO_COPY]

if __name__ == '__main__':
    keep = select(sys.argv)
    ok = True
    for design, writers, msgs, cap, replies, tso, expect in CASES:
        name, threads, mem0 = rm.case(design, writers, msgs, cap, replies)
        name = name.replace('reservation/', 'reservation-ooo/')
        if not keep(name):
            continue
        res = explore(name, threads, mem0, tso=tso, max_seconds=1200,
                      liveness_max_states=6_000_000)
        ok &= report(res, expect_ok=expect, tso=tso)
    sys.exit(0 if ok else 1)
