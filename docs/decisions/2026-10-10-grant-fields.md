# 2026-10-10: a reduce-scatter grant's field for each receiver

The author's third answer at v0.2 step 4's merge point
(`docs/decisions/2026-10-10-step4-merge-point.md`, "The author's answers"):
the word two progress helpers wrote at once is to be changed, not only
documented, right after the merge point's merge.

## What it was

- **The grant:** in Träff's reduction (v0.2 step 3), each lead's reduce-scatter
  has one sender a round. As a lead enters, it grants the sender of each of its
  rounds its lane. Into that sender's grant line it put two things, then added
  one to the sender's grant counter, fenced after them:
  - the collective's head - epoch, identity and bytes - at the line's start;
  - its lane, into the field for the round.
- **Every receiver of a sender put the same head into the same 16 bytes.** With
  no helpers the one agent applied the puts one after another. With helpers the
  receivers' data wires are different owners' (`owner-map.h`), so two helpers
  copied the same bytes at once, with nothing ordering them.
- **The bytes were identical, and the sender reads after every add,** so nothing
  went wrong on any hardware. But it was a word two owners wrote, which the
  progress helpers' design rules out; 39 of the 95 ThreadSanitizer reports Part
  3's verification passed at two and three helpers; and the brief's own
  stop-and-ask (correctness Q2-4).
- **The sender checked the head against its own call** - the last receiver's to
  land. So a divergence among its receivers could pass it, and be found a hop
  later or by the deadlock detector. That was step 3's after-merge item: "a
  field per receiver with its epoch".

## What was built

**A field for each round, which that round's receiver alone writes:** the
epoch, identity and bytes of the collective it entered, and the lane it lends,
24 bytes (`struct circulant_grant_field`, `internode-generic.c`).

- **The receiver** puts its field and adds to the counter, one put where there
  were two.
- **The sender,** once its counter shows every grant, checks every field against
  its own call. A field that differs ends the job naming its receiver's node, its
  round, and both calls: "PEs called different collectives, or the same one with
  different arguments", as before, now with the receiver named.
- **The fields are read, then cleared,** as the head and the lanes were.

**The grant's lines** are as many as the counter and the fields take
(`osoe_internal_circulant_grant_lines`, `internal-region.h`):

- one line up to five rows, so up to 32 nodes nothing moves;
- from 33 nodes a second line, a third from 1,025, and a fourth past 32,768.

A team number's reduce-scatter words grow past 32 nodes by a line, 128 bytes;
past 1,024 by two, and past 32,768 by three. At the default 256 team numbers
that is 32, 64 and 96 KiB a PE: the internal region 16.04 MiB at 64 nodes where
it was 16.01, and 16.32 at 500 where it was 16.29; a team number 27.0 KiB a PE
at 500 nodes where it was 26.9, and 27.4 at a thousand where it was 27.25. The
README's memory table and settings rows say so, and its 64 × 64 column of what
every PE maps rounds to 17.1 GiB where it gave 17.0.

**The model needed nothing.** `circulant_model.py` had always given each grant a
field of its own carrying its collective, and checked each one. The C now does
what the model did, and the model's comment says so.

## The verification

