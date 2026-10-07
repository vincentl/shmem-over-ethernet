#!/bin/bash
# v0.2 step 3 Part 3, an experiment after the main run: what holds Träff's
# back at 1 MiB on the Pi. e-band at 4 x 1, alternating three times: the
# branch at `link` (W 4), at `link` with OSOE_COLLECTIVE_PIPELINE=8, a variant
# whose rounds take an arrived piece before sending the next (vtake), and the
# branch at `tree` with the pipeline at 8. Usage: exp1.sh VARIANT_TREE OUTDIR.
set -u
V=$1 OUT=$2
mkdir -p $OUT; LOG=$OUT/run.txt; : > $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
rsync -a --delete --exclude=/build --exclude=/.git "$V"/ pi-login:osoe/build/v02s3/vtake/ || exit 1
ssh pi-login 'cd osoe/build/v02s3/vtake && cmake --preset release > /dev/null 2>&1 && cmake --build build/release -j4 > ../vtake-build.txt 2>&1; echo "vtake built: $?"
  build/release/tools/wrapper/oshcc -O2 -o ../e-band-vtake ../e-band.c; echo "vtake e-band: $?"' >> $LOG
run() {   # label, environment, command (run in ~/osoe)
  ssh pi-login "cd osoe && env $2 $3 > build/v02s3/$1.txt 2>&1; echo \"$1: \$?\"" >> $LOG
}
for i in 1 2 3; do
  run exp-link4-$i "OSOE_COLLECTIVE_SHAPE=link" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=1 build/v02s3/e-band"
  run exp-link8-$i "OSOE_COLLECTIVE_SHAPE=link OSOE_COLLECTIVE_PIPELINE=8" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=1 build/v02s3/e-band"
  run exp-vtake-$i "OSOE_COLLECTIVE_SHAPE=link" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=1 build/v02s3/e-band-vtake"
  run exp-tree8-$i "OSOE_COLLECTIVE_SHAPE=tree OSOE_COLLECTIVE_PIPELINE=8" "srun -K1 --cpu-bind=none -N4 --ntasks-per-node=1 build/v02s3/e-band"
done
scp -q 'pi-login:osoe/build/v02s3/exp-*.txt' $OUT/
echo "end $(date +%H:%M:%S)" >> $LOG
