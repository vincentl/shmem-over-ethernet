#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# P1 on the Pi cluster under srun -K1, the library before P1 (~/osoe, main at cbfaa7e1) against the
# library after (P1's tree, ~/osoe/build/p1-tree): test-global-exit.c's all-callers - every PE
# calling shmem_global_exit at once with 7 + its rank, each with a handler a second long - on two
# nodes and four, two PEs a node; and its two-nodes - PE 1 and PE 2, on two nodes, PE 2 100 ms
# later, node 0's goodbye to node 1 held half a second, PE 2's handler a second long - RUNS times
# each. One source, compiled by each build's oshcc. Each run's srun status, the tasks srun says it
# terminated, and the handlers begun and finished, then a summary of each.
#   pi-srun.sh [RUNS]          - on the head node, in one ssh session held for the whole run
set -u
RUNS=${1:-10}
OLD=~/osoe/build/release
NEW=~/osoe/build/p1-tree/build/release
SRC=~/osoe/build/p1-tree/shmem/tests/test-global-exit.c
O=~/osoe/build/p1-pi
mkdir -p "$O"
"$OLD/tools/wrapper/oshcc" -O2 -o "$O/ge-old" "$SRC" || exit 3
"$NEW/tools/wrapper/oshcc" -O2 -o "$O/ge-new" "$SRC" || exit 3
LINES=$O/runs.txt
: > "$LINES"
offload=$(srun -N4 --ntasks-per-node=1 sh -c 'echo "$(hostname)=$(/usr/sbin/ethtool -k eth0 | awk "/^tcp-segmentation-offload:|^generic-segmentation-offload:/ {printf \"%s\", \$2 == \"on\" ? \"1\" : \"0\"}")"' 2>/dev/null | sort | tr '\n' ' ')
echo "# $(date +%FT%T) old $(cat ~/osoe/.osoe-commit), new $(cat ~/osoe/build/p1-tree/.osoe-commit); offload (tso,gso): $offload" | tee "$O/summary.txt"
for lib in old new; do
    for shape in all-callers.2 all-callers.4 two-nodes.2; do
        mode=${shape%.*} nodes=${shape#*.}
        for run in $(seq 1 "$RUNS"); do
            if [ "$mode" = two-nodes ]; then
                args="7 1 two-nodes"; hold=0:1
            else
                args="7 0 all-callers"; hold=
            fi
            began_at=$(date +%s.%N)
            out=$(env ${hold:+OSOE_TEST_HOLD_GOODBYE=$hold} timeout 150 srun -K1 --cpu-bind=none \
                  -N"$nodes" --ntasks-per-node=2 "$O/ge-$lib" $args 2>&1 < /dev/null)
            status=$?
            took=$(echo "$began_at $(date +%s.%N)" | awk '{ printf "%.1f", $2 - $1 }')
            terminated=$(printf '%s\n' "$out" | grep -c "Terminated\|Killed")
            began=$(printf '%s\n' "$out" | grep -c "handler began")
            ran=$(printf '%s\n' "$out" | grep -c "handler ran after a second")
            echo "$lib $shape run=$run status=$status terminated_lines=$terminated handlers_began=$began handlers_ran=$ran took=${took}s" >> "$LINES"
            printf '%s\n' "$out" > "$O/out-$lib-$shape-$run.txt"
        done
    done
done
# Each shape's runs: srun's statuses, runs with a task terminated, runs with a handler cut short.
awk '{ key = $1 " " $2; n[key]++; split($4, s, "="); st[key] = st[key] " " s[2]
       split($5, t, "="); if (t[2] > 0) term[key]++
       split($6, b, "="); split($7, r, "="); if (r[2] < b[2]) cut[key]++ }
     END { for (k in n) printf "%s: %d runs; statuses%s; %d with a task terminated; %d with a handler cut short\n",
                                k, n[k], st[k], term[k] + 0, cut[k] + 0 }' "$LINES" | sort | tee -a "$O/summary.txt"
echo "# $(date +%T) done" | tee -a "$O/summary.txt"
