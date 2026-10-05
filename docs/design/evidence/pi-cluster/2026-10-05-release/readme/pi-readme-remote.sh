#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The release checklist's README items on the Pi cluster, run on its head node:
# the quick start as printed on a fresh copy of the tagged tree (but the apt
# line, its packages there), the install, the ring built by the installed oshcc
# and run by srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4, and the README's
# batch script with its secret. Everything under ~/osoe/build/release-run/readme.
set -u
R=~/osoe/build/release-run/readme; rm -rf $R; mkdir -p $R; cd $R
note() { echo "$*" >> $R/result.txt; }
: > $R/result.txt
note "host: $(hostname) $(. /etc/os-release; echo $PRETTY_NAME) $(uname -m) $(uname -r)"
tar -xf ~/osoe/build/release-run/tagged.tar
python3 - <<'PY' > ring.c
import re
s=open('README.md').read()
for block in re.findall(r"```c\n(.*?)```", s, re.S):
    if 'int main' in block and 'box' in block:
        print(block, end=''); break
PY
note "ring.c: $(wc -l < ring.c) lines"
start=$(date +%s); cmake --preset release > qs-configure.txt 2>&1; cmake --build --preset release -j4 > qs-build.txt 2>&1; rc=$?
note "quick start build: $rc in $(( $(date +%s) - start )) s, $(grep -c 'warning:' qs-build.txt) warnings"
build/release/tools/wrapper/oshcc -O2 -o ring ring.c; note "ring built: $?"
build/release/tools/launch/osoe-launch -n 4 -- ./ring > ring-1.txt 2>&1; note "ring, four PEs: $? $(grep -c 'my box holds' ring-1.txt) lines"
build/release/tools/launch/osoe-launch -n 4 -m 0,0,1,1 -- ./ring > ring-2.txt 2>&1; note "ring, two logical nodes: $? $(grep -c 'my box holds' ring-2.txt) lines"
build/release/tools/probe/probe > probe.txt 2>&1; note "probe: $? $(grep -c '^ok' probe.txt) ok"
cmake --install build/release --prefix $R/prefix > install.txt 2>&1; note "install: $? $(ls $R/prefix/bin | tr '\n' ' ')"
export PATH=$R/prefix/bin:$PATH
oshcc -O2 -o ring-installed ring.c; note "installed oshcc: $?"
srun -K1 --cpu-bind=none -N2 --ntasks-per-node=4 ./ring-installed > ring-srun.txt 2>&1; note "ring under srun -N2 x 4: $? $(grep -c 'my box holds' ring-srun.txt) lines"
cat > job.sh <<'EOS'
#!/bin/sh
#SBATCH --nodes=4
#SBATCH --ntasks-per-node=4
export OSOE_JOB_SECRET=$(od -An -tx1 -N16 /dev/urandom | tr -d ' \n')
export OSOE_NO_PROGRESS_TIMEOUT=60      # while the cluster is new
srun -K1 --cpu-bind=none ./ring-installed
EOS
sbatch --wait -o batch.out job.sh > sbatch.txt 2>&1; note "batch script: $? $(grep -c 'my box holds' batch.out) lines"
note "DONE"
