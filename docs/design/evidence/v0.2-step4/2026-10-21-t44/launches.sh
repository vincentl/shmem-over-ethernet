#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# T44: the launches of `check` (mpi-and-shmem.c), each a line in launches.txt.
#
#   launches.sh TREE OUT [SECTION...]
#
# TREE is an osoe tree built with the release preset and OUT holds the program
# build.sh built; the outputs of every case go there too. Sections:
#   a  osoe-launch, each rank an MPI singleton
#   b  mpirun on one node, osoe's variables made by open-mpi-bootstrap.sh
#   c  mpirun and the wrapper again, the ranks in several logical nodes
#   d  srun, with Slurm's PMIx (and without)
#   s  a first look: one case of each kind, and the variables mpirun gives a rank
# All of a to d by default. Under the VM's lock (/tmp/osoe-vm.lock).
set -u
TREE=$1
OUT=$2
shift 2
HERE=$(cd "$(dirname "$0")" && pwd)
. "$HERE/lib.sh"
SUMMARY=launches
sections=${*:-a b c d}
export SHMEM_DEBUG=1
echo "# $(date -u +%FT%TZ), load $(cut -d' ' -f1-3 /proc/loadavg)" | tee -a "$OUT/$SUMMARY.txt"

for section in $sections; do
    case $section in
    s)
        LIMIT=60
        case_run s1-shmem-only 2 "$LAUNCH" -n 2 -t 60 -- "$PROGRAM" check shmem
        case_run s2-mpi-only 2 mpirun --bind-to none -n 2 "$PROGRAM" check mpi
        case_run s3-singletons 2 "$LAUNCH" -n 2 -t 60 -- "$PROGRAM" check mpi,shmem
        case_run s4-variables 0 mpirun --bind-to none -n 2 env
        grep -E '^(OMPI|PMIX|PRTE|OPAL|SLURM|OSOE|SHMEM)' "$OUT/s4-variables.txt" | sort \
            > "$OUT/mpirun-variables.txt"
        case_run s5-mpirun-and-wrapper 4 mpirun --bind-to none -n 4 "$WRAPPER" "$PROGRAM" check \
            mpi,shmem
        ;;
    a)
        case_run a1-shmem-only 2 "$LAUNCH" -n 2 -t 60 -- "$PROGRAM" check shmem
        case_run a2-mpi-only 2 "$LAUNCH" -n 2 -t 90 -- "$PROGRAM" check mpi
        case_run a3-mpi-then-shmem 2 "$LAUNCH" -n 2 -t 90 -- "$PROGRAM" check mpi,shmem
        case_run a4-shmem-then-mpi 2 "$LAUNCH" -n 2 -t 90 -- "$PROGRAM" check shmem,mpi
        case_run a5-mpi-then-shmem-4-on-2-nodes 4 "$LAUNCH" -n 4 -m 0,0,1,1 -t 90 -- \
            "$PROGRAM" check mpi,shmem
        case_run a6-shmem-then-mpi-4-on-2-nodes 4 "$LAUNCH" -n 4 -m 0,0,1,1 -t 90 -- \
            "$PROGRAM" check shmem,mpi
        case_run a7-shmem-restarted-three-times 2 "$LAUNCH" -n 2 -t 90 -- "$PROGRAM" sessions mpi,shmem
        ;;
    b)
        mpirun="mpirun --bind-to none"
        case_run b1-mpi-only 4 $mpirun -n 4 "$PROGRAM" check mpi
        case_run b2-no-wrapper 4 $mpirun -n 4 "$PROGRAM" check mpi,shmem
        case_run b3-shmem-only 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" check shmem
        case_run b4-mpi-then-shmem 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" check mpi,shmem
        case_run b5-shmem-then-mpi 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" check shmem,mpi
        case_run b6-mpi-then-shmem-mpi-ends-first 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" check \
            mpi,shmem mpi,shmem
        case_run b7-shmem-then-mpi-shmem-ends-first 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" check \
            shmem,mpi shmem,mpi
        case_run b8-two-pes 2 $mpirun -n 2 "$WRAPPER" "$PROGRAM" check mpi,shmem
        case_run b9-one-pe 1 $mpirun -n 1 "$WRAPPER" "$PROGRAM" check mpi,shmem
        case_run b10-default-binding-2 2 mpirun -n 2 "$WRAPPER" "$PROGRAM" check mpi,shmem
        case_run b11-default-binding-4 4 mpirun -n 4 "$WRAPPER" "$PROGRAM" check mpi,shmem
        case_run b12-oversubscribed-8 8 $mpirun --oversubscribe -n 8 "$WRAPPER" "$PROGRAM" check \
            mpi,shmem
        T44_THREADS=multiple case_run b13-thread-multiple 4 $mpirun -n 4 \
            "$WRAPPER" "$PROGRAM" check mpi,shmem
        OSOE_PROGRESS_HELPERS=2 case_run b14-two-helpers 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" \
            check mpi,shmem
        case_run b15-tcp-btl 4 $mpirun --mca btl tcp,self -n 4 "$WRAPPER" "$PROGRAM" check \
            mpi,shmem
        case_run b17-shmem-restarted-three-times 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" sessions mpi,shmem
        case_run b18-tcp-progress-thread 4 $mpirun --mca btl tcp,self --mca btl_tcp_progress_thread 1 \
            -n 4 "$WRAPPER" "$PROGRAM" check mpi,shmem
        T44_ROUNDS=5 LIMIT=180 case_run b16-oversubscribed-16 16 $mpirun --oversubscribe -n 16 \
            "$WRAPPER" "$PROGRAM" check mpi,shmem
        ;;
    c)
        mpirun="mpirun --bind-to none"
        T44_NODES=2 case_run c1-mpi-then-shmem-2-nodes 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" \
            check mpi,shmem
        T44_NODES=2 case_run c2-shmem-then-mpi-2-nodes 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" \
            check shmem,mpi
        T44_NODES=4 case_run c3-mpi-then-shmem-4-nodes 4 $mpirun -n 4 "$WRAPPER" "$PROGRAM" \
            check mpi,shmem
        T44_NODES=2 case_run c4-8-pes-2-nodes 8 $mpirun --oversubscribe -n 8 "$WRAPPER" \
            "$PROGRAM" check mpi,shmem
        T44_NODES=2 case_run c5-tcp-btl-2-nodes 4 $mpirun --mca btl tcp,self -n 4 "$WRAPPER" \
            "$PROGRAM" check mpi,shmem
        T44_NODES=2 OSOE_PROGRESS_HELPERS=2 case_run c6-two-helpers-2-nodes 4 $mpirun -n 4 \
            "$WRAPPER" "$PROGRAM" check mpi,shmem
        T44_NODES=2 case_run c9-shmem-restarted-three-times-2-nodes 4 $mpirun -n 4 "$WRAPPER" \
            "$PROGRAM" sessions mpi,shmem
        T44_NODES=4 T44_ROUNDS=5 LIMIT=180 case_run c8-16-pes-4-nodes 16 $mpirun --oversubscribe \
            -n 16 "$WRAPPER" "$PROGRAM" check mpi,shmem
        T44_NODES=2 case_run c7-wrapper-refuses-5-on-2-nodes 5 $mpirun --oversubscribe -n 5 \
            "$WRAPPER" "$PROGRAM" check mpi,shmem
        ;;
    d)
        srun="srun --cpu-bind=none -K1"
        case_run d1-pmix-mpi-then-shmem 4 $srun --mpi=pmix_v5 -n 4 "$PROGRAM" check mpi,shmem
        case_run d2-pmix-shmem-then-mpi 4 $srun --mpi=pmix_v5 -n 4 "$PROGRAM" check shmem,mpi
        case_run d3-pmix-mpi-only 4 $srun --mpi=pmix_v5 -n 4 "$PROGRAM" check mpi
        case_run d4-no-pmix-mpi-then-shmem 4 $srun --mpi=none -n 4 "$PROGRAM" check mpi,shmem
        case_run d5-pmix-shmem-restarted-three-times 4 $srun --mpi=pmix_v5 -n 4 "$PROGRAM" sessions \
            mpi,shmem
        ;;
    esac
done
