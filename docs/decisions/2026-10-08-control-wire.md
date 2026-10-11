# 2026-10-08: the control wire (v0.2 step 4, Part 2)

The third part of v0.2's fourth step, on `docs/prompts/done/v0.2-step4-progress-helpers-prompt.md`
(the brief), its "Part 2": a node pair is joined by two connections, a control
wire and a data wire, at every helper count - no helper thread exists yet. The
author's answer to the proposal's §8 (`docs/decisions/2026-10-07.md` items 4
and 5): "mixing control with data can lead to deadlocks", and two wires a pair
always, zero helpers included, "it simplifies validation and we want dedicated
control". The design is `docs/design/progress-helpers-proposal.md` §3.1.

**Where it stands.** Built on the branch `control-wire`, from the branch
`owners` (Part 1) as it was merged: the models at `c7d021c` and `f39f7da`, the
mesh's two purposes at `31c5405` (which does not build on its own: the engine
had no control wires yet), the engine, its tests and the documents at
`4bc0f1d`, at `3d30d1c` the segment lookup that the first whole run's two
timeouts led to (item 8), the first adversarial read's answers at `17df5ac`
and the second's at `1527d3e`. Its verification, both reads and the Pi's lines
against Part 1's tree are below.

## What was built

1. **The models first.** `close_model.py` gains two wires a pair, each with its
   own goodbye, end of stream and close (`two_wires`), and two breakages: a
   goodbye on the control wire alone, whose data wire's end of stream is then a
   death (`two_wires_one_goodbye`), and a close of both sockets on either
   wire's end, which resets the other (`two_wires_close_either`). On a pair of
   nodes, since twelve channels at three ran past the half-hour bound even
   with one acknowledgement (9.1 million states): the design holds with
   liveness in 10,484 states, both breakages fail, and the one-wire cases'
   counts are unchanged. `mesh_timing.py` makes every call twice: the mesh's
   chain about 1.8 times as long, 1.7 ms at 4 nodes where it was 0.95 and
   583 ms at 1,024 where it was 322.
2. **The mesh** (`listener.[ch]`, `wire.c`). The hello names the wire it is
   for (`OSOE_HELLO_CONTROL_WIRE`, `OSOE_HELLO_DATA_WIRE`; hello version 4); a
   lead accepts either while it waits for its lower nodes, and calls each
   higher node twice, the control wire first. Owner 0's ring carries its data
   wires and then every control wire, by peer slot (`owner-map.h`'s ring
   layout), so a data wire's place on its ring is still its owner's index and
   the pass's data path is unchanged; the ring hands out readable bits for
   any range of its wires.
3. **The control wire** (`progress.[ch]`). Owner 0's alone: a queue of the
   control plane's messages - the tree's ARRIVE and RELEASE, the deadlock
   detector's - from owner 0's pool, sealed a segment at a time into a private
   buffer of 4 KiB that no PE writes, one send in flight, so that what queues
   behind a send goes out together when it completes. `forward`, `release_local`
   and `osoe_progress_deadlock_send` write the control wire's queue toward the
   node; owner 0's pass visits the control wires that received something or have
   something to send, before its data wires. Its segments are acknowledged as a
   data wire's are - the adversarial read's F1, below: as first built nothing on
   a control wire was. The brief's item 2 names team control among the wire's
   messages too: osoe has no such message - a node learns a team from its record
   when it first meets it (`known_team`) - so the tree's two and the detector's
   are the whole control plane.
4. **The planes kept apart.** A message of the control plane's kind arriving
   on a data wire, or any other on a control wire, ends the job naming both;
   the abortive goodbye goes on every wire. A data connection keeps the AMO
   results, its *value* queue (renamed from `control`), drained first, and the
   get replies' bulk queue.
5. **Each wire closes on its own** (§6.4): its goodbye after its own last
   bytes, its end of stream after its goodbye, its socket closed once the
   peer's end of stream has arrived on it, its end judged by its own goodbye
   (one `receive_wire` for both kinds). Idle, close, closed, abort, aborted,
   the peers' end and the silent check cover owner 0's control wires.
