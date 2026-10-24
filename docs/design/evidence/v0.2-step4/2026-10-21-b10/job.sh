#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# B10: one job, run as the throwaway account.
#
#   job.sh RUN_DIRECTORY LATE_SECONDS TIMEOUT_SECONDS [HOLD_SECONDS]
#
# Four PEs on this machine (osoe-launch -n 4); rank 1 calls shmem_init
# LATE_SECONDS late (late-pe.c), and osoe-launch ends the job after
# TIMEOUT_SECONDS. In RUN_DIRECTORY it writes job.started, job.out, job.err (the
# PEs' lines and the launcher's), job.ended, and job.status last: "exit N", the
# launcher's exit status. With HOLD_SECONDS every PE sleeps that long after
# shmem_init (late-pe.c). The tree is /tmp/b10, main's, built by build.sh.
run_directory=$1
late_seconds=$2
timeout_seconds=$3
tree=/tmp/b10
cd "$run_directory" || exit 2
if [ -n "${4:-}" ]; then
    B10_HOLD_SECONDS=$4
    export B10_HOLD_SECONDS
fi
date +%s.%N > job.started
"$tree/build/release/tools/launch/osoe-launch" -n 4 -t "$timeout_seconds" \
    -e "1:B10_DELAY_SECONDS=$late_seconds" -- "$tree/build/release/late-pe" \
    > job.out 2> job.err
status=$?
date +%s.%N > job.ended
echo "exit $status" > job.status
