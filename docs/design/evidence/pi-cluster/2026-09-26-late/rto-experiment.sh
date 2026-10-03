#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# F1 experiment (2026-09-26 late): is the 16 KiB fcollect at 4 x 2 slow because
# switch losses each cost the kernel's 200 ms minimum retransmission timeout?
# A scratch copy of main with one env-gated setsockopt(TCP_RTO_MIN_US), swapped
# in under the unchanged OSU binary by LD_LIBRARY_PATH. Outputs in ~/pi-results-rto.
set -u
O=~/pi-results-rto; mkdir -p $O; : > $O/summary.txt
say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
rsync -a --delete --exclude build ~/shmem-over-ethernet/ ~/osoe-rto/
cd ~/osoe-rto
python3 - <<'PY'
p = 'shmem/src/wire.c'
s = open(p).read()
old = "        osoe_fatal(\"cannot set TCP_NODELAY toward node %u: %s\", peer, strerror(errno));\n"
new = old + """#ifndef TCP_RTO_MIN_US
#define TCP_RTO_MIN_US 45   /* linux/tcp.h; glibc's header lacks it */
#endif
#ifdef TCP_RTO_MIN_US
    { const char *e = getenv("OSOE_EXPERIMENT_RTO_MIN_US"); /* EXPERIMENT */
      if (e) { int us = atoi(e);
        if (setsockopt(fd, IPPROTO_TCP, TCP_RTO_MIN_US, &us, sizeof us) != 0)
            osoe_fatal("TCP_RTO_MIN_US: %s", strerror(errno)); } }
#endif
"""
assert s.count(old) == 1
s = s.replace(old, new)
if '#include <stdlib.h>' not in s:
    s = s.replace('#include <string.h>', '#include <stdlib.h>\n#include <string.h>', 1)
open(p, 'w').write(s)
PY
grep -c EXPERIMENT shmem/src/wire.c | sed 's/^/experiment lines: /' | tee -a $O/summary.txt
cmake --preset release > $O/build.txt 2>&1 && cmake --build build/release -j4 --target osoe >> $O/build.txt 2>&1
status=$?; [ $status = 0 ] || { say "build failed"; exit 1; }
say "build exit $?"
L=~/osoe-rto/build/release/shmem
cd ~/osoe-corpus/osu/c/openshmem
srun -N1 -n1 env LD_PRELOAD=$L/libosoe.so.0 ldd ./osu_oshm_fcollect | grep osoe | tee -a $O/summary.txt
timeouts() { srun -N4 --ntasks-per-node=1 sh -c 'echo "$(hostname) $(nstat -az TcpExtTCPTimeouts TcpRetransSegs | awk "NR>1{printf \"%s=%s \", \$1, \$2}")"' 2>/dev/null | sort | tr '\n' ' '; }
run() {  # label, then env
  local label=$1; shift
  local before; before=$(timeouts)
  timeout 300 env LD_PRELOAD=$L/libosoe.so.0 "$@" srun -K1 -N4 --ntasks-per-node=2 --export=ALL ./osu_oshm_fcollect -m 16384:16384 -i 200 > $O/$label.txt 2>&1
  local status=$?
  say "$label exit $status: $(grep -E '^16384' $O/$label.txt | tr -s ' ')"
  echo "$label before: $before" >> $O/counters.txt
  echo "$label after:  $(timeouts)" >> $O/counters.txt
}
run default-a OSOE_NOTHING=1
run rto5ms-a OSOE_EXPERIMENT_RTO_MIN_US=5000
run rto1ms-a OSOE_EXPERIMENT_RTO_MIN_US=1000
run default-b OSOE_NOTHING=1
run rto5ms-b OSOE_EXPERIMENT_RTO_MIN_US=5000
run rto1ms-b OSOE_EXPERIMENT_RTO_MIN_US=1000
say DONE
