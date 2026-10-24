#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# T44: what goes wrong, or does not, when the two libraries share a process:
# each case a line in probes.txt, its output in its own file.
#
#   probes.sh TREE OUT [SECTION...]
#
# TREE is an osoe tree built with the release preset and OUT holds the program
# build.sh built. Sections:
#   t  threads and signal handlers after each start
#   n  sockets, and two ranges of ports made to overlap
#   w  the deadlock detector: a PE blocked in MPI, MPI's threads alive
#   f  fork: the application's, and what MPI_Init forks
#   e  a job ended by a crash, MPI_Abort, shmem_global_exit or exit, or by SIGTERM
#   m  MPI's one-sided windows on the symmetric heap
#   g  the names each kind of job leaves in /dev/shm
#   u  the detector and the threads again with UCX's file system switched off
#   p  mpirun's own binding of each rank, against none
#   o  osoe-stat on a job mpirun started
#   k  the threads when Open MPI's TCP transport has a progress thread of its own, and
#      where each thread of a PE that started MPI waits
#   r  repeated starts of the launches that work, to find a rare failure
# All of them by default. Under the VM's lock (/tmp/osoe-vm.lock).
set -u
TREE=$1
OUT=$2
shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
SUMMARY=probes
sections=${*:-t n w f e m g u p o k r}
export SHMEM_DEBUG=1
mpirun="mpirun --bind-to none"
echo "# $(date -u +%FT%TZ), load $(cut -d' ' -f1-3 /proc/loadavg)" | tee -a "$OUT/$SUMMARY.txt"

