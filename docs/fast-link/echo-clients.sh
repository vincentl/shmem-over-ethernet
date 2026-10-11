#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §4: K echo-uring clients at once toward ADDRESS's ports
# 18000 to 18000 + K - 1, each streaming MEBIBYTES a trial, each line of each
# client's output prefixed "connection=I". A client whose connect is refused -
# its server not listening yet - tries again a second later, ten times, so that
# a slow start of the servers' step costs a second and not the whole run; one
# that fails otherwise is reported and not retried.
# usage: echo-clients.sh ECHO_URING K ADDRESS MEBIBYTES
ECHO=$1; K=$2; ADDRESS=$3; MEBIBYTES=$4
for ((i = 0; i < K; i++)); do
    (
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
        echo "$out" | sed "s/^/connection=$i /"
        echo "connection=$i exit $status after $attempt attempts"
    ) &
done
wait
