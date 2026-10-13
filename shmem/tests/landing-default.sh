#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Each PE's landing zone for its gets, as its SHMEM_DEBUG line at start-up
# shows it (P32; am-configuration.h, osoe_get_landing_default_bytes): by
# default its node's 4 MiB split among the node's PEs, in whole 1 KiB extents,
# and no less than 256 KiB nor more than 1 MiB a PE - each line's own count of
# its node's PEs, "(local i of L)", saying what that PE's zone must be, so that
# the nodes of one job may differ - or BYTES on every PE where
# OSOE_GET_LANDING_BYTES sets it.
#
#   landing-default.sh LAUNCHER PROGRAM MAP [BYTES]
#
# PROGRAM is any program that initializes, once, and finishes.
set -u
LAUNCHER=$1 PROGRAM=$2 MAP=$3 BYTES=${4:-}
ranks=$(printf '%s\n' "$MAP" | tr ',' '\n' | wc -l | tr -d ' ')
if [ -n "$BYTES" ]; then
    export OSOE_GET_LANDING_BYTES="$BYTES"
else
    unset OSOE_GET_LANDING_BYTES
fi
out=$(SHMEM_DEBUG=1 "$LAUNCHER" -n "$ranks" -m "$MAP" -t 60 -- "$PROGRAM" 2>&1)
status=$?
printf '%s\n' "$out"
echo "launcher exited $status"
[ "$status" -eq 0 ] || exit 1
lines=$(printf '%s\n' "$out" | grep '^osoe: PE [0-9]* of [0-9]* (local')
count=$(printf '%s\n' "$lines" | grep -c .)
[ "$count" -eq "$ranks" ] || { echo "$count start-up lines for $ranks PEs"; exit 1; }
printf '%s\n' "$lines" | {
    wrong=0
    while IFS= read -r line; do
        pes=$(printf '%s\n' "$line" |
              sed -n 's/^osoe: PE [0-9]* of [0-9]* (local [0-9]* of \([0-9]*\)).*/\1/p')
        zone=$(printf '%s\n' "$line" | sed -n 's/.*, landing zone \([0-9]*\) bytes,.*/\1/p')
        if [ -n "$BYTES" ]; then
            due=$BYTES
        else
            due=$((4194304 / pes / 1024 * 1024))
            [ "$due" -lt 262144 ] && due=262144
            [ "$due" -gt 1048576 ] && due=1048576
        fi
        echo "a PE of a node of $pes: a zone of ${zone:-no} bytes, $due due"
        [ "$zone" = "$due" ] || wrong=$((wrong + 1))
    done
    [ "$wrong" -eq 0 ] && echo "LANDING OK"
}