6. **Counters** (node layout 35). A connection's record counts its data wire
   - the node's segment and message counts are the data plane's, divided
   among the records - and its `control_queue_depth` is now its control
   wire's queue, with the value queue's gauges beside it; the node record
   gains the control plane's own counts (`transport.am.control_segments_sent`,
   `_applied`, `control_messages_applied`) and the value queue's high water.
   `osoe-stat`'s connection table shows `ctrlq`, `valq` and `bulkq`.
7. **Tests.** Every engine test opens two wires a pair; `shmem.owners` runs a
   barrier at 2 and 3 helpers, over owner 0's control wires while the helpers'
   rings carry the data, which Part 1 could not.
8. **A test's read of its own node's segment** (`tools/stat/segment.c`). The
   first whole debug run lost `shmem.circulant.8x2` and `shmem.circulant.5.perturb`
   to `osoe-launch`'s 240 s, run beside the 64- and 70-node cases. Not a hang:
   alone, four copies at once and beside the 64-node case they passed every
   time. But the many-node tests ran longer than Part 1's, on a quiet host and
   in the order A B B A: `shmem.circulant.64` 69.5 s against 47.0,
   `shmem.circulant.33` 14.6 against 11.2, `shmem.shape.chosen.pi.64` 5.0
   against 3.0, and four at once, as the suite runs them, 8x2 over 240 s against
   209 - where Part 1's own verification had it at 236.6 s of 240. The engine
   was not the cost: at 64 nodes, release builds, three runs each, a barrier
   took 347 to 453 µs on Part 2 - one phase of six 765 - and 375 to 394 on
   Part 1, an 8-byte reduction
   after them 487 to 598 and 486 to 522, a put with a quiet 127 to 267 and 152
   to 219, and a loop of 20,000 reductions the same CPU time (user 9.7 and 10.0
   s against 9.6 and 9.4, system 19.1 and 19.0 against 18.8 and 19.9).
   The tests' own checks were. Each check of what a node's lead published opens
   that node's segment by name, a live job's segment has no name (§3.14), and
   the open found it by a walk of every process's descriptors, a `readlink`
   each: N(N - 1) sockets on N logical nodes of one host, twice that with two
   wires a pair. The open now looks at its own process's descriptors first,
   where a PE reading its own node's segment finds it, and only then walks
   /proc, as `osoe-stat` must. With it, Part 1 and Part 2 take the same time,
   and less than either did: `shmem.circulant.64` 17.3 s and 17.8,
   `shmem.circulant.33` 5.5 and 5.6, `shmem.shape.chosen.pi.64` 0.88 and 0.91,
   and four at once 8x2 24.0 s and 25.4, the 64-node case 44.4 and 46.1, the
   70-node 45.3 and 46.8: the suite's margin under 240 s about tenfold again,
   for both trees. The evidence:
   `docs/design/evidence/v0.2-step4/2026-10-08-control-wire/` (`ab/`, the
   probes).

## Where the build departs from the proposal

Two small ones, recorded rather than asked. §3.4 counts the control wires'
private buffers with the connection area's growth, and they are in the lead's
own memory instead (`osoe_am.control`, progress-host.c), since no PE reads or
writes them (§3.1: "its send buffer is private to the lead"). 4 KiB a peer node
- §3.1's "a page", which on the Pi 5, whose pages are 16 KiB, it is not - and
with the wire's other state 4.6 KB a peer node of the lead's memory (4,336
bytes on aarch64 with its acknowledgements), 2.2 MiB at 500 nodes, in the
README's table. And the brief's
item 1 has the listener tell the two wires apart "by the first message's kind";
the first message is the hello, and its purpose field names the wire, hello
version 4.

