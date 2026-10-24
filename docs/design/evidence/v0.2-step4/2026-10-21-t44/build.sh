#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
#
# T44: the program built with osoe's oshcc and Open MPI's mpicc, and the facts
# of the machine it ran on.
#
#   build.sh TREE OUT
#
# TREE is an osoe tree built with the release preset; OUT is where the program
# and the notes go. Under the VM's lock (/tmp/osoe-vm.lock), as every build
# there is.
set -u
TREE=$1
OUT=$2
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$OUT"
OSHCC=$TREE/build/release/tools/wrapper/oshcc

# OSOE_CC names the compiler oshcc runs: Open MPI's wrapper, which runs the C
# compiler with Open MPI's flags. oshcc puts osoe's -I first; whether mpicc keeps
# it first is what the second line shows.
export OSOE_CC=mpicc
{
    echo "# oshcc, as it would run mpicc:"
    "$OSHCC" -show -O2 -g -std=gnu11 -Wall -Wextra -o "$OUT/mpi-and-shmem" "$HERE/mpi-and-shmem.c"
    echo "# and mpicc, as it would run the C compiler:"
    # shellcheck disable=SC2046
    mpicc $("$OSHCC" -show -O2 -g -std=gnu11 -Wall -Wextra -o "$OUT/mpi-and-shmem" \
        "$HERE/mpi-and-shmem.c" | cut -d' ' -f2-) -showme
} > "$OUT/build-commands.txt" 2>&1
cat "$OUT/build-commands.txt"

"$OSHCC" -O2 -g -std=gnu11 -Wall -Wextra -o "$OUT/mpi-and-shmem" "$HERE/mpi-and-shmem.c" \
    > "$OUT/build.txt" 2>&1
status=$?
echo "build: exit $status, $(grep -c 'warning:' "$OUT/build.txt") warnings"
[ "$status" -eq 0 ] || { cat "$OUT/build.txt"; exit "$status"; }

# The baseline: the same source, no Open MPI.
(
    unset OSOE_CC
    "$OSHCC" -O2 -g -std=gnu11 -Wall -Wextra -DT44_WITHOUT_MPI -I"$HERE" -o "$OUT/shmem-without-mpi" \
        "$HERE/mpi-and-shmem.c" > "$OUT/build-without-mpi.txt" 2>&1
)
echo "baseline build: exit $?, $(grep -c 'warning:' "$OUT/build-without-mpi.txt") warnings"
ldd "$OUT/shmem-without-mpi" > "$OUT/link-facts-without-mpi.txt" 2>&1

# The other order, Open MPI's -I first: which shmem.h the compiler finds, and whether the
# program compiles against it.
{
    # shellcheck disable=SC2046
    gcc -std=gnu11 -Wall -Wextra -fsyntax-only -H $(mpicc -showme:compile) -I"$TREE/shmem/include" \
        "$HERE/mpi-and-shmem.c" 2>&1 | grep -E '/shmem\.h$|error:|warning:' | head -20
} > "$OUT/build-wrong-order.txt" 2>&1
echo "Open MPI's include first: $(grep -c 'error:' "$OUT/build-wrong-order.txt") errors; the shmem.h found:" \
    "$(grep -m1 '/shmem\.h$' "$OUT/build-wrong-order.txt")"

# What the linker did with the program.
{
    echo "# ldd"
    ldd "$OUT/mpi-and-shmem"
    echo "# sections that hold the statics, and the copies of shared libraries' variables"
    readelf -SW "$OUT/mpi-and-shmem" | grep -E 'osoe_statics|\.dynbss|\.data|\.bss' || true
    echo "# MPI's variables that the program refers to, and where the linker put them"
    nm -D "$OUT/mpi-and-shmem" | grep -E ' [BDV] ' || true
    readelf -rW "$OUT/mpi-and-shmem" | grep COPY || true
    echo "# addresses: the statics' ends, and Open MPI's predefined handles"
    nm "$OUT/mpi-and-shmem" | grep -E '__osoe_statics_(data|bss)_(start|end)|ompi_mpi_comm_world|ompi_mpi_int$|ompi_mpi_double$|ompi_mpi_op_sum$|static_big|release_flag' || true
    echo "# file type"
    file "$OUT/mpi-and-shmem"
} > "$OUT/link-facts.txt" 2>&1
cat "$OUT/link-facts.txt"
