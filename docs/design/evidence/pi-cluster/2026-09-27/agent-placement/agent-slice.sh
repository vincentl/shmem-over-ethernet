#!/bin/bash
# agent-slice.sh NS [CPUS]: ask for an NS-ns slice for the lead's agent on this
# node (set-slice), and widen its affinity to CPUS when given.
ns=$1 cpus=${2:-}
node=$(hostname)
for pid in $(pgrep -x osoe-pacer); do
    ls -l /proc/$pid/fd 2>/dev/null | grep -q 'io_uring' || continue
    for t in /proc/$pid/task/*; do
        tid=${t##*/}
        [ "$tid" = "$pid" ] && continue
        echo "$node lead agent $tid: $(~/agent-placement/set-slice $tid $ns 2>&1)"
        [ -n "$cpus" ] && echo "$node lead agent $tid: $(taskset -p -c $cpus $tid 2>&1 | tr '\n' ' ')"
    done
done
