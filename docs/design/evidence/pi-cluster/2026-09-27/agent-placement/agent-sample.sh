#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# agent-sample.sh LABEL: one line per thread of every osoe-pacer PE on this
# node, from /proc - what the scheduler did to each thread so far:
#   LABEL time node pid procid tid role cpus_allowed last_cpu run_ns
#   runqueue_wait_ns timeslices voluntary_switches involuntary_switches
# role: main (tid = pid) or agent; lead- when the process holds the node's
# io_uring (the lead's agent drives the node's network, design 3.12).
label=$1
node=$(hostname)
now=$(date +%s.%N)
for pid in $(pgrep -x osoe-pacer); do
    procid=$(tr '\0' '\n' < /proc/$pid/environ 2>/dev/null | sed -n 's/^SLURM_PROCID=//p')
    lead=
    ls -l /proc/$pid/fd 2>/dev/null | grep -q 'io_uring' && lead=lead-
    for t in /proc/$pid/task/*; do
        tid=${t##*/}
        role=agent
        [ "$tid" = "$pid" ] && role=main
        read run wait slices < $t/schedstat
        cpus=$(sed -n 's/^Cpus_allowed_list:[[:space:]]*//p' $t/status)
        vol=$(sed -n 's/^voluntary_ctxt_switches:[[:space:]]*//p' $t/status)
        nonvol=$(sed -n 's/^nonvoluntary_ctxt_switches:[[:space:]]*//p' $t/status)
        psr=$(awk '{print $39}' $t/stat)
        echo "$label $now $node $pid $procid $tid $lead$role $cpus $psr $run $wait $slices $vol $nonvol"
    done
done
