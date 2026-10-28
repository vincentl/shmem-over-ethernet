#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# How long each data wire made on demand at 64 logical nodes waited (H23,
# 2026-10-27): from tier3-64.sh's stderr files, the SHMEM_DEBUG line each lead
# writes for each wire it makes on demand, "N us after its first need" - at the
# end that dialled, and at an end that asked its peer to dial - by job.
#   wire-waits.sh DIRECTORY
set -u
cd "$1" || exit 1
for f in barrier-on-demand-1 barrier-on-demand-2 ring-on-demand-1 ring-on-demand-2; do
    for kind in 'it dialled' 'the peer dialled, as asked'; do
        grep "on demand: $kind, " "$f.err" | grep -o '[0-9]* us after' | awk '{ print $1 }' |
            sort -n | awk -v f="$f" -v k="$kind" '{ a[NR] = $1 }
                END { if (NR) printf "%s [%s]: n=%d min=%d median=%d p90=%d max=%d us\n",
                      f, k, NR, a[1], a[int((NR + 1) / 2)], a[int(NR * 0.9 + 0.5)], a[NR] }'
    done
    echo "  $f [the peer dialled for its own need]: n=$(grep -c 'on demand: the peer dialled for its own need' "$f.err")"
done
