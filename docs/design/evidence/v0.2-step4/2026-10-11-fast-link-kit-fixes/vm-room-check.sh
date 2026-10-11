#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine's kit fixes, in the osoe VM: the records' room running out. The
# check probe built again with RECORDS_A_NODE at 2^12 in place of 2^20, so
# that a 64 KiB put stream fills each PE's room within the window; the node's
# figures must then stop at the first PE's last record - peers_window_s short -
# and still equal analyze.py's count over the same records. Under the lock.
# usage: vm-room-check.sh OUTDIR
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=/tmp/kit-fixes
OUT=$1
mkdir -p "$OUT"
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
sed 's/RECORDS_A_NODE = 1 << 20/RECORDS_A_NODE = 1 << 12/' "$T/multistream-check.c" > "$T/multistream-small-room.c"
grep -c 'RECORDS_A_NODE = 1 << 12' "$T/multistream-small-room.c" | sed 's/^/the room changed in lines: /'
"$T/src/build/release/tools/wrapper/oshcc" -O2 -o "$T/multistream-small-room" "$T/multistream-small-room.c" || exit 1
rm -rf "$T/logs/room"; mkdir -p "$T/logs/room"
SHMEM_SYMMETRIC_SIZE=32m MULTISTREAM_LOG="$T/logs/room" \
    "$T/src/build/release/tools/launch/osoe-launch" -n 10 -m 0,0,1,1,2,2,3,3,4,4 -t 120 -- \
    "$T/multistream-small-room" put 65536 4 > "$OUT/room.txt" 2>&1
echo "exit $?"
grep '^multistream node=[0-9]* pes' "$OUT/room.txt"
python3 -I "$HERE/compare.py" "$T/logs/room" "$OUT/room.txt" | tee "$OUT/room.compare.txt"
rm -rf "$T/logs/room"
