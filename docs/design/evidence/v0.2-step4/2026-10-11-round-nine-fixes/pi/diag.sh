#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# What shmem.start-up.lead-ends-in-join waits on when it runs to ctest's 38 s
# beside tier 2's largest jobs on a Pi node: lead-ends.sh in ~/osoe given
# timestamps (rank 0's kill and reap, each rank's start), and a sampler in the
# same step writing the job's processes' states, wait channels and ages each
# second; tier 2 three times in one held session, as tier2-held.sh. The script
# is put back afterwards.
set -u
P=<repo>/build/scratch/review-nine-fixes/pi
LOG=$P/diag.txt; : > $LOG
ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login 'sleep 7200' &
HOLD=$!
sleep 5
ssh -o ConnectTimeout=20 pi-login 'cd osoe && cp shmem/tests/lead-ends.sh /tmp/lead-ends.sh.orig && cat > shmem/tests/lead-ends.sh' <<'SH'
#!/bin/sh
# (diagnostic copy, 2026-10-11)
SECONDS_TO_KILL=$1 NEVER=$2
shift 2
D=/tmp/lead-ends-diag.txt
echo "$(date +%s.%N) rank $OSOE_PROCID start pid $$" >> $D
[ "$OSOE_PROCID" = "$NEVER" ] && exit 3
if [ "$OSOE_PROCID" = 0 ]; then
    "$@" &
    pid=$!
    sleep "$SECONDS_TO_KILL"
    echo "$(date +%s.%N) rank 0 killing $pid" >> $D
    kill -KILL "$pid"
    wait "$pid"
    echo "$(date +%s.%N) rank 0 reaped $pid" >> $D
    exit 4
fi
exec "$@"
SH
ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login "cd osoe && rm -f /tmp/lead-ends-diag.txt && srun -N1 -n1 -c4 sh -c '
  (while true; do
     echo \"=== \$(date +%s.%N)\"
     ps -eo pid,ppid,stat,etimes,wchan:30,args | grep -E \"shmem_test_init|lead-ends|refusal|osoe-launch -k -n 3\" | grep -v grep
     sleep 1
   done) > /tmp/lead-ends-ps.txt 2>&1 &
  S=\$!
  for i in 1 2 3; do
    ctest --test-dir build/release -L tier2 -E \"^model[.]\" -j4 --timeout 300 --output-on-failure > build/release-run/diag-\$i.txt 2>&1
    echo \"diag tier2 run \$i: \$? \$(grep \"tests passed\" build/release-run/diag-\$i.txt)\"; grep -E \"[*][*][*]Failed|Timeout\" build/release-run/diag-\$i.txt | head -5
  done
  kill \$S
  cp /tmp/lead-ends-ps.txt /tmp/lead-ends-diag.txt build/release-run/'
  cp /tmp/lead-ends.sh.orig shmem/tests/lead-ends.sh" >> $LOG 2>&1
scp -q "pi-login:osoe/build/release-run/diag-*.txt" "pi-login:osoe/build/release-run/lead-ends-*.txt" $P/ 2>> $LOG
kill $HOLD
echo "DONE $(date +%H:%M:%S)" >> $LOG
