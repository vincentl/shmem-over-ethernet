#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The experiment's grid: turnover-herd under each variant (LABEL:NAME=VALUE,...), their order
# rotated each series, in the diagnostic copy.
#   variants.sh OUT SERIES SECONDS "WRITERS" "BLOCKS" VARIANT...
set -u
OUT=$1 SERIES=$2 SECONDS_EACH=$3 WRITERS=$4 BLOCKS=$5
shift 5
VARIANTS=("$@")
T=/var/tmp/hand-on-diag
exec 9>/tmp/osoe-vm.lock
flock -w 3600 9 || exit 99
echo "# $(date -u +%FT%TZ) variants ${VARIANTS[*]}; load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$OUT"
n=${#VARIANTS[@]}
for ((s = 1; s <= SERIES; s++)); do
    for w in $WRITERS; do
        map=$(printf '0,%.0s' $(seq 1 "$w"))1
        for b in $BLOCKS; do
            for ((j = 0; j < n; j++)); do
                v=${VARIANTS[$(( (j + s - 1) % n ))]}
                label=${v%%:*}
                IFS=, read -r -a envs <<< "${v#*:}"
                line=$(env SHMEM_SYMMETRIC_SIZE=4m "${envs[@]}" $T/build/release/tools/launch/osoe-launch \
                       -n $((w + 1)) -m "$map" -t 120 -- $T/turnover-herd "$b" "$SECONDS_EACH" 2>&1)
                status=$?
                herd=$(echo "$line" | grep '^herd ')
                echo "series=$s exit=$status variant=$label diag_lines=$(echo "$line" | grep -c osoe-diag) ${herd:-no herd line}" >> "$OUT"
                echo "$line" | grep osoe-diag | sed "s/^/series=$s variant=$label writers=$w bytes=$b: /" >> "$OUT.diag"
            done
        done
    done
done
echo "# $(date -u +%T) done" >> "$OUT"