And one addition, F1's answer (below): the proposal's control wire asks for
no acknowledgement, and its §3.1 argued the planes need nothing of each other
partly because "the detector reasons about counters, not messages" - but the
detector's counters are acknowledgements, so the control wire's segments are
acknowledged as a data wire's are. It keeps what the design notes state, the
detector's proof and invariant 22 ("every applied segment is eventually
acknowledged"), and the proposal's sentence carries a dated amendment saying so.
The other fix the read named, the detector's reports carrying each node's
counts of control messages sent and applied, would have changed the detector's
protocol and its proof (§3.15); it was not taken, and the author may choose it
instead.

## What waits

The brief's item 4 has `osoe_progress_idle` and `_quiet` answered through
request words the owners answer. With no helper thread, owner 0 answers its
own; the words come with the threads in Part 3, as Part 1's stop and abort
words do (`docs/decisions/2026-10-08-owners.md`). So does a peer's abort across
owners: with no helpers owner 0 holds both of a peer's wires and treats an abort
read on either as the peer's (the second read's S1 and the first's N2); with
helpers the data wire is a helper's, and the word must cross between the two
owners, which Part 3 decides with the stop and abort words.

## The verification

**Before the commit**, the static checks and the changed areas' tests, from a
tar of the tree with the segment lookup in it, in debug: `shmem.exports`,
`memory-orders`, `profiling_aliases`, `boundary`, `static-link` and
`strict-c11`, with `shmem.owners`, `owner-map`, `work-discovery`, the engine's
tests, `shmem.stat.*`, `stat.*`, `shmem.shape.chosen.*` and the circulant cases
that had timed out - 63 of 63, no warning.

**At `3d30d1c`**, from a tar of the commit (`git archive`), in the VM `osoe`
from 13:17 (`verify.sh`, the rungs' script): the four presets with the floor
pins on - debug and release 844 of 844 (release without the models), asan 829
of 829, tsan 588 of 588, as many as Part 1's - `tier3` 113 of 113, and
`osoe-pacer` with no failure, `mailbox` and `collectives` a minute each and
`mix` ten minutes, ending at 13:49. The debug suite took 263 s of test time
where Part 1's took 520, the segment lookup's doing: `shmem.circulant.8x2` 27.5
s where Part 1's run had it at 236.6, `5.perturb` 18.7 where it was 188, the
64- and 70-node cases 53.6 and 55.8 where they were 215 and 260. In the VM
`osoe-load`, from the same tar: the `models` preset in a release build, 35 of
35 in 42 minutes, and the SOS suite at 4 × 2, 161 programs built and passing on
every map, the two that do not build and the two that fail by design as at the
tag.

**After the read**, at `17df5ac`, the whole debug suite first on a tar of the
tree (846 of 846, no warning: the static checks, and the two forged tests the
answers add), and then the same as at `3d30d1c` from a tar of the commit, in
the VM `osoe` from 14:27 to 14:59: debug and release 846 of 846, asan 831 of
831, tsan 590 of 590 - the two forged tests the difference - `tier3` 113 of
113, and `osoe-pacer` with no failure, `mailbox` and `collectives` a minute
each and `mix` ten minutes. In the VM `osoe-load`, from the same tar: the
`models` preset, 35 of 35 in 44 minutes, `deadlock_model.py`'s two new cases
among its own, and the SOS suite at 4 × 2 as at `3d30d1c`.

**After the second read**, at `1527d3e`, from a tar of the commit, in the VM
`osoe` from 15:30 to 16:00: debug and release 846 of 846, asan 831 of 831, tsan
590 of 590, `tier3` 113 of 113, and `osoe-pacer` with no failure, `mailbox` and
`collectives` a minute each and `mix` ten minutes; in the
VM `osoe-load`, the three models it touches through their registrations,
`model.deadlock`, `model.control_ack` and `model.carrier`, each passing - the
models preset 37 now.
The merge's library is `1527d3e`'s: what follows it on the branch is documents
- `main`'s since Part 1, this record and the indexes. Merged into `main`.

## The adversarial read

By a fresh agent given the diff from `95f3339` to `3d30d1c`, the brief's Part 2
and its stop list, the proposal's §3.1, §3.4, §3.5 and §8, and eleven
properties to attack, the first whether any control message needs a data
message applied before it: `docs/reviews/archive/v0.2-step4-part2-adversarial-read.md`.
Its verdict: no control message needs a data message applied first; the send
path, the ring's split between the two ranges (checked over 73,700 layouts by
a program of its own, two wrong variants failing), the close and the abort per
wire, the reply pool's bound, the mesh and the segment lookup hold; one fatal
finding, two should-fixes and three notes, each answered on the branch.

