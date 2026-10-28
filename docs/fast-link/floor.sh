#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §4: the floor and the bare stream, between the list's first
# two nodes. shmem_test_floor, one PE a node (design §8.4); then CONNECTIONS
# ("1 2 4 8 16") echo-uring streams at once each way - servers on both nodes,
# each node's clients toward the other's - each stream ECHO_MEBIBYTES (1024) a
# trial; and iperf3 with as many streams each way at once (--bidir), whose sum
# over its ten seconds is the bare aggregate the helpers' table is set against
# (§5). echo-uring's per-connection medians, summed, are an upper bound beside
# it: nothing checks that its streams ran at once. The servers, the clients and
# iperf3 are each a job of one task (on and spawn, common.sh), unbound: the
# node's every core may serve them, since slurm-setup.sh's TaskPlugin is
# task/none. PARTS ("floor echo-uring iperf3") names which of the three run.
# echo-uring is a fault of the kit's on the cluster: in both sessions, at 1, 2
# and 16 connections, the clients toward the first node met the 300 s timeout
# (exit 124) while the other direction finished - 15 of the second session's
# 17 minutes here, 25 of the first's 26 - and no result depended on it; not
# fixed, and the third session runs PARTS=iperf3 (docs/decisions/2026-10-27.md,
# the author's second answer).
# usage: floor.sh NODES OUTDIR
: "${KIT_TIMEOUT:=300}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
CONNECTIONS=${CONNECTIONS:-1 2 4 8 16}
PARTS=${PARTS:-floor echo-uring iperf3}
part() { case " $PARTS " in *" $1 "*) return 0 ;; esac; return 1; }
ECHO_MEBIBYTES=${ECHO_MEBIBYTES:-1024}
SERVER=$(node 1)
CLIENT=$(node 2)
[ -n "$CLIENT" ] || { echo "floor.sh needs two nodes" >&2; exit 2; }
ECHO=$KIT_BUILD/tools/echo-uring/echo-uring
# On one machine both ends are this machine, at its loopback address.
[ "$KIT_LAUNCHER" = osoe-launch ] && SERVER_ADDRESS=127.0.0.1 || SERVER_ADDRESS=$SERVER

# The one task a node every core of it, as docs/pi-cluster.md §5 gives the Pi's:
# its main thread, its agent and the floor's echo would otherwise share one.
part floor && KIT_SALLOC="$KIT_SALLOC $(pair)" KIT_SRUN="$KIT_SRUN --cpus-per-task=${FLOOR_CPUS:-4}" \
    launch 2 1 "$OUT/floor.txt" -- "$KIT_BUILD/shmem/tests/shmem_test_floor"

# echo-uring's servers on both nodes, one a port; each node's clients toward the
# other's servers, both ways at once; each prints its own lines, summed below by
# summarize.py. Servers whose client never came are ended once the clients are.
CLIENT_ADDRESS=$CLIENT
[ "$KIT_LAUNCHER" = osoe-launch ] && CLIENT_ADDRESS=127.0.0.1
end_after() {   # PID SECONDS: wait for PID, ending it after SECONDS
    ( sleep "$2"; kill "$1" 2> /dev/null ) &
    local guard=$!
    wait "$1"
    kill "$guard" 2> /dev/null
}
ECHO_CONNECTIONS=$CONNECTIONS
part echo-uring || ECHO_CONNECTIONS=
for k in $ECHO_CONNECTIONS; do
    f=$OUT/echo-$k.txt
    echo "# $(date -u +%H:%M:%S) $k connections each way between $CLIENT and $SERVER" > "$f"
    spawn "$SERVER" "$OUT/echo-$k-servers-1.txt" bash "$KIT/echo-servers.sh" "$ECHO" "$k"
    servers1=$!
    if [ "$KIT_LAUNCHER" != osoe-launch ]; then
        spawn "$CLIENT" "$OUT/echo-$k-servers-2.txt" bash "$KIT/echo-servers.sh" "$ECHO" "$k"
        servers2=$!
    fi
    # The second direction's connections numbered from k, so that summarize.py
    # sums all 2k of them.
    spawn "$CLIENT" "$f.to-first" bash "$KIT/echo-clients.sh" "$ECHO" "$k" "$SERVER_ADDRESS" \
        "$ECHO_MEBIBYTES" 0
    clients1=$!
    if [ "$KIT_LAUNCHER" != osoe-launch ]; then
        spawn "$SERVER" "$f.to-second" bash "$KIT/echo-clients.sh" "$ECHO" "$k" \
            "$CLIENT_ADDRESS" "$ECHO_MEBIBYTES" "$k"
        clients2=$!
    fi
    wait "$clients1"; echo "# exit toward the first node $?" >> "$f"
    [ -n "${clients2:-}" ] && { wait "$clients2"; echo "# exit toward the second node $?" >> "$f"; }
    cat "$f.to-first" ${clients2:+"$f.to-second"} >> "$f"
    rm -f "$f.to-first" "$f.to-second"
    end_after "$servers1" 30
    [ -n "${servers2:-}" ] && end_after "$servers2" 30
    unset clients2 servers2
    echo "$(date -u +%H:%M:%S) echo-uring $k connections each way done" >> "$LOG"
done

if part iperf3 && command -v iperf3 > /dev/null; then
    for k in $CONNECTIONS; do
        spawn "$SERVER" "$OUT/iperf3-$k-server.txt" iperf3 -s -1 -p 5201
        server=$!
        # The JSON alone in its file: srun's own messages would spoil it. A
        # client the server was not yet listening for tries again.
        for attempt in 1 2 3 4 5 6 7 8 9 10; do
            sleep 1
            on "$CLIENT" iperf3 -c "$SERVER_ADDRESS" -p 5201 -P "$k" -t 10 --bidir -J \
                > "$OUT/iperf3-$k.json" 2> "$OUT/iperf3-$k-client.txt" && break
            grep -q 'unable to connect' "$OUT/iperf3-$k.json" "$OUT/iperf3-$k-client.txt" || break
        done
        end_after "$server" 30
        echo "$(date -u +%H:%M:%S) iperf3 $k streams each way done" >> "$LOG"
    done
elif part iperf3; then
    echo "iperf3 is not installed: no cross-check" >> "$LOG"
fi
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
