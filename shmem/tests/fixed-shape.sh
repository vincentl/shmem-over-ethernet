#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# OSOE_COLLECTIVE_SHAPE=fixed repeats a run's results bit for bit (T23; the
# README's Limits), whatever the progress helpers and whatever else the
# machine runs:
#
#   fixed-shape.sh LAUNCHER PROGRAM MAP [NAME=VALUE]...
#
# runs PROGRAM (test-fixed-shape.c) under fixed, with the settings given, as
# four separate launches over the node map MAP - at OSOE_PROGRESS_HELPERS=0
# and 2, each with the machine idle and beside a busy loop a core - and
# requires each to succeed and to print one hash, and every hash to be the
# same. Nothing is compared across versions: the promise is this version's.
set -u
LAUNCHER=$1 PROGRAM=$2 MAP=$3
shift 3
ranks=$(printf '%s\n' "$MAP" | tr ',' '\n' | wc -l | tr -d ' ')
cores=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)
loops=""
stop_loops() {
    for pid in $loops; do
        kill "$pid" 2>/dev/null
    done
    loops=""
}
trap stop_loops EXIT
trap 'stop_loops; exit 1' INT TERM
hashes=""
failed=0
launches=0
for load in idle loaded; do
    if [ "$load" = loaded ]; then
        # Each loop ends with this script, should it be killed outright.
        i=0
        while [ "$i" -lt "$cores" ]; do
            (while kill -0 $$ 2>/dev/null; do :; done) &
            loops="$loops $!"
            i=$((i + 1))
        done
    fi
    for helpers in 0 2; do
        out=$(env OSOE_COLLECTIVE_SHAPE=fixed OSOE_PROGRESS_HELPERS=$helpers "$@" \
            "$LAUNCHER" -n "$ranks" -m "$MAP" -t 240 -- "$PROGRAM" 2>&1)
        status=$?
        launches=$((launches + 1))
        printf '%s\n' "$out"
        line=$(printf '%s\n' "$out" | grep '^FIXED SHAPE: hash ')
        hash=$(printf '%s\n' "$line" | sed -n 's/^FIXED SHAPE: hash \([0-9a-f]*\) .*/\1/p')
        echo "fixed-shape.sh: $helpers helpers, $load: exited $status, hash ${hash:-none}"
        if [ "$status" -ne 0 ] || [ "$(printf '%s\n' "$hash" | grep -c .)" -ne 1 ]; then
            failed=$((failed + 1))
        else
            hashes="$hashes $hash"
        fi
    done
    stop_loops
done
distinct=$(printf '%s\n' $hashes | sort -u | grep -c .)
echo "fixed-shape.sh: $launches launches, $failed failed, $distinct distinct hash(es):$hashes"
[ "$failed" -eq 0 ] && [ "$distinct" -eq 1 ] && echo "FIXED SHAPE REPEATS"