- **F1, fatal: the deadlock detector's proof no longer held.** The detector
  declares only when every node was quiet in two samples that bracket a moment
  t*, and its proof's step is that a message in flight at t* was acknowledged -
  applied - before its sender's first sample ended (§3.15). The tree's ARRIVE
  and RELEASE are application work their receiver counts. On the data wire,
  where they rode until this part, their segments were acknowledged before
  their sender could look quiet; on the control wire as first built nothing
  was, and `osoe_progress_quiet` looked at the data wires alone. So a barrier's
  message held in the network - a path dropping it while TCP retransmits, past
  the root's two collections, about 11 s at the defaults - could have a job
  that would finish declared deadlocked and ended with status 1. No test over
  loopback can show it; the reader's copy of `deadlock_model.py`, with the
  ARRIVE unacknowledged, fails its safety in 5,238 states, and the same
  message acknowledged holds. Of the two fixes it named, the one built
  acknowledges a control wire's segments as a data wire's are, and quiet waits
  for them; the other, the detector's reports carrying counts of control
  messages sent and applied, would change the detector's protocol (above). The
  one built changes nothing the design states - invariant 22 already said every
  applied segment is eventually acknowledged, and §3.15's proof needed it; the
  control wire's segments carry the same header, whose `seq` and `ack` the
  first build sent unused; the detector, its messages and its proof are as they
  were. The control wire keeps a data connection's five numbers of
  acknowledgement (`progress.h`), seals one owed or a request still to carry as
  a header-only segment, asks by the data wire's rule (`asks_for_ack`, shared
  now), and `osoe_progress_quiet` asks of the control wires as of the data
  wires. `deadlock_model.py` gains the first build as a breakage
  (`control_wire`, failing) and the design (`control_wire_acked`, holding with
  liveness in 18,808 states); `shmem.barrier-am` checks that a node whose
  ARRIVE has gone but which its parent has not released is not quiet, and that
  every node is quiet once released.
- **S1, sentences this commit made false**: the README's "one connection" in
  three places and the mesh's connection count, design §3.8's flush of a
  child's connection, invariants 3 and 22, "a page" for the private buffer, and
  two comments; each corrected.
- **S2, the zero-copy setting's node limit halved**: the completion queue
  counted 16 notifications for every wire on a ring, the control wires among
  them, which never send by reference, so a job of more than 1,819 nodes was
  refused with the setting on. The ring counts notifications for the wires
  that may lend - an owner's data wires, first on its ring - and a control wire
  is attached with the setting off: 3,274 nodes, where one wire a pair allowed
  3,638, the control wires' own two completions each the difference (the README
  and §3.3).
- **N1, a misrouted payload landed before its refusal, and no test sent one**:
  the planes are checked before a payload's destination is resolved, as well as
  when a message completes, and the refusal names the message's kind;
  `shmem.progress.forged-barrier-on-data` and `-get-reply-on-control` send one
  of each and expect it - the second ended the job before as a reply nobody
  asked for, its landing looked up as a data connection's.
- **N2, the planes shared the pass, data first**: the pass visits owner 0's
  control wires before its data wires, so a barrier's message reaped beside a
  stream is applied first and what it forwards reaches the kernel first; with
  no helpers, where one owner holds both of a peer's wires, its abort read on
  either excuses the other's reset; and the README and §3.1 say the planes
  share the link and the progress thread, not only the link.
