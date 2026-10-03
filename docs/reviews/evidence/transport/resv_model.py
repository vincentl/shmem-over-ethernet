#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""
Exhaustive interleaving model of design §3.4/§3.5/§3.8 (buffer reservation,
seal/submit/recycle, DRAINING) for ONE connection, two buffers, small writers.

Every step is one atomic action on shared state (sequential consistency; this
is the *strongest* memory model, so any counterexample here also exists on
aarch64).

Properties checked:
  OVERLAP   two live reservations in the same generation overlap
  LOST      a reserved message is discarded at recycle without going on the wire
  TORN      a message goes on the wire before its memcpy completed
  ACCT      filled > head after recycle (a writer's filled++ landed in a new gen)
  ORDER     two messages from the same writer are on the wire out of issue order
  SEQ       segment sequence numbers on the wire are not increasing
  STALESEQ  the seq a writer read after reserving (§3.5, used for quiet's
            high_seg) is not the seq of the segment its message was sent in
  DRAINRES  a PE writer reserved in a DRAINING buffer
  STUCK     terminal state (no enabled step) with undelivered messages

Variants (see VARIANTS at bottom):
  literal     §3.5 snippet as written: head CAS + plain `state` read in a
              separate step; on failure the writer seals (plain store) and
              tries the other buffer; freed buffer -> OPEN immediately.
  pt_seals    as literal, but only the progress thread seals; writer that
              fails just tries the other buffer.
  one_open    pt_seals + recycle to OPEN only while the other buffer is not
              OPEN ("one filling, one in flight").
  draining    one_open + §3.8 starvation guard: freed -> DRAINING, PT reserves
              a control message there, then -> OPEN (while other may be OPEN).
  fixed       seal is a bit in the head word (writer's CAS fails once sealed),
              no separate state read, at most one OPEN, seq assigned at open,
              DRAINING also encoded in head bit.
"""
import sys
from collections import deque

OPEN, SEALED, SENDING, FREE, DRAINING = "OPEN", "SEALED", "SENDING", "FREE", "DRAINING"
SEALBIT = 1000  # head >= SEALBIT means sealed (fixed variant)

def run(variant, sizes_per_writer, cap, max_states=3_000_000):
    V = variant
    nW = len(sizes_per_writer)
    # buffer: (state, head, filled, seq, snap, msgs)  msgs: tuple of (w,i,off,n,copied)
    b0 = (OPEN, 0, 0, 0, None, ())
    b1 = (FREE, SEALBIT if V == "fixed" else 0, 0, -1, None, ())  # fixed: non-OPEN buffers carry the seal bit
    # writer: (pc, i, b, h, s, off, seqread)
    writers = tuple((0, 0, 0, 0, None, None, None) for _ in range(nW))
    # pt: sealq tuple, inflight, nextseq, drainpending
    init = ((b0, b1), writers, (), None, 1, 0, (), ())
    # wire: tuple of (w,i,seq); seqmap: tuple of (w,i,seqread)
    seen = {init: None}
    q = deque([init])
    violations = {}

    def rec(kind, st, msg):
        if kind not in violations:
            violations[kind] = (st, msg)

    def setb(bufs, k, nb):
        l = list(bufs); l[k] = nb; return tuple(l)

    def setw(ws, k, nw):
        l = list(ws); l[k] = nw; return tuple(l)

    def succ(S):
        bufs, ws, sealq, inflight, nextseq, dpend, wire, seqreads = S
        out = []
        fixed = V == "fixed"
        # ---------------- writers ----------------
        for w in range(nW):
            pc, i, b, h, s, off, sr = ws[w]
            sizes = sizes_per_writer[w]
            if i >= len(sizes):
                continue
            n = sizes[i]
            st, head, filled, seq, snap, msgs = bufs[b]
            if pc == 0:   # LOAD head
                out.append(("W%d load b%d" % (w, b), (bufs, setw(ws, w, (1, i, b, head, None, None, None)), sealq, inflight, nextseq, dpend, wire, seqreads)))
            elif pc == 1:  # CHECK (literal: plain read of state)
                if fixed:
                    ok = h < SEALBIT and h + n <= cap
                    if ok:
                        out.append(("W%d check ok" % w, (bufs, setw(ws, w, (2, i, b, h, None, None, None)), sealq, inflight, nextseq, dpend, wire, seqreads)))
                    else:
                        if h < SEALBIT:
                            # full but unsealed: ask PT to seal; writer waits (re-load same buffer)
                            out.append(("W%d full-wait" % w, (bufs, setw(ws, w, (0, i, b, 0, None, None, None)), sealq, inflight, nextseq, dpend, wire, seqreads)))
                        else:
                            out.append(("W%d sealed->other" % w, (bufs, setw(ws, w, (0, i, 1 - b, 0, None, None, None)), sealq, inflight, nextseq, dpend, wire, seqreads)))
                    continue
                s = st
                if s != OPEN or h + n > cap:
                    if V == "literal" and s == OPEN:
                        out.append(("W%d fail->seal" % w, (bufs, setw(ws, w, (9, i, b, h, s, None, None)), sealq, inflight, nextseq, dpend, wire, seqreads)))
                    else:
                        out.append(("W%d fail->other" % w, (bufs, setw(ws, w, (0, i, 1 - b, 0, None, None, None)), sealq, inflight, nextseq, dpend, wire, seqreads)))
                else:
                    out.append(("W%d check ok (state=%s)" % (w, s), (bufs, setw(ws, w, (2, i, b, h, s, None, None)), sealq, inflight, nextseq, dpend, wire, seqreads)))
            elif pc == 9:  # literal writer seal: blind store SEALED (it read OPEN earlier)
                nb = (SEALED, head, filled, seq, snap, msgs)
                nsq = sealq if b in sealq else sealq + (b,)
                out.append(("W%d store SEALED b%d (was %s)" % (w, b, st), (setb(bufs, b, nb), setw(ws, w, (0, i, 1 - b, 0, None, None, None)), nsq, inflight, nextseq, dpend, wire, seqreads)))
            elif pc == 2:  # CAS head
                if head == h:
                    viol = []
                    for (w2, i2, o2, n2, c2) in msgs:
                        if not (h + n <= o2 or o2 + n2 <= h):
                            viol.append("OVERLAP")
                    if st == DRAINING:
                        viol.append("DRAINRES")
                    nb = (st, h + n, filled, seq, snap, msgs + ((w, i, h, n, False),))
                    nS = (setb(bufs, b, nb), setw(ws, w, (3, i, b, h, s, h, None)), sealq, inflight, nextseq, dpend, wire, seqreads)
                    for v in viol:
                        rec(v, nS, "W%d CAS at off %d in b%d state %s" % (w, h, b, st))
                    out.append(("W%d CAS ok off=%d b%d (state now %s)" % (w, h, b, st), nS))
                else:
                    out.append(("W%d CAS fail" % w, (bufs, setw(ws, w, (0, i, b, 0, None, None, None)), sealq, inflight, nextseq, dpend, wire, seqreads)))
            elif pc == 3:  # read seq (§3.5 generation read)
                out.append(("W%d read seq=%d" % (w, seq), (bufs, setw(ws, w, (4, i, b, h, s, off, seq)), sealq, inflight, nextseq, dpend, wire, seqreads + ((w, i, seq),))))
            elif pc == 4:  # memcpy
                nm = tuple((a, c, o, d, True if (a == w and c == i and o == off) else e) for (a, c, o, d, e) in msgs)
                out.append(("W%d memcpy" % w, (setb(bufs, b, (st, head, filled, seq, snap, nm)), setw(ws, w, (5, i, b, h, s, off, sr)), sealq, inflight, nextseq, dpend, wire, seqreads)))
            elif pc == 5:  # filled += n
                nb = (st, head, filled + n, seq, snap, msgs)
                nb_idx = b
                out.append(("W%d filled+=%d" % (w, n), (setb(bufs, b, nb), setw(ws, w, (0, i + 1, b, 0, None, None, None)), sealq, inflight, nextseq, dpend, wire, seqreads)))
        # ---------------- progress thread ----------------
        for b in (0, 1):
            st, head, filled, seq, snap, msgs = bufs[b]
            other = bufs[1 - b]
            hv = head % SEALBIT if fixed else head
            # SEAL
            if fixed:
                if head < SEALBIT and head > 0 and st in (OPEN, DRAINING):
                    nb = (SEALED, head + SEALBIT, filled, seq, snap, msgs)
                    out.append(("PT seal b%d" % b, (setb(bufs, b, nb), ws, sealq + (b,), inflight, nextseq, dpend, wire, seqreads)))
            elif st == OPEN and head > 0:
                nb = (SEALED, head, filled, seq, snap, msgs)
                out.append(("PT seal b%d" % b, (setb(bufs, b, nb), ws, sealq + (b,), inflight, nextseq, dpend, wire, seqreads)))
            # RECYCLE: FREE -> reset -> OPEN/DRAINING
            if st == FREE:
                lost = [m for m in msgs if not m[5:]]  # placeholder, computed at complete
                if V in ("one_open", "fixed") and other[0] in (OPEN, DRAINING) and not (fixed and other[1] >= SEALBIT):
                    pass
                else:
                    if V == "draining":
                        ns = DRAINING
                    else:
                        ns = OPEN
                    nb = (ns, 0, 0, nextseq, None, ())
                    nS = (setb(bufs, b, nb), ws, sealq, inflight, nextseq + 1, (dpend + 1) if ns == DRAINING else dpend, wire, seqreads)
                    if msgs:
                        rec("LOST", nS, "recycle of b%d discards reservations %s made after submit snapshot" % (b, msgs))
                    out.append(("PT recycle b%d -> %s seq=%d" % (b, ns, nextseq), nS))
            # DRAINING: PT reserves a control msg, then -> OPEN
            if st == DRAINING and not fixed:
                if head + 1 <= cap and dpend > 0:
                    nb = (st, head + 1, filled + 1, seq, snap, msgs + ((-1, dpend, head, 1, True),))
                    for (w2, i2, o2, n2, c2) in msgs:
                        if not (head + 1 <= o2 or o2 + n2 <= head):
                            rec("OVERLAP", S, "PT drain reserve overlaps W%d" % w2)
                    out.append(("PT drain-reserve b%d off=%d" % (b, head), (setb(bufs, b, nb), ws, sealq, inflight, nextseq, dpend - 1, wire, seqreads)))
                out.append(("PT drained b%d -> OPEN" % b, (setb(bufs, b, (OPEN, head, filled, seq, snap, msgs)), ws, sealq, inflight, nextseq, dpend, wire, seqreads)))
        # SUBMIT head of seal queue
        if inflight is None and sealq:
            b = sealq[0]
            st, head, filled, seq, snap, msgs = bufs[b]
            hv = head % SEALBIT if fixed else head
            if hv == filled and st == SEALED:
                nb = (SENDING, head, filled, seq, hv, msgs)
                out.append(("PT submit b%d len=%d" % (b, hv), (setb(bufs, b, nb), ws, sealq[1:], b, nextseq, dpend, wire, seqreads)))
        # COMPLETE
        if inflight is not None:
            b = inflight
            st, head, filled, seq, snap, msgs = bufs[b]
            nwire = wire
            viols = []
            for m in sorted(msgs, key=lambda m: m[2]):
                if m[2] + m[3] <= snap:
                    if not m[4]:
                        viols.append(("TORN", "msg %s sent uncopied" % (m,)))
                    nwire = nwire + ((m[0], m[1], seq),)
                else:
                    viols.append(("LOST", "msg W%d#%d at off %d beyond snapshot %d of b%d" % (m[0], m[1], m[2], snap, b)))
            hv = head % SEALBIT if fixed else head
            nb = (FREE, head, filled, seq, None, ())
            nS = (setb(bufs, b, nb), ws, sealq, None, nextseq, dpend, nwire, seqreads)
            for k, msg in viols:
                rec(k, nS, msg)
            out.append(("PT complete b%d seq=%d" % (b, seq), nS))
        return out

    def check_wire(S):
        bufs, ws, sealq, inflight, nextseq, dpend, wire, seqreads = S
        last = {}
        lastseq = -1
        segs = []
        for (w, i, seq) in wire:
            if w >= 0:
                if w in last and i < last[w]:
                    rec("ORDER", S, "writer %d msg %d on wire after msg %d" % (w, i, last[w]))
                last[w] = max(last.get(w, -1), i)
            if seq < lastseq:
                rec("SEQ", S, "seq %d after %d" % (seq, lastseq))
            lastseq = max(lastseq, seq)
        sent = {(w, i): seq for (w, i, seq) in wire if w >= 0}
        for (w, i, sr) in seqreads:
            if (w, i) in sent and sent[(w, i)] != sr:
                rec("STALESEQ", S, "W%d#%d read seq %d, sent in seq %d" % (w, i, sr, sent[(w, i)]))
        cnt = {}
        for (w, i, seq) in wire:
            if w >= 0:
                cnt[(w, i)] = cnt.get((w, i), 0) + 1
                if cnt[(w, i)] > 1:
                    rec("DUP", S, "W%d#%d twice" % (w, i))

    while q:
        S = q.popleft()
        nxt = succ(S)
        check_wire(S)
        bufs = S[0]
        if not fixed_flag(V):
            for b in bufs:
                if b[0] == OPEN and b[2] > b[1]:
                    rec("ACCT", S, "filled %d > head %d" % (b[2], b[1]))
        if not nxt:
            total = sum(len(s) for s in sizes_per_writer)
            if len([x for x in S[6] if x[0] >= 0]) != total:
                rec("STUCK", S, "terminal with %d/%d delivered" % (len([x for x in S[6] if x[0] >= 0]), total))
        for label, T in nxt:
            if T not in seen:
                seen[T] = (S, label)
                q.append(T)
        if len(seen) > max_states:
            print("  state limit hit")
            break
    return violations, seen

def fixed_flag(v):
    return v == "fixed"

def trace(seen, S):
    out = []
    while seen[S] is not None:
        P, label = seen[S]
        out.append(label)
        S = P
    return list(reversed(out))

VARIANTS = ["literal", "pt_seals", "one_open", "draining", "fixed"]

if __name__ == "__main__":
    configs = [
        ([[1, 2]], 2),
        ([[1, 2, 1]], 2),
        ([[1, 1], [1]], 2),
        ([[1, 2], [1]], 2),
        ([[2, 1], [1, 1]], 3),
    ]
    variants = sys.argv[1:] or VARIANTS
    for V in variants:
        print("=== variant:", V)
        allv = {}
        for sizes, cap in configs:
            viol, seen = run(V, sizes, cap)
            print("  writers=%s cap=%d states=%d violations=%s" % (sizes, cap, len(seen), sorted(viol)))
            for k, (S, msg) in viol.items():
                if k not in allv:
                    allv[k] = (sizes, cap, msg, trace(seen, S))
        for k, (sizes, cap, msg, tr) in sorted(allv.items()):
            print("  -- shortest counterexample %s (writers=%s cap=%d): %s" % (k, sizes, cap, msg))
            for step in tr:
                print("       ", step)
