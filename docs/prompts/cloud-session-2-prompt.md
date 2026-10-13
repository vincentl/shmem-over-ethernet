# Cloud session 2: `collect`'s places four ways, then the diagnostics

*Written by the lead session on 2026-10-13 for the author's second cloud session. The author
updates the environment's setup script to the appendix's (it adds `ethtool`), opens the session on
the repository's `main` with **Claude Opus 5.5 at high effort**, and sends: "Read
docs/prompts/cloud-session-2-prompt.md and do what it says."*

## What this session is for

The first cloud session (`docs/prompts/cloud-session-1-response.md`, merged 2026-10-13) showed
that this sandbox runs osoe's full build whole - the probe, tiers 1 and 2 with 449 internode
tests, the models - and that tier 3 needed only `ethtool`, which the setup script now installs.
So this session takes C work with tests:

1. **Part 1, first: `collect`'s places four ways** - the author's decisions of 2026-10-13
   (`docs/decisions/2026-10-13.md`, "The author's answers, that day"): T32's rest, P30's C, P33,
   and P31's choice among the four.
2. **Part 2, as the credit allows: the diagnostics** (§2 of the open items, D2 to D30), in
   batches by file.

Everything goes on the branch `cloud-session-2`, with a manifest as the first session kept, and
the session **ends by writing a response file** (below).

## Read first

- **The first session's brief**, `docs/prompts/cloud-session-1-prompt.md`: its "Read first",
  "Rules the lead keeps that the repository does not spell out" and "Branch, commits, pushes"
  apply here as written, with the branch `cloud-session-2`.
- **The first session's response and manifest**: what the sandbox does, and T32's engine
  (`tools/modelcheck/engine.py`: a breakage names the property it must fail).
