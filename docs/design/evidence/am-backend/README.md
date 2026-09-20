# Evidence: the in-process AM backend (§7.1 step 2)

Three models written before the data structures they check, and the record of
what they found. Run them with `python3 <model>.py`; each exits nonzero if any case
disagrees with its recorded expectation, so they are regression tests and not
only a one-off argument. `engine.py` is the copy they were run against.

| File | Checks |
|---|---|
| `wire_model.py` | The node-pair byte stream and its wake protocol (§3.1, §3.10) |
| `credit_model.py` | Reply credits and the two bounds they keep (§3.8, §3.12, §4.6) |
| `arrival_model.py` | The internode arrival handoff, and whether one word per team is enough (§5.3, §3.12, §4.9) |

**§3.5's reservation protocol is not modelled here.** It already has two
independent exhaustive models from the review, in
`docs/reviews/evidence/transport/` and `docs/reviews/evidence/refutation/`, and
their `fixed` variant is exactly the protocol this step implements. Re-running
`docs/reviews/evidence/transport/resv_model.py` at step 2 reproduced all five
recorded outputs byte for byte, including the four broken variants that fail
with counterexamples. A third model of the same protocol would have added
nothing.

## What the models found

Three results, none of them the prediction the models started from. Each is
written into the design section it belongs to.

**The credit take needs a CAS loop, and §3.8 did not say so.** The credit is per
*connection* and shared by the node's PEs, so a test-then-decrement lets two PEs
of one node both pass the test and both decrement, handing out a credit that
does not exist. This is the same argument §3.5 already makes for the reservation
itself, and it was found because the model's own first encoding of the take had
the bug. The `race_take` case keeps it.

**Every breakage passes with one PE per node.** The credit only binds when a
node has more than one PE contending for it, which is every real node. A model
run only at `pes=1` would have called all four breakages correct — a reminder
that the smallest configuration is not always the one that discriminates.

**The two bounds separate, and not where expected.** The credit keeps a bound at
each end of the connection: reply descriptors at the target (§3.8) and replies
in flight toward the origin's landing zone (§3.12). Returning the credit at
handler time breaks both. Returning it when the reply is *drained onto the wire*
rather than when it *lands* keeps descriptors bounded and breaks only the
landing zone — and that cannot be seen at all while gets are blocking, because a
PE waits for its own reply before issuing again (§4.6). Only the `nbi` cases,
where a PE issues every request before landing any, show why §3.8 says "lands".
Recorded so nobody simplifies it later on the evidence of the blocking cases
alone.

**One word per team is enough, and the reason is §5.4's bounded skew.** §5.3's
arriver cannot perform the arrival itself - the progress state is private to the
lead's agent thread, in the lead's process - so it hands the epoch over through
a word in the node header and wakes that agent (§3.12: "a flag plus a futex
wake"). Whether that word needs a queue behind it is the question
`arrival_model.py` was written to settle, and the answer is no: epoch k+2's
arrival cannot be posted until every local member has been released from k+1,
which cannot happen until the lead has already consumed and forwarded k+1. The
`$posted`/`$consumed` pair checks that argument rather than asserting it, and
`consume_reread` shows what a single word costs when the rule is broken - the
agent records the epoch by re-reading the shared word *after* forwarding, a
member re-enters and posts the next epoch into it first, and that arrival is
lost. Every other node then waits for a node that will never arrive.

**The smallest configuration that fails is not the same for each defect.**
`wake_after` - the agent loading the handoff before the wake word - fails with a
single epoch, because the very first arrival can land between the two loads.
`consume_reread` and `noconsume` need two, because both are defects in *reusing*
the word and one epoch never reuses it. A model run only at one epoch would have
called two of the three breakages correct. This is the same shape as the credit
model's `pes=1` result and the barrier test's two-node result: the configuration
that is cheapest to run is repeatedly the one that discriminates least.

**`noconsume` is in the model only to make a check able to fail.** The ordering
check - arrivals reach the root once each, in epoch order - was never reached by
any other case, so it asserted nothing. A breakage that forwards without
recording the epoch forwards it twice and reaches it. Adding the case was the
cheaper half of this project's standing rule that a check nothing can fail is a
check that is not there.

**A fifth, found while testing the wire.** A reader that publishes `consumed`
before copying the byte out of the slot lets the writer refill it mid-copy. The
single-process test cannot see this at all — nothing races — and the two-process
test sees it in about half of runs, measured at 4 of 8 with the defect present
and 0 of 8 without. It is settled here instead, as `consumed_early`, because a
model reaches it in every interleaving that contains it. The lesson generalises
to the rest of this step: a publication-ordering defect belongs to a model, and
a test across two processes is corroboration rather than the proof.

## What they do not cover

- The reservation protocol itself; see above.
- §3.8's round-robin drain policy and the DRAINING half-buffer cap, which are
  fairness and throughput properties rather than safety ones. The review's
  `draining_sim.py` covers the cap.
- Real io_uring behaviour: multishot recv, `ENOBUFS`, provided-buffer rings, CQ
  sizing. Those arrive with step 3 and belong to `tools/probe` and §8.4's
  `echo-uring`, not to a model.
