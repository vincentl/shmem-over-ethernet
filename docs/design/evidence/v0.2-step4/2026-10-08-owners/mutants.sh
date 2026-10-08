#!/bin/bash
# v0.2 step 4 part 1: the adversarial read's S1 - each mutant tree, built in
# debug, against shmem.owners.* and shmem.owner-map, which must fail on it.
# usage: mutants.sh DIR (DIR holds NAME.tar for each mutant). Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
D=$1; OUT=$D/mutants.txt; : > $OUT
for tar in $D/*.tar; do
  name=$(basename $tar .tar)
  T=/tmp/owners-mutant-$name; rm -rf $T; mkdir -p $T; tar -C $T -xf $tar; cd $T
  echo "== $name $(date +%H:%M:%S)" >> $OUT
  cmake --preset debug > $D/$name.configure 2>&1
  if ! cmake --build build/debug -j4 > $D/$name.build 2>&1; then
    echo "BUILD FAILED" >> $OUT; grep -E "error" $D/$name.build | head -5 >> $OUT
  else
    ctest --test-dir build/debug -j4 --timeout 120 --output-on-failure -R "shmem\.(owners|owner-map)" > $D/$name.full 2>&1
    grep -E "tests passed|\*\*\*Failed|\*\*\*Timeout|Not Run" $D/$name.full >> $OUT
    grep -m3 -E "osoe_fatal|CHECK\(|record of slot|owner [0-9]+, of" $D/$name.full >> $OUT
  fi
  cd /; rm -rf $T
done
echo "DONE $(date +%H:%M:%S)" >> $OUT
