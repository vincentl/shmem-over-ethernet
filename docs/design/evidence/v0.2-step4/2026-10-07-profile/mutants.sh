#!/bin/bash
# Each mutant of the fill-the-room rung, built debug, against the tests that
# must catch it. usage: mutants.sh TAR OUT NAME... Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
TAR=$1; OUT=$2; shift 2; mkdir -p $OUT; : > $OUT/mutants.txt
S=/Users/vincent/shmem-over-ethernet/build/scratch/ladder/mutants
for NAME in "$@"; do
  T=/tmp/ladder-mutant-$NAME; rm -rf $T; mkdir -p $T; tar -C $T -xf $TAR 2>/dev/null
  python3 $S/mutate.py $T $NAME >> $OUT/mutants.txt || { echo "$NAME: MUTATION DID NOT APPLY" >> $OUT/mutants.txt; continue; }
  (cd $T && cmake --preset debug > $OUT/$NAME.configure.txt 2>&1)
  if ! (cd $T && cmake --build build/debug -j4 > $OUT/$NAME.build.txt 2>&1); then
    echo "$NAME: BUILD FAILED (caught by the compiler)" >> $OUT/mutants.txt; rm -rf $T; continue
  fi
  (cd $T && ctest --test-dir build/debug -j4 --timeout 60 -R 'shmem\.(connection|internode|multinode|signal|floor)' > $OUT/$NAME.tests.txt 2>&1)
  r=$(grep -E 'tests passed' $OUT/$NAME.tests.txt)
  failed=$(grep -E '\*\*\*Failed|\*\*\*Timeout|Exception' $OUT/$NAME.tests.txt | awk '{print $4}' | head -6 | paste -sd' ' -)
  echo "$NAME: $r | caught by: $failed" >> $OUT/mutants.txt
  rm -rf $T
done
echo DONE >> $OUT/mutants.txt
