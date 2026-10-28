#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The third cluster session as one chain (docs/prompts/cluster-session-3-plan.md,
# the author's plan of 2026-10-27, docs/decisions/2026-10-27.md): the plan's
# sections in turn, one measurement at a time, each by the kit's own scripts
# with the plan's settings, from the head node and unattended - the author
# approving the plan once and bringing the cluster up, the lead watching. Each
# section's output goes to OUTDIR/SECTION/ (and OUTDIR/SECTION-*/), its log to
# OUTDIR/SECTION.log, its table to OUTDIR/SECTION-summary.txt (summarize.py),
# and, as it ends, its line to OUTDIR/sections.txt and this script's output:
# done or FAILED, its minutes beside the plan's, its jobs and how many failed.
# A section that fails is said there and the chain goes on - but for the wait
# and the setup's packages, Slurm and build, which every section after stands
# on, where it stops. OUTDIR/summary.txt is the whole, at the end.
#
#    0 wait       the machines' start-up scripts, by systemd's state (§1)
#    1 setup      the packages on every machine, Slurm, the build, the facts, the
#                 probe, a job of 32 PEs a node, tier 2 on the first node (§1 to
#                 §3); SOS fetched and built on this machine meanwhile (sos.sh)
#    2 floor      §4, iperf3 alone: echo-uring's client is the kit's fault
#    3 link-cost  §19, P38: its own five rounds
#    4 window     §14, P37: the chooser at windows 4 and 8, its trees at 8 and
#                 the fixed (4, 8), five rounds
#    5 many-pes   §20, H12: its own settings, one round
#    6 gets       §8, H28: one round
#    7 behind     §13, H15 to H19: 0 and 2 helpers, one round
#    8 trees      §16, H6: the four trees built, then one round
#    9 scale      §10, H11 and B8's SOS: mix for 300 s, SOS at 2 PEs a node
#   10 binding    §5 under binding.sh site, H26: 0 and 2 helpers at 8 PEs a
#                 node, two rounds bound to one CPU a PE, then two with two
#                 CPUs a PE (--cpus-per-task=2); then binding.sh none. Last,
#                 since it restarts Slurm
#   11 pack       OUTDIR packed beside itself as OUTDIR.tar.gz, to bring back (§11)
#
#   SECTIONS          the sections to run, in the order above (default: all)
#   STARTUP_UNIT      the unit each machine's start-up script runs as
#                     (google-startup-scripts.service; empty: no wait)
#   STARTUP_EXPECTED  minutes after which a wait is checked by another means,
#                     each pending machine's unit status and log (4)
#   STARTUP_MINUTES   minutes after which the wait fails, and the chain with it (10)
#   SUITE_TESTS       ctest -R's regex for tier 2 (empty: all of it)
#   SUITE_JOBS        tier 2's tests at once (8, the node's cores)
#   PERF              perf's command (sudo -n perf: perf_event_paranoid is 3
#                     on the image, and in the development VM)
#   SOS_SOURCE        sos.sh's
#   H26_BOUND, H26_RESERVED  H26's two steps' srun options (-K1
#                     --cpu-bind=threads, and -K1 --cpus-per-task=2)
#
# Run it from the tree on the head node - a clone, whose .git trees.sh needs -
# from an ssh session that forwards the agent, kept open to the end: the setup,
# sos.sh, trees.sh and binding.sh reach the nodes by ssh, and the agent is the
# session's. In a session of its own, so that a dropped connection costs the
# agent and not the chain, and the connection kept from idling; from the Mac,
# in the background:
#   ssh -A -o ServerAliveInterval=30 slurm-controller 'cd osoe; setsid nohup
#       docs/fast-link/session-3.sh ~/NODES ~/out > ~/session-3.txt 2>&1 < /dev/null &
#       tail -f --pid=$! ~/session-3.txt'
# and watch ~/out/sections.txt. To stop it: kill -TERM -$(cat ~/out/chain.pid),
# its process group; pkill -u $USER -x timeout, since each job's timeout is a
# group of its own; and scancel -u $USER. SECTIONS then runs what is left. Under
# KIT_LAUNCHER=osoe-launch, one machine's logical nodes (dry-run.sh), what needs
# other machines is skipped and says so: the wait, the packages, Slurm, SOS's
# copies and the binding's switch.
# usage: session-3.sh NODES OUTDIR
set -u
KIT=$(cd "$(dirname "$0")" && pwd)
source "$KIT/common.sh"
[ $# -eq 2 ] || { sed -n '/^# usage/p' "$0" >&2; exit 2; }
[ -r "$1" ] || { echo "no node list $1" >&2; exit 2; }
NODES=$(cd "$(dirname "$1")" && pwd)/$(basename "$1")
mkdir -p "$2" || exit 1
CHAIN=$(cd "$2" && pwd)
ALL="wait setup floor link-cost window many-pes gets behind trees scale binding pack"
SECTIONS=${SECTIONS:-$ALL}
for s in $SECTIONS; do
    case " $ALL " in *" $s "*) ;; *) echo "no section $s: they are $ALL" >&2; exit 2 ;; esac
