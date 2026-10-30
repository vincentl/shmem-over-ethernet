#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T9 on the tree to be tagged: the release point's Pi runbook's part D, its first half - an hour of
# mix (release), the release checklist's. Usage: pi-mix.sh (from the Mac, after pi-a.sh).
set -u
OUT=<repo>/build/scratch/pi-tag-0edd95a9/d
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
PI="ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login"
echo "start $(date +%H:%M:%S)" >> $LOG
$PI 'mkdir -p osoe/build/release-run/pacer && cd osoe && srun -K1 -N4 --ntasks-per-node=4 build/release/tools/pacer/osoe-pacer --seconds 3600 --fold-nodes mix > build/release-run/pacer/pacer-mix.txt 2>&1; echo "mix: $? $(tail -1 build/release-run/pacer/pacer-mix.txt)"' >> $LOG
echo "mix done $(date +%H:%M:%S)" >> $LOG
scp -q 'pi-login:osoe/build/release-run/pacer/pacer-mix.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
