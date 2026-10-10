#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §4: the floor and the bare stream, between the list's first
# two nodes. shmem_test_floor, one PE a node (design §8.4); echo-uring from the
# second node to the first, one connection; then CONNECTIONS ("1 2 4 8 16")
# pairs of echo-uring at once, each its own server port, each stream
# ECHO_MEBIBYTES (1024) a trial, their rates summed; and iperf3 with as many
# streams where it is installed.
# usage: floor.sh NODES OUTDIR
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
CONNECTIONS=${CONNECTIONS:-1 2 4 8 16}
ECHO_MEBIBYTES=${ECHO_MEBIBYTES:-1024}
SERVER=$(node 1)
CLIENT=$(node 2)
[ -n "$CLIENT" ] || { echo "floor.sh needs two nodes" >&2; exit 2; }
ECHO=$KIT_BUILD/tools/echo-uring/echo-uring
# On one machine both ends are this machine, at its loopback address.
[ "$KIT_LAUNCHER" = osoe-launch ] && SERVER_ADDRESS=127.0.0.1 || SERVER_ADDRESS=$SERVER

# The one task a node every core of it, as docs/pi-cluster.md §5 gives the Pi's:
# its main thread, its agent and the floor's echo would otherwise share one.
KIT_SRUN="$KIT_SRUN --cpus-per-task=${FLOOR_CPUS:-4}" \
    launch 2 1 "$OUT/floor.txt" -- "$KIT_BUILD/shmem/tests/shmem_test_floor"

# echo-uring's servers on the first node, one a port; its clients on the second,
# all at once; each prints its own lines, summed below by summarize.py.
for k in $CONNECTIONS; do
    f=$OUT/echo-$k.txt
    echo "# $(date -u +%H:%M:%S) $k connections from $CLIENT to $SERVER" > "$f"
    on "$SERVER" bash -c "for ((i = 0; i < $k; i++)); do $ECHO --server \$((18000 + i)) & done; wait" \
        > "$OUT/echo-$k-servers.txt" 2>&1 &
    servers=$!
    sleep 2
    on "$CLIENT" bash -c "for ((i = 0; i < $k; i++)); do
            stdbuf -oL $ECHO --client $SERVER_ADDRESS \$((18000 + i)) --rounds 2000 --mebibytes $ECHO_MEBIBYTES \
                | sed \"s/^/connection=\$i /\" &
        done; wait" >> "$f" 2>&1
    echo "# exit $?" >> "$f"
    wait "$servers"
    echo "$(date -u +%H:%M:%S) echo-uring $k connections done" >> "$LOG"
done

if command -v iperf3 > /dev/null; then
    for k in $CONNECTIONS; do
        on "$SERVER" iperf3 -s -1 -p 5201 > "$OUT/iperf3-$k-server.txt" 2>&1 &
        server=$!
        sleep 2
        on "$CLIENT" iperf3 -c "$SERVER_ADDRESS" -p 5201 -P "$k" -t 10 -J > "$OUT/iperf3-$k.json" 2>&1
        wait "$server"
        echo "$(date -u +%H:%M:%S) iperf3 $k streams done" >> "$LOG"
    done
else
    echo "iperf3 is not installed: no cross-check" >> "$LOG"
fi
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
