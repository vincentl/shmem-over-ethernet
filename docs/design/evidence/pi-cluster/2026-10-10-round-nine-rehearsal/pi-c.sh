#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The release point's Pi runbook, part C: §6's pacer runs but the two hours
# (part D) - default, the three sorts, mailbox, teams, phase 3's five, waits,
# deprecated, start_pes, the counted coverage and --teams-exhaust. Outputs in
# ~/osoe/build/release-run/pacer. Usage: pi-c.sh (after part B).
set -u
OUT=<repo>/build/scratch/pi-rehearsal-3b45472/c
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
PI="ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login"
echo "start $(date +%H:%M:%S)" >> $LOG
$PI 'mkdir -p osoe/build/release-run/pacer' || exit 1
ENV='cd osoe && P=build/release/tools/pacer/osoe-pacer; R=build/release-run/pacer'
run() {   # name, then the command's tail after srun's shape
  local name=$1; shift
  $PI "$ENV; $* > \$R/$name.txt 2>&1; echo \"$name: \$? \$(tail -1 \$R/$name.txt)\"" >> $LOG
}
run pacer-default 'srun -K1 -N4 --ntasks-per-node=4 $P --seconds 300'
run pacer-accumulate 'SHMEM_SYMMETRIC_SIZE=1500m srun -K1 -N4 --ntasks-per-node=4 $P --seconds 600 --sort accumulate --memory 1g sort'
run pacer-pull 'srun -K1 -N4 --ntasks-per-node=4 $P --seconds 600 --exchange alltoall sort'
run pacer-no-ptr 'srun -K1 -N4 --ntasks-per-node=4 $P --seconds 600 --no-ptr sort'
run pacer-mailbox 'srun -K1 -N4 --ntasks-per-node=4 $P --seconds 600 mailbox'
run pacer-teams 'srun -K1 -N4 --ntasks-per-node=4 $P --seconds 600 teams'
for e in atomics rma locks memory collectives; do
  run pacer-$e "srun -K1 -N4 --ntasks-per-node=4 \$P --seconds 600 $e"
done
run pacer-waits 'srun -K1 -N4 --ntasks-per-node=4 $P --seconds 300 waits'
run pacer-deprecated 'srun -K1 -N4 --ntasks-per-node=4 $P --seconds 300 deprecated'
run pacer-start-pes 'srun -K1 -N4 --ntasks-per-node=4 build/release/tools/pacer/osoe-pacer-start-pes'
run pacer-teams-exhaust 'srun -K1 -N4 --ntasks-per-node=4 $P --teams-exhaust teams'
run coverage-run 'srun -K1 -N4 --ntasks-per-node=4 build/release/tools/pacer/osoe-pacer-counted --quick --rounds 792 --coverage $R/coverage.txt sort alltoall mailbox teams atomics rma locks waits memory collectives threads print deprecated'
run coverage-mix-run 'srun -K1 -N4 --ntasks-per-node=4 build/release/tools/pacer/osoe-pacer-counted --seconds 120 --coverage $R/coverage-mix.txt mix'
run coverage-start-pes-run 'srun -K1 -N4 --ntasks-per-node=4 build/release/tools/pacer/osoe-pacer-start-pes-counted --coverage $R/coverage-start-pes.txt'
rsync -a 'pi-login:osoe/build/release-run/pacer/' $OUT/pacer/
echo "end $(date +%H:%M:%S)" >> $LOG