done
STARTUP_UNIT=${STARTUP_UNIT-google-startup-scripts.service}
STARTUP_EXPECTED=${STARTUP_EXPECTED:-4}
STARTUP_MINUTES=${STARTUP_MINUTES:-10}
SUITE_TESTS=${SUITE_TESTS:-}
SUITE_JOBS=${SUITE_JOBS:-8}
CLUSTER=1
[ "$KIT_LAUNCHER" = osoe-launch ] && CLUSTER=0
export PERF=${PERF:-sudo -n perf}
H26_BOUND=${H26_BOUND:--K1 --cpu-bind=threads}
H26_RESERVED=${H26_RESERVED:--K1 --cpus-per-task=2}
SOS_DIR=$REPOSITORY/build/sos
SSH="ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes -o ConnectTimeout=10"
HERE=$(hostname -s)
STATUS=$CHAIN/sections.txt
BEGAN=$(date +%s)
COMMIT=${KIT_COMMIT:-$(git -C "$REPOSITORY" rev-parse HEAD 2>/dev/null || echo unknown)}
# The plan's minutes for each section, beside which each one's own are said.
planned() {
    case $1 in
        wait) echo 4 ;; setup) echo 5 ;; floor) echo 1 ;; link-cost) echo 0.2 ;;
        window) echo 2 ;; many-pes) echo 7 ;; gets) echo 2.3 ;; behind) echo 5 ;;
        trees) echo 6 ;; scale) echo 9.5 ;; binding) echo 8 ;; pack) echo 2 ;;
    esac
}

minutes() { awk -v s="$1" 'BEGIN { printf "%.1f", s / 60 }'; }
# This machine first unless the list has it, then the list: every machine the
# wait and the packages reach.
machines() { grep -qxF "$HERE" "$NODES" || echo "$HERE"; grep . "$NODES"; }
# on_machine MACHINE COMMAND: COMMAND there by ssh, or here if it is this machine.
on_machine() {
    local m=$1
    shift
    if [ "${m%%.*}" = "$HERE" ]; then bash -c "$*"; else $SSH -n "$m" "$*"; fi
}