**At `bdb181a`, the change as first committed**
(`<repo>/build/scratch/ladder/merge-point/verify-grant-fields.sh` and
`verify-grant-fields/`, the lead's scratch). The merge point's verification ran with the circulant
model and its digest for the models, the only model file changed:

- the static checks: 12 of 12;
- debug and release: 857 of 857 at no helpers, two and three;
- AddressSanitizer: 842 of 842 at two and three helpers, and at none all but
  `shmem.helper-wake` (below);
- ThreadSanitizer: 600 of 600 at none, and at two and three, 13 and 14 tests
  failing, the gate passing both;
- tier 3: 113 of 113;
- the circulant model and its digest: both;
- `osoe-pacer` at no helpers and two: `mix`, `mailbox` and `collectives` with
  the Pi's link a minute each and `mix` ten minutes, every one `failures=0` and
  no deadlock.

**ThreadSanitizer's two landings of one word** are gone: one at each count,
where the merge point's verification at `4f1014d` had 22 and 49, the change's
whole purpose.

- **The one left** is a small row's lane word in `shmem.lanes.rotation`, written
  by successive collectives' parents as the trees rotate, ordered through
  another process: the correctness review's Q2-2 named it.
- **The reports in all** fell from 42 and 87 to 26 and 47. The README's
  ThreadSanitizer paragraph is written from them: about a dozen tests, under
  half to three quarters of the reports in the library's own words, and the
  grant's head named as what it was.

**The mutant, the field check removed** (`bool same = true;`), passed
`shmem.circulant.4.diverge`. The fresh read had said it would (below):

- **Why:** with no field checked, every lead starts its rounds. The receiver
  of the divergent lead's first round refuses its second piece, one byte where
  16,384 were due, and `check_round_piece` ends the job with the closing phrase
  the test's pass expression matched.
- **The fix:** the expression is now the field check's own words. The tip's run
  shows the mutant failing it.

**`shmem.helper-wake`,** failing again under AddressSanitizer at no helpers.

- **Where:** at the same place in the suite as at `4f1014d`, its neighbours the
  same each run: node 1's owner woken 8, 0 and 0 times in three attempts, and
  node 3's - the same helper - 99 a little later.
- **Why:** a helper starved of a core by the tests beside it, which the retries
  within a tenth of a second cannot outlast. Alone it passed 20 runs of 20.
- **The fix:** the suite runs it alone (`RUN_SERIAL`). Waiting for the owner's
  sleeping flag, which would hold it beside busy loops too, stays on the release
  checklist.

**At the tip** (`verify-grant-fields-final.sh`, `verify-grant-fields-final/`):

- the static checks;
- the four presets built;
- the AddressSanitizer suite at no helpers, `shmem.helper-wake` run alone;
- the divergence test as it is and against the mutant.

At `4a557b1`, the commit that merges:

- **The static checks:** 12 of 12.
- **The four presets:** each built without a warning.
- **The AddressSanitizer suite at no helpers:** 842 of 842, `shmem.helper-wake`
  run alone at its end and passing.
- **`shmem.circulant.4.diverge`:** passing as it is, and failing against the
  mutant with no field checked, whose only message is the piece header's a
  round later: "a piece of the reduce-scatter, from node 1's lead (PE 1), should
  be round 0's piece 1 (16384 bytes) ...".

**Where it stands.** Merged into `main` after this record, the branch's tree
whole.

## The adversarial read

A fresh agent read `bdb181a` against this record and the review's Q2-4
(`docs/reviews/archive/v0.2-step4-grant-fields-adversarial-read.md`).

**Nothing fatal.** It checked five things:

- **The layout:** every offset is inside its team number's words at every row
  count from 1 to 16, and the region reserves what the code writes.
- **One writer a field:** each field has one writer, the receiver the message
  names, by a program over the real schedule for 2 to 300 leads.
- **The handover:** it is ordered.
- **The next collective:** a field is read and cleared before its writer can
  write it again, since each writer waits for the reader's allgather grant.
- **The check and the model:** no correct program can fail the check, and the
  model needed no change.

**Four should-fix, all made in the commit that merges:**

1. The divergence test's pass expression, which the mutant passed by a piece's
   header (above).
2. README numbers the change moved: a team number 27.4 KiB at a thousand nodes
   where it said 27.25; at 500 nodes, `OSOE_SMALL_COLLECTIVE_BYTES`'s 9.0,
   11.3 and 27.0 KiB; and the 64 × 64 mapping's 17.1 GiB.
3. The ThreadSanitizer paragraph, written from this run.
4. Three comments still giving the old layout.

**Of its notes, made:**

- the checklist's citation of step 3's findings, now 2 and 7;
- this record's sizes past 1,024 nodes;
- the model's comment on the fields;
- the design sentence, split in two.

**Left as notes:**

- **The model's tag names the writer and the round, which the C's field does
  not.** The C trusts the schedule for who writes field k, and both sides
  compute it with the same function.
- **The model never clears a field.** The C's clearing rests on the allgather
  grant.
