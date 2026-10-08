#!/bin/bash
# v0.2 step 4 part 0, the fourth rung: the third adversarial read's N3, two builds
# alternating - (1) the 1 MiB stream, eight more pairs; (2) the receiving ring's
# counters at the defaults, 64 KiB and 1 MiB; (3) the 64 KiB stream through small
# provided buffers (1 KiB x 1024, 4 KiB x 256); (4) a stream of 64-byte puts.
# Each line: the rate, and node 1's ring counters over 5 s of the run.
# usage: rung-extra.sh OUT BASE RUNG. Takes the VM lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
OUT=$1; BASE=$2; RUNG=$3; mkdir -p $OUT; : > $OUT/extra.txt
S=/Users/vincent/shmem-over-ethernet/build/scratch/ladder
for b in $BASE $RUNG; do
  W=/tmp/ladder/$b/work; mkdir -p $W
  [ -x $W/putstream ] || /tmp/ladder/$b/build/release/tools/wrapper/oshcc -O2 -g -o $W/putstream $S/putstream.c || exit 1
done
one() {   # label build put size count
  local label=$1 b=$2 put=$3 size=$4 count=$5
  local B=/tmp/ladder/$b/build/release
  OSOE_WIRE_BYTES=$size OSOE_WIRE_BUFFERS=$count $B/tools/launch/osoe-launch -n 2 -m 0,1 -t 120 -- /tmp/ladder/$b/work/putstream $put 12 > $OUT/run.txt 2>&1 &
  local job=$!
  sleep 4
  local segs; segs=$($B/tools/stat/osoe-stat -l 2>/dev/null | awk '$1 ~ /osoe\./ {print $1}' | grep -v tombstone)
  $B/tools/stat/osoe-stat -d -j $segs > $OUT/dump0.json 2>/dev/null
  sleep 5
  $B/tools/stat/osoe-stat -d -j $segs > $OUT/dump1.json 2>/dev/null
  wait $job
  python3 - "$label build=$b put=$put buffers=${size}x$count" $OUT/dump0.json $OUT/dump1.json "$(grep '^putstream' $OUT/run.txt)" <<'PY' | tee -a $OUT/extra.txt
import json, sys
tag, a, b, line = sys.argv[1:5]
def load(fn):
    out = []
    for l in open(fn):
        l = l.strip()
        if l:
            try: out.append(json.loads(l))
            except Exception: pass
    return out
def find(o, key):
    if isinstance(o, dict):
        for k, v in o.items():
            if k == key: return v
            r = find(v, key)
            if r is not None: return r
    elif isinstance(o, list):
        for v in o:
            r = find(v, key)
            if r is not None: return r
    return None
A, B = load(a), load(b)
rate = line.split('mib_per_s=')[-1] if 'mib_per_s=' in line else '?'
parts = [tag, 'mib_per_s=' + rate]
if len(A) > 1 and len(B) > 1:
    for k in ['transport.am.completions', 'transport.am.receive_buffers', 'transport.am.receive_enobufs',
              'transport.am.receive_rearms', 'transport.am.passes']:
        u, v = find(A[1], k), find(B[1], k)
        if isinstance(u, list): u = sum(u)
        if isinstance(v, list): v = sum(v)
        if isinstance(u, (int, float)) and isinstance(v, (int, float)):
            parts.append(f'{k.split(".")[-1]}={int(v - u)}')
print(' '.join(parts))
PY
}
echo "== (1) the 1 MiB stream, eight pairs" >> $OUT/extra.txt
for i in 1 2 3 4 5 6 7 8; do for b in $BASE $RUNG; do one "round=$i" $b 1048576 16384 64; done; done
echo "== (2) the ring's counters at the defaults, 64 KiB puts" >> $OUT/extra.txt
for i in 1 2; do for b in $BASE $RUNG; do one "round=$i" $b 65536 16384 64; done; done
echo "== (3) the 64 KiB stream through small provided buffers" >> $OUT/extra.txt
for i in 1 2; do for c in 1024x1024 4096x256; do for b in $BASE $RUNG; do one "round=$i" $b 65536 ${c%x*} ${c#*x}; done; done; done
echo "== (4) a stream of 64-byte puts" >> $OUT/extra.txt
for i in 1 2 3; do for b in $BASE $RUNG; do one "round=$i" $b 64 16384 64; done; done
echo DONE >> $OUT/extra.txt
