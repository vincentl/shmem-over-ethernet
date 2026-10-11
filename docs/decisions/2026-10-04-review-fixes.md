# 2026-10-04: the fixes of the round-four review (the release fixes' merge point)

The round-four review ran on `main` at `08b3da0`, the tree the release fixes
left (`2026-10-02-release-fixes.md`), as the first of the three merge points of
`docs/prompts/done/task-first-release.md`: five investigators, a refutation pass and
a synthesis, `docs/reviews/archive/phase1-release-fixes-review.md`, whose verdict
is *release after named changes*. This record is those changes, made on the
branch `review-fixes` from `08b3da0`, by Claude under the task's mandate:
decide what a brief does not list, and record the decision with its undo.

## The findings, and what was done

1. **A start by the same ids bound the job tables and connection area a failed
   start left** (correctness F1, confirmed by the refutation). A non-lead now
   takes the job tables or the connection area only if their creator is the
   creator of the control segment it joined - the node's lead of this launch,
   whose segment Part 3's launch check already tells from a leftover's. A
   leftover left ready names a pid that is gone; the non-lead waits, sweeping
   and with its stall watch, until the lead removes its name as it makes its
   own, and starts over, never counting itself in the leftover's `opened`, which
   would remove the name with nothing made (`job.c`, `follow`;
   `connection-area.c`; `shm-object.h`, `OSOE_SHM_ANY_CREATOR` for the
   transport's own tests, which have no control segment). The three headers
   recorded their creator's pid already, so no layout changes.
   Test: `shmem.leftovers.restart` (`restart-leftovers.sh`), the refutation's
   case - a start cancelled once node 0's lead has made both objects, then a
   start by the same job id with no `osoe-stat --gc` between - which must exit 0
   and leave no name. On `08b3da0` it fails (below). And, after the adversarial
   read, `.slow-lead` and `.other-size`, which reach the connection area's half
   (below).
2. **The launcher-free recipe failed when run again after a failed start**
   (readiness Q5-2): the recipe exports `OSOE_LAUNCH=$(date +%s%N)` before
   each start, the README conditions "a later start with the same ids takes
   over" on a new launch number, and the variable's row says a start that may
   follow a failed one must set it - the refutation's correction of the
   proposed `$RANDOM`, which is empty under dash.
3. **RHEL 10 and containers** (portability Q1): a README paragraph after the
   io_uring requirement - off by default on RHEL 10 and its rebuilds, refused
   by Docker 25 and later and containerd 2.0 and later with `EPERM` and by
   Podman with `ENOSYS`, about 300 of the suite's tests failing there, and what
   to do.
4. **`OSOE_AM_HUGEPAGES=1` never takes effect** (portability Q2, readiness
   Q5-1): the README row, design §3.4's table and paragraph and §6.2 say so,
   with the refutation's correction that a `/dev/shm` mounted with `huge=`
   gives transparent huge pages either way.
5. **The global exit**: the README's second limit says what happens when PEs
   of several nodes call it - the usual case - and that the killed callers'
   handlers and output are lost (conformance F1); its handlers' thread and
   signal mask are stated (F2); and "the other callers leave with 0 after their
   handlers" says "usually", a caller that loses the race with another node's
   end leaving as one that did not call (conformance M2). And the refutation's
   liveness fix: each of the ending loops - the abort's two and the close's -
   asks its condition again after a poll that moved nothing before it sleeps,
   since a poll can take a peer's end of stream and report no progress, and the
   loop then slept a whole 100 ms sweep (`progress-host.c`). It narrows the
   window in which a node that ends first under `srun -K1` can have another's
   PEs killed; the job-wide single carrier that closes it waits until after the
   release, modelled first.
6. **A reopened buffer wakes every waiting writer** (scale S1): the README's
   one-thread limit says so, with what was and was not measured; design
   §3.11's example and the two comments that called the room divisible
   (`base/wait-word.h`, `transport-am.c`) are corrected. The hand-on wake waits
   until after the release, `wait_word_model.py` first.
7. **Which history to publish** (readiness Q5-3): the author's decision, asked
   in the session's summary. Nothing here changes the history.

## The minors taken

The synthesis's rule: a minor that is a sentence goes with the fixes.

- The README's Pi round-trip ratio says the library adds some 15 µs whatever
  the link, so the ratio does not travel, with the VM's loopback beside it
  (scale S3); the Pi's stalls are "up to 52 s", what the raw files hold.
- `osoe-stat --gc` as root runs from a timer, not the epilog, whose safe line
  is the `rm` (portability Q4).
- A `shmem_malloc` the PEs call with different sizes is listed beside the
  collectives under Failures, undefined and silent (readiness Q5-6); the
  settings read after the join end the whole job, which the README and
  `init.c`'s comment now say.
- `bootstrap.h`'s internal region at 1,024 numbers is about 200 MiB, not 160.
- The scrub's residues (readiness Q5-4), the runbook's stale "What to expect"
  for the examples and SOS (conformance M4), the release checklist's 1.6 line
  and the docs index's pointer to the specification (Q5-6): by a Sonnet
  subagent, each change read here before the commit.

The rest of the minors and the after-release findings are in the release
checklist's list of what follows the release.

## Choices made without the author

- **The creator's pid, not a word in the control segment.** The refutation
  offered both. The word - raised by the lead after each object's `ready` -
  needs a new layout of the control segment, and step D's two parts, built on
  their branches beside this one, have taken layouts 29 and 30: merged first,
  this would renumber both. The pid needs nothing new, and fails only if a
  requeued start's lead has the dead lead's pid on the same node - and then as
  the review found, the leftover taken - which `pid_max` makes a coincidence,
  unless each launch runs in a pid namespace of its own and `/dev/shm`
  outlives it. *Undo*: the word, at the next layout.
- **The close's loop asks again too**, which the refutation did not measure:
  the same shape, and the same sweep lost now and then at a multi-node
  `shmem_finalize` - the adversarial read below measured 100-130 ms lost in 4
  of 12 runs of a two-node program over three sessions on `08b3da0`, and none
  in 24 with the change.
  *Undo*: the poll's result alone decides the sleep again.

## Verification

On the branch, in the `osoe-x86` VM (`build/scratch/review-fixes-r4/`):

- **The four presets**, the floor pins on: debug 665 of 665, release 665 of
  665, asan 664 of 664, tsan 448 of 449 - the one failure the new test itself:
  under tsan it started `test-internode.c`, whose agent writing a heap its
  main thread holds tsan reports as a race, as `shmem.internode.*`'s comment
  says, and as the suite has it the test now starts `test-multinode.c` there;
  the leftover tests then passed five times each under tsan, with no report.
- **`shmem.leftovers.restart`** ten times in the debug build, every run
  passing; and its case against `08b3da0`'s library three times with each
  program: every run there failed - with `test-internode.c` a false deadlock
  at 11 s, "PE 1 in shmem_quiet", and both leftovers named afterwards; with
  `test-multinode.c` the second start exiting 1, both named - where every run
  here passed.
- **An adversarial read** of the diff, as committed (`edb8b60`), by a fresh
  agent with probes in the VM, found nothing high. What it found, and what
  became of each:
  1. *Medium*: `shmem.leftovers.restart` never reached the connection area's
     half of the fix. Once the job tables' half makes the node-mate wait for
     its lead's tables, the lead has nearly always replaced the leftover area
     by the time the node-mate looks, and a build with the area's check
     disabled passed the case 11 of 11; with the lead's making of the area
     half a second late, by an `LD_PRELOAD` shim, the same build failed 3 of 3
     as F1 had. *Fixed*: `shmem.leftovers.restart.slow-lead` runs the case with
     that shim (`slow-create.c`, after `slow-message.c`), outside the
     sanitizers as its precedent is.
  2. *Low*: a smaller leftover area - a failed start on fewer nodes, or with
     smaller `OSOE_AM_*` settings - ended a node-mate with "every PE of a job
     must set OSOE_AM_* the same" before the creator's check could run, its
     lead having made nothing. *Fixed*: the size is refused only for an area
     this start's lead made (`connection-area.c`, `made_by`), and
     `shmem.leftovers.restart.other-size` starts the second job on three nodes
     over a two-node leftover, with the slow lead. The job tables' `follow`
     already checked the creator before the shape.
  3. *Low*: the README's "some 15 µs, which the library adds whatever the link"
     - the file it cites has about 27 over the VM's loopback. Both figures now.
  4. *Low*: "every PE saying it ends on behalf of the one that refused" holds
     where one PE alone has the bad value; with an exported one, some PEs name
     it and the launcher ends the rest without a word. The README says both.
  5. *Low*: this record said the close lost a sweep at *every* multi-node
     `shmem_finalize`; it measured 4 of 12 on `08b3da0` and none of 24 with
     the change, now in the choice above.
  6. *Low*: a new lead with the dead lead's pid gives F1's own failure, not a
     refusal, and a launch in a pid namespace of its own makes equal pids
     likely if `/dev/shm` outlives it - in the choice above, and in design
     §6.2, which had not stated the rule (the read's seventh finding).
  7. The nits: the moved briefs' paths in the decisions index and the live
     briefs; the suite's count in the container paragraph; "when their
     messages are that large" in the README's wake sentence, as §3.11 has it;
     containerd's version, 2.0, which took io_uring out of its default profile.

  It found sound: one creator, local id 0, for all three objects, whose pid is
  stored before `ready` and read after it - but by `made_by`, which reads it
  before, from an area perhaps half made, and so can only wait the longer for
  a pid not yet written, never refuse wrongly, the size it judges being the
  creator's one `ftruncate` (the second read, below); a second `shmem_init`
  after a `shmem_finalize` (its own planted leftovers under the second session's
  names); no new hang - only the lead's create removes a leftover's name, and
  each way the lead can fail before it ends the waiter by its sweep; the
  retry path leaking nothing; the test failing on `08b3da0` for the right
  reason; and item 5's change, which only skips a sleep when the loop's own
  condition holds.
- **After the read**, in the `osoe-x86` VM (`build/scratch/review-fixes-r4/v2.*`):
  the debug suite 667 of 667; the three restart cases three times each, every
  run passing; and each of the two fixes removed in turn, each case three
  times - without the area's creator check, `.slow-lead` failed 3 of 3 as F1
  did, a false deadlock with "PE 1 in shmem_quiet", the plain case passing;
  with the size refused before it, `.other-size` failed 3 of 3 on the false
  "must set OSOE_AM_* the same".
- **A second adversarial read**, by another fresh agent, of these fixes alone
  (`edb8b60` to the amended commit) and of step D's second part's own, before
  the merge, found nothing high or medium. Its lows here: the first wait's
  notice and fatal still said an area smaller than this PE's was not created
  or not sized, where it may be a leftover - now named as one, and the lead
  as replacing it; the two shim cases could pass without reaching their case,
  the shim's half second counted from nothing in particular and its running
  never checked - now the shim holds the lead's area until the job tables'
  name is gone, which is once the node-mate has taken them, and says on stderr
  that it held, and a case run with it fails if it did not; and the pid's
  read before `ready`, above. Then, in the `osoe-x86` VM
  (`build/scratch/review-fixes-r4/v3.*`, `v4.*`): the debug suite 667 of 667,
  four tests at a time, the shim's two cases among them; the restart cases
  three times each, every run passing; and each fix removed again, three runs
  each - `.slow-lead` failing every run without the area's creator check, as
  F1 did, the shim's line in each, and `.other-size` every run with the size
  refused first. That one's runs ended during the shim's hold, before its line,
  so the shim came to say it holds as the hold begins; once more each with
  that, the line in both failing runs and the restart cases passing three
  times.
