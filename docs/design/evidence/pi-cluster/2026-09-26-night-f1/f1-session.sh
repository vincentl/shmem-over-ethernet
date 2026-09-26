#!/bin/bash
# F1, 2026-09-26 night: map the trigger, find the losing flow, try socket
# knobs that shape bursts without shrinking the chunk. Main's library for A
# and B; C swaps in ~/osoe-f1, main plus env-gated setsockopt calls, by
# LD_PRELOAD. Outputs in ~/pi-results-f1.
set -u
O=~/pi-results-f1; mkdir -p $O; : > $O/summary.txt
say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
R=~/shmem-over-ethernet
OSHCC=$R/build/release/tools/wrapper/oshcc
$OSHCC -O2 ~/f1probe.c -o ~/f1probe > $O/probe-build.txt 2>&1; say "probe build $?"
cat /proc/sys/net/ipv4/tcp_available_congestion_control /proc/sys/net/ipv4/tcp_allowed_congestion_control > $O/cc.txt
probe() {  # label, shape, blocks...
  local label=$1 shape=$2; shift 2
  timeout 900 srun -K1 $shape ~/f1probe "$@" > $O/$label.txt 2>&1 < /dev/null
  say "$label exit $?"; sed 's/^/    /' $O/$label.txt | grep block | tee -a $O/summary.txt > /dev/null
}
# A: where the trigger is.
probe A-4x1 "-N4 --ntasks-per-node=1" 16384 20480 24576 28672 32768 36864 40960 49152 57344 65536
probe A-4x2 "-N4 --ntasks-per-node=2" 8192 12288 16384 20480 24576 32768
probe A-4x4 "-N4 --ntasks-per-node=4" 4096 6144 8192 12288 16384
# B: which flow retransmits, 4x1 at 32 KiB, every node's sockets sampled.
( srun -N4 --ntasks-per-node=1 sh -c 'for i in $(seq 60); do echo "== $(hostname) $(date +%s.%N)"; ss -tinH state established; sleep 0.5; done' > $O/B-ss.txt 2>&1 ) &
sleep 1
probe B-4x1-32k "-N4 --ntasks-per-node=1" 32768 32768
wait
# C: the knobs.
rsync -a --delete --exclude build $R/ ~/osoe-f1/
cd ~/osoe-f1
python3 - <<'PY'
p = 'shmem/src/wire.c'
s = open(p).read()
old = "        osoe_fatal(\"cannot set TCP_NODELAY toward node %u: %s\", peer, strerror(errno));\n"
new = old + """    { /* EXPERIMENT: burst shaping for F1 */
      const char *e;
      if ((e = getenv("OSOE_EXPERIMENT_PACING"))) { unsigned long r = strtoul(e, NULL, 0);
        if (setsockopt(fd, SOL_SOCKET, SO_MAX_PACING_RATE, &r, sizeof r) != 0)
            osoe_fatal("SO_MAX_PACING_RATE: %s", strerror(errno)); }
      if ((e = getenv("OSOE_EXPERIMENT_SNDBUF"))) { int b = atoi(e);
        if (setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &b, sizeof b) != 0)
            osoe_fatal("SO_SNDBUF: %s", strerror(errno)); }
      if ((e = getenv("OSOE_EXPERIMENT_CC"))) {
        if (setsockopt(fd, IPPROTO_TCP, TCP_CONGESTION, e, strlen(e)) != 0)
            osoe_fatal("TCP_CONGESTION %s: %s", e, strerror(errno)); }
    }
"""
assert s.count(old) == 1
open(p, 'w').write(s.replace(old, new))
PY
grep -c EXPERIMENT shmem/src/wire.c >> $O/summary.txt
cmake --preset release > $O/build.txt 2>&1 && cmake --build build/release -j4 --target osoe >> $O/build.txt 2>&1
say "experimental build $?"
L=~/osoe-f1/build/release/shmem/libosoe.so.0
B=$R/build/release/tools/bench/osoe-bench
knob() {  # label, env...
  local label=$1; shift
  timeout 900 env LD_PRELOAD=$L "$@" srun -K1 -N4 --ntasks-per-node=1 ~/f1probe 32768 > $O/C-$label-4x1.txt 2>&1 < /dev/null
  say "C $label 4x1: $(grep block $O/C-$label-4x1.txt)"
  timeout 900 env LD_PRELOAD=$L "$@" srun -K1 -N4 --ntasks-per-node=2 ~/f1probe 16384 > $O/C-$label-4x2.txt 2>&1 < /dev/null
  say "C $label 4x2: $(grep block $O/C-$label-4x2.txt)"
  timeout 900 env LD_PRELOAD=$L "$@" srun -K1 -N2 --ntasks-per-node=1 -c4 $B latency bandwidth > $O/C-$label-bench.txt 2>&1 < /dev/null
  say "C $label bench: $(grep -E 'latency get .*bytes=(262144|1048576)|bandwidth put .*bytes=1048576' $O/C-$label-bench.txt | sed -E 's/latency |target=1 where=remote |fastest_us=[0-9.]+//g' | tr -s ' ' | tr '\n' ';')"
}
knob none OSOE_NOTHING=1
knob pace100 OSOE_EXPERIMENT_PACING=100000000
knob pace60 OSOE_EXPERIMENT_PACING=60000000
knob sndbuf64k OSOE_EXPERIMENT_SNDBUF=65536
knob sndbuf32k OSOE_EXPERIMENT_SNDBUF=32768
knob reno OSOE_EXPERIMENT_CC=reno
knob chunk16k OSOE_AM_CHUNK=16384
say DONE
