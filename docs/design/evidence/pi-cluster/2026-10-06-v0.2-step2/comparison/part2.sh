#!/bin/bash
# v0.2 step 2 Part 2 on the Pi (the brief's "Measurements"): the branch's tree
# in ~/osoe (nothing outside it), release; OSU and the e-band probe built with
# its oshcc. Then, alternating three times, `link` against `fixed` at 0.1.0's
# defaults (k 4, D 2): e-band (reduce, broadcast, scan, 8 B to 1 MiB) at 4 x 4
# and 4 x 1, OSU reduce, broadcast and fcollect to 1 MiB at 4 x 1 and 4 x 4,
# osoe-bench at 4 x 4. Then the 1 to 16 KiB sweep for the model's refit: OSU
# reduce and broadcast at fixed shapes k 2, 3, 4 and D 2, 4, 4 x 1.
# Usage: part2.sh TREE OUTDIR (from the Mac).
set -u
TREE=$1 OUT=$2
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$TREE"/ pi-login:osoe/ || { echo "rsync failed" >> $LOG; exit 1; }
ssh pi-login 'rm -rf osoe/build/v02s2 && mkdir -p osoe/build/v02s2' || exit 1
scp -q <repo>/docs/design/evidence/pi-cluster/2026-10-04-step-e/e-band.c pi-login:osoe/build/v02s2/e-band.c || exit 1
rsync -a <repo>/corpus/osu-omb/osu-micro-benchmarks-7.5.2/ pi-login:osoe/build/v02s2/osu/ || exit 1
ssh pi-login 'cd osoe && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > build/v02s2/build.txt 2>&1; echo "built: $?"
  OSHCC=$HOME/osoe/build/release/tools/wrapper/oshcc
  $OSHCC -O2 -o build/v02s2/e-band build/v02s2/e-band.c; echo "e-band: $?"
  cd build/v02s2/osu && ./configure CC=$OSHCC CXX=g++ > ../osu-configure.txt 2>&1
  find . \( -name configure.ac -o -name Makefile.am -o -name aclocal.m4 \) -exec touch -t 202001010000 {} +
  find . -name configure -exec touch {} +; find . -name Makefile.in -exec touch {} +; find . -name Makefile -exec touch {} +
  make -C c/openshmem -j4 > ../osu-build.txt 2>&1; echo "osu: $?"' >> $LOG
# And the tag's own tree, v0.1.0, under build/v02s2/tag with its own e-band
# and OSU, the merge point's comparison.
scp -q <repo>/build/scratch/shape/pi/tag-v0.1.0.tar pi-login:osoe/build/v02s2/tag.tar || exit 1
ssh pi-login 'cd osoe/build/v02s2 && mkdir -p tag && tar -C tag -xf tag.tar && cd tag && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../tag-build.txt 2>&1; echo "tag built: $?"
  OSHCC=$HOME/osoe/build/v02s2/tag/build/release/tools/wrapper/oshcc
  $OSHCC -O2 -o ../e-band-tag ../e-band.c; echo "tag e-band: $?"
  cp -R ../osu ../osu-tag-src 2>/dev/null; rm -rf ../osu-tag && mv ../osu-tag-src ../osu-tag && cd ../osu-tag && make distclean > /dev/null 2>&1; ./configure CC=$OSHCC CXX=g++ > ../osu-tag-configure.txt 2>&1
  find . \( -name configure.ac -o -name Makefile.am -o -name aclocal.m4 \) -exec touch -t 202001010000 {} +
  find . -name configure -exec touch {} +; find . -name Makefile.in -exec touch {} +; find . -name Makefile -exec touch {} +
  make -C c/openshmem -j4 > ../osu-tag-build.txt 2>&1; echo "tag osu: $?"' >> $LOG
FIXED="OSOE_COLLECTIVE_SHAPE=fixed OSOE_COLLECTIVE_FANOUT=4 OSOE_COLLECTIVE_PIPELINE=2"
run() {   # label, environment, command (run in ~/osoe)
  ssh pi-login "cd osoe && env $2 $3 > build/v02s2/$1.txt 2>&1; echo \"$1: \$?\"" >> $LOG
}
for i in 1 2 3; do
  for side in link fixed tag; do
    env="OSOE_COLLECTIVE_SHAPE=link"; band=build/v02s2/e-band; osu=build/v02s2/osu; bench=build/release/tools/bench/osoe-bench
    [ $side = fixed ] && env="$FIXED"
    [ $side = tag ] && { env="OSOE_TAG=1"; band=build/v02s2/e-band-tag; osu=build/v02s2/osu-tag; bench=build/v02s2/tag/build/release/tools/bench/osoe-bench; }
    run band-4x4-$side-$i "$env" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 $band"
    run band-4x1-$side-$i "$env" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=1 $band"
    for p in reduce broadcast fcollect; do
      run osu-$p-4x1-$side-$i "$env" "srun -K1 -N4 --ntasks-per-node=1 $osu/c/openshmem/osu_oshm_$p -m :1048576"
      run osu-$p-4x4-$side-$i "$env" "srun -K1 -N4 --ntasks-per-node=4 $osu/c/openshmem/osu_oshm_$p -m :1048576"
    done
    run bench-4x4-$side-$i "$env" "srun -K1 -N4 --ntasks-per-node=4 $bench"
  done
done
for k in 2 3 4; do
  for d in 2 4; do
    for p in reduce broadcast; do
      run sweep-$p-k$k-d$d "OSOE_COLLECTIVE_SHAPE=fixed OSOE_COLLECTIVE_FANOUT=$k OSOE_COLLECTIVE_PIPELINE=$d" "srun -K1 -N4 --ntasks-per-node=1 build/v02s2/osu/c/openshmem/osu_oshm_$p -m 1024:16384"
    done
  done
done
scp -q 'pi-login:osoe/build/v02s2/*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
