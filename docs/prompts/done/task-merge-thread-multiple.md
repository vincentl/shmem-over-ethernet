# Merge the `SHMEM_THREAD_MULTIPLE` design branch

Merge branch **`design/thread-multiple`** into the branch the main line of work is
on (currently `phase1-slice1`). The branch is a design task: it adds text, models,
and tools, and changes no library code or phase-1 behaviour.

## Context

- **Repository:** `/Users/vincent/Desktop/Maker Space/shmem-over-ethernet`.
- **Base:** the branch forked from `phase1-slice1` at `459262e` ("Document running
  a one-node job without Slurm") and has one commit on top.
- **What it adds:**
  - `docs/design/shmem-ethernet-design.md`:
    - a new **§4.11 `SHMEM_THREAD_MULTIPLE`**, which is designed and
      model-checked but not scheduled;
    - one-paragraph pointers to §4.11 in §4.6 ("Thread level") and §4.9 ("Under
      `MULTIPLE`");
    - **invariants 29–31** in §9;
    - a new §10 closed-decisions row;
    - the §10 "Still open" `MULTIPLE` bullet replaced by the points §4.11 leaves
      open.
  - `docs/design/evidence/multiple/`: the model-checking engine, three models,
    `selftest.py`, a README, and the recorded `.out` runs §4.11 cites.
  - `tools/modelcheck/`: a reusable copy of the same scripts, with a README on
    writing and speeding up models.
  - This prompt.
- **What it deliberately does not change:**
  - the phase-1 thread level, which stays up to `SHMEM_THREAD_SERIALIZED`;
  - `shmem/src/transport.h`. Its future "Threads" paragraph is quoted in §4.11
    item 6 and is applied only when `MULTIPLE` is implemented;
  - `docs/reviews/`, which is read-only. The review's D6 item 2 fix is recorded as
    unsound in §4.11 and §10, not edited in the review.

## Steps

1. **Check the tree.** `git status` must be clean on the target branch. If other
   work is uncommitted, stop and ask.
2. **Merge without fast-forward**, so the design lands as one visible merge:
   `git merge --no-ff design/thread-multiple`.
3. **Resolve conflicts, which can only be in the design document.** Likely places,
   if the target branch has moved:
   - **§9 invariants.** If new invariants were added after 28, renumber this
     branch's 29–31 to follow them, and update any "invariant N" references to
     them. `grep -n "invariant 29\|invariant 30\|invariant 31"` finds them; at the
     time of writing there are none.
   - **§10 closed table and "Still open — design, deferrable".** Keep both sides'
     rows and bullets. Remove any other bullet that still calls
     `SHMEM_THREAD_MULTIPLE` open.
   - **§4.6 and §4.9 wording.** Keep the target branch's text and re-apply this
     branch's pointer sentences to §4.11.
   - **Section numbers.** If a §4.11 already exists on the target branch, renumber
     this one to the next free §4.x and update every "§4.11" in the document, in
     `docs/design/evidence/multiple/*.py`, and in the READMEs.
4. **Verify the models still run.** They need only `python3`, on the host or in
   the VM:
   - `python3 docs/design/evidence/multiple/selftest.py` must end with
     `REDUCTIONS PRESERVE EVERY VERDICT` (about a minute);
   - `python3 docs/design/evidence/multiple/watch_list_model.py` must end with
     `ALL AS EXPECTED` (about a minute);
   - `python3 tools/modelcheck/dirty_set_model.py "2 issuers x 1 put"` is a quick
     check that the tools copy imports and runs.

   The full get-completion and dirty-set runs take about 20 and 5 minutes. They are
   recorded already, so rerun them only if the engine or a model changed in the
   merge.
5. **Build and test as usual** (`limactl shell shmem cmake --workflow --preset
   debug`). Nothing in the branch is built, so a failure here comes from the
   target branch, not the merge.
6. **Report:** the merge commit, any conflicts and how you resolved them, and the
   last line of each verification run.

## Rules

- **Do not change the design's decisions** while resolving conflicts. If the target
  branch has since made §4.11 wrong (for example, it changed §4.6's completion
  structures or §4.9's watch slot), leave §4.11 as it is. Add a §10 bullet saying
  what now disagrees, and report it.
- **Do not delete the evidence copies** to remove the duplication with
  `tools/modelcheck/`. The evidence directory is the record of what was run; the
  tools copy is where models are developed next.
