#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A peer's abort across owners (v0.2 step 4, Part 3; written by Part 3's
adversarial read, its S1, and adopted; its send side by the second read, its
S1, and adopted). Explicit-state, in the form of carrier_model.py: every
interleaving of a few steps, searched breadth first.

Two nodes. N is ending (its abort declared); P is not, unless a variant says so.
Each node has two owners: the agent, holding the control wire C, and a helper,
holding the data wire D. A wire is a FIFO of what N's kernel has taken: its
abortive goodbye ('ABORT') and, once N's process ends, its end of stream
('END' - a FIN, or a reset behind data already acknowledged, which a reader
takes after that data). A goodbye not yet handed to the kernel when the
process ends is lost. In the `reset` variants the end is a reset ('RESET'),
which a reader takes as an error on its wire (w->error, ring.c) rather than
an end of stream.

N, as progress-host.c has it (osoe_progress_host_abort, helper_main,
helper_abort): its agent sends ABORT on C; its helper, at some later turn, sends
ABORT on D; its process ends once the agent's wait allows:
  wait        (the design) the agent waits for the helper's done word, so the
              process ends after both goodbyes are the kernel's;
  no_wait     the agent does not wait (the read's mutant `abort-without-helpers`,
              and what a grace that runs out amounts to);
  abrupt      N ends with no goodbye at all - a fatal's _exit on any thread of a
              lead's process, or an agent ending from a sweep of its own close
              (sweep.c), which waits for no helper.

P, as progress.c's receive_wire has it: each owner reads its wire in order. An
ABORT sets the wire's peer_aborted and declares P's abort (abort_arrived). An
END or a RESET is excused if the owner has begun its own abort, or the wire's
peer_aborted is set, or - the node excuse, `node_aborting` - P's abort is
declared; otherwise the owner raises a fatal naming N. An owner begins its own
abort at some turn after P's abort is declared. In the `p_ending` variants P's
abort may also be declared for a reason of its own (another peer, a local
fatal) at any moment.

The send side, as progress.c's flush and flush_control have it: a reading owner
also sends, in the same visit and in every later one while a segment on its
wire keeps the connection busy. In the `stream` variants each of P's owners has
a segment on its wire when N ends. Until the owner's own abort - whose
flush_aborting finds a failed wire with nobody to tell - the visit's flush
returns early on the wire's peer_aborted and, as built, on its error; offering
the segment again on a failed wire is send_from's fatal ("the connection to
node N failed"). A FIN sets no error and an idle wire offers nothing.

Checked: no fatal at P. A fatal while P's abort is already declared is the
costly one - on a global exit the helper that raises it pauses for ever in
osoe_abort_end and its agent waits the whole grace for it (wait_helpers), its
other wires untold; on a fatal it _exits P before P's goodbyes - and with N
ending by its abort, any fatal at all ends P with a death rather than the
abort's status.

Variants (expected verdict):
  wait                    holds
  wait_no_excuse          holds: with the wait, ABORT is ahead of END on D
  no_wait                 FAILS: P's helper takes D's END before P's agent has
                          taken C's ABORT (the record's residual window, made
                          ordinary)
  abrupt_p_ending         holds
  abrupt_p_ending_no_excuse
                          FAILS: P declared, P's helper not yet aborting, takes
                          N's END: a fatal while P is ending
  abrupt_p_ending_fin_stream
                          holds: a FIN sets no error, and the send goes on
  abrupt_p_ending_reset_idle
                          holds: nothing on the wire, so the flush sends nothing
  abrupt_p_ending_reset_stream
                          holds: the reset excused, the flush returns on the
                          wire's error
  abrupt_p_ending_reset_stream_unchecked
                          FAILS: the flush as it was at 7903b15, returning on
                          the send side's error alone - the reset excused in
                          receive_wire, then a fatal in the same visit's send
  wait_reset_stream       holds: with N's own goodbyes, each wire's peer_aborted
                          is set before its reset
