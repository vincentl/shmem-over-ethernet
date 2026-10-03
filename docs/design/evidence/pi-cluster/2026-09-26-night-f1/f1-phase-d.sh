#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# F1 phase D: plain TCP with osoe's exchange; node subsets with osoe.
set -u
O=~/pi-results-f1; say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
until grep -q DONE $O/summary.txt; do sleep 10; done
for b in 16384 24576 32768 65536; do
  timeout 600 srun -K1 -N4 --ntasks-per-node=1 python3 ~/tcp-bruck.py $b 400 > $O/D-tcp-$b.txt 2>&1
  say "D tcp 4 nodes: $(cat $O/D-tcp-$b.txt | tr '\n' ' ')"
done
for b in 32768; do
  timeout 600 srun -K1 -N4 --ntasks-per-node=1 python3 ~/tcp-bruck.py $b 1500 > $O/D-tcp-long-$b.txt 2>&1
  say "D tcp 4 nodes long: $(cat $O/D-tcp-long-$b.txt | tr '\n' ' ')"
done
for set in pi-[001-003] pi-[001-002,004] pi-[001,003-004] pi-[002-004]; do
  timeout 900 srun -K1 -w $set -N3 --ntasks-per-node=1 ~/f1probe 24576 32768 36864 49152 > $O/D-osoe-$set.txt 2>&1 < /dev/null
  say "D osoe 3 nodes $set:"; sed 's/^/    /' $O/D-osoe-$set.txt | grep block | tee -a $O/summary.txt > /dev/null
done
say PHASE-D-DONE
