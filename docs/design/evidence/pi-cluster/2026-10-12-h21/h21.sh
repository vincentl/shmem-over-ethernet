#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# H21 on the Pi (docs/prompts/v0.2-open-items.md): v0.2 step 3 found the event
# simulation's Träff's 11 to 22% optimistic from 256 KiB, and osoe-bench's
# 64 KiB reduction back to back no faster under Träff's than under the trees
# (docs/decisions/2026-10-07-circulant-data-path.md). Step 3 ran OSU and
# osoe-bench under Slurm's default binding - a lead's main thread and its
# progress thread on one core - and before step 4's helpers. Here, on main's
# tree, release: `link` against `tree`, under the default binding and under
# --cpu-bind=none, at no helpers and at two, alternating three times: OSU
# reduce 64 KiB to 4 MiB at 4 x 1 and osoe-bench at 4 x 4.
# Usage: h21.sh TREE OUTDIR REPO (from the Mac; REPO holds corpus/).
set -u
TREE=$1 OUT=$2 REPO=$3
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$TREE"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/h21 && mkdir -p osoe/build/h21' || exit 1
rsync -a "$REPO"/corpus/osu-omb/osu-micro-benchmarks-7.5.2/ pi-login:osoe/build/h21/osu/ || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/h21/build.txt 2>&1; s=$?
  echo "built: $s, warnings: $(grep -c "warning:" build/h21/build.txt)"; [ $s = 0 ] || exit 1
  OSHCC=$HOME/osoe/build/release/tools/wrapper/oshcc
  cd build/h21/osu && ./configure CC=$OSHCC CXX=g++ > ../osu-configure.txt 2>&1
  find . \( -name configure.ac -o -name Makefile.am -o -name aclocal.m4 \) -exec touch -t 202001010000 {} +
  find . -name configure -exec touch {} +; find . -name Makefile.in -exec touch {} +; find . -name Makefile -exec touch {} +
  make -C c/openshmem -j4 > ../osu-build.txt 2>&1; s=$?; echo "osu: $s"; [ $s = 0 ]' >> $LOG || { echo "build failed" >> $LOG; exit 1; }
run() {   # label, environment, command (run in ~/osoe)
  ssh pi-login "cd osoe && env $2 $3 > build/h21/$1.txt 2>&1; echo \"$1: \$? \$(date +%H:%M:%S)\"" >> $LOG
}
osu=build/h21/osu/c/openshmem; bench=build/release/tools/bench/osoe-bench
for i in 1 2 3; do
  for helpers in 0 2; do
    for bind in bound none; do
      b=""; [ $bind = none ] && b="--cpu-bind=none"
      for side in link tree; do
        env="OSOE_COLLECTIVE_SHAPE=$side OSOE_PROGRESS_HELPERS=$helpers"
        run osu-reduce-4x1-$bind-h$helpers-$side-$i "$env" "srun -K1 -t 8 $b -N4 --ntasks-per-node=1 $osu/osu_oshm_reduce -m 65536:4194304"
        run bench-4x4-$bind-h$helpers-$side-$i "$env" "srun -K1 -t 8 $b -N4 --ntasks-per-node=4 $bench"
      done
    done
  done
done
scp -q 'pi-login:osoe/build/h21/*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
