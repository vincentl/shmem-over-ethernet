#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §4: K echo-uring servers, on ports 18000 to 18000 + K - 1,
# each until its client sends QUIT; floor.sh ends them if a client never does,
# and a signal to this script ends every server it started.
# usage: echo-servers.sh ECHO_URING K
ECHO=$1; K=$2
trap 'kill $(jobs -p) 2> /dev/null; exit 143' TERM INT
for ((i = 0; i < K; i++)); do
    "$ECHO" --server $((18000 + i)) &
done
wait
