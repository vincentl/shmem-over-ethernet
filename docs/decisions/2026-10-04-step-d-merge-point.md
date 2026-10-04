# 2026-10-04: step D's merge point

The merge point of `docs/prompts/done/phase1-step7-slice3-prompt.md`, the team
redesign's step D: with both parts on `main` - the first,
`2026-10-03-team-storage.md`, and the second, `2026-10-03-team-allocation.md`,
on the round-four review's fixes, `2026-10-04-review-fixes.md` - the brief asks
for the four presets, `tier3` and `models` on `main` whole, the SOS sweep, and
on the Pi at 4 × 4 the runbook's barrier and 8-byte collective lines against
the records, where D, changing no data path, may move nothing beyond noise and
a difference is a stop-and-ask. `main` is then the checkpoint step E starts
from, and E's brief, written from D's record, is
`docs/prompts/phase1-step7-slice4-prompt.md`.

## The merge

`main` went from `08b3da0` to `d8a651d` by four commits: the round-four
review's fixes (`c51ba87`), D's first part (`5e25592`), its second part's
library (`532fba4`) and the pacer's renewal of a strided team, which needs
numbers given back (`d8a651d`). Each part was read adversarially before it
merged, three times for the second part - the record of each read is its
part's - and the second part's library commit was checked alone: it builds
without a warning and its 158 team and pacer tests pass, the pacer as it was
before the renewal.

## On `main` whole

In the `osoe` VM, under the lock, from `main`'s own archive
(`build/scratch/merge-d/main/`): the four presets with the floor pins on, debug
690 of 690, release 690 of 690, asan 687 of 687, tsan 465 of 465; tier 3
113 of 113; the `models` preset 28 of 28.

## The SOS sweep

On D's tree (`build/scratch/merge-d/sos/`; its code that of `d8a651d` but for
the third read's fixes, which no SOS program reaches): the team programs on
`0,1` and `0,0,1,1`, 26 clean and `shmem_team_b2b_collectives` failing on both,
which the runbook names - it collects 40 longs into a destination of 10; the
whole suite at 4 × 2, 161 clean, the two C11 programs that do not build and the
two failing runs (`shmem_team_b2b_collectives`, `shmemalign`) the runbook's.
The same as the second part's record, line for line. Past 61 PEs the second
part's acceptance ran at 70 (its record's table).

## The Pi

`docs/pi-cluster-results.md`, "Step D's merge point": with D and without it,
alternating three runs each, every line of `osoe-bench` within 1% but the 4 KiB
alltoall's, 2%, inside its runs' spread. Against the records of 2026-09-28 the
barrier is within its 176 to 183 µs, and the 8-byte reduce and broadcast are 6
and 7% faster than that morning's 211.1 and 211.2 µs - and so is the records'
own tree, run again alternating with D's: within 1% of D on every line. The
code moves nothing; the cluster runs these collectives faster than it did that
morning, with the same kernel and the same NIC settings, its nodes rebooted on
2026-10-02.

## The stop-and-ask

The brief lists "the Pi moving on any line at the merge point". Two lines
moved against the records, and the session put it to the author with the
evidence above and one recommendation: accept the difference as the
cluster's, record the day's numbers as the baseline E is measured against, and
not chase its cause. **The author, the same day: not concerned** - the
cluster had been rebuilt, and how its nodes were plugged into the switch can
move these lines. Accepted: the day's runs are the baseline step E is measured
against, the trees run alternately on the day, as E's brief has it; the
2026-09-28 records stay as they were, context rather than a baseline.

## What D leaves

- **For step E**, by design: a number's 200 KiB of slots a PE and the default
  of 64, the README's two *temporary* sentences; E's brief.
- **For after the release**: a team of one PE claims a pool block it does not
  use (the second part's first read, item 5), on the release checklist's list.
