#!/bin/bash
# v0.2 step 4 part 0: the zero-copy send off and on over loopback, one build,
# alternating: the 64 KiB and 1 MiB put streams with the receiving and sending
# nodes' counters over 5 s of each run. usage: zc-loopback.sh OUT NAME. Takes the lock.
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
OUT=$1; NAME=$2; mkdir -p $OUT; : > $OUT/zc-loopback.txt
S=<repo>/build/scratch/ladder
B=/tmp/ladder/$NAME/build/release; W=/tmp/ladder/$NAME/work; mkdir -p $W
[ -x $W/putstream ] || $B/tools/wrapper/oshcc -O2 -g -o $W/putstream $S/putstream.c || exit 1
for put in 65536 1048576; do
  for i in 1 2 3; do for zc in off on; do
    OSOE_WIRE_ZERO_COPY=$zc $B/tools/launch/osoe-launch -n 2 -m 0,1 -t 120 -- $W/putstream $put 12 > $OUT/run.txt 2>&1 &
    job=$!
    sleep 4
    segs=$($B/tools/stat/osoe-stat -l 2>/dev/null | awk '$1 ~ /osoe\./ {print $1}' | grep -v tombstone)
    $B/tools/stat/osoe-stat -d -j $segs > $OUT/dump0.json 2>/dev/null
    sleep 5
    $B/tools/stat/osoe-stat -d -j $segs > $OUT/dump1.json 2>/dev/null
    wait $job
    python3 - "put=$put round=$i zero_copy=$zc" $OUT/dump0.json $OUT/dump1.json "$(grep '^putstream' $OUT/run.txt)" <<'PY' | tee -a $OUT/zc-loopback.txt
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
for node, (x, y) in enumerate(zip(A, B)):
    for k in ['transport.am.sends', 'transport.am.short_sends', 'transport.am.zero_copy_sends',
              'transport.am.zero_copy_notifications', 'transport.am.zero_copy_copied']:
        u, v = find(x, k), find(y, k)
        if isinstance(u, list): u = sum(u)
        if isinstance(v, list): v = sum(v)
        if isinstance(u, (int, float)) and isinstance(v, (int, float)) and v - u:
            parts.append(f'n{node}.{k.split(".")[-1]}={int(v - u)}')
print(' '.join(parts))
PY
  done; done
done
echo DONE >> $OUT/zc-loopback.txt
