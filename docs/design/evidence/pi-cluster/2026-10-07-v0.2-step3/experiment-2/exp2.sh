#!/bin/bash
# v0.2 step 3 Part 3, the second experiment: a variant whose every round
# spreads over (ceiling + 1) / rows of the lane's rows, its window that many
# times the capacity - 16 pieces on four nodes - against the branch at `link`
# (a window of 4) and `tree`. e-band at 4 x 1 and 4 x 4 and OSU reduce at 4 x 1
# to 4 MiB, alternating three times. Usage: exp2.sh VARIANT_TREE OUTDIR.
set -u
V=$1 OUT=$2
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$V"/ pi-login:osoe/build/v02s3/vrows/ || exit 1
ssh pi-login 'cd osoe/build/v02s3/vrows && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../vrows-build.txt 2>&1; echo "vrows built: $?"
  build/release/tools/wrapper/oshcc -O2 -o ../e-band-vrows ../e-band.c; echo "vrows e-band: $?"
  OSHCC=$HOME/osoe/build/v02s3/vrows/build/release/tools/wrapper/oshcc
  rm -rf ../osu-vrows && cp -R ../osu ../osu-vrows && cd ../osu-vrows && make distclean > /dev/null 2>&1; ./configure CC=$OSHCC CXX=g++ > ../osu-vrows-configure.txt 2>&1
  find . \( -name configure.ac -o -name Makefile.am -o -name aclocal.m4 \) -exec touch -t 202001010000 {} +
  find . -name configure -exec touch {} +; find . -name Makefile.in -exec touch {} +; find . -name Makefile -exec touch {} +
  make -C c/openshmem -j4 > ../osu-vrows-build.txt 2>&1; echo "vrows osu: $?"' >> $LOG
run() {   # label, environment, command (run in ~/osoe)
  ssh pi-login "cd osoe && env $2 $3 > build/v02s3/$1.txt 2>&1; echo \"$1: \$?\"" >> $LOG
}
B=build/v02s3
for i in 1 2 3; do
  for l in 4x1 4x4; do
    p=1; [ $l = 4x4 ] && p=4
    run x2-band-$l-link-$i "OSOE_COLLECTIVE_SHAPE=link" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=$p $B/e-band"
    run x2-band-$l-vrows-$i "OSOE_COLLECTIVE_SHAPE=link" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=$p $B/e-band-vrows"
    run x2-band-$l-tree-$i "OSOE_COLLECTIVE_SHAPE=tree" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=$p $B/e-band"
  done
  run x2-osu-4x1-link-$i "OSOE_COLLECTIVE_SHAPE=link" "srun -K1 -N4 --ntasks-per-node=1 $B/osu/c/openshmem/osu_oshm_reduce -m 16384:4194304"
  run x2-osu-4x1-vrows-$i "OSOE_COLLECTIVE_SHAPE=link" "srun -K1 -N4 --ntasks-per-node=1 $B/osu-vrows/c/openshmem/osu_oshm_reduce -m 16384:4194304"
  run x2-osu-4x1-tree-$i "OSOE_COLLECTIVE_SHAPE=tree" "srun -K1 -N4 --ntasks-per-node=1 $B/osu/c/openshmem/osu_oshm_reduce -m 16384:4194304"
done
scp -q 'pi-login:osoe/build/v02s3/x2-*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
