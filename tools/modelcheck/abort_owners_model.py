#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-or-later
"""A peer's abort across owners (v0.2 step 4, Part 3; written by Part 3's
adversarial read, its S1, and adopted). Explicit-state, in the form of
carrier_model.py: every interleaving of a few steps, searched breadth first.

Two nodes. N is ending (its abort declared); P is not, unless a variant says so.
Each node has two owners: the agent, holding the control wire C, and a helper,
holding the data wire D. A wire is a FIFO of what N's kernel has taken: its
abortive goodbye ('ABORT') and, once N's process ends, its end of stream
('END' - a FIN, or a reset behind data already acknowledged, which a reader
takes after that data). A goodbye not yet handed to the kernel when the
process ends is lost.

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
END is excused if the owner has begun its own abort, or the wire's
peer_aborted is set, or - the node excuse, `node_aborting` - P's abort is
declared; otherwise the owner raises a fatal naming N. An owner begins its own
abort at some turn after P's abort is declared. In the `p_ending` variants P's
abort may also be declared for a reason of its own (another peer, a local
fatal) at any moment.

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
"""
import sys
from collections import deque

VARIANTS = {
    # name: (n_mode, excuse, p_ending, expect_ok)
    'wait': ('wait', True, False, True),
    'wait_no_excuse': ('wait', False, False, True),
    'no_wait': ('no_wait', True, False, False),
    'abrupt_p_ending': ('abrupt', True, True, True),
    'abrupt_p_ending_no_excuse': ('abrupt', False, True, False),
}


def run(name):
    n_mode, excuse, p_ending, _ = VARIANTS[name]
    # State: (n_agent_sent, n_helper_sent, n_ended,
    #         chan_C, chan_D,
    #         p_declared, p_agent_aborting, p_helper_aborting,
    #         p_peer_aborted_C, p_peer_aborted_D, fatal)
    start = (False, False, False, (), (), False, False, False, False, False, '')

    def succ(S):
        (a_sent, h_sent, ended, C, D, decl, pa_ab, ph_ab, pab_c, pab_d, fatal) = S
        out = []
        if fatal:
            return out
        if not ended and n_mode != 'abrupt':
            if not a_sent:
                out.append(((True, h_sent, ended, C + ('ABORT',), D, decl, pa_ab, ph_ab,
                             pab_c, pab_d, fatal), 'N agent sends ABORT on C'))
            if not h_sent:
                out.append(((a_sent, True, ended, C, D + ('ABORT',), decl, pa_ab, ph_ab,
                             pab_c, pab_d, fatal), 'N helper sends ABORT on D'))
        if not ended:
            may_end = (n_mode == 'abrupt' or (n_mode == 'no_wait' and a_sent) or
                       (n_mode == 'wait' and a_sent and h_sent))
            if may_end:
                out.append(((a_sent, h_sent, True, C + ('END',), D + ('END',), decl, pa_ab,
                             ph_ab, pab_c, pab_d, fatal), 'N ends'))
        # P declares for a reason of its own.
        if p_ending and not decl:
            out.append(((a_sent, h_sent, ended, C, D, True, pa_ab, ph_ab, pab_c, pab_d,
                         fatal), 'P declares for another reason'))
        # P's owners begin their aborts at a later turn.
        if decl and not pa_ab:
            out.append(((a_sent, h_sent, ended, C, D, decl, True, ph_ab, pab_c, pab_d,
                         fatal), 'P agent begins its abort'))
        if decl and not ph_ab:
            out.append(((a_sent, h_sent, ended, C, D, decl, pa_ab, True, pab_c, pab_d,
                         fatal), 'P helper begins its abort'))
        # P's agent reads C.
        if C:
            item, rest = C[0], C[1:]
            if item == 'ABORT':
                out.append(((a_sent, h_sent, ended, rest, D, True, pa_ab, ph_ab, True, pab_d,
                             fatal), 'P agent reads ABORT on C'))
            else:
                ok = pa_ab or pab_c or (excuse and decl)
                f = '' if ok else ('agent, P declared' if decl else 'agent')
                out.append(((a_sent, h_sent, ended, rest, D, decl, pa_ab, ph_ab, pab_c, pab_d,
                             f), 'P agent reads END on C' + ('' if ok else ': FATAL')))
        # P's helper reads D.
        if D:
            item, rest = D[0], D[1:]
            if item == 'ABORT':
                out.append(((a_sent, h_sent, ended, C, rest, True, pa_ab, ph_ab, pab_c, True,
                             fatal), 'P helper reads ABORT on D'))
            else:
                ok = ph_ab or pab_d or (excuse and decl)
                f = '' if ok else ('helper, P declared' if decl else 'helper')
                out.append(((a_sent, h_sent, ended, C, rest, decl, pa_ab, ph_ab, pab_c, pab_d,
                             f), 'P helper reads END on D' + ('' if ok else ': FATAL')))
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
        for T, label in succ(S):
            if T not in seen:
                seen[T] = (S, label)
                q.append(T)
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
    for name, (_, _, _, expect) in VARIANTS.items():
        held, states, trace = run(name)
        print(f'abort_owners/{name}: {"holds" if held else "FAILS: a fatal at P"} '
              f'({states} states){"" if held == expect else "  <-- UNEXPECTED"}', flush=True)
        if trace:
            print('   trace: ' + ' ; '.join(trace))
        ok &= held == expect
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
