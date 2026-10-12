#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The release point's Pi runbook, part D: the two hours - an hour of mix
# (release), the release checklist's, then an hour of threads in a debug build
# with the two test hooks and an overfilled landing zone. Usage: pi-d.sh.
set -u
OUT=<repo>/build/scratch/pi-rehearsal-3b45472/d
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
PI="ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login"
echo "start $(date +%H:%M:%S)" >> $LOG
$PI 'mkdir -p osoe/build/release-run/pacer && cd osoe && srun -K1 -N4 --ntasks-per-node=4 build/release/tools/pacer/osoe-pacer --seconds 3600 --fold-nodes mix > build/release-run/pacer/pacer-mix.txt 2>&1; echo "mix: $? $(tail -1 build/release-run/pacer/pacer-mix.txt)"' >> $LOG
echo "mix done $(date +%H:%M:%S)" >> $LOG
$PI 'cd osoe && (cmake --preset debug && cmake --build build/debug -j4 --target osoe-pacer) > build/release-run/pacer/debug-build.txt 2>&1; echo "debug build: $?";
  OSOE_CHECK_QUIET=1 OSOE_TEST_PERTURB=1 OSOE_GET_LANDING_BYTES=8192 srun -K1 -N4 --ntasks-per-node=4 build/debug/tools/pacer/osoe-pacer --seconds 3600 threads > build/release-run/pacer/pacer-threads-hour.txt 2>&1;
  echo "threads: $? $(tail -1 build/release-run/pacer/pacer-threads-hour.txt) check_quiet_lines=$(grep -c OSOE_CHECK_QUIET: build/release-run/pacer/pacer-threads-hour.txt)"' >> $LOG
rsync -a 'pi-login:osoe/build/release-run/pacer/pacer-mix.txt' 'pi-login:osoe/build/release-run/pacer/pacer-threads-hour.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
