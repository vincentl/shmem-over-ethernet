#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §6: Part 0's rungs that a setting switches, between the
# list's first two nodes, two PEs each: hol - a fetch-add's round trip alone and
# behind a 1 MiB put stream on the same connection, and the stream's rate - at
# each case below, ROUNDS (3) times in another order each round.
#   unsent:  OSOE_WIRE_UNSENT_BYTES unset (the rule's, which SHMEM_DEBUG reports),
#            0 (the system's), 64 KiB, 256 KiB, 1 MiB, 4 MiB
#   buffer:  OSOE_AM_BUFFER 128 KiB, 256 KiB (the default), 1 MiB, 4 MiB
#   receive: OSOE_WIRE_BYTES x OSOE_WIRE_BUFFERS 16 KiB x 64 (the default),
#            64 KiB x 64, 16 KiB x 256
# Every case under SHMEM_DEBUG, whose start-up line says the unsent limit its
# own link measurement gave. The default case, buffer-256k and receive-16kx64,
# is unsent-rule's; it runs once a round among the eleven.
# usage: ladder.sh NODES OUTDIR
: "${KIT_TIMEOUT:=120}"
source "$(dirname "$0")/common.sh"
kit_start "$1" "$2"
ROUNDS=${ROUNDS:-3}
HOL_SECONDS=${HOL_SECONDS:-10}
CASES="unsent-rule unsent-0 unsent-64k unsent-256k unsent-1m unsent-4m
       buffer-128k buffer-1m buffer-4m receive-64kx64 receive-16kx256"
settings() {
    case $1 in
        unsent-rule)     echo ;;
        unsent-0)        echo OSOE_WIRE_UNSENT_BYTES=0 ;;
        unsent-64k)      echo OSOE_WIRE_UNSENT_BYTES=65536 ;;
        unsent-256k)     echo OSOE_WIRE_UNSENT_BYTES=262144 ;;
        unsent-1m)       echo OSOE_WIRE_UNSENT_BYTES=1048576 ;;
        unsent-4m)       echo OSOE_WIRE_UNSENT_BYTES=4194304 ;;
        buffer-128k)     echo OSOE_AM_BUFFER=131072 ;;
        buffer-1m)       echo OSOE_AM_BUFFER=1048576 ;;
        buffer-4m)       echo OSOE_AM_BUFFER=4194304 ;;
        receive-64kx64)  echo OSOE_WIRE_BYTES=65536 OSOE_WIRE_BUFFERS=64 ;;
        receive-16kx256) echo OSOE_WIRE_BYTES=16384 OSOE_WIRE_BUFFERS=256 ;;
    esac
}
for ((round = 1; round <= ROUNDS; round++)); do
    for c in $(rotate "$(echo $CASES)" "$round"); do
        # shellcheck disable=SC2046
        KIT_SRUN="$KIT_SRUN $(pair)" launch 2 2 "$OUT/hol-$c-r$round.txt" SHMEM_DEBUG=1 \
            $(settings "$c") -- "$KIT_BIN/hol" "$HOL_SECONDS"
    done
done
echo "# done $(date -u +%H:%M:%S)" >> "$LOG"
