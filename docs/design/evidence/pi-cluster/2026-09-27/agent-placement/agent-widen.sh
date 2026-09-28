#!/bin/bash
# agent-widen.sh CPUS: give the lead's agent thread on this node the affinity
# CPUS (0-3: the node's four cores, every core the step's cgroup allows),
# leaving every PE's main thread and every other agent as Slurm bound them.
cpus=$1
node=$(hostname)
for pid in $(pgrep -x osoe-pacer); do
    ls -l /proc/$pid/fd 2>/dev/null | grep -q 'io_uring' || continue
    for t in /proc/$pid/task/*; do
        tid=${t##*/}
        [ "$tid" = "$pid" ] && continue
        echo "$node lead pid $pid agent $tid: $(taskset -p -c $cpus $tid 2>&1 | tr '\n' ' ')"
    done
done