# 0. Each machine's start-up script, by its unit's state (fast-link.md §1): done
# when ActiveState=inactive and Result=success and its main process has exited -
# a unit not yet started reads inactive and success too, without an exit time -
# or active and exited, should the image ever keep it so; still running while
# activating; any other result failed, which stops the
# chain for the lead to look at. Never by pgrep -f through ssh, whose pattern is in
# its own shell's command line. Past STARTUP_EXPECTED minutes the machines still
# pending are looked at another way - the unit's status and its last lines -
# and past STARTUP_MINUTES the wait fails.
section_wait() {
    if [ "$CLUSTER" = 0 ] || [ -z "$STARTUP_UNIT" ]; then
        echo "no start-up scripts to wait for: ${STARTUP_UNIT:+one machine}${STARTUP_UNIT:-STARTUP_UNIT is empty}"
        return 0
    fi
    local began m out state sub result loaded exited pending failed looked=0
    began=$(date +%s)
    while :; do
        pending= failed=
        for m in $(machines); do
            out=$(on_machine "$m" "systemctl show -p LoadState -p ActiveState -p SubState \
                -p Result -p ExecMainExitTimestamp $STARTUP_UNIT" 2>&1)
            loaded=$(sed -n 's/^LoadState=//p' <<< "$out")
            state=$(sed -n 's/^ActiveState=//p' <<< "$out")
            sub=$(sed -n 's/^SubState=//p' <<< "$out")
            result=$(sed -n 's/^Result=//p' <<< "$out")
            exited=$(sed -n 's/^ExecMainExitTimestamp=//p' <<< "$out")
            if [ -z "$state" ] && grep -q 'Permission denied' <<< "$out"; then
                failed="$failed $m(ssh refused: is the agent forwarded?)"
            elif [ -z "$state" ]; then
                pending="$pending $m(unreachable)"
            elif [ "$loaded" = not-found ]; then
                failed="$failed $m(no unit $STARTUP_UNIT)"
            elif [ "$state" = inactive ] && [ "$result" = success ] && [ -n "$exited" ]; then
                :
            elif [ "$state" = active ] && [ "$sub" = exited ] && [ "$result" = success ]; then
                :
            elif [ "$state" = inactive ] && [ "$result" = success ]; then
                pending="$pending $m(not started)"
            elif [ "$state" = activating ] || [ "$state" = active ] || [ "$state" = reloading ]; then
                pending="$pending $m($state)"
            else
                failed="$failed $m($state, $result)"
            fi
        done
        local waited=$(( $(date +%s) - began ))
        if [ -z "$pending$failed" ]; then
            echo "$(date -u +%H:%M:%S) start-up scripts: every machine done"
        else
            echo "$(date -u +%H:%M:%S) start-up scripts:${pending:+ pending$pending}${failed:+ failed$failed}"
        fi
        [ -n "$failed" ] && { echo "a start-up script failed:$failed"; return 1; }
        [ -z "$pending" ] && { echo "every machine's start-up script done, after $(minutes "$waited") min"; return 0; }
        if [ "$looked" = 0 ] && [ "$waited" -ge $((STARTUP_EXPECTED * 60)) ]; then
            looked=1
            echo "past the expected $STARTUP_EXPECTED min: each pending machine's unit, looked at another way"
            for m in $pending; do
                m=${m%%(*}
                on_machine "$m" "systemctl status --no-pager -n 5 $STARTUP_UNIT" 2>&1 | sed "s/^/$m: /"
            done
        fi
        [ "$waited" -ge $((STARTUP_MINUTES * 60)) ] && {
            echo "the start-up scripts were not done after $STARTUP_MINUTES min:$pending"; return 1; }
        sleep 10
    done
}

# 1. The setup. The packages on every machine at once (the start-up scripts
# install all but linux-perf), Slurm and the build: what every section after
# needs, so that their failure stops the chain. Then SOS's build on this
# machine in the background, at the lowest priority, once Slurm's daemons are
# up and nothing else builds here - before the build on a head node off the
# list, after it where this machine builds too - and the facts, the probe, a
# job of 32 PEs a node and tier 2 on the first node, whose failures are
# findings, said in OUTDIR/setup/verdicts.txt.
PACKAGES="build-essential cmake pkg-config liburing-dev git rsync ethtool iperf3 linux-perf"
packages() {
    local install="sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=300 -q update &&
        sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=300 install -y -q $PACKAGES &&
        sudo DEBIAN_FRONTEND=noninteractive apt-get -o DPkg::Lock::Timeout=300 install -y -q \
            --no-install-recommends slurm-wlm munge"
    local m pids=() status=0
    for m in $(machines); do
        ( on_machine "$m" "$install" > "$CHAIN/setup/apt-$m.txt" 2>&1
          echo "exit $?" >> "$CHAIN/setup/apt-$m.txt" ) &
        pids+=($!)
    done
    wait "${pids[@]}"
    for m in $(machines); do
        echo "$m: packages $(tail -1 "$CHAIN/setup/apt-$m.txt")"
        [ "$(tail -1 "$CHAIN/setup/apt-$m.txt")" = "exit 0" ] || status=1
    done
    return $status
}
start_sos() {
    ( nice -n 19 bash "$KIT/sos.sh" "$NODES" "$SOS_DIR" > "$CHAIN/setup-sos.log" 2>&1 < /dev/null
      echo "exit $?" > "$CHAIN/setup-sos.status" ) &
    echo $! > "$CHAIN/setup-sos.pid"
    echo "SOS's build started on this machine (sos.sh, its log $CHAIN/setup-sos.log)"
}
section_setup() {
    mkdir -p "$CHAIN/setup"
    rm -f "$CHAIN/setup-sos.status" "$CHAIN/setup-sos.pid"
    local sos_first=1 verdicts=$CHAIN/setup/verdicts.txt
    { [ "$CLUSTER" = 0 ] || grep -qxF "$HERE" "$NODES"; } && sos_first=0
    if [ "$CLUSTER" = 1 ]; then
        packages || { echo "the packages failed on a machine"; return 1; }
    else
        echo "one machine: no packages installed, and no Slurm"
    fi
    if [ "$CLUSTER" = 1 ]; then
        if grep -qxF "$HERE" "$NODES"; then
            bash "$KIT/slurm-setup.sh" "$NODES"
        else
            CONTROLLER=$HERE bash "$KIT/slurm-setup.sh" "$NODES"
        fi > "$CHAIN/setup/slurm-setup.txt" 2>&1 || {
            echo "slurm-setup.sh failed:"; tail -20 "$CHAIN/setup/slurm-setup.txt"; return 1; }
        tail -n "$(( $(grep -c . "$NODES") + 1 ))" "$CHAIN/setup/slurm-setup.txt"
    fi
    [ "$sos_first" = 1 ] && start_sos
    OUT=$CHAIN/setup-build bash "$KIT/build.sh" "$NODES" > "$CHAIN/setup/build.txt" 2>&1 || {
        echo "build.sh failed:"; tail -20 "$CHAIN/setup/build.txt"; return 1; }
    cat "$CHAIN/setup/build.txt"
    [ "$sos_first" = 0 ] && start_sos
    bash "$KIT/facts.sh" "$NODES" "$CHAIN/setup-facts" ||
        echo "facts: facts.sh failed (exit $?)" >> "$verdicts"
    kit_start "$NODES" "$CHAIN/setup"
    launch "$NODE_COUNT" 1 "$CHAIN/setup/probe.txt" -- "$KIT_BUILD/tools/probe/probe"
    echo "probe: PROBE PASSED on $(grep -c 'PROBE PASSED' "$CHAIN/setup/probe.txt") of $NODE_COUNT nodes" >> "$verdicts"
    launch "$NODE_COUNT" 32 "$CHAIN/setup/startup-32.txt" -- "$KIT_BIN/startup"
    echo "a job of 32 PEs a node: $(grep -h '^startup ' "$CHAIN/setup/startup-32.txt" || echo 'no startup line')" >> "$verdicts"
    local tests=(--test-dir "$KIT_BUILD" -L tier2 -E '^model\.' -j"$SUITE_JOBS" --timeout 300)
    [ -n "$SUITE_TESTS" ] && tests+=(-R "$SUITE_TESTS")
    local suite_status
    KIT_TIMEOUT=1800 on "$(node 1)" ctest "${tests[@]}" > "$CHAIN/setup/suite.txt" 2>&1
    suite_status=$?
    echo "tier 2 on $(node 1), exit $suite_status: $(grep -h 'tests passed' "$CHAIN/setup/suite.txt" || echo 'no summary line')" >> "$verdicts"
    # T35's litmus, which closes T36 run natively on x86-64.
    local litmus
    litmus=$(grep -h 'Test *#[0-9]*: shmem\.quiet\.litmus\.L2 ' "$CHAIN/setup/suite.txt" | head -1 |
        sed 's/^ *//; s/ \.\.*/ /; s/  */ /g')
    echo "the litmus: ${litmus:-not in the output of the suite}" >> "$verdicts"
    cat "$verdicts"
}

# 2 to 8: the kit's scripts with the plan's settings.
section_floor() { PARTS=iperf3 bash "$KIT/floor.sh" "$NODES" "$CHAIN/floor"; }
section_link_cost() { bash "$KIT/link-cost.sh" "$NODES" "$CHAIN/link-cost"; }
section_window() {
    ROUNDS=5 CASES="link-d4 link-d8 tree-d8 fixed-k4-d8" bash "$KIT/window.sh" "$NODES" "$CHAIN/window"
}
section_many_pes() { bash "$KIT/many-pes.sh" "$NODES" "$CHAIN/many-pes"; }
section_gets() { ROUNDS=1 bash "$KIT/gets.sh" "$NODES" "$CHAIN/gets"; }
section_behind() { ROUNDS=1 HELPERS="0 2" bash "$KIT/behind.sh" "$NODES" "$CHAIN/behind"; }
section_trees() { ROUNDS=1 bash "$KIT/trees.sh" "$NODES" "$CHAIN/trees"; }

# 9. The cluster at size, SOS's programs among it: sos.sh's build, started in
# the setup, waited for while its process lives, ten minutes at most - or,
# where this run started none, a build an earlier run finished, or one now.
# sos.sh's done.txt says a build was whole and copied to every node.
section_scale() {
    local sos= pid waited=0
    pid=$(cat "$CHAIN/setup-sos.pid" 2>/dev/null)
    if [ -n "$pid" ] && kill -0 "$pid" 2> /dev/null; then
        echo "$(date -u +%H:%M:%S) waiting for SOS's build (sos.sh)"
        while kill -0 "$pid" 2> /dev/null && [ "$waited" -lt 600 ]; do
            sleep 5
            waited=$((waited + 5))
        done
        echo "$(date -u +%H:%M:%S) SOS's build: $(cat "$CHAIN/setup-sos.status" 2>/dev/null || echo "still running after $waited s")"
    elif [ -z "$pid" ] && [ ! -e "$SOS_DIR/done.txt" ]; then
        echo "no SOS build yet: sos.sh now"
        nice -n 19 bash "$KIT/sos.sh" "$NODES" "$SOS_DIR" > "$CHAIN/setup-sos.log" 2>&1 < /dev/null
        echo "exit $?" > "$CHAIN/setup-sos.status"
    fi
    tail -3 "$CHAIN/setup-sos.log" 2>/dev/null
    if [ -e "$SOS_DIR/done.txt" ]; then
        sos=$SOS_DIR
        cat "$SOS_DIR/done.txt"
        mkdir -p "$CHAIN/scale"
        cp "$SOS_DIR/build.txt" "$CHAIN/scale/sos-build.txt"
    else
        echo "no SOS run: no build whole and copied ($SOS_DIR/done.txt)"
    fi
    MIX_SECONDS=300 SOS_BUILD=$sos bash "$KIT/scale.sh" "$NODES" "$CHAIN/scale"
}

# 10. H26: §5's workloads under a site's binding, each PE bound to one CPU, as
# a site's srun binds a task of one CPU, and again with two CPUs a PE
# (--cpus-per-task=2), a CPU for its helpers - on a c4-standard-16 the PE's
# whole core, its two hyperthreads; first each variant's tasks' allowed CPUs,
# the binding's record; then the binding switched back for the kit. The binding
# is asked for: in a whole-node allocation, as the kit's jobs are, a step's
# tasks of one CPU are bound to nothing - in the VM's own Slurm
# (task/affinity) each had every CPU, with --overcommit or without - where
# --cpu-bind=threads bound each to its own and --cpus-per-task=2 each to two.
section_binding() {
    local status=0 variant srun
    if [ "$CLUSTER" = 1 ]; then
        CONTROLLER=$HERE bash "$KIT/binding.sh" "$NODES" site > "$CHAIN/binding-site.txt" 2>&1 || {
            echo "binding.sh site failed:"; tail -20 "$CHAIN/binding-site.txt"
            CONTROLLER=$HERE bash "$KIT/binding.sh" "$NODES" none > "$CHAIN/binding-none.txt" 2>&1 ||
                echo "and binding.sh none failed too: some nodes may be left bound"
            return 1; }
        tail -n 4 "$CHAIN/binding-site.txt"
    else
        echo "one machine: no Slurm to bind, and the cases run as osoe-launch starts them"
    fi
    for variant in bound reserved; do
        srun=$H26_BOUND
        [ "$variant" = reserved ] && srun=$H26_RESERVED
        ( kit_start "$NODES" "$CHAIN/binding-$variant"
          # shellcheck disable=SC2016
          KIT_SRUN=$srun launch "$NODE_COUNT" 8 "$CHAIN/binding-$variant/cpus-allowed.txt" -- \
              bash -c 'echo "$(hostname) task ${SLURM_PROCID:-?}: $(grep Cpus_allowed_list /proc/self/status)"' )
        KIT_SRUN=$srun ROUNDS=2 HELPERS="0 2" PES_PER_NODE=8 \
            bash "$KIT/helpers.sh" "$NODES" "$CHAIN/binding-$variant" || status=1
    done
    if [ "$CLUSTER" = 1 ]; then
        CONTROLLER=$HERE bash "$KIT/binding.sh" "$NODES" none > "$CHAIN/binding-none.txt" 2>&1 || {
            echo "binding.sh none failed: the nodes are left bound"; status=1; }
    fi
    return $status
}

# 11. Everything, for bringing back: the tarball holds all but its own line.
section_pack() {
    summary
    tar -C "$(dirname "$CHAIN")" -czf "$CHAIN.tar.gz" "$(basename "$CHAIN")" && ls -l "$CHAIN.tar.gz"
}

# The whole: every section's line, the setup's verdicts and the time.
summary() {
    {
        echo "# the third cluster session's chain on $HERE, kit $COMMIT, $(grep -c . "$NODES") nodes"
        echo "# began $(date -u -d "@$BEGAN" +%FT%TZ 2>/dev/null || date -u +%FT%TZ), $(minutes $(( $(date +%s) - BEGAN ))) min so far"
        cat "$STATUS" 2>/dev/null
        [ -s "$CHAIN/setup/verdicts.txt" ] && sed 's/^/setup: /' "$CHAIN/setup/verdicts.txt"
        [ -s "$CHAIN/scale/sos-build.txt" ] && echo "SOS: $(grep -c ': built$' "$CHAIN/scale/sos-build.txt") programs built, $(grep -c 'build failed' "$CHAIN/scale/sos-build.txt") not"
        echo "each section's table: OUTDIR/SECTION-summary.txt; its log: OUTDIR/SECTION.log"
    } > "$CHAIN/summary.txt"
}

# One section: run if wanted, in a subshell of its own so that nothing it does
# ends the chain, timed, its line written as it ends; the two the rest stand on
# end the chain where they fail.
run_section() {   # NUMBER NAME [stop]
    local number=$1 name=$2 stop=${3:-} began status seconds jobs failed verdict line d
    case " $SECTIONS " in *" $name "*) ;; *) return 0 ;; esac
    began=$(date +%s)
    echo "$(date -u +%H:%M:%S) $number $name: started, $(planned "$name") min planned"
    ( "section_${name//-/_}" ) > "$CHAIN/$name.log" 2>&1 < /dev/null
    status=$?
    seconds=$(( $(date +%s) - began ))
    # shellcheck disable=SC2086
    jobs=$(cat "$CHAIN/$name"/run.txt "$CHAIN/$name"-*/run.txt 2>/dev/null |
        grep -cE '^[0-9]{2}:[0-9]{2}:[0-9]{2} exit [0-9]+ ')
    failed=$(cat "$CHAIN/$name"/run.txt "$CHAIN/$name"-*/run.txt 2>/dev/null |
        grep -cE '^[0-9]{2}:[0-9]{2}:[0-9]{2} exit [1-9]')
    [ "$status" -eq 0 ] && verdict="done" || verdict="FAILED"
    line=$(printf '%2d %-9s %-6s %s UTC, exit %d, %s min (planned %s), %d jobs, %d failed' \
        "$number" "$name" "$verdict" "$(date -u +%H:%M:%S)" "$status" "$(minutes "$seconds")" \
        "$(planned "$name")" "$jobs" "$failed")
    echo "$line" | tee -a "$STATUS"
    for d in "$CHAIN/$name" "$CHAIN/$name"-*; do
        [ -d "$d" ] && python3 "$KIT/summarize.py" "$d"
    done > "$CHAIN/$name-summary.txt" 2>&1
    if [ -n "$stop" ] && [ "$status" -ne 0 ]; then
        echo "the chain stops here: every section after stands on $name ($CHAIN/$name.log)" | tee -a "$STATUS"
        summary
        exit 1
    fi
}

{
    echo "# $(date -u +%FT%TZ) session-3.sh on $HERE: kit $COMMIT, $(grep -c . "$NODES") nodes,"
    echo "# launcher $KIT_LAUNCHER, sections: $SECTIONS"
} | tee -a "$STATUS"
echo $$ > "$CHAIN/chain.pid"
trap 'echo "stopped by a signal at $(date -u +%H:%M:%S)" | tee -a "$STATUS"; summary; exit 130' INT TERM
run_section 0 wait stop
run_section 1 setup stop
run_section 2 floor
run_section 3 link-cost
run_section 4 window
run_section 5 many-pes
run_section 6 gets
run_section 7 behind
run_section 8 trees
run_section 9 scale
run_section 10 binding
run_section 11 pack
summary
echo "the chain ended at $(date -u +%H:%M:%S), $(minutes $(( $(date +%s) - BEGAN ))) min: $CHAIN/summary.txt" |
    tee -a "$STATUS"
