#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
T=${1:?tree}; O=${2:?outdir}; E=$(dirname "$0"); mkdir -p $O
for p in ring sleeper fork-in-pe env-size; do
  gcc -std=c11 -D_GNU_SOURCE -O1 -g -I$T/shmem/include $E/$p.c -o $O/$p -L$T/build/debug/shmem -losoe -Wl,-rpath,$T/build/debug/shmem || exit 1
done
