# 2026-10-07, overnight: v0.2's steps 2 and 3 - where each stands

The author's prompt of 2026-10-06's evening, after answering the five open
questions (`2026-10-06.md`, items 2 to 7): "With these, can you start an
overnight implementation and testing session? pi cluster is up". The night's
rules were the 2026-10-05 prompt's - briefs' parts, merge points and
stop-and-ask lists; a stop-and-ask written down with a recommendation and the
work carried on around it; `main` merged at green checkpoints only; no tag, no
push, not step 4. This is where each step stands at the session's end; each
step's own record has the detail.

## Step 2: the tree's shape from the link - done

**Part 1** (`2026-10-06-tree-shape.md`): the link measured at the end of
`shmem_init` - the root's lead against its first children's, through the data
path, handed to every PE by an AND over the finalize team - 27.1 to 27.6 ms of
a start on the Pi, the hop and the bandwidth agreeing within 8% from start to
start. Its adversarial read (`docs/reviews/archive/v0.2-step2-part1-adversarial-read.md`)
found one defect, fixed: the streams were sized from the root's own
`OSOE_MAX_THREADS`, which nodes need not agree on, and ran past the region of a
node with fewer threads, ending the job at start under a message that blamed
the wrong settings.

**Part 2** (`2026-10-07-shape-data-path.md`): every large reduction and
broadcast takes its call's fan-out and window from the chooser on the link,
under the author's ceiling of 8 and capacity of 4. On the Pi's four nodes,
against 0.1.0's one tree, alternating three times: a 1 MiB broadcast 1.43 to
1.47 times faster, a 1 MiB reduction 1.20 to 1.26, every size from 64 KiB up
faster, none slower beyond the noise. The model, refitted to the 1 to 16 KiB
sweep the author asked for, chooses within 10% of the fastest measured shape
on both sessions' tables.

**One stop-and-ask, answered for the merge**: the scans, which cannot change
their tree from call to call, took the ceiling's, and with its new default of 8
a scan over 16 PEs on four nodes was 1.5 to 2.0 times slower than 0.1.0's. They
now keep 0.1.0's shape under `link`, the one no measured size is slower in.

**Merged** (`main` from `dba1080` to `2489481`, the brief to `done/`), after the
step's merge verification on the merged tree and on the commit before it: the
four presets with the floor pins, `tier3`, the pacer's runs and ten minutes of
`mix`; the models whole; the SOS suite at 4 × 2, 161 clean as at the release
point; the acceptance past 61 PEs, line for line the release point's; the
single-node build on Rocky Linux 9. And after the whole step's adversarial read
(`docs/reviews/archive/v0.2-step2-adversarial-read.md`): no defect on the data
path, but its tests weaker than its mechanism - a data path that built another
tree than it published, or a reduction that granted another tree's children,
passed every test. Each is caught now - the shapes test holds the tree a call
built, and a new test runs collectives of differing fan-outs back to back - each
shown failing on the mutation it was written for.

## Step 3: Träff's circulant reduce-scatter - modelled, not merged

On the branch `circulant` (`2026-10-07-circulant.md`): Träff's algorithm
modelled - its arithmetic for every node count from 2 to 70, every lead moving
N - 1 blocks a half where the brief's fold moved up to 3.5 vectors on a fold
lead's link, and its protocol on two to five leads, holding, with seven
breakages failing; its arithmetic in C, held to the model; and its cost in the
chooser, which the data path does not yet ask for. In simulation on the Pi's link it halves the fold's
1 MiB time where the node count is not a power of two (16.4 against 32.6 ms on
7 nodes, the best tree 37.2), and matches it at powers of two. It replaces the
fold for every node count; the fold's model stays as the comparison's evidence.

**The data path waits on one question**: the protocol the model holds needs
words of its own in the internal region - its counters cannot be the tree's,
since its allgather ends with no step every lead waits for and a lead that
leaves can start a small collective whose child adds to its parent's counters
at once - and grants that carry a lane a round, since a lead's receivers are
as many leads as there are rounds; both are on the brief's stop-and-ask list.
The same two problems were in the fold's design, which Part 1's model did not
show.

## What the author is asked, in one place

1. Step 2, Part 2's question 1: the scans' shape - per call as the brief has it
   (its own model and per-row bases first), 0.1.0's fan-out with the
   capacity's window (1.30 times faster at 1 MiB, 6.6% slower at 64 KiB), or
   0.1.0's shape, as merged.
2. Step 3's question 1: the reduce-scatter's own words and its grants' form.
3. Carried from Part 1: the message cost's spread between starts (0 to 21 µs),
   merged with the recommendation (a), the measurement as built.

## Housekeeping

- **Branches**: `tree-shape` and `shape-data-path` merged, their worktrees
  removed. `circulant` - step 3 - rebased onto `main` at `2489481`, not merged;
  its presets and `tier3` passed in the VM before the rebase, and its model,
  grid and C tests on the host after (the presets on the rebased tip are in its
  record). `rabenseifner` - the fold, superseded - kept as it is.
- **Raw files**: the Pi's under
  `docs/design/evidence/pi-cluster/2026-10-06-v0.2-step2/`; the VM's logs under
  `build/scratch/shape/` (not in git).
- **The Pi** holds only `~/osoe`, at the scans' final tree.
