# Adversarial read of the large-cluster playbook, 2026-11-01

**What was read:** `docs/prompts/large-cluster-playbook.md` as first drafted on
the night of 2026-11-01, against the tree at `6f6b81d5` (the 0.2.0 tag, `main`
= `origin`). The playbook is a document, not code: the review's question was
whether an agent following it cold on a large cluster would do the right things
in the right order, and whether what it says of the tree is true.

**How:** one fresh agent (Claude, the lead's model), given the project's review
method (`docs/reviews/adversarial-review-prompt.md`), the playbook, the
off-network brief it builds on, `docs/ceilings.md`, `CLAUDE.md` and the
decisions index, and told to verify every claim about the code against the
source and every number against the records, to attack the gates' order, the
ranking and the algorithm arithmetic, and to list what it checked and found
correct. It reported 25 findings, each with a verdict of its own (VERIFIED or
INFERRED). The lead then checked the five that would change the playbook most
against the source before accepting any, refuted or narrowed what did not
stand, and rewrote the playbook. This file is the record; the playbook is the
text after it.

The draft itself rested on four Sonnet surveys of the code (the transport and
start-up; the collectives; the data path and memory; the tests and the kit),
whose concrete claims the lead spot-checked before drafting: the per-call
allocation in `shmem_pe_quiet` (`transport-internode.c:1932, 2087`),
`OSOE_AM_HUGEPAGES` present and without effect (`connection-area.c:408-421`,
the README's row), the 16 KiB piece (`internal-region.h:148`) and the
pipeline's highest of 8 (`bootstrap.h:208`), and the README's own words on the
progress threads' core under a task's binding (its `OSOE_PROGRESS_HELPERS`
row).

## The findings, and what became of each

Severity is the reviewer's; the verdict column is the lead's after checking.
"Accepted" means the playbook was changed as the finding asked; "narrowed"
means part of it stood; "refuted" means the lead read the same place and
disagrees, with the reason.

| # | Finding, in short | Severity | Lead's verdict | What changed |
|---|---|---|---|---|
| 1 | `OSOE_NODELIST` does not exist; `OSOE_*` variables are read only as a whole set where `OSOE_PROCID` is set, never beside Slurm's, so a site with multi-range node names has no remedy but the parser change | blocking | **Accepted**, checked: `bootstrap.c:130-140`, `:604`; README:2104 names `OSOE_STEP_NODELIST` | A0 says so; the parser change (C2, promoted from C13) is A0's prerequisite on such a site; the per-task wrapper named as the other way |
| 2 | `osoe-bench quick` at the whole cluster runs 400 alltoalls in one 900 s job and loses the rung's other numbers | blocking | **Accepted**, checked: `KIT_TIMEOUT` 900 (`common.sh:41`); the bench's `collectives` includes the alltoall with no finer switch (`osoe-bench.c:9-25`) | §3.1 runs `barrier latency atomic` in one job and `collectives` in its own, with a timeout from the arithmetic; the alltoall row says what to expect |
| 3 | The playbook's branch and return file contradict the brief's (`off-network-cluster`, `off-network-cluster-response.md`) | major | **Accepted** | the brief's names kept; the header says so |
| 4 | C7 asks the agent to build the least-try message cost, which was built and decided on 2026-10-28 | major | **Accepted**, checked: `link-measure.c:176-202` computes `served_least - alone_least`; the decisions index records it | the item removed from C7 and §6; C7 says it is built and timed on no cluster since |
| 5 | The header cites two files that did not exist at the time, and the review's name lacks the archive's `v0.2-` prefix | major | **Narrowed**: both files were written the same night and the header now says so in the past tense; the name is kept - the convention (`CLAUDE.md`) names a review for the work it reviewed, and this work is not v0.2's | the header |
| 6 | "Under the site's binding" is undefined: the kit's default is `--cpu-bind=none --overcommit` | major | **Accepted**, checked: `common.sh:40` | §2 defines "under the site's binding" (`KIT_SRUN`) and "unbound"; §3.1 names both runs |
| 7 | The playbook has the agent start models and build a harness, which the brief forbids | major | **Accepted as a statement of intent**: the extension was deliberate but unsaid | §6 names four grants, G1 to G4, each the author's to give or withhold; the brief's rule stands until he does |
| 8 | C17's fan-out arithmetic ignores the parent's k serial sends at a message cost of 23 to 45 µs, and ⌈log₃₂ 4096⌉ is 3, not 2 | major | **Accepted** | C16 (renumbered) states both costs and corrects the levels; 16 to be timed against 8, 32 saves nothing below 32,768 nodes |
| 9 | `--gc` runs only after the kit's own kill (exit 124 or 137), never after Slurm's time limit | major | **Accepted**, checked: `common.sh:113` | §2: `--gc` at the start of every allocation; the stop rules; C19 |
| 10 | "Nothing is timed until the gates pass" is false of A3 and A5 | minor-major | **Accepted** | §0: the gates' timings are yardsticks, and no number goes in a table before A6 and A7 pass |
| 11 | `gets.sh` cannot vary the credits | minor | **Accepted** | §3 and §3.2: `OSOE_AM_CREDITS` set in the environment by hand |
| 12 | `scale.sh` runs the bench, ten minutes of `mix` and the SOS sweep, not the start-up probe alone | minor | **Accepted** | A5 and §3.1 name `kit/startup` |
| 13 | bale has no build recipe in the tree | minor-major | **Accepted** | A7: "if time allows", the build the agent's to work out and record |
| 14 | An abort's wait is bounded (20 s); finalize's by `OSOE_NO_PROGRESS_TIMEOUT` alone | minor | **Accepted** | C19 |
| 15 | No decision record of 2026-09-20 exists for "no fatal timeouts" | minor | **Accepted**: the rule is design §3.15's and the decisions index's | C19 cites those |
| 16 | The silent-peer check walks the lead's agent's wires: n - 1 with helpers | minor | **Accepted** | C18 and §3.1 |
| 17 | `\|` in a ctest regex is a literal bar | minor | **Accepted** | A1 uses alternation, with a note on the table cell's escaping |
| 18 | The brief does set `OSOE_NO_PROGRESS_TIMEOUT=60` | minor | **Accepted** | §2's sentence removed; the default of 0 stays in §1's reading list through `common.sh` |
| 19 | Tier 2's 1,071 ran on one node, not eight | minor | **Accepted** | A2 |
| 20 | Per-peer bytes a PE are 45 (README), `pe_quiet`'s array is of flags, `tcp_wmem`'s default is 16 KiB | minor | **Accepted** | §2.1 and C19 |
| 21 | The lessons run L1 to L30 | minor | **Accepted** | §1 |
| 22 | "One allocation a rung" needs the brief's `common.sh` change first | minor-major | **Accepted** | §3 and G4 |
| 23 | Missing: Slurm's time limit and signals, the queue's cost, the output's volume, the report's size; a binomial tree and a dissemination barrier in §5 | - | **Accepted**: the design notes name neither alternative as tried | C20 added; §5's barrier and small-collective rows split, with both alternatives and recursive doubling; §7 sizes the report |
| 24 | Ranking: C13 (names) underrated, C3 and C18 (the mesh, the detector) overrated for a first session, C8 (alltoall) underrated in phase B | - | **Accepted in the main**: names promoted to C2 and A0's prerequisite; the mesh and the detector moved to C17 and C18; the alltoall's phase B caution added. Not accepted that the mesh matters little: its kernel state is unmeasured and the top rung is where it would show | the order of §4 |
| 25 | "About 38 cross-node programs" is unsupported; the tier-3 function is called about 72 times | minor | **Accepted** | C1 says about 70 registrations over three shapes |

Of the 25: 22 accepted whole, 2 narrowed (5, 24), none refuted outright. The
blocking two (1, 2) would each have cost the agent a day or more; the lead's
own draft had the first wrong because it wrote a remedy from memory without
reading `bootstrap.c`, which is the kind of claim this read exists to catch.

## What the reviewer checked and found correct

Kept here so the next read need not repeat it (the reviewer's citations):

- the put path's three locked operations - the reservation's CAS
  (`connection.c:94`), the commit's `fetch_add` (`:116`), the owner's dirty
  bitmap `fetch_or` (`connection-area.h:177`) - then the sleeping flag and
  `futex_wake` (`:165-171`);
- the 16 KiB piece, the pipeline's 1 to 8 with 8 the default, the fan-out's
  default 8, the small class's 1 KiB (`internal-region.h:148`,
  `bootstrap.h:206-219`);
- the lead's serial combine in local-id order (`collectives.c:28-31, 209-215`)
  and its release by `futex_wake(INT_MAX)` (`base/epoch.c:63-65`); the
  barrier's release a chain (`:79`);
- the credits a node pair, in ticket order, puts taking none
  (`transport-internode.c:1181-1215`); the reply descriptor's 48 bytes
  (`reply.h:9`);
- `OSOE_AM_HUGEPAGES=1` without effect (`connection-area.c:416-419`);
- the agents inheriting the task's mask, and `auto` counting the CPUs online,
  not those allowed (`bootstrap.c:105`); the sweep of 100 ms
  (`base/deadline.h:16`);
- the deadlock detector a star from node 0 (`deadlock.h:33`); the scan's
  pre-order tree at fan-out 4 and window 2 (`internode-generic.c:2446, 2590`);
  the alltoall's loop from `me + 1` with `MORE` (`collectives.c:956-978`);
- the control wires eager and the data wires on demand by setting
  (`bootstrap.c:579-581`); the hostlist's one range (`base/hostlist.c:80`);
  IPv4 only, `INADDR_ANY`, no interface setting (`listener.c:324-345, 1242`);
- the kit's `--gc` outside the allocation (`common.sh:113-116`); the
  multi-node Slurm fixtures hand-written; fourteen settings read a wire attach
  (`wire.c:89-93`);
- every number quoted from the records: 102, 122 and 117 ms; 1,071 in 148 s;
  263 ms; 672 µs; 1,287 against 296; 2,709 MiB/s; 41 µs; 1.25 million a second;
  23 to 45 µs; 24 to 33%; the streams at a third of the link; the control
  segment's 72.8 MiB and the lead's 8.1 KB a peer; the closed form checked
  against the simulation to 31 nodes (`shape_timing.py:890`); Träff's
  2⌈log₂ n⌉ rounds; the mesh model's 0.26 and 0.45 s;
- that of §5's alternatives only Rabenseifner's was tried and set aside by the
  design (`docs/decisions/2026-10-06-rabenseifner.md`); design §5.8 and §10
  name none of van de Geijn's broadcast, recursive doubling, the dissemination
  barrier or a two-level alltoall.

## What this read did not do

It read a document against a tree; it ran nothing. The playbook's own
estimates - the alltoall's seconds, the pipeline's cap, the combine's
milliseconds - are arithmetic from the code's constants and the records'
numbers, as the playbook says of them, and the ladder is what tests them. The
four surveys' reports were the reviewer's starting point as much as the
lead's, so a claim both took from a survey and neither re-read could stand
wrong: the survey claims the lead did re-read are listed above, and the
reviewer's own list covers the rest of §4's "Today" statements.
