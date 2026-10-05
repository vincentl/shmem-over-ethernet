#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The release point's Pi runbook, the rest of part C and all of part D, run
# detached on the head node (nohup setsid), so that no connection from the Mac
# need stay open for hours: it waits for the running default pacer job to end,
# then runs §6's remaining pacer runs, an hour of mix and, in a debug build, an
# hour of threads, one at a time, each verdict a line of rest.log, and writes
# ALL DONE at the end. Touches nothing outside ~/osoe.
cd ~/osoe || exit 1
R=build/release-run/pacer; LOG=build/release-run/rest.log; mkdir -p $R
P=build/release/tools/pacer/osoe-pacer
note() { echo "$* $(date +%H:%M:%S)" >> $LOG; }
note "start"
while [ "$(squeue -h -u pi | wc -l)" -gt 0 ]; do sleep 30; done
note "pacer-default: done, $(tail -1 $R/pacer-default.txt)"
step() {   # name, then the command
  local name=$1; shift
  bash -c "$*" > $R/$name.txt 2>&1; local rc=$?
  note "$name: $rc $(tail -1 $R/$name.txt)"
}
step pacer-accumulate "SHMEM_SYMMETRIC_SIZE=1500m srun -K1 -N4 --ntasks-per-node=4 $P --seconds 600 --sort accumulate --memory 1g sort"
step pacer-pull "srun -K1 -N4 --ntasks-per-node=4 $P --seconds 600 --exchange alltoall sort"
step pacer-no-ptr "srun -K1 -N4 --ntasks-per-node=4 $P --seconds 600 --no-ptr sort"
step pacer-mailbox "srun -K1 -N4 --ntasks-per-node=4 $P --seconds 600 mailbox"
step pacer-teams "srun -K1 -N4 --ntasks-per-node=4 $P --seconds 600 teams"
for e in atomics rma locks memory collectives; do
  step pacer-$e "srun -K1 -N4 --ntasks-per-node=4 $P --seconds 600 $e"
done
step pacer-waits "srun -K1 -N4 --ntasks-per-node=4 $P --seconds 300 waits"
step pacer-deprecated "srun -K1 -N4 --ntasks-per-node=4 $P --seconds 300 deprecated"
step pacer-start-pes "srun -K1 -N4 --ntasks-per-node=4 build/release/tools/pacer/osoe-pacer-start-pes"
step pacer-teams-exhaust "srun -K1 -N4 --ntasks-per-node=4 $P --teams-exhaust teams"
step coverage-run "srun -K1 -N4 --ntasks-per-node=4 build/release/tools/pacer/osoe-pacer-counted --quick --rounds 792 --coverage $R/coverage.txt sort alltoall mailbox teams atomics rma locks waits memory collectives threads print deprecated"
step coverage-mix-run "srun -K1 -N4 --ntasks-per-node=4 build/release/tools/pacer/osoe-pacer-counted --seconds 120 --coverage $R/coverage-mix.txt mix"
step coverage-start-pes-run "srun -K1 -N4 --ntasks-per-node=4 build/release/tools/pacer/osoe-pacer-start-pes-counted --coverage $R/coverage-start-pes.txt"
note "part C done"
step pacer-mix "srun -K1 -N4 --ntasks-per-node=4 $P --seconds 3600 --fold-nodes mix"
step debug-build "cmake --preset debug && cmake --build build/debug -j4 --target osoe-pacer"
step pacer-threads-hour "OSOE_CHECK_QUIET=1 OSOE_TEST_PERTURB=1 OSOE_GET_LANDING_BYTES=8192 srun -K1 -N4 --ntasks-per-node=4 build/debug/tools/pacer/osoe-pacer --seconds 3600 threads"
note "check_quiet lines in the threads hour: $(grep -c OSOE_CHECK_QUIET: $R/pacer-threads-hour.txt)"
note "ALL DONE"
