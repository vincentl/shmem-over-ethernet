#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The 0.2 tag's Pi checks, rehearsed on main at 5cefc7fb, the tree round ten reviews, before the tree to be
# tagged exists, so that a problem shows now rather than at the tag: 0.1.0's
# release-point runbook, its four parts in order (pi-a.sh to pi-d.sh, adapted
# copies of docs/design/evidence/pi-cluster/2026-10-05-release/): the tree built
# fresh, the facts, probe, tiers, smoke, floor, benchmark and endings; the
# corpus; the pacer's runs; an hour of mix and an hour of threads.
cd <repo>/build/scratch/pi-tag-5cefc7fb || exit 1
echo "rehearsal start $(date +%H:%M:%S)" > rehearsal.txt
bash pi-a.sh <repo>/build/scratch/pi-tag-5cefc7fb/tree; echo "part a: $? $(date +%H:%M:%S)" >> rehearsal.txt
bash pi-b.sh; echo "part b: $? $(date +%H:%M:%S)" >> rehearsal.txt
bash pi-c.sh; echo "part c: $? $(date +%H:%M:%S)" >> rehearsal.txt
bash pi-d.sh; echo "part d: $? $(date +%H:%M:%S)" >> rehearsal.txt
echo "DONE $(date +%H:%M:%S)" >> rehearsal.txt
