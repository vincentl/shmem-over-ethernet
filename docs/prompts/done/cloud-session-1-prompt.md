# Cloud session 1: the environment, and the work that needs no running library

*Written by the lead session on 2026-10-12 for the author's first cloud session. The author
opens the session on the repository's `main` with **Claude Opus 5.5 at high effort** and the
message: "Read docs/prompts/cloud-session-1-prompt.md and do what it says."*

## What this session is for

osoe is an OpenSHMEM 1.6 library for clusters joined by ordinary Ethernet (C11, Linux), worked
toward its 0.2 tag through the open items in `docs/prompts/v0.2-open-items.md` (150 when this was
written; its key explains each item's source, kind, place and size). The lead session works
elsewhere, on machines this session does not have: a development VM, a Raspberry Pi cluster, and a
Google Cloud cluster. This session has a fresh clone and whatever its sandbox allows, so it does
two things:

1. **Finds out what this sandbox can run** - the build, the probe, a sample of the suites - so the
   author and the lead can decide what a second cloud session should take.
2. **Closes the items that need no running library**: models, compile-only tests and build fixes.

Everything goes on one branch, with a manifest the lead reviews after the author pulls. Nothing
merges to `main` here.

## Read first

`CLAUDE.md` (the working agreements: Sonnet subagents for mechanical work, names that are words,
longest-first renames and audits that can be shown to find something); `README.md`'s first
screen and "Building"; `docs/README.md` (where everything is); the open items' key; and, for each
item you take, its source and the code it is about. The design notes are
`docs/design/shmem-ethernet-design.md`; comments cite their sections as `§4.10`, and `§9.x.y`
means the OpenSHMEM specification, which is not in the repository (`docs/reference/README.md`).

## Rules the lead keeps that the repository does not spell out

- **The README is true at every commit**: a change carries the README's sentences it makes true or
  false, in the same commit.
- **A test that fails without the change**, shown: run the new test against the unchanged code
  (a stash, or a copy with the fix reverted) and record that it fails, then that it passes.
- **Static checks before anything long**: in the release build,
  `ctest -R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)'`.
- **Gate every test run on the build's exit status**: a failed incremental build leaves the old
  library, and the tests pass on it.
- **No machine's paths in the tree**: anything copied into `docs/` writes the repository as
  `<repo>` and a home directory as `~`; `git grep -l -e /home/ -e /Users/ -e /workspace -- docs tools shmem base`
  must find nothing new.
- **Every source file keeps its licence line**, `SPDX-License-Identifier: LGPL-3.0-or-later`,
  and every new test a ctest label and a timeout, as its neighbours have.
- The author is "he", or "they"; the documents say "they".
- Commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.

## Branch, commits, pushes

Create the branch `cloud-session-1` from the `main` you were given. One commit per item (the
environment report is one too). Push the branch after every commit, so that a session cut short
still leaves its work. Never force-push, never merge, never open a pull request, never touch another
branch.

## Off limits: the lead's own work

The lead is changing `collect` locally (P30 and P31) and will merge the branch later. Do not edit
`shmem/src/collectives.c`, `shmem/src/internode-generic.c`, `shmem/src/internode.h`,
`shmem/src/team.c`, `shmem/src/team.h`, `tools/modelcheck/allgather_model.py`, or the design notes'
§5.8. Do not edit `docs/prompts/v0.2-open-items.md`, `docs/prompts/task-v0.2-to-the-tag.md`,
`docs/prompts/README.md` or `docs/decisions/`: list in the manifest what you closed, and the lead
applies it when merging, so the counts do not conflict. No Pi, no cluster, no host but the package
mirrors and GitHub.

## Part 1: the environment report (first, before any item)

Record, in the manifest's first section: the kernel (`uname -r`), the distribution, CPUs, memory,
`ulimit -l`, whether `/dev/shm` is writable, whether `unshare -rn true` works (tier 3 runs its
logical nodes in network namespaces), the compilers (gcc and clang versions), cmake, Python 3 and
liburing (install the distribution's liburing development package, cmake, ninja or make, gcc,
clang and Python 3 if the package manager allows). The sandbox is a fresh Ubuntu 24.04 VM on x86-64 (4 vCPUs, 16 GB, 30 GB of disk), and you run
as root. The environment's setup script (the appendix) installed the tools; what it cannot keep -
mounts, limits, kernel settings - you set here, and record each one: if `/dev/shm` is under 4 GiB,
`mount -o remount,size=8G /dev/shm`; run every suite under `ulimit -l unlimited`; and if the probe
reports io_uring refused, read `sysctl kernel.io_uring_disabled` and set it to 0 if it lets you.
Running as root, a test that expects a permission refused may behave differently: group such
failures as their own cause. Then:

1. Configure and build the `release` preset (`cmake --preset release`, `cmake --build build/release -j$(nproc)`); record the warnings' count.
2. Run the probe, `build/release/tools/probe/probe`, and copy every line into the manifest: it
   says which kernel facilities the full build needs and has here (futex2, io_uring's operations,
   memlock).
3. Run the static checks (above).
4. Sample the suites, each bounded so the report comes first: `ctest --test-dir build/release -L tier1 -j$(nproc) --timeout 120`;
   then tier 2 (`-L tier2 -LE model`), stopped after 30 minutes if it is still running. Record the
   pass counts, and group the failures by cause - io_uring refused, futex2 refused, locked memory,
   namespaces, timeouts, other - naming two or three tests of each.
5. If the probe fails, build the single-node build too (`cmake -S . -B build/single-node -DOSOE_SINGLE_NODE=ON`)
   and sample its suite the same way: it needs neither futex2 nor io_uring.
6. End the section with one paragraph: what a second session could do here - C items with tests,
   and in which build - and what it could not.

## Part 2: the items, in this order

For each: confirm in the tree that it is still open (its source and the code it names), make the
change, show its test failing without it, run the static checks and the tests it touches, keep the
README true, commit, add its manifest entry, push. If an item turns out to need a decision of the
author's, or the running library, or a file above, stop it, say why in the manifest, and go on.

1. **T42** - a C23 compile test of the public headers (R9 P9-6, `docs/reviews/archive/v0.2-before-part4-review.md`):
   compile-only, beside the strict-C11 check (`shmem.strict-c11`), skipped by name where no
   compiler takes `-std=c23`.
2. **B4** - where the cache routines are declared (conformance M5, `docs/release-checklist.md`'s
   "After the release").
3. **B3** - Example 48's plain profiling names in C11 (conformance M1, the same list).
4. **B5** - a Clang user's first command: the strict-C11 warning, and `CC` not passed to the
   nested build (round four Q5).
5. **P12** - assert that the shared 64-bit atomics are lock-free (the step D review, portability
   Q4): at compile time where C11 allows it, at start-up otherwise. This is §3.16's ground: assert,
   and change no memory order.
6. **T30** - `posted_bitmap_model.py`'s ORDER check reached by no listed breakage (the step D
   review, G3): add a breakage only ORDER catches, and show it failing.
