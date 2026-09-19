# Evidence: the in-process AM backend (§7.1 step 2)

Two models written before the backend's data structures, and the record of what
they found. Run them with `python3 <model>.py`; each exits nonzero if any case
disagrees with its recorded expectation, so they are regression tests and not
only a one-off argument. `engine.py` is the copy they were run against.

| File | Checks |
|---|---|
| `wire_model.py` | The node-pair byte stream and its wake protocol (§3.1, §3.10) |
| `credit_model.py` | Reply credits and the two bounds they keep (§3.8, §3.12, §4.6) |

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

## What they do not cover

- The reservation protocol itself; see above.
- §3.8's round-robin drain policy and the DRAINING half-buffer cap, which are
  fairness and throughput properties rather than safety ones. The review's
  `draining_sim.py` covers the cap.
- Real io_uring behaviour: multishot recv, `ENOBUFS`, provided-buffer rings, CQ
  sizing. Those arrive with step 3 and belong to `tools/probe` and §8.4's
  `echo-uring`, not to a model.