for section in $sections; do
    case $section in
    t)
        export T44_SECONDS=3
        case_run t0-shmem-built-without-mpi 2 "$LAUNCH" -n 2 -t 60 -- "$BASELINE" hold shmem
        case_run t1-shmem-only 2 "$LAUNCH" -n 2 -t 60 -- "$PROGRAM" hold shmem
        case_run t2-mpi-only-mpirun 2 $mpirun -n 2 "$PROGRAM" hold mpi
        case_run t3-mpi-then-shmem-mpirun 2 $mpirun -n 2 "$WRAPPER" "$PROGRAM" hold mpi,shmem
        case_run t4-shmem-then-mpi-mpirun 2 $mpirun -n 2 "$WRAPPER" "$PROGRAM" hold shmem,mpi
        case_run t5-singletons 2 "$LAUNCH" -n 2 -t 90 -- "$PROGRAM" hold mpi,shmem
        T44_EARLY=mpi case_run t6-mpi-finalized-early 2 $mpirun -n 2 "$WRAPPER" "$PROGRAM" \
            hold mpi,shmem
        T44_THREADS=multiple case_run t7-thread-multiple 2 $mpirun -n 2 "$WRAPPER" "$PROGRAM" \
            hold mpi,shmem
        OSOE_PROGRESS_HELPERS=2 case_run t8-two-helpers 2 $mpirun -n 2 "$WRAPPER" "$PROGRAM" \
            hold mpi,shmem
        for f in "$OUT"/t[0-9]-*.txt; do
            echo "== $(basename "$f" .txt), PE 0"
            grep -E '^PE 0: (thread|[0-9]+ threads|signal|[0-9]+ signals)' "$f"
        done > "$OUT/threads-and-signals.txt"
        unset T44_SECONDS
        ;;
    n)
        # Two logical nodes, four ranks: the sockets that listen while the job holds.
        export T44_SECONDS=8
        for order in mpi,shmem shmem,mpi; do
            name=n1-listening-$(echo "$order" | tr , -)
            T44_NODES=2 $mpirun -n 4 "$WRAPPER" "$PROGRAM" hold "$order" > "$OUT/$name.txt" 2>&1 &
            job=$!
            sleep 5
            { echo "# listening"; ss -ltnpH; echo "# established, by the program"; ss -tnpH state established; } \
                > "$OUT/$name-sockets.txt" 2>&1
            wait $job
            echo "$name: exit $?; sockets in $name-sockets.txt" | tee -a "$OUT/$SUMMARY.txt"
            grep -c 'users:(("mpi-and-shmem' "$OUT/$name-sockets.txt" | sed 's/^/  sockets of the program: /'
        done
        unset T44_SECONDS
        # OSOE_PORT_RANGE and Open MPI's TCP ports made the same four.
        for order in mpi,shmem shmem,mpi; do
            T44_NODES=2 OSOE_PORT_RANGE=17000-17003 case_run n2-same-ports-$(echo "$order" | tr , -) 4 \
                $mpirun --mca btl tcp,self --mca btl_tcp_port_min_v4 17000 --mca btl_tcp_port_range_v4 4 \
                -n 4 "$WRAPPER" "$PROGRAM" check "$order"
        done
        ;;
    w)
        export OSOE_DEADLOCK_SECONDS=3
        LIMIT=25 case_run w0-built-without-mpi-deadlock 3 "$LAUNCH" -n 3 -t 20 -- "$BASELINE" \
            deadlock-osoe shmem
        LIMIT=25 case_run w1-shmem-alone-deadlock 3 "$LAUNCH" -n 3 -t 20 -- "$PROGRAM" \
            deadlock-osoe shmem
        LIMIT=25 case_run w2-mpi-and-shmem-deadlock 3 $mpirun -n 3 "$WRAPPER" "$PROGRAM" \
            deadlock-osoe mpi,shmem
        LIMIT=25 case_run w3-shmem-then-mpi-deadlock 3 $mpirun -n 3 "$WRAPPER" "$PROGRAM" \
            deadlock-osoe shmem,mpi
        T44_EARLY=mpi LIMIT=25 case_run w4-mpi-finalized-deadlock 3 $mpirun -n 3 "$WRAPPER" \
            "$PROGRAM" deadlock-osoe mpi,shmem
        LIMIT=25 case_run w5-singletons-deadlock 3 "$LAUNCH" -n 3 -t 20 -- "$PROGRAM" \
            deadlock-osoe mpi,shmem
        LIMIT=25 case_run w6-mixed-deadlock 3 $mpirun -n 3 "$WRAPPER" "$PROGRAM" \
            deadlock-mixed mpi,shmem
        OSOE_DEADLOCK_SECONDS=2 T44_SECONDS=12 LIMIT=60 case_run w7-benign 4 $mpirun -n 4 \
            "$WRAPPER" "$PROGRAM" benign-wait mpi,shmem
        T44_NODES=2 LIMIT=25 case_run w8-two-nodes-deadlock 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" \
            deadlock-osoe mpi,shmem
        T44_NODES=2 LIMIT=25 case_run w9-two-nodes-shmem-alone-deadlock 4 $mpirun -n 4 "$WRAPPER" \
            "$PROGRAM" deadlock-osoe shmem
        unset OSOE_DEADLOCK_SECONDS
        ;;
    f)
        case_run f1-fork-mpi-then-shmem 2 $mpirun -n 2 "$WRAPPER" "$PROGRAM" fork mpi,shmem
        case_run f2-fork-shmem-then-mpi 2 $mpirun -n 2 "$WRAPPER" "$PROGRAM" fork shmem,mpi
        case_run f3-fork-singletons 2 "$LAUNCH" -n 2 -t 90 -- "$PROGRAM" fork mpi,shmem
        # What the start forks: one rank under strace, the process calls only.
        rm -rf "$OUT/strace-mpirun" "$OUT/strace-singleton"
        mkdir -p "$OUT/strace-mpirun" "$OUT/strace-singleton"
        T44_SECONDS=1 case_run f4-strace-mpirun 1 $mpirun -n 1 strace -ff -tt -e trace=process \
            -o "$OUT/strace-mpirun/trace" "$WRAPPER" "$PROGRAM" hold mpi,shmem
        T44_SECONDS=1 case_run f5-strace-singleton 1 "$LAUNCH" -n 1 -t 90 -- strace -ff -tt \
            -e trace=process -o "$OUT/strace-singleton/trace" "$PROGRAM" hold mpi,shmem
        # Who makes the threads: the first four pthread_create calls of a PE that starts
        # shmem alone, with the stacks that made them.
        case_run f6-threads-made 1 $mpirun -n 1 "$WRAPPER" gdb -batch -ex 'set breakpoint pending on' \
            -ex 'break pthread_create' -ex run -ex 'bt 8' -ex continue -ex 'bt 8' -ex continue \
            -ex 'bt 8' -ex continue -ex 'bt 8' --args "$PROGRAM" hold shmem
        # And the thread libucs makes as it loads (it is in every process that links
        # Open MPI, started or not): attach to a PE that holds and ask each thread where it is.
        T44_SECONDS=8 $mpirun -n 1 "$WRAPPER" "$PROGRAM" hold shmem > "$OUT/f7-holding.txt" 2>&1 &
        job=$!
        sleep 4
        pid=$(ps -eo pid,args | awk '$2 ~ /mpi-and-shmem$/ && $3 == "hold" { print $1; exit }')
        gdb -p "$pid" -batch -ex 'thread apply all bt 10' > "$OUT/f7-thread-stacks.txt" 2>&1
        wait $job
        echo "f7-thread-stacks: attached to $pid, in f7-thread-stacks.txt" | tee -a "$OUT/$SUMMARY.txt"
        for kind in mpirun singleton; do
            {
                echo "== $kind: processes the program's tree created (clone without CLONE_THREAD, fork, vfork)"
                grep -h -E 'clone3?\(|fork\(|vfork\(' "$OUT/strace-$kind"/trace.* | grep -v CLONE_THREAD | \
                    cut -c1-170 | sort | uniq -c | sort -rn | head -20
                echo "== $kind: threads created (clone with CLONE_THREAD)"
                grep -h 'CLONE_THREAD' "$OUT/strace-$kind"/trace.* | wc -l
                echo "== $kind: execve"
                grep -h -E 'execve\(' "$OUT/strace-$kind"/trace.* | cut -c1-170
            } > "$OUT/strace-$kind-summary.txt" 2>&1
        done
        ;;
    e)
        for mode in crash mpi-abort global-exit plain-exit; do
            for wait in shmem mpi; do
                T44_WAIT=$wait LIMIT=60 case_run e-$mode-others-in-$wait-mpirun 3 $mpirun -n 3 \
                    "$WRAPPER" "$PROGRAM" "$mode" mpi,shmem
            done
        done
        for mode in crash mpi-abort global-exit plain-exit; do
            LIMIT=60 case_run e-$mode-others-in-shmem-singletons 3 "$LAUNCH" -n 3 -t 50 -- \
                "$PROGRAM" "$mode" mpi,shmem
        done
        for mode in crash mpi-abort global-exit plain-exit; do
            LIMIT=60 case_run e-$mode-others-in-shmem-srun 3 srun --cpu-bind=none -K1 --mpi=pmix_v5 \
                -n 3 "$PROGRAM" "$mode" mpi,shmem
        done
        # A terminal's ^C or a scheduler's SIGTERM, to mpirun and to osoe-launch, while the job holds.
        T44_SECONDS=30 case_signalled e-sigterm-to-mpirun 3 TERM 6 $mpirun -n 3 "$WRAPPER" \
            "$PROGRAM" hold mpi,shmem
        T44_SECONDS=30 case_signalled e-sigint-to-mpirun 3 INT 6 $mpirun -n 3 "$WRAPPER" \
            "$PROGRAM" hold mpi,shmem
        T44_SECONDS=30 case_signalled e-sigterm-to-osoe-launch 3 TERM 6 "$LAUNCH" -n 3 -t 50 -- \
            "$PROGRAM" hold mpi,shmem
        ;;
    g)
        export T44_ROUNDS=3
        case_run g1-singletons-clean 2 "$LAUNCH" -n 2 -t 60 -- "$PROGRAM" check mpi,shmem
        case_run g2-mpirun-clean 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" check mpi,shmem
        case_run g3-srun-clean 4 srun --cpu-bind=none -K1 --mpi=pmix_v5 -n 4 "$PROGRAM" check mpi,shmem
        case_run g4-mpirun-plain-exit 3 $mpirun -n 3 "$WRAPPER" "$PROGRAM" plain-exit mpi,shmem
        case_run g5-mpirun-crash 3 $mpirun -n 3 "$WRAPPER" "$PROGRAM" crash mpi,shmem
        case_run g6-mpirun-mpi-abort 3 $mpirun -n 3 "$WRAPPER" "$PROGRAM" mpi-abort mpi,shmem
        case_run g7-mpirun-global-exit 3 $mpirun -n 3 "$WRAPPER" "$PROGRAM" global-exit mpi,shmem
        case_run g8-srun-global-exit 3 srun --cpu-bind=none -K1 --mpi=pmix_v5 -n 3 "$PROGRAM" \
            global-exit mpi,shmem
        T44_SECONDS=30 case_signalled g9-mpirun-sigterm 3 TERM 6 $mpirun -n 3 "$WRAPPER" "$PROGRAM" \
            hold mpi,shmem
        T44_SECONDS=30 case_signalled g10-singletons-sigterm 3 TERM 6 "$LAUNCH" -n 3 -t 50 -- \
            "$PROGRAM" hold mpi,shmem
        unset T44_ROUNDS
        ;;
    u)
        # The thread libucs makes at load serves UCX's virtual file system (UCX_VFS_ENABLE).
        export OSOE_DEADLOCK_SECONDS=3 UCX_VFS_ENABLE=n
        T44_SECONDS=3 case_run u1-threads-shmem-only 2 $mpirun -n 2 "$WRAPPER" "$PROGRAM" hold shmem
        T44_SECONDS=3 case_run u2-threads-mpi-started 2 $mpirun -n 2 "$WRAPPER" "$PROGRAM" hold mpi,shmem
        T44_EARLY=mpi T44_SECONDS=3 case_run u3-threads-mpi-finalized 2 $mpirun -n 2 "$WRAPPER" \
            "$PROGRAM" hold mpi,shmem
        for f in "$OUT"/u[1-3]-*.txt; do
            echo "== $(basename "$f" .txt), PE 0"
            grep -E '^PE 0: (thread|[0-9]+ threads)' "$f"
        done > "$OUT/threads-without-the-vfs.txt"
        LIMIT=25 case_run u4-shmem-only-deadlock 3 $mpirun -n 3 "$WRAPPER" "$PROGRAM" \
            deadlock-osoe shmem
        LIMIT=25 case_run u5-mpi-started-deadlock 3 $mpirun -n 3 "$WRAPPER" "$PROGRAM" \
            deadlock-osoe mpi,shmem
        T44_EARLY=mpi LIMIT=25 case_run u6-mpi-finalized-deadlock 3 $mpirun -n 3 "$WRAPPER" \
            "$PROGRAM" deadlock-osoe mpi,shmem
        T44_NODES=2 T44_EARLY=mpi LIMIT=25 case_run u7-mpi-finalized-deadlock-2-nodes 4 $mpirun -n 4 \
            "$WRAPPER" "$PROGRAM" deadlock-osoe mpi,shmem
        unset OSOE_DEADLOCK_SECONDS UCX_VFS_ENABLE
        ;;
    p)
        # mpirun binds a rank to a core when it starts two or fewer, to the NUMA node above that
        # (here the whole machine); osoe's README asks for no binding.
        export T44_ROUNDS=300
        unset SHMEM_DEBUG
        for run in 1 2 3; do
            case_run p-2-bound-by-mpirun-$run 2 mpirun -n 2 "$WRAPPER" "$PROGRAM" check mpi,shmem
            case_run p-2-unbound-$run 2 $mpirun -n 2 "$WRAPPER" "$PROGRAM" check mpi,shmem
        done
        for run in 1 2 3; do
            case_run p-4-bound-by-mpirun-$run 4 mpirun -n 4 "$WRAPPER" "$PROGRAM" check mpi,shmem
            case_run p-4-unbound-$run 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" check mpi,shmem
            case_run p-4-bound-to-cores-$run 4 mpirun --bind-to core -n 4 "$WRAPPER" "$PROGRAM" \
                check mpi,shmem
        done
        unset T44_ROUNDS
        export SHMEM_DEBUG=1
        ;;
    o)
        # The job id the wrapper gave osoe is mpirun's pid, which osoe-stat takes.
        T44_SECONDS=10 $mpirun -n 3 "$WRAPPER" "$PROGRAM" hold mpi,shmem > "$OUT/o1-holding.txt" 2>&1 &
        job=$!
        sleep 5
        { echo "# osoe-stat --summary $job"; "$STAT" --summary "$job"; echo "# exit $?"; } \
            > "$OUT/o1-osoe-stat-summary.txt" 2>&1
        { echo "# osoe-stat $job"; "$STAT" "$job"; echo "# exit $?"; } > "$OUT/o1-osoe-stat.txt" 2>&1
        wait $job
        echo "o1-osoe-stat: on job $job, mpirun's pid; exit $(tail -1 "$OUT/o1-osoe-stat.txt")" |
            tee -a "$OUT/$SUMMARY.txt"
        ;;
    k)
        export T44_SECONDS=3
        case_run k1-tcp-progress-thread 2 $mpirun --mca btl tcp,self --mca btl_tcp_progress_thread 1 \
            -n 2 "$WRAPPER" "$PROGRAM" hold mpi,shmem
        { echo "== k1-tcp-progress-thread, PE 0"; grep -E '^PE 0: (thread|[0-9]+ threads)' "$OUT/k1-tcp-progress-thread.txt"; } \
            > "$OUT/threads-with-the-tcp-progress-thread.txt"
        unset T44_SECONDS
        # Where each thread of a PE that started both libraries waits (UCX's file system off, so
        # that the thread MPI_Init makes is the only one besides the agent's).
        UCX_VFS_ENABLE=n T44_SECONDS=8 $mpirun -n 1 "$WRAPPER" "$PROGRAM" hold mpi,shmem \
            > "$OUT/k2-holding.txt" 2>&1 &
        job=$!
        sleep 4
        pid=$(ps -eo pid,args | awk '$2 ~ /mpi-and-shmem$/ && $3 == "hold" { print $1; exit }')
        gdb -p "$pid" -batch -ex 'thread apply all bt 10' > "$OUT/k2-thread-stacks.txt" 2>&1
        wait $job
        echo "k2-thread-stacks: attached to $pid, in k2-thread-stacks.txt" | tee -a "$OUT/$SUMMARY.txt"
        ;;
    m)
        case_run m1-window-mpi-then-shmem 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" window mpi,shmem
        case_run m2-window-shmem-then-mpi 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" window shmem,mpi
        T44_NODES=2 case_run m3-window-2-nodes 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" window mpi,shmem
        case_run m4-window-tcp-btl 4 $mpirun --mca btl tcp,self -n 4 "$WRAPPER" "$PROGRAM" window \
            mpi,shmem
        ;;
    r)
        SUMMARY=repeats
        export T44_ROUNDS=5
        unset SHMEM_DEBUG
        for run in $(seq 1 15); do
            case_run r-mpi-then-shmem-$run 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" check mpi,shmem
            case_run r-shmem-then-mpi-$run 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" check shmem,mpi
            T44_NODES=2 case_run r-mpi-then-shmem-2-nodes-$run 4 $mpirun -n 4 "$WRAPPER" \
                "$PROGRAM" check mpi,shmem
            T44_NODES=2 case_run r-shmem-then-mpi-2-nodes-$run 4 $mpirun -n 4 "$WRAPPER" \
                "$PROGRAM" check shmem,mpi
        done
        unset T44_ROUNDS
        ;;
    esac
done
