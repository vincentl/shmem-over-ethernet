#!/bin/sh
# Kill a whole job, launcher included, with SIGKILL; then see what is left in
# /dev/shm, what osoe-stat --gc removes, and whether a job with the SAME job id
# starts on the same node afterwards.
. <repo>/build/scratch/review-diagnosability/env.sh
log=$E/logs/5c-sigkill-all.log
job=77700123
F=""; for r in 0 1 2 3; do F="$F -e $r:OSOE_JOB_ID=$job"; done
exec > $log 2>&1
$LAUNCH -n 4 -m 0,0,1,1 -t 60 $F -- $W/hang barrier 3 &
launcher=$!
sleep 4
pids=$(pgrep -P $launcher)
echo "# SIGKILL launcher $launcher and PEs" $pids
kill -KILL $launcher $pids
sleep 1
echo '$ ls /dev/shm | grep osoe.'$job; ls -la /dev/shm | grep osoe.$job
echo '$ osoe-stat --list (this job only)'; $STAT --list | grep $job
echo '$ osoe-stat --gc'; $STAT --gc 2>&1 | grep $job
echo '$ ls /dev/shm | grep osoe.'$job' (after --gc)'; ls -la /dev/shm | grep osoe.$job
echo "# second job, same job id, WITHOUT removing what --gc left"
$LAUNCH -n 4 -m 0,0,1,1 -t 20 $F -- $W/hang clean
echo "# second job exit $?"
ls -la /dev/shm | grep osoe.$job
rm -f /dev/shm/osoe.$job.*