- **For Part 1**: `docs/design/collect-scan-proposal.md` (P30's design, its stage-2 change list,
  and why P33 was asked for); design §5.8 (collect's places as P29 built them); in
  `shmem/src/`, `collectives.c` (`collect_varying`, `places_from_node_totals`), `team.c`
  (`osoe_team_node_runs`), `internode-generic.c` (the gather, its design comment near the top of
  its collect section) and `collective-shape.c` (how the reduction's tree or circulant is chosen,
  the pattern P31 follows); `tools/modelcheck/allgather_model.py` (P29's PLACE cases, P30's ranged
  cases); and P29's own commit, `2b4c3375`, as the shape the work takes - code, model, tests, a
  mutant beside its control (`docs/design/evidence/v0.2-step4/2026-10-12-collect-node-totals/`).

## The environment

As the first session found it. Before the first build: `/dev/shm` as it needs, the suites under
the locked-memory limit it allows (the two refusal tests skip). Then try tier 3 once
(`cmake --preset tier3`, `cmake --build build/tier3 -j4`, `ctest --preset tier3 --test-dir build/tier3`)
and record whether it runs here now; the manifest needs one paragraph on what changed since the
first report, not a new report.

## Who owns what

The lead will not change `collect` while this session runs: for Part 1's items this session owns
`collectives.c`, `internode-generic.c`, `internode.h`, `team.c`, `team.h`, `collective-shape.c`,
`allgather_model.py` and design §5.8. As before, do **not** edit
`docs/prompts/v0.2-open-items.md`, `docs/prompts/task-v0.2-to-the-tag.md`, `docs/prompts/README.md`
or `docs/decisions/`: the manifest and the response list what closed and what the records need,
and the lead applies them at the merge.

## Part 1: `collect`'s places four ways, in this order

1. **T32's rest** - `allgather_model.py`'s breakages name the property each must fail, as the
   first session did for every other model; every verdict unchanged, `selftest.py` passing. First,
   since the items after it extend this model.
2. **P30's C**, as the proposal designs it (its stage-2 change list: a leads' scan and a gather by
   ranges in `internode.h` and `internode-generic.c`, their internal-region words, collect's third
   way, the runs' order in `team.c`, the counter, `lane_model.py` beside the reductions). The model
   is done and green; the C follows it. Where the C finds the design wrong, stop that part, say why
   in the manifest and amend the proposal - do not improvise a different protocol. Say plainly
   whether the internal-region words are a wire change and whether any layout changes.
3. **P33**, the direct exchange of the node totals: each lead puts its own total into a slot of
   every other lead and adds to its counter, fenced after - one hop where P29's tree takes two.
   **Modelled first**: in `allgather_model.py` or a model of its own, the slots' reuse from one
   collective to the next made safe as the model's staged design makes step 0's (two slots by the
   collective's parity), or by the READY rule, with breakages each shown to fail; then the C, a
   short design section beside P30's in the proposal or §5.8, and tests.
4. **P31**, the choice among the four - the members' vector, P29's tree of totals, P30's scan and
   P33's exchange - as `collective-shape.c` chooses a reduction's tree or circulant: a setting that
   forces each (an `OSOE_COLLECTIVE_*` name in the README's settings table, which
   `shmem.settings-documented` checks), and defaults by the team's nodes from the cost of each,
   in `tools/modelcheck/shape_timing.py`'s manner, marked provisional until H24 and H25 measure
   them. Then **H24's harness**: a script that times `collect` by each method at tier 3's logical
   nodes, 8 to 254, with every length equal, most of them zero, and one large; run it here once as
   a sample - its numbers are this sandbox's, and the lead runs it again in the development VM for
   H24 itself.

The tests, for P30 and P33 as P29's were and each method forced through P31's setting: block,
cyclic (falling back to the members' vector), a negative stride, zero lengths, at tier 2's maps
and tier 3's links, and a mutant beside its control for each of P30 and P33. Before Part 1 is
done: the static checks, the release suite whole, tier 3's collect tests, the `debug` and `asan`
presets' collect and team tests, the `tsan` preset's with its gate at 0 and 2 helpers
(`python3 -I cmake/thread-sanitizer-gate.py judge ...`, as `docs/fast-link/verify.sh`'s tsan
part runs it), and the changed models with `selftest.py`.

## Part 2: the diagnostics, by file, as the credit allows

From §2 of `docs/prompts/v0.2-open-items.md` - read the list for the current ids and texts - in
batches by the file they touch: `osoe-stat`'s; `fatal.c` and `abort.c`'s; start-up's messages;
the settings' validation; `osoe-launch`'s; and D30's line splitting. Each item with a test that
fails without it, the README true, a commit per batch and a manifest entry per item. Sonnet
subagents suit the mechanical edits and the runs; read their results before acting on them.

## The manifest, and the response that ends the session

- **The manifest**, `docs/prompts/cloud-session-2-manifest.md`, in the first session's form:
  current after every item, committed and pushed each time.
- **Conclude the session by writing `docs/prompts/cloud-session-2-response.md`**, as the first
  session wrote `cloud-session-1-response.md`: the short version for the author and the lead - in
  short; the environment's change; a table of the items with their outcomes and commits; for each,
  what changed and how it was shown; where the tests stand at the end; what needs someone else
  (the author's decisions, and for the lead's merge: the items to close, the records and README
  lines that wait for it, and what to re-run in the development VM, which is aarch64); decisions
  and deviations; findings. Commit and push it as the last thing - and if the author stops the
  session early, write it before anything else.

## The credit

About $167 of the author's cloud credit is left; the first session used about $83 for its
environment report, eight small items and a model suite of 2 h 35 min. Part 1 comes first; Part 2
only while credit remains. Opus for the design, the models and the protocol code; Sonnet subagents,
precisely instructed, for the mechanical parts. Do not run the whole model suite again unless a
change reaches the engine: run the models you change, and `selftest.py`.

## Appendix: the environment's setup script

The first session's, with `ethtool` added for tier 3. Changing it makes the environment rebuild its
cached snapshot at the next session's start.

```bash
#!/bin/bash
# osoe's cloud environment on Ubuntu 24.04 (docs/prompts/cloud-session-2-prompt.md).
set -x
export DEBIAN_FRONTEND=noninteractive
# The full build's tools; gcc-14 for C23; an aarch64 cross compiler; ethtool for tier 3.
apt-get update -q || true
apt-get install -y -q --no-install-recommends build-essential pkg-config cmake ninja-build \
    python3 gcc-14 clang gcc-aarch64-linux-gnu libc6-dev-arm64-cross iproute2 util-linux \
    procps git ethtool || true
# liburing 2.9, as Debian 13 ships it: Ubuntu 24.04's is 2.5, and osoe needs 2.6 or newer.
if ! pkg-config --atleast-version=2.6 liburing 2>/dev/null; then
    cd /tmp && rm -rf liburing \
        && git clone -q --depth 1 --branch liburing-2.9 https://github.com/axboe/liburing.git \
        && cd liburing && ./configure --prefix=/usr/local \
        && make -j"$(nproc)" -C src && make install && ldconfig || true
fi
pkg-config --modversion liburing || true
exit 0
```
