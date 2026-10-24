#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# T44: the machine, the Open MPI and the Slurm the runs were made on.
#
#   facts.sh TREE
set -u
TREE=$1
echo "# $(date -u +%FT%TZ)"
uname -a
grep PRETTY_NAME /etc/os-release
echo "CPUs: $(nproc); $(free -m | awk '/^Mem:/ { print $2 " MiB" }'); /dev/shm: $(df -h /dev/shm | awk 'NR == 2 { print $2 }')"
echo "ptrace_scope: $(cat /proc/sys/kernel/yama/ptrace_scope 2>/dev/null)"
echo "max locked memory: $(ulimit -l) KiB; open files: $(ulimit -n)"
gcc --version | head -1
echo "# osoe"
"$TREE/build/release/tools/launch/osoe-launch" --version
echo "# Open MPI"
mpirun --version 2>&1 | head -3
ompi_info 2>&1 | grep -E '^ +(Open MPI|Thread support|C compiler:)' | sed 's/^ *//'
dpkg -l openmpi-bin libopenmpi-dev libopenmpi40 openmpi-common libpmix2t64 libucx0 libevent-2.1-7t64 \
    libhwloc15 2>/dev/null | awk '/^ii/ { print $2, $3 }'
echo "# /etc/openmpi/openmpi-mca-params.conf, without comments"
grep -v '^#' /etc/openmpi/openmpi-mca-params.conf | grep -v '^$'
echo "# mpicc"
mpicc -showme
echo "# what the package puts on the PATH under osoe's names"
ls /usr/bin | grep -E '^(osh|shmem)' | tr '\n' ' '
echo
type -a oshcc oshrun 2>&1 | tr '\n' ' '
echo
oshcc --showme 2>&1 | head -1
ls /usr/lib/aarch64-linux-gnu/openmpi/include | grep -E 'shmem|openshmem' | tr '\n' ' '
echo
echo "# Slurm"
srun --version
srun --mpi=list 2>&1 | tr '\n' ' '
echo
sinfo -h -o '%P %a %D %t %c'
echo "# the host's name"
hostname
getent hosts "$(hostname)"
