#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T11 and T12, the 0.2 tag's README checks on the Pi cluster, run on its head node in one ssh
# session (RemoveIPC): the quick start as printed on a fresh copy of the tree to be tagged (but the
# apt line, its packages there), both rings checked line by line against the README's four, and
# the probe; then the install, the ring built by the installed oshcc and run by
# srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4, the probe on every node, the job path's
# osoe-pacer --quick, the README's batch script with its secret, and osoe-stat --summary on every
# node of a running job through srun --overlap. Outputs in ~/osoe/build/release-run/readme, the
# tree in its tree/. Adapted from 0.1.0's
# (docs/design/evidence/pi-cluster/2026-10-05-release/readme/pi-readme-remote.sh).
set -u
R=~/osoe/build/release-run/readme; rm -rf $R; mkdir -p $R/tree; cd $R/tree || exit 1
note() { echo "$*" >> $R/result.txt; }
ring_lines() {   # FILE N: the ring's N lines, each PE's box holding the number of the PE before it
  local want; want=$(for ((i = 0; i < $2; i++)); do echo "PE $i of $2: my box holds $(( (i + $2 - 1) % $2 ))"; done | sort)
  if [ "$(grep 'my box holds' "$1" | sort)" = "$want" ]; then echo "the $2 lines right"; else echo "the lines WRONG"; fi
}
: > $R/result.txt
note "host: $(hostname) $(. /etc/os-release; echo $PRETTY_NAME) $(uname -m) $(uname -r); $(tr -d '\0' < /proc/device-tree/model 2>/dev/null); memory $(free -m | awk '/^Mem:/ { print $2 }') MiB, /dev/shm $(df -m /dev/shm | awk 'NR == 2 { print $4 }') MiB free"
tar -xf ~/osoe/build/release-run/tagged.tar
python3 - <<'PY' > ring.c
import re
s = open('README.md').read()
for block in re.findall(r"```c\n(.*?)```", s, re.S):
    if 'int main' in block and 'box' in block:
        print(block, end=''); break
PY
note "ring.c: $(wc -l < ring.c) lines, from the README"
start=$(date +%s); cmake --preset release > $R/qs-configure.txt 2>&1; cmake --build --preset release -j4 > $R/qs-build.txt 2>&1; rc=$?
note "quick start build: $rc in $(( $(date +%s) - start )) s, $(grep -c 'warning:' $R/qs-build.txt) warnings"
note "version: $(build/release/tools/launch/osoe-launch --version 2>&1)"
build/release/tools/wrapper/oshcc -O2 -o ring ring.c; note "ring built: $?"
build/release/tools/launch/osoe-launch -n 4 -- ./ring > $R/ring-1.txt 2>&1; rc=$?
note "ring, four PEs: $rc, $(ring_lines $R/ring-1.txt 4)"
if [ $rc -ne 0 ]; then
    SHMEM_SYMMETRIC_SIZE=64m build/release/tools/launch/osoe-launch -n 4 -- ./ring > $R/ring-1-64m.txt 2>&1
    note "ring, four PEs, SHMEM_SYMMETRIC_SIZE=64m as the README says for a small machine: $?, $(ring_lines $R/ring-1-64m.txt 4)"
fi
build/release/tools/launch/osoe-launch -n 4 -m 0,0,1,1 -- ./ring > $R/ring-2.txt 2>&1; rc=$?
note "ring, two logical nodes: $rc, $(ring_lines $R/ring-2.txt 4)"
build/release/tools/probe/probe > $R/probe.txt 2>&1; note "probe: $? $(grep -c '^ok' $R/probe.txt) ok, $(grep -c -v '^ok' $R/probe.txt) other lines"
cmake --install build/release --prefix $R/prefix > $R/install.txt 2>&1; note "install: $? $(ls $R/prefix/bin | tr '\n' ' ')"
export PATH=$R/prefix/bin:$PATH
cd $R || exit 1
cp tree/ring.c ring.c
oshcc -O2 -o ring-installed ring.c; note "installed oshcc: $?"
srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4 ./ring-installed > ring-srun.txt 2>&1; rc=$?
note "ring under srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4: $rc, $(ring_lines ring-srun.txt 8)"
srun -N4 --ntasks-per-node=1 tree/build/release/tools/probe/probe > probe-every-node.txt 2>&1
note "probe on every node (Getting started, step 2): $? $(grep -c '^ok' probe-every-node.txt) ok, $(grep -c -v '^ok' probe-every-node.txt) other lines"
srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 osoe-pacer --quick > pacer-quick.txt 2>&1
note "the job path, srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4 osoe-pacer --quick (step 4): $? $(tail -1 pacer-quick.txt)"
cat > job.sh <<'EOS'
#!/bin/sh
#SBATCH --nodes=4
#SBATCH --ntasks-per-node=4
export OSOE_JOB_SECRET=$(od -An -tx1 -N16 /dev/urandom | tr -d ' \n')
export OSOE_NO_PROGRESS_TIMEOUT=60      # while the cluster is new
srun -K1 --cpu-bind=none ./ring-installed
EOS
sbatch --wait -o batch.out job.sh > sbatch.txt 2>&1; note "batch script: $?, $(ring_lines batch.out 16)"
# osoe-stat on every node of a running job, as the README prints it, the job a minute of mix.
J=$(sbatch --parsable -N4 --ntasks-per-node=4 -o stat-job.out --wrap "srun -K1 --cpu-bind=none osoe-pacer --seconds 60 mix" 2> sbatch-stat.txt)
for i in $(seq 1 90); do [ "$(squeue -h -j "$J" -o %T 2>/dev/null)" = RUNNING ] && break; sleep 1; done
sleep 20
srun --jobid=$J --overlap -N4 --ntasks-per-node=1 osoe-stat --summary $J > stat.txt 2>&1; rc=$?
note "osoe-stat --summary $J through srun --jobid=$J --overlap -N4 --ntasks-per-node=1: $rc, lines of $(awk '$1 == "node" { print $2 }' stat.txt | sort -u | wc -l) nodes, $(wc -l < stat.txt) lines"
while [ -n "$(squeue -h -j "$J" 2>/dev/null)" ]; do sleep 5; done
note "the job it read: $(tail -1 stat-job.out)"
note "DONE"