- **N3, a message queued after a wire's goodbye**: the goodbye is sealed on an
  empty queue, closing, rather than on an empty segment, which an
  acknowledgement is too; a control message after its wire's goodbye is
  dropped if the detector's and refused by name if the tree's; and a wire whose
  goodbye has gone is visited only while it is still on the wire.

The mutants, each a copy of the tree with one change, built in debug and run
against the tests of what it breaks (`mutants/` in the evidence): at
`3d30d1c`, the control range taken one wire late - eighteen tests, "receive
buffers held across a reap"; a short send's rest offered from the segment's
start - twelve, "segment 2 claims 0 bytes"; a control wire's busy bit always
cleared - the barrier, progress, owners and multinode tests, each hanging to
its limit; the segment lookup's walk of /proc dropped - the team-storage,
`osoe-stat` and unswept-wait tests, which run `osoe-stat` against a live job.
After the read's fixes, against the forged tests, the engine's barrier and
the deadlock detector's tests: the planes checked never - both forged tests,
the barrier taken for one whose tree it is not, the get reply refused as one
nobody asked for; checked only as a message completes - the get reply's;
quiet looking at the data wires alone, F1 itself - all four `shmem.barrier-am`
cases, a node not yet released found quiet; and a control wire never asking -
four `shmem.deadlock` cases, a deadlock never declared, since nothing answers
an acknowledgement nobody requests.

## The second adversarial read

The answers changed the control plane's protocol, so a second fresh agent read
them - the diff from `3d30d1c` to `17df5ac`, with ten properties to attack,
first the control wire's acknowledgements against the data wire's:
`docs/reviews/archive/v0.2-step4-part2-second-adversarial-read.md`. Its
verdict: no fatal; F1's answer holds - the tree's messages acknowledged before
their sender can look quiet, the detector's excluded, no answer that asks,
nothing owed without its busy bit, quiet and the close always reached - and so
do the answers to S1, S2, N1 and N3. One should-fix and four notes, answered:

- **S1, two callers of `shmem_global_exit` on two nodes could each wait 20 s
  for the other.** Each side tells a peer nothing more on a wire where the
  peer's abortive goodbye has arrived, so two that cross arrive on different
  wires, and `osoe_progress_peers_ended`'s carrier rule - wait for every peer's
  stream but one whose abort names another caller - judged each wire alone:
  each carrier waited for the other's end of stream on the wire its abort had
  not come by, to `OSOE_ABORT_LEAD_WAIT_SECONDS`. With no helpers a peer whose
  abort naming another caller has arrived on either wire is not waited for on
  either (`peer_carries_another`). The reader's `carrier_model.py` is in the
  tree: one wire holds, two judged per wire fail, either wire holds. No test
  places the crossing, which needs each side's goodbye to arrive between the
  other's two wires' visits; the model checks the rule.
- **N1, the excuse works in one order within a pass**: a peer's abort on its
  control wire excuses its data wire's reset in the same pass, the control
  wires going first; the converse needs a control wire reset, so its receive
  window full, which nothing that rides it fills. The comment and design §6.4
  say so now.
- **N2, no model checked the control wire's acknowledgements**:
  `deadlock_model.py`'s `control_wire_acked` assumes them. The reader's
  `control_ack_model.py` is in the tree: the C's steps searched exhaustively
  between two leads - safety, no echo, nothing stranded, nothing after a
  goodbye, quiet and the close reachable - the design holding, ten breakages
  each failing the check written for it, and `control_busy`'s request term
  shown redundant, kept as the data wire's `busy()` keeps its own.
- **N3, `shmem.barrier-am`'s final quiet does not need the request**: a child's
  goodbye carries the root's acknowledgement. Its comment says so; the request
  path is `shmem.deadlock`'s, as the mutant that never asks showed, and the
  model's.
