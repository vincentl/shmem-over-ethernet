#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T10's timing on the Pi, on the tree to be tagged (after pi-a.sh built it in ~/osoe): the 1 MiB
# lines step 3's merge point timed (docs/design/evidence/pi-cluster/2026-10-07-v0.2-step3/part3.sh),
# not timed since step 4 changed the engine under them and Träff's grant became a field a receiver
# reads - `link`, the default, against `tree`, step 2's trees alone - and, for round ten's S3, the
# default against OSOE_PROGRESS_HELPERS=0; the three alternating, three times: the e-band probe at
# 4 x 4 and 4 x 1, OSU's reduce to 1 MiB at 4 x 1 and 4 x 4, and its broadcast to 1 MiB at 4 x 4.
# Usage: pi-t10.sh (from the Mac). Everything on the Pi under ~/osoe/build/t10.
set -u
OUT=<repo>/build/scratch/pi-tag-0edd95a9/t10
REPO=<repo>
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
PI="ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login"
echo "start $(date +%H:%M:%S)" >> $LOG
$PI 'rm -rf osoe/build/t10 && mkdir -p osoe/build/t10' || exit 1
scp -q $REPO/docs/design/evidence/pi-cluster/2026-10-04-step-e/e-band.c pi-login:osoe/build/t10/e-band.c || exit 1
rsync -a -e 'ssh -o HostKeyAlias=pi-login.local' $REPO/corpus/osu-omb/osu-micro-benchmarks-7.5.2/ pi-login:osoe/build/t10/osu/ || exit 1
$PI 'cd osoe && OSHCC=$HOME/osoe/build/release/tools/wrapper/oshcc
  $OSHCC -O2 -o build/t10/e-band build/t10/e-band.c; echo "e-band: $?"
  cd build/t10/osu && ./configure CC=$OSHCC CXX=g++ > ../osu-configure.txt 2>&1
  find . \( -name configure.ac -o -name Makefile.am -o -name aclocal.m4 \) -exec touch -t 202001010000 {} +
  find . -name configure -exec touch {} +; find . -name Makefile.in -exec touch {} +; find . -name Makefile -exec touch {} +
  make -C c/openshmem -j4 > ../osu-build.txt 2>&1; echo "osu: $?"' >> $LOG
run() {   # label, environment, command (run in ~/osoe)
  $PI "cd osoe && env $2 timeout 600 $3 > build/t10/$1.txt 2>&1; echo \"$1: \$?\"" >> $LOG
}
band=build/t10/e-band; osu=build/t10/osu/c/openshmem
for i in 1 2 3; do
  for side in link tree helpers-0; do
    case $side in
      link) env="OSOE_COLLECTIVE_SHAPE=link" ;;
      tree) env="OSOE_COLLECTIVE_SHAPE=tree" ;;
      helpers-0) env="OSOE_PROGRESS_HELPERS=0" ;;
    esac
    run band-4x4-$side-$i "$env" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 $band"
    run band-4x1-$side-$i "$env" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=1 $band"
    run osu-reduce-4x1-$side-$i "$env" "srun -K1 -N4 --ntasks-per-node=1 $osu/osu_oshm_reduce -m :1048576"
    run osu-reduce-4x4-$side-$i "$env" "srun -K1 -N4 --ntasks-per-node=4 $osu/osu_oshm_reduce -m :1048576"
    run osu-broadcast-4x4-$side-$i "$env" "srun -K1 -N4 --ntasks-per-node=4 $osu/osu_oshm_broadcast -m :1048576"
  done
done
scp -q 'pi-login:osoe/build/t10/*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
