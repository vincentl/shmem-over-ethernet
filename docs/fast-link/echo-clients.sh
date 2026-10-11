#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §4: K echo-uring clients at once toward ADDRESS's ports
# 18000 to 18000 + K - 1, each streaming MEBIBYTES a trial, each line of each
# client's output prefixed "connection=N", N counted from FIRST (0 unless
# given), so that the two directions' connections are told apart. A client
# whose connect is refused - its server not listening yet - tries again a
# second later, ten times, so that a slow start of the servers' step costs a
# second and not the whole run; one that fails otherwise is not retried. The
# script's status is the first failing client's, or 0.
# usage: echo-clients.sh ECHO_URING K ADDRESS MEBIBYTES [FIRST]
ECHO=$1; K=$2; ADDRESS=$3; MEBIBYTES=$4; FIRST=${5:-0}
pids=()
for ((i = 0; i < K; i++)); do
    (
        n=$((FIRST + i))
        for attempt in 1 2 3 4 5 6 7 8 9 10; do
            out=$("$ECHO" --client "$ADDRESS" $((18000 + i)) --rounds 2000 \
                      --mebibytes "$MEBIBYTES" 2>&1)
            status=$?
            case $status:$out in
                0:*) break ;;
                *[Cc]onnect*[Rr]efused*) sleep 1 ;;
                *) break ;;
            esac
        done
        echo "$out" | sed "s/^/connection=$n /"
        echo "connection=$n exit $status after $attempt attempts"
        exit "$status"
    ) &
    pids+=($!)
done
result=0
for p in "${pids[@]}"; do
    wait "$p" || { [ "$result" -ne 0 ] || result=$?; }
done
exit "$result"
