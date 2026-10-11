#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §4: K echo-uring servers, on ports 18000 to 18000 + K - 1,
# each until its client sends QUIT; floor.sh ends them if a client never does.
# usage: echo-servers.sh ECHO_URING K
ECHO=$1; K=$2
for ((i = 0; i < K; i++)); do
    "$ECHO" --server $((18000 + i)) &
done
wait