"""
import sys
from collections import deque

VARIANTS = {
    # name: (n_mode, excuse, p_ending, reset, stream, flush_checks_error, expect_ok)
    'wait': ('wait', True, False, False, False, True, True),
    'wait_no_excuse': ('wait', False, False, False, False, True, True),
    'no_wait': ('no_wait', True, False, False, False, True, False),
    'abrupt_p_ending': ('abrupt', True, True, False, False, True, True),
    'abrupt_p_ending_no_excuse': ('abrupt', False, True, False, False, True, False),
    'abrupt_p_ending_fin_stream': ('abrupt', True, True, False, True, True, True),
    'abrupt_p_ending_reset_idle': ('abrupt', True, True, True, False, True, True),
    'abrupt_p_ending_reset_stream': ('abrupt', True, True, True, True, True, True),
    'abrupt_p_ending_reset_stream_unchecked': ('abrupt', True, True, True, True, False, False),
    'wait_reset_stream': ('wait', True, False, True, True, True, True),
}


def run(name):
    n_mode, excuse, p_ending, reset, stream, checks_error, _ = VARIANTS[name]
    end_item = 'RESET' if reset else 'END'
    # State: (n_agent_sent, n_helper_sent, n_ended,
    #         chan_C, chan_D,
    #         p_declared, p_agent_aborting, p_helper_aborting,
    #         p_peer_aborted_C, p_peer_aborted_D,
    #         p_error_C, p_error_D, p_flushed_C, p_flushed_D, fatal)
    start = (False, False, False, (), (), False, False, False, False, False,
             False, False, False, False, '')

    def succ(S):
        (a_sent, h_sent, ended, C, D, decl, pa_ab, ph_ab, pab_c, pab_d,
         err_c, err_d, fl_c, fl_d, fatal) = S
        out = []
        if fatal:
            return out

        def T(**k):
            v = dict(a_sent=a_sent, h_sent=h_sent, ended=ended, C=C, D=D, decl=decl,
                     pa_ab=pa_ab, ph_ab=ph_ab, pab_c=pab_c, pab_d=pab_d, err_c=err_c,
                     err_d=err_d, fl_c=fl_c, fl_d=fl_d, fatal=fatal)
            v.update(k)
            return (v['a_sent'], v['h_sent'], v['ended'], v['C'], v['D'], v['decl'],
                    v['pa_ab'], v['ph_ab'], v['pab_c'], v['pab_d'], v['err_c'],
                    v['err_d'], v['fl_c'], v['fl_d'], v['fatal'])

        if not ended and n_mode != 'abrupt':
            if not a_sent:
                out.append((T(a_sent=True, C=C + ('ABORT',)), 'N agent sends ABORT on C'))
            if not h_sent:
                out.append((T(h_sent=True, D=D + ('ABORT',)), 'N helper sends ABORT on D'))
        if not ended:
            may_end = (n_mode == 'abrupt' or (n_mode == 'no_wait' and a_sent) or
                       (n_mode == 'wait' and a_sent and h_sent))
            if may_end:
                out.append((T(ended=True, C=C + (end_item,), D=D + (end_item,)),
                            f'N ends ({end_item} behind what it sent)'))
        # P declares for a reason of its own.
        if p_ending and not decl:
            out.append((T(decl=True), 'P declares for another reason'))
        # P's owners begin their aborts at a later turn.
        if decl and not pa_ab:
            out.append((T(pa_ab=True), 'P agent begins its abort'))
        if decl and not ph_ab:
            out.append((T(ph_ab=True), 'P helper begins its abort'))
        # P's agent reads C, and P's helper D (receive_wire).
        for owner, chan, aborting, peer_aborted in (('agent', 'C', pa_ab, pab_c),
                                                    ('helper', 'D', ph_ab, pab_d)):
            wire = C if chan == 'C' else D
            if not wire:
                continue
            item, rest = wire[0], wire[1:]
            if item == 'ABORT':
                k = {chan: rest, 'decl': True, 'pab_' + chan.lower(): True}
                out.append((T(**k), f'P {owner} reads ABORT on {chan}'))
            else:
                ok = aborting or peer_aborted or (excuse and decl)
                f = '' if ok else (f'{owner}, P declared' if decl else owner)
                k = {chan: rest, 'fatal': f}
                if item == 'RESET':
                    k['err_' + chan.lower()] = True
                out.append((T(**k), f'P {owner} reads {item} on {chan}' +
                            (' (excused)' if ok else ': FATAL')))
        # P's owners' flushes, after a read of the end in the same visit and at
        # every later visit while the segment on the wire keeps the connection
        # busy, until the owner's own abort begins (then flush_aborting, which
        # finds the failed wire with nobody to tell).
        for owner, chan, aborting, peer_aborted, err, flushed in (
                ('agent', 'C', pa_ab, pab_c, err_c, fl_c),
                ('helper', 'D', ph_ab, pab_d, err_d, fl_d)):
            if not stream or not err or flushed or aborting:
                continue
            if peer_aborted or checks_error:
                out.append((T(**{'fl_' + chan.lower(): True}),
                             f'P {owner} flushes {chan}: returns early'))
            else:
                f = f'{owner} flush, P declared' if decl else f'{owner} flush'
                out.append((T(**{'fl_' + chan.lower(): True, 'fatal': f}),
                             f'P {owner} flushes {chan}: send_from on its error: FATAL'))
        return out

    seen = {start: None}
    q = deque([start])
    bad = None
    while q:
        S = q.popleft()
        if S[-1]:
            # In the abrupt variants a fatal before P's own declaration is a
            # peer's death read as one - correct; only one while P is ending
            # is the defect. Otherwise every fatal is.
            if n_mode != 'abrupt' or 'P declared' in S[-1]:
                bad = S
                break
            continue
        for T_, label in succ(S):
            if T_ not in seen:
                seen[T_] = (S, label)
                q.append(T_)
    if bad is None:
        return True, len(seen), []
    trace = []
    S = bad
    while seen.get(S):
        S, label = seen[S]
        trace.append(label)
    return False, len(seen), list(reversed(trace))


def main():
    ok = True
    for name, variant in VARIANTS.items():
        expect = variant[-1]
        held, states, trace = run(name)
        print(f'abort_owners/{name}: {"holds" if held else "FAILS: a fatal at P"} '
              f'({states} states){"" if held == expect else "  <-- UNEXPECTED"}', flush=True)
        if trace:
            print('   trace: ' + ' ; '.join(trace))
        ok &= held == expect
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