7. **T32** - model tests that say which property fails, not only pass or fail (round four's
   correctness review, F5): each breakage case names the check it must fail, and the engine
   (`tools/modelcheck/engine.py`) compares; every model's verdicts unchanged, `selftest.py` passing.
   Every model but `allgather_model.py`, which is the lead's; name it in the manifest as left. The
   whole model suite runs as `ctest --test-dir build/release -L model -j N`, N no more than one for
   each whole 8 GiB of memory; it takes about an hour one at a time.
8. **B6** - the cross build's nested strict-C11 check finds the host's liburing (R9 P9-5): only if
   an aarch64 cross toolchain installs; otherwise skip it with a note.

Use Sonnet subagents for the mechanical parts - T32's edits across the models, the suite runs -
with precise, bounded instructions, and read their results before acting on them.

## The manifest

`docs/prompts/cloud-session-1-manifest.md` on the branch, written after Part 1 and after every
item, committed and pushed each time. In it:

- **The environment report** (Part 1), whole.
- **One section per item**: done, partial or skipped, and why; its commits; the files it changed;
  the exact commands run and their pass counts; its failing-without-the-change evidence; what this
  sandbox could not verify, with the exact commands the lead should run in the development VM.
- **Decisions taken** that might have been the author's, and **deviations** from this brief.
- **Findings**: anything stale, wrong or missing you met on the way that no item covers - proposed,
  not fixed.
- **For the lead's merge**: the items to close in the open items, and any README or record lines
  that waited for the merge.

Stop when the items are done, when nothing more can be verified here, or when the session's own
budget is spent - the manifest is the last thing written each time.

## Appendix: the environment's setup script

What the author put in the cloud environment's **Setup script** field (it runs as root before the
environment's first session, and its result is cached for later ones):

```bash
#!/bin/bash
# osoe's cloud environment on Ubuntu 24.04 (docs/prompts/cloud-session-1-prompt.md).
set -x
export DEBIAN_FRONTEND=noninteractive
# The full build's tools; gcc-14 for C23 (T42); an aarch64 cross compiler (B6).
apt-get update -q || true
apt-get install -y -q --no-install-recommends build-essential pkg-config cmake ninja-build \
    python3 gcc-14 clang gcc-aarch64-linux-gnu libc6-dev-arm64-cross iproute2 util-linux \
    procps git || true
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
