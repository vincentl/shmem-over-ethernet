#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# v0.2 step 4 part 0: the provided buffers' size and count (OSOE_WIRE_BYTES,
# OSOE_WIRE_BUFFERS) under a put stream, alternating, with the receiving node's
# ring counters over 5 s of each run (osoe-stat -d -j, two dumps).
# usage: buffers-sweep.sh OUT NAME ROUNDS BYTES CONFIG... (CONFIG = size x count)
exec 9>/tmp/osoe-vm.lock; flock -w 10800 9 || exit 1
OUT=$1; NAME=$2; ROUNDS=$3; PUT=$4; shift 4; mkdir -p $OUT
S=<repo>/build/scratch/ladder
B=/tmp/ladder/$NAME/build/release; W=/tmp/ladder/$NAME/work; mkdir -p $W
[ -x $W/putstream ] || $B/tools/wrapper/oshcc -O2 -g -o $W/putstream $S/putstream.c || exit 1
L=$B/tools/launch/osoe-launch; STAT=$B/tools/stat/osoe-stat
for i in $(seq $ROUNDS); do for c in "$@"; do
  size=${c%x*}; count=${c#*x}; tag="put=$PUT round=$i bytes=$size buffers=$count"
  OSOE_WIRE_BYTES=$size OSOE_WIRE_BUFFERS=$count $L -n 2 -m 0,1 -t 120 -- $W/putstream $PUT 12 > $OUT/run.txt 2>&1 &
  job=$!
  sleep 4
  segs=$($STAT -l 2>/dev/null | awk '$1 ~ /osoe\./ {print $1}' | grep -v tombstone)
  $STAT -d -j $segs > $OUT/dump0.json 2>/dev/null
  sleep 5
  $STAT -d -j $segs > $OUT/dump1.json 2>/dev/null
  wait $job
  python3 - "$tag" $OUT/dump0.json $OUT/dump1.json "$(grep '^putstream' $OUT/run.txt)" <<'PY' | tee -a $OUT/sweep.txt
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
    for k in ['transport.am.completions', 'transport.am.receive_buffers', 'transport.am.receive_enobufs',
              'transport.am.receive_rearms', 'transport.am.passes', 'transport.am.sends', 'transport.am.short_sends']:
        u, v = find(x, k), find(y, k)
        if isinstance(u, list): u = sum(u)
        if isinstance(v, list): v = sum(v)
        if isinstance(u, (int, float)) and isinstance(v, (int, float)):
            parts.append(f'n{node}.{k.split(".")[-1]}={int(v - u)}')
print(' '.join(parts))
PY
done; done
echo DONE >> $OUT/sweep.txt
