#!/bin/bash
# Step 8 slice 3's before: osoe-bench's threads line on two Pis, three runs,
# on the tree in ~/osoe-slice3 (the bench added on slice 2's library).
set -u
O=~/pi-results-threads-bench-${1:-before}; mkdir -p $O; : > $O/summary.txt
cd ~/osoe-slice3
cmake --preset release > $O/build.txt 2>&1 && cmake --build build/release -j4 --target osoe-bench >> $O/build.txt 2>&1
echo "build exit $?" >> $O/summary.txt
for i in 1 2 3; do
  timeout 300 srun -K1 -N2 --ntasks-per-node=1 -c4 build/release/tools/bench/osoe-bench threads 2>&1 | grep -E "^threads|osoe:" >> $O/summary.txt
done
echo DONE >> $O/summary.txt
