#!/bin/bash
# After step 8 slice 2 (main bc5fbb6): the runbook's §11 item 4 - SOS's threaded
# programs and shmem_test_multiple under Slurm at 4 x 2, and the thread tests
# of tier 2. Outputs in ~/pi-results-threads.
set -u
O=~/pi-results-threads; mkdir -p $O; : > $O/summary.txt
say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
R=~/shmem-over-ethernet; cd $R
cmake --preset release > $O/build.txt 2>&1 && cmake --build build/release -j4 >> $O/build.txt 2>&1
say "build exit $?"
srun -N1 -n1 -c4 ctest --test-dir build/release -R 'multiple|thread|context|get-nbi|lock' -j2 --timeout 300 > $O/tier2-threads.txt 2>&1
say "tier2 threads: $(grep -E 'tests passed' $O/tier2-threads.txt)"
for shape in "-N4 --ntasks-per-node=2" "-N4 --ntasks-per-node=1"; do
  for landing in 8192 262144; do
    timeout 180 env OSOE_GET_LANDING_BYTES=$landing srun -K1 $shape build/release/shmem/tests/shmem_test_multiple > $O/multiple-$(echo $shape | tr -d ' -=')-$landing.txt 2>&1 < /dev/null
    say "shmem_test_multiple [$shape] landing $landing exit $?"
  done
done
OSHCC=$R/build/release/tools/wrapper/oshcc
SOS=$R/../osoe-corpus/sos
S=$O/sos; mkdir -p $S/unit; cd $S
for p in mt_a2a mt_contention mt_lock_test mt_membar threading thread_wait query_thread gettid_register; do
  [ -f $SOS/test/unit/$p.c ] || [ -f $SOS/unit/$p.c ] || { say "$p: no source"; continue; }
  src=$SOS/test/unit/$p.c; [ -f $src ] || src=$SOS/unit/$p.c
  $OSHCC -O1 -I$SOS/include $src -o unit/$p -lm -pthread > unit/$p.build 2>&1 || { say "$p: build failed"; continue; }
  timeout 180 srun -K1 -N4 --ntasks-per-node=2 ./unit/$p > unit/$p.out 2>&1 < /dev/null
  say "$p 4x2 exit $?"
done
say DONE
