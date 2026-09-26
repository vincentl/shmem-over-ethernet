#!/bin/bash
# F1 phase F: TCP_THIN_LINEAR_TIMEOUTS on osoe's sockets, which keeps a thin
# stream's retransmission timeout from doubling - a bound on a stall's length.
set -u
O=~/pi-results-f1; say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
cd ~/osoe-f1
python3 - <<'PY'
p = 'shmem/src/wire.c'
s = open(p).read()
old = '      if ((e = getenv("OSOE_EXPERIMENT_CC"))) {'
new = '''      if ((e = getenv("OSOE_EXPERIMENT_THIN"))) { int one = 1;
        if (setsockopt(fd, IPPROTO_TCP, TCP_THIN_LINEAR_TIMEOUTS, &one, sizeof one) != 0)
            osoe_fatal("TCP_THIN_LINEAR_TIMEOUTS: %s", strerror(errno)); }
''' + old
if 'OSOE_EXPERIMENT_THIN' not in s:
    assert s.count(old) == 1
    open(p, 'w').write(s.replace(old, new))
PY
grep -c OSOE_EXPERIMENT_THIN shmem/src/wire.c | sed 's/^/thin lines: /' >> $O/summary.txt
cmake --build build/release -j4 --target osoe > $O/build-thin.txt 2>&1; say "thin build $?"
L=~/osoe-f1/build/release/shmem/libosoe.so.0
for cfg in OSOE_NOTHING=1 OSOE_EXPERIMENT_THIN=1 OSOE_NOTHING=1 OSOE_EXPERIMENT_THIN=1; do
  timeout 900 env LD_PRELOAD=$L $cfg srun -K1 -N4 --ntasks-per-node=1 ~/f1probe 28672 32768 > $O/F-$cfg.txt 2>&1 < /dev/null
  say "F $cfg: $(grep block $O/F-$cfg.txt | awk '{print $2":mean_us="$6",slow="$8",max_ms="$10}' | tr '\n' ' ')"
done
say PHASE-F-DONE
