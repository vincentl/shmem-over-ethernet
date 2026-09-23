#!/bin/sh
# Init reclaims what a job killed during init left behind, and refuses what is
# not a leftover (§6.2; review round two, D11 and D15).
#
#   leftovers.sh LAUNCHER PROGRAM reclaim|live
#
# Both run a two-PE job under a fixed job id, so the names are known before it
# starts, after planting objects under them:
#   reclaim  a zero-filled control segment, job tables and connection area of
#            node 0 -
#            what a job killed between shm_open and its first header store
#            leaves. No creator is recorded, so each is reclaimed and the job
#            runs; nothing of the job may have a name afterwards.
#   live     node 0's job tables with a recorded creator that is this script's
#            own shell, which is running. The job must refuse them, naming that
#            pid.
# The job tables are node-local since phase 1 step 3 (job.h):
# osoe.<job>.<step>.<node>.job.<session>, where the step-2 job segment was
# osoe.<job>.<step>.job.<session>, one for the whole job.
set -u
LAUNCHER=$1 PROGRAM=$2 MODE=$3
J=$((900000000 + $$))
plant() { head -c 8192 /dev/zero > "/dev/shm/osoe.$J.0.$1"; }
cleanup() { rm -f /dev/shm/osoe.$J.*; }
trap cleanup EXIT

plant 0.0            # the control segment of node 0, session 0
plant 0.job.0        # node 0's job tables
plant 0.conn.0       # node 0's connection area
if [ "$MODE" = live ]; then
    # The job header's creator_pid is the int32 at offset 24 (job.c). Store this
    # shell's pid there, little-endian.
    printf "$(printf '\\%03o\\%03o\\%03o\\%03o' $(($$ & 255)) $((($$ >> 8) & 255)) \
        $((($$ >> 16) & 255)) $((($$ >> 24) & 255)))" |
        dd of="/dev/shm/osoe.$J.0.0.job.0" bs=1 seek=24 conv=notrunc 2>/dev/null
fi
# Two nodes of one PE each, so that node 0 opens a connection area too.
out=$("$LAUNCHER" -n 2 -m 0,1 -t 30 -e 0:OSOE_JOB_ID=$J -e 1:OSOE_JOB_ID=$J \
    -- "$PROGRAM" 2>&1)
status=$?
printf '%s\n' "$out"
echo "job exited $status"
# ctest's regex has no backreference, so the pid the refusal names is compared
# here: it must be this shell's.
if printf '%s\n' "$out" | grep -q "its creator, pid $$, is still running"; then
    echo "the refusal names the owner: yes"
fi
left=$(ls /dev/shm | grep -c "^osoe\.$J\.")
echo "objects of job $J with a name afterwards: $left"
