#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine's kit fixes, in the osoe VM: S1's fix checked. Each case is one
# job of the check probe (the kit's multistream with its records also written
# to a log a PE, make-check-probe.py) on osoe-launch's logical nodes, a
# four-second window as the S1 refuter's; then compare.py sets each node's
# swept figures beside analyze.py's offline count from the same records. The
# kit's own multistream runs once beside them, at 8 x 8. Under the VM's lock,
# one job at a time; the logs are packed beside each job's output.
#   CASE: name probe nodes pes mode bytes [NAME=VALUE...]
# usage: vm-multistream-check.sh OUTDIR CASEFILE
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
T=/tmp/kit-fixes
OUT=$1; CASES=$2
mkdir -p "$OUT"
LOG=$OUT/run.txt
L=$T/src/build/release/tools/launch/osoe-launch
exec 9>/tmp/osoe-vm.lock
map() {   # NODES PES
    local m= i j
    for ((i = 0; i < $1; i++)); do for ((j = 0; j < $2; j++)); do m=$m${m:+,}$i; done; done
    echo "$m"
}
echo "start $(date -u +%FT%TZ) cases $CASES" >> "$LOG"
while read -r name probe nodes pes mode bytes settings; do
    case $name in ''|'#'*) continue ;; esac
    flock -w 43200 9 || { echo "no lock" >> "$LOG"; exit 1; }
    f=$OUT/$name.txt
    logs=$T/logs/$name
    rm -rf "$logs"; mkdir -p "$logs"
    echo "-- $name $(date -u +%H:%M:%S) load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$LOG"
    # shellcheck disable=SC2086
    env SHMEM_SYMMETRIC_SIZE=32m MULTISTREAM_LOG="$logs" $settings \
        "$L" -n $((nodes * pes)) -m "$(map "$nodes" "$pes")" -t 120 -- \
        "$T/src/build/release/kit/$probe" "$mode" "$bytes" 4 > "$f" 2>&1
    status=$?
    echo "   exit $status settings: ${settings:-none}" >> "$LOG"
    grep -E '^multistream (node=[0-9]+ pes|job)' "$f" | sed 's/^/   /' >> "$LOG"
    if [ "$probe" = multistream-check ] && [ "$status" -eq 0 ]; then
        python3 -I "$HERE/compare.py" "$logs" "$f" > "$OUT/$name.compare.txt" 2>&1
        sed 's/^/   /' "$OUT/$name.compare.txt" >> "$LOG"
        tar -C "$T/logs" -czf "$OUT/$name.logs.tar.gz" "$name"
    fi
    rm -rf "$logs"
    flock -u 9
done < "$CASES"
echo "DONE $(date -u +%FT%TZ)" >> "$LOG"
