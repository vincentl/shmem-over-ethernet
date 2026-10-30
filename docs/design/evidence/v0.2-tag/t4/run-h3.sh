#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# run-h3.sh: T4's SOS sweep at OSOE_PROGRESS_HELPERS=3, which T20's closing note (2026-10-24) left
# to the tag's runs and T4's of 2026-10-30, at 0 and 2, did not include: the tag tree built again
# (build.sh) and sos-sweep.sh 3 under the VM's lock; then the helpers' threads read at 3 as
# helpers-threads.sh reads them at 0 and 2 (sleep4.c under the same map), which takes the lock itself.
#   run-h3.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
LOG=$HERE/progress-h3.log
say() { echo "$(date +%H:%M:%S) $*" | tee -a "$LOG"; }
: > "$LOG"
(
exec 9>/tmp/osoe-vm.lock
flock -w 3600 9 || { say "no lock"; exit 99; }
say "lock held; load $(cut -d' ' -f1-3 /proc/loadavg)"
bash "$HERE/build.sh" > "$HERE/phase-build-h3.log" 2>&1 || { say "build failed"; exit 1; }
say "built: $(grep -E '^built' "$HERE/phase-build-h3.log")"
bash "$HERE/sos-sweep.sh" 3 > "$HERE/phase-sos-h3.log" 2>&1
say "sos-h3 ends rc=$?; load $(cut -d' ' -f1-3 /proc/loadavg)"
) || exit $?
T=/var/tmp/t4-tree/build/release
exec 9>/tmp/osoe-vm.lock
flock -w 600 9 || { say "no lock for the threads"; exit 99; }
$T/tools/wrapper/oshcc -O1 -o /var/tmp/t4-sleep4 "$HERE/sleep4.c" || { say "sleep4 did not build"; exit 1; }
OSOE_PROGRESS_HELPERS=3 timeout 60 $T/tools/launch/osoe-launch -n 8 -m 0,0,1,1,2,2,3,3 -t 40 -- /var/tmp/t4-sleep4 > /var/tmp/t4-sleep4.3.out 2>&1 < /dev/null &
sleep 2.5
n=0
for p in /proc/[0-9]*; do
    [ "$(readlink "$p/exe" 2>/dev/null)" = /var/tmp/t4-sleep4 ] || continue
    n=$((n + $(cat "$p"/task/*/comm 2>/dev/null | grep -c '^osoe-helper')))
done
wait
say "OSOE_PROGRESS_HELPERS=3: $n threads named osoe-helper-N in the job's processes at 2.5 s (three a lead, four leads: 12)"
say "DONE"
