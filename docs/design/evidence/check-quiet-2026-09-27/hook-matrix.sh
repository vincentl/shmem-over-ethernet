#!/bin/bash
# The slice-3 race mutations against the owner-checked stamp (wt/quiet-hook):
# both hooks on, 0,1,2,0,1,0, ten runs each of the library's threaded test.
exec 9>/tmp/osoe-vm.lock; flock -w 3600 9 || exit 1
R=<repo>; Q=$R/build/scratch/wt/quiet-hook
C=$R/build/scratch/pacer/check-quiet; O=$C/hook-matrix.txt; : > $O
cd $Q
for m in none noretag plain_max pe_quiet_stores_full bit_first_moved; do
  cp $C/transport-am.patched.c shmem/src/transport-am.c
  [ $m != none ] && { python3 $R/docs/design/evidence/phase1-step8-slice3-mutations/apply-mutation.py $m 2>> $O || echo "$m NOT APPLIED" >> $O; }
  echo "$m: $(grep -c MUTATION shmem/src/transport-am.c) mutated lines" >> $O
  cmake --build build/release -j4 --target osoe shmem_test_multiple osoe-launch 2>&1 | grep -E " error|error:" | head -2 >> $O
  fails=0; first=""
  for i in $(seq 10); do
    out=$(timeout 150 env OSOE_CHECK_QUIET=1 OSOE_TEST_PERTURB=1 OSOE_GET_LANDING_BYTES=8192 build/release/tools/launch/osoe-launch -n 6 -m 0,1,2,0,1,0 -t 120 -- build/release/shmem/tests/shmem_test_multiple 2>&1)
    if [ $? -ne 0 ]; then fails=$((fails+1)); [ -z "$first" ] && first=$(echo "$out" | grep -m1 -E "OSOE_CHECK_QUIET|read|failed|timeout" | cut -c1-200); fi
  done
  echo "$m [both hooks]: $fails/10 failed. $first" >> $O
done
cp $C/transport-am.patched.c shmem/src/transport-am.c
cmake --build build/release -j4 --target osoe shmem_test_multiple osoe-launch > /dev/null 2>&1
echo DONE >> $O
