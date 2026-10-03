#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# local-driver.sh: agent-placement.sh's runs, driven from the author's Mac,
# since only a hop from here carries the key the compute nodes accept. The
# job's step runs on pi-login under ssh; the samples and the widening go to
# each node directly (ProxyJump pi-login). Outputs: the pacer's on pi-login's
# ~/agent-placement, the samples and summary here.
set -u
cd "$(dirname "$0")" || exit 1
L=$PWD/out3
mkdir -p $L
NODES="pi-001 pi-002 pi-003 pi-004"
say() { echo "$(date +%T) $*" | tee -a $L/summary.txt; }
sample() {  # label file
    local pids=
    for n in $NODES; do ssh -o BatchMode=yes $n agent-placement/agent-sample.sh $1 > $L/.s.$n 2>&1 < /dev/null & pids="$pids $!"; done
    wait $pids
    for n in $NODES; do cat $L/.s.$n >> $L/$2; done
    rm -f $L/.s.*
}
ACT="agent-widen.sh 0-3"
widen() {  # file: $ACT on every node's lead agent
    local pids=
    for n in $NODES; do ssh -o BatchMode=yes $n agent-placement/$ACT > $L/.w.$n 2>&1 < /dev/null & pids="$pids $!"; done
    wait $pids
    for n in $NODES; do cat $L/.w.$n >> $L/$1; done
    rm -f $L/.w.*
    say "$ACT: $(grep -c -E 'after:.*slice 100000 |new affinity list: 0-3' $L/$1) lead agents changed"
}
nstat_all() {  # label
    for n in $NODES; do echo "$1 $n $(ssh -o BatchMode=yes $n 'nstat -az TcpRetransSegs TcpExtTCPTimeouts' < /dev/null 2>/dev/null | awk 'NR>1{printf "%s=%s ", $1, $2}')"; done >> $L/nstat.txt
}
run() {  # name seconds s1 s2 widen-at s3 s4 bind pacer-arguments...
    local name=$1 seconds=$2 s1=$3 s2=$4 at=$5 s3=$6 s4=$7 bind=$8; shift 8
    local arguments="$*"
    nstat_all "$name-before"
    local start=$(date +%s)
    ssh -o BatchMode=yes pi-login "cd ~/osoe-pacer-phase4b && srun -K1 -N4 --ntasks-per-node=4 $bind build/release/tools/pacer/osoe-pacer --seconds $seconds $arguments > ~/agent-placement/$name.txt 2>&1" < /dev/null &
    local job=$!
    local t=0 event
    for event in "$s1 sample" "$s2 sample" "$at widen" "$s3 sample" "$s4 sample"; do
        set -- $event
        [ "$1" = 0 ] && continue
        sleep $(( $1 - t )); t=$1
        if [ "$2" = sample ]; then sample "$t" $name-samples.txt; else widen $name-widen.txt; fi
    done
    wait $job
    local status=$?
    nstat_all "$name-after"
    say "$name exit $status in $(( $(date +%s) - start )) s: $(ssh -o BatchMode=yes pi-login "grep '^osoe-pacer failures=' ~/agent-placement/$name.txt" < /dev/null)"
}
# One session held open on every node for the whole run (RemoveIPC, 2026-09-27-night item 6).
holders=
for n in $NODES; do ssh -o BatchMode=yes $n 'sleep 7200' < /dev/null > /dev/null 2>&1 & holders="$holders $!"; done
sleep 2
timeouts() { for n in $NODES; do echo -n "$n=$(ssh -o BatchMode=yes $n "nstat -az TcpExtTCPTimeouts | awk 'NR>1{print \$2}'" < /dev/null) "; done; }
run_plain() {  # name tree pacer-arguments...
    local name=$1 tree=$2; shift 2
    local before=$(timeouts) start=$(date +%s)
    ssh -o BatchMode=yes pi-login "cd ~/$tree && srun -K1 -N4 --ntasks-per-node=4 build/release/tools/pacer/osoe-pacer $* > ~/agent-placement/$name.txt 2>&1" < /dev/null
    local status=$?
    say "$name exit $status in $(( $(date +%s) - start )) s: $(ssh -o BatchMode=yes pi-login "grep '^osoe-pacer failures=' ~/agent-placement/$name.txt" < /dev/null)"
    say "    timeouts before: $before"
    say "    timeouts after:  $(timeouts)"
}
say "start: unbound with the agent's slice, then sort 16 against 11, then mix with waits"
ACT="agent-slice.sh 100000"
run mailbox-unbound-slice  95 20 35 40 60 75 "--cpu-bind=none"  mailbox
run_plain sort-16 osoe-pacer-phase4b --seconds 120 sort
run_plain sort-11 osoe-pacer-decisions --seconds 120 sort
run_plain mix-waits osoe-pacer-decisions --seconds 600 mix
say DRIVER3-DONE
kill $holders 2>/dev/null
