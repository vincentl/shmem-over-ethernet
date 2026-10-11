#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine's kit fixes: the evidence of their checks, from this directory
# into the tree's docs/design/evidence/v0.2-step4/2026-10-11-fast-link-kit-fixes/,
# the repository's directory written <repo> and any other home path ~, as the
# decisions index asks ("The tree names no one's machine"). The raw outputs -
# every job's file and the per-PE records - stay here.
# usage: copy-evidence.sh TREE
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPOSITORY=$(cd "$HERE/../../../.." && pwd)
DESTINATION=$1/docs/design/evidence/v0.2-step4/2026-10-11-fast-link-kit-fixes
rm -rf "$DESTINATION"
mkdir -p "$DESTINATION"
clean() {   # SOURCE NAME: SOURCE into the evidence as NAME, its paths written as the index asks
    sed -e "s|$REPOSITORY|<repo>|g" -e "s|$HOME|~|g" "$1" > "$DESTINATION/$2"
}
for f in vm-build.sh vm-dry-run.sh make-check-probe.py compare.py analyze.py vm-multistream-check.sh \
         cases-check.txt vm-room-check.sh vm-cost-check.sh multistream-main.c vm-slurm-final.sh \
         vm-r1-echo.sh vm-r2-probe.sh vm-r2-packages.sh copy-evidence.sh; do
    clean "$HERE/$f" "$f"
done
# The build's output without its closing listing of the probes, which names
# the VM's user.
sed '/^total /,/^-rwx/d' "$HERE/final/build.txt" | grep -v '^-rwx\|^drwx' > "$HERE/final/build-trimmed.txt"
clean "$HERE/final/build-trimmed.txt" build.txt
clean "$HERE/final/dry-run.txt" dry-run.txt
for s in floor helpers ladder zero-copy gets fan-in scale; do
    clean "$HERE/final/dry-run/$s-summary.txt" "dry-run-$s-summary.txt"
done
clean "$HERE/final/check/run.txt" multistream-check.txt
clean "$HERE/final/room/room.compare.txt" multistream-room.txt
clean "$HERE/final/cost.txt" multistream-cost.txt
clean "$HERE/final/slurm.txt" slurm.txt
clean "$HERE/final/slurm/a-helpers-summary.txt" slurm-helpers-summary.txt
clean "$HERE/logs/r1-echo.txt" r1-echo-1.txt
clean "$HERE/final/r1-echo.txt" r1-echo-2.txt
clean "$HERE/final/r1-echo-2.txt" r1-echo-3.txt
clean "$HERE/logs/r2-probe.txt" r2-probe.txt
clean "$HERE/logs/r2-packages.txt" r2-packages.txt
ls "$DESTINATION" | wc -l | sed 's/$/ files/'
grep -l -F -e "$REPOSITORY" -e "$HOME" -e "$(id -un)" "$DESTINATION"/* || echo "no user's name or home path left"