- **N4, the documents**: the proposal's §3.1 amended (dated) where F1 refuted
  it and where "a page" is 4 KiB; design §3.1's parenthesis; the departures
  above, which now list the acknowledgement; the control wire's state 4,336
  bytes.

## On the Pi

The four Raspberry Pi 5 nodes at a gigabit, twice: the branch at `3d30d1c`
from 13:18 to 13:45, and after the adversarial read's answers at `17df5ac`, the
tree that merges, from 14:30 to 14:57 - each against Part 1's tree (`95f3339`,
whose library `main` has), alternating three times, as the brief's item 5 asks
at no helpers: a barrier behind a 1 MiB put stream between the same two nodes,
alone and under it (`barrier-behind.c`: PEs 1 and 3, one on each node, syncing
a team of the two while PE 0 streams to PE 2) - the control plane's own number;
a fetch-add behind that stream (`hol.c`); `osoe-bench` at 4 × 4 and 4 × 1; the
stream with each node's agent CPU (`putstream-cpu.c`); the mailbox
(`osoe-pacer`, 4 × 2, 20 s); and start-up at 4 × 1 and 4 × 4 (`startup.c`). The
scripts, every output and both comparisons are in
`docs/design/evidence/v0.2-step4/2026-10-08-control-wire/` (`pi-compare.sh`,
`pi-compare.py`, `pi-17df5ac/`, `pi-3d30d1c/` and their `pi-comparison-*.txt`);
`hol.c` and `putstream-cpu.c` are the earlier records' evidence, unchanged.

**A barrier behind the stream: 2.5 to 2.6 times faster at the median, 2.9 at
the 99th percentile**, the medians of three runs. At `17df5ac` medians 1,663 to 1,665 µs where Part 1's were
4,139 to 4,415, the 99th percentiles 2,216 to 2,224 where they were 6,089 to
6,388, the stream at 112 MiB/s under both; at `3d30d1c` 1,663 to 1,667 and
2,220 to 2,237 - the pass's new order, control wires first, moved nothing here.
On one wire the barrier's ARRIVE and RELEASE waited behind the stream's bytes in
the connection's buffers and the socket, as a fetch-add still does: 3,877 to
3,883 µs and 3,853 to 3,888, since it rides the data wire. What is left, 1.7
ms, is the link's: the stream's bytes in flight ahead of the control wire's
segment in the sender's queue and the switch, which a second connection does
not pass.

**Nothing else moved, and the barrier alone did not cost** - the brief's stop
condition. The barrier alone: medians 165 to 174 µs, Part 1's 174 to 175 (at
`3d30d1c` 161 to 174 and 171 to 176). Every remote line of `osoe-bench`, at 4 ×
4 and at 4 × 1, within 0.3% as the median of three runs (a put and quiet of 8
bytes at 4 × 4 169.5 and 169.6, of 1 MiB 9,079.5 and 9,081.0, the stream 112.1
MiB/s, a fetch-add 163.6 and 163.8); at `3d30d1c` the 4 × 1 small lines fell
in the two modes Part 1's record describes, in both trees. The stream 112.1
MiB/s in all twelve runs; the sending agent 0.284 to 0.320 CPU-seconds a GiB
here and 0.289 to 0.298 on Part 1's tree; the receiving one 1.61 to 1.70 s of
20 against 1.51 to 1.54 in this run - in the first, 1.62 to 1.64 against 1.55
to 1.64, and Part 1's record had 1.56 to 1.60: within the runs' spread taken
together, if at its top. The mailbox 37,873 to 40,703 messages a second
against 36,836 to 40,623. Start-up, `shmem_init` on the slowest PE: at 4 × 1
84.8 to 88.9 ms against 82.4 to 95.6, at 4 × 4 83.7 to 100.4 against 89.3 to
94.8 - the mesh's second connection a pair, a millisecond by the model at four
nodes, inside the noise of a start of 90.
