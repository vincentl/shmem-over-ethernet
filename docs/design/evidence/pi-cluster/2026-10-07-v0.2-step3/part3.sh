#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 3 Part 3 on the Pi (the brief's "Part 3: the measurements"): the
# branch's tree in ~/osoe (nothing outside it), release; OSU and the e-band
# probe built with its oshcc. Then, alternating three times, `link` (Träff's
# where the chooser takes it) against `tree` (step 2's trees alone): e-band
# (reduce, broadcast, scan, 8 B to 1 MiB) at 4 x 4 and 4 x 1, OSU reduce to
# 1 MiB at 4 x 1 and 4 x 4, osoe-bench at 4 x 4. Then OSU reduce to 4 MiB at
# 4 x 1, both shapes, past the brief's sizes, once.
# Usage: part3.sh TREE OUTDIR (from the Mac).
set -u
TREE=$1 OUT=$2
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$TREE"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/v02s3 && mkdir -p osoe/build/v02s3' || exit 1
scp -q <repo>/docs/design/evidence/pi-cluster/2026-10-04-step-e/e-band.c pi-login:osoe/build/v02s3/e-band.c || exit 1
rsync -a <repo>/corpus/osu-omb/osu-micro-benchmarks-7.5.2/ pi-login:osoe/build/v02s3/osu/ || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/v02s3/build.txt 2>&1; echo "built: $?"
  OSHCC=$HOME/osoe/build/release/tools/wrapper/oshcc
  $OSHCC -O2 -o build/v02s3/e-band build/v02s3/e-band.c; echo "e-band: $?"
  cd build/v02s3/osu && ./configure CC=$OSHCC CXX=g++ > ../osu-configure.txt 2>&1
  find . \( -name configure.ac -o -name Makefile.am -o -name aclocal.m4 \) -exec touch -t 202001010000 {} +
  find . -name configure -exec touch {} +; find . -name Makefile.in -exec touch {} +; find . -name Makefile -exec touch {} +
  make -C c/openshmem -j4 > ../osu-build.txt 2>&1; echo "osu: $?"' >> $LOG
run() {   # label, environment, command (run in ~/osoe)
  ssh pi-login "cd osoe && env $2 $3 > build/v02s3/$1.txt 2>&1; echo \"$1: \$?\"" >> $LOG
}
band=build/v02s3/e-band; osu=build/v02s3/osu/c/openshmem; bench=build/release/tools/bench/osoe-bench
for i in 1 2 3; do
  for side in link tree; do
    env="OSOE_COLLECTIVE_SHAPE=$side"
    run band-4x4-$side-$i "$env" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 $band"
    run band-4x1-$side-$i "$env" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=1 $band"
    run osu-reduce-4x1-$side-$i "$env" "srun -K1 -N4 --ntasks-per-node=1 $osu/osu_oshm_reduce -m :1048576"
    run osu-reduce-4x4-$side-$i "$env" "srun -K1 -N4 --ntasks-per-node=4 $osu/osu_oshm_reduce -m :1048576"
    run bench-4x4-$side-$i "$env" "srun -K1 -N4 --ntasks-per-node=4 $bench"
  done
done
for side in link tree; do
  run osu-reduce-big-4x1-$side "OSOE_COLLECTIVE_SHAPE=$side" "srun -K1 -N4 --ntasks-per-node=1 $osu/osu_oshm_reduce -m 1048576:4194304"
done
scp -q 'pi-login:osoe/build/v02s3/*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
