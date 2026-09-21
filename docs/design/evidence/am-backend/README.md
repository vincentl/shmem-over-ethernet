# Evidence: the in-process AM backend (§7.1 step 2)

Four models, and the record of what they found. The first three were written
before the data structures they check; the fourth, `ack_model.py`, was written
after the second adversarial review found the acknowledgement protocol wrong,
from the C it was wrong in. Run them with `python3 <model>.py`; each exits nonzero if any case
disagrees with its recorded expectation, so they are regression tests and not
only a one-off argument. `engine.py` is the copy they were run against.

| File | Checks |
|---|---|
| `wire_model.py` | The node-pair byte stream and its wake protocol (§3.1, §3.10) |
| `credit_model.py` | Reply credits and the two bounds they keep (§3.8, §3.12, §4.6) |
| `arrival_model.py` | The internode arrival handoff, and whether one word per team is enough (§5.3, §3.12, §4.9) |
| `ack_model.py` | The acknowledgement protocol between two leads: quiet's termination, and whether a quiesced pair falls silent (§3.4, §4.6) |
| `reservation_model.py` | §3.5's reservation protocol as the C implements it, and §3.8's pre-open drain (§3.5, §3.8) |

Every recorded `.out` reproduces by plain `diff`: result lines carry no
wall-clock when not written to a terminal, and a `+live` suffix says the
engine's trapped-state check ran and found nothing (`engine.py`, "Liveness").

**§3.5's reservation protocol** was at first left to the review's two models of
the design text (`docs/reviews/evidence/transport/`, `…/refutation/`). The
second review wrote a third from the C, and slice 2 promoted it here
(`reservation_model.py`) because it needed §3.8's pre-open drain added: replies
written into a freed buffer while it is still sealed, then the buffer opened
past them. Four breakages cover the drain - an open that resets `head` over the
replies (QUIET fails), a drain whose `filled` misses the header (STUCK: the
accounting defect the review measured), a drain that does not consume its reply
(ONCE, first), and one that takes the newest reply first (ORDER, first). The
review found that neither ONCE nor ORDER had ever been the first check to fail
in any variant; these two exist so that each has. The review's copy also saw
only a duplicate across segments, not within one, so a same-segment duplicate
used to surface as ORDER; `deliver_check` now counts both. `current`, the tree
before slice 2 with no pre-open drain, passes every safety check: starving a
reply is a property of the scheduling policy, not of this protocol.

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

## The acknowledgement protocol (`ack_model.py`, phase 1 step 2 slice 2)

The second review (`docs/reviews/phase1-step2-slice1-correctness.md`, C1 and
C2) found the protocol wrong twice, each defect hiding the other, and said a
model was owed. Three variants, two properties, and the verdicts the review
predicted - with one correction to the property it predicted them against.

- **`current` fails QUIESCES in 95 states**: a segment carries ACK_REQUESTED
  when no writer on either node wants anything, because one bool means both
  "the peer asked me" and "my writer asked". That is C1's echo.
- **`naive` fails TERMINATES** - the engine's trapped-state check, with `done`
  being both quiets returned. Splitting the bool ends the echo and exposes C2:
  the request is consumed by whichever segment is finalized next, possibly one
  sealed before the write the writer waits for, and nothing asks again. It is
  also caught sending an unwanted request, once, which is bounded and so is
  not C1 - but is not right either.
- **`design` passes both**, SC and TSO, at one and two messages per side: the
  request carries "acknowledge through S", and a header-only segment sealed
  only to pay a debt never carries the flag.

**The property as first stated could not be met by any correct protocol.** It
was "in a quiesced state no lead has a send step enabled", and `design` failed
it: a lead that receives ACK_REQUESTED must answer, and it cannot know that the
asking node's quiet was satisfied meanwhile by some other segment's cumulative
ack - so one last header-only segment goes into a quiesced pair. What separates
it from the echo is that it carries no request, so nothing answers it. QUIESCES
is therefore checked where the echo is decided: *a segment carries the flag only
while some writer, on either node, wants an acknowledgement beyond what its peer
has acknowledged*. The first version of that check compared only the sending
node's own writer, and passed `current` - each hop of an echo is sent by a node
whose own writer is satisfied, while the peer's may still be waiting. The
second version of the model then passed `current` again, because the check
instruction sat on a path nothing jumped to; it was found by asking the model
how many states ever reached it (none). Both are recorded because both passed
every case they were given.

**The model is bounded by capping header-only seals, and nothing else.** Data
segments are bounded by how many messages a writer sends, and they are what
quiet waits for; capping them would report a hang the protocol does not have.
Each TERMINATES verdict is run at a cap and at the cap plus one: identical state
counts mean the cap did not bind, so the verdict is the protocol's. `design`
needs **three** header-only seals per node in its worst interleaving - at two it
is TRAPPED, at three and four it passes with the same 71,648 states. `naive`
is TRAPPED with the same 78,698 states at three and four: a real hang. `current`
cannot be asked TERMINATES at all: the echo is the only thing that carries a
late acknowledgement there, so every finite cap starves quiet, and its state
count grows with the cap to say so.

## What they do not cover

- The reservation protocol itself; see above.
- §3.8's round-robin drain policy and the DRAINING half-buffer cap, which are
  fairness and throughput properties rather than safety ones. The review's
  `draining_sim.py` covers the cap.
- Real io_uring behaviour: multishot recv, `ENOBUFS`, provided-buffer rings, CQ
  sizing. Those arrive with step 3 and belong to `tools/probe` and §8.4's
  `echo-uring`, not to a model.
