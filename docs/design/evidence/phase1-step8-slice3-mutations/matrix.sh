#!/bin/bash
# Which hook catches each race mutation: OSOE_CHECK_QUIET alone, OSOE_TEST_PERTURB
# alone, or both; ten runs each on 0,1,2,0,1,0, counting failures.
exec 9>/tmp/osoe-vm.lock
flock -w 3600 9 || exit 1
R=/Users/vincent/shmem-over-ethernet; T=$R/build/scratch/wt/slice3; M=$R/build/scratch/wt/mutslice3
O=$R/build/scratch/slice3/matrix.txt; : > $O
cd $M
for m in none noretag plain_max pe_quiet_stores_full bit_first_moved; do
  cp $T/shmem/src/transport-am.c shmem/src/transport-am.c
  [ $m != none ] && { python3 $R/build/scratch/slice3/apply-mutation.py $m || echo "$m NOT APPLIED" >> $O; }
  echo "$m: $(grep -c MUTATION shmem/src/transport-am.c) mutated lines" >> $O
  cmake --build build/release -j4 --target osoe shmem_test_multiple 2>&1 | grep -E "error" | head -2 >> $O
  for cfg in "OSOE_CHECK_QUIET=1" "OSOE_TEST_PERTURB=1" "OSOE_CHECK_QUIET=1 OSOE_TEST_PERTURB=1"; do
    fails=0; first=""
    for i in $(seq 10); do
      out=$(timeout 150 env $cfg OSOE_GET_LANDING_BYTES=8192 build/release/tools/launch/osoe-launch -n 6 -m 0,1,2,0,1,0 -t 120 -- build/release/shmem/tests/shmem_test_multiple 2>&1)
      if [ $? -ne 0 ]; then fails=$((fails+1)); [ -z "$first" ] && first=$(echo "$out" | grep -m1 -E "OSOE_CHECK_QUIET|read|failed|timeout" | cut -c1-230); fi
    done
    echo "$m [$cfg]: $fails/10 failed. $first" >> $O
  done
done
cp $T/shmem/src/transport-am.c shmem/src/transport-am.c
echo DONE >> $O
