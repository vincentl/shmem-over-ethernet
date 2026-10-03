#!/bin/sh
# SPDX-License-Identifier: LGPL-3.0-or-later
# Transitive project-header closure of every .c file in shmem/src, and which
# SHMEM-layer files (those implementing pshmem_* entry points, or named by the
# review brief) reach an AM-layer header. Run from the repository root.
cd shmem/src || exit 1
AM_HEADERS="am.h connection.h connection-area.h wire.h reply.h progress.h progress-host.h"
closure() {
    seen=""
    todo="$1"
    while [ -n "$todo" ]; do
        set -- $todo; f=$1; shift; todo="$*"
        case " $seen " in *" $f "*) continue;; esac
        seen="$seen $f"
        [ -f "$f" ] || continue
        for h in $(grep -h '^#include "' "$f" | sed 's/#include "\(.*\)"/\1/' | grep -v '^base/'); do
            todo="$todo $h"
        done
    done
    echo "$seen" | tr ' ' '\n' | grep '\.h$' | sort | tr '\n' ' '
}
echo "## direct includes"
for f in *.c; do
    echo "$f: $(grep -h '^#include "' "$f" | sed 's/#include "\(.*\)"/\1/' | tr '\n' ' ')"
done
echo
echo "## transitive closure, AM-layer headers reached"
for f in *.c; do
    c=$(closure "$f")
    hit=""
    for a in $AM_HEADERS; do
        case " $c " in *" $a "*) hit="$hit $a";; esac
    done
    api=$(grep -c 'OSOE_API' "$f")
    echo "$f (OSOE_API entry points: $api): ${hit:- none}"
done
