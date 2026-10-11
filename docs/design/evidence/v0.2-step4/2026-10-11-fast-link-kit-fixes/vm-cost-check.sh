#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine's kit fixes, in the osoe VM: what keeping the records costs the
# stream. Three probes alternating, ROUNDS rounds, each a four-second window
# on osoe-launch's logical nodes: the kit's multistream as changed (records);
# the same with its record's store taken out (records-off: the extra clock
# read stays); and main's multistream at 7d1d868 (main: its atomic OR into the
# node's first PE each block). The host is shared and loaded, so only a large
# effect could show. Under the lock, a job at a time.
# usage: vm-cost-check.sh OUTDIR ROUNDS
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=/tmp/kit-fixes
OUT=$1; ROUNDS=$2
mkdir -p "$OUT"
exec 9>/tmp/osoe-vm.lock
OSHCC=$T/src/build/release/tools/wrapper/oshcc
L=$T/src/build/release/tools/launch/osoe-launch
flock -w 43200 9 || exit 1
sed 's/            if (recorded < room)$/            if (0 \&\& recorded < room)/' "$T/src/docs/fast-link/multistream.c" > "$T/records-off.c"
grep -c 'if (0 && recorded < room)' "$T/records-off.c" | sed 's/^/records-off: lines changed /'
"$OSHCC" -O2 -o "$T/records-off" "$T/records-off.c" || exit 1
"$OSHCC" -O2 -o "$T/main" "$HERE/multistream-main.c" || exit 1
cp "$T/src/build/release/kit/multistream" "$T/records"
flock -u 9
map() { local m= i j; for ((i = 0; i < $1; i++)); do for ((j = 0; j < $2; j++)); do m=$m${m:+,}$i; done; done; echo "$m"; }
for ((r = 1; r <= ROUNDS; r++)); do
    for layout in "5 2 65536" "8 8 1048576"; do
        set -- $layout
        probes="records records-off main"
        [ $((r % 3)) -eq 2 ] && probes="records-off main records"
        [ $((r % 3)) -eq 0 ] && probes="main records records-off"
        for p in $probes; do
            flock -w 43200 9 || exit 1
            f=$OUT/$p-$1x$2-$3-r$r.txt
            SHMEM_SYMMETRIC_SIZE=32m "$L" -n $(($1 * $2)) -m "$(map "$1" "$2")" -t 120 -- \
                "$T/$p" put "$3" 4 > "$f" 2>&1
            echo "$p $1x$2 $3 r$r exit $? $(grep '^multistream job' "$f" | grep -o 'mib_per_s=[0-9.]*' | tail -1) load $(cut -d' ' -f1 /proc/loadavg)"
            flock -u 9
        done
    done
done
