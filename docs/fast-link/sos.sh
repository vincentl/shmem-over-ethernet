#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §10: the SOS test suite's programs, for scale.sh's
# SOS_BUILD, built on this machine - a head node, while the nodes run the facts
# and the suite - and copied to the same path, DIR, on every node of the list.
# The sources are SOS_SOURCE, the reading corpus's tests-sos, fetched by
# tools/corpus/fetch.sh into OSOE_CORPUS (~/osoe-corpus, outside the tree, so
# that no copy of the tree carries it) where they are not there yet, and never
# built in place: test/'s unit/, spec-example/ and apps/ are copied into DIR
# with its include/, and each program built alone, beside its source, by this
# tree's oshcc, as docs/pi-cluster.md §8 builds them - a C++ one with g++ as
# its compiler, mt_lock_test with its helper, shmem_ctx with OpenMP, the MPI
# ones and the helper left out. DIR/build.txt has a line for each, built or not,
# and each program's compiler output is beside it (.build). The oshcc is this
# machine's release build of the library alone (the osoe target), configured
# here unless it is: a program finds the library by the build's path, which
# build.sh gives every node. The build's parallel jobs are JOBS (this
# machine's CPUs). On one machine (KIT_LAUNCHER=osoe-launch) nothing is copied.
# usage: sos.sh NODES DIR
: "${KIT_TIMEOUT:=1200}"
source "$(dirname "$0")/common.sh"
[ $# -eq 2 ] || { echo "usage: sos.sh NODES DIR" >&2; exit 2; }
NODES_FILE=$1
mkdir -p "$2" || exit 1
DIR=$(cd "$2" && pwd)
case $DIR in / | "$HOME" | "$REPOSITORY" | "$REPOSITORY/build") echo "sos.sh: not into $DIR" >&2; exit 2 ;; esac
OSOE_CORPUS=${OSOE_CORPUS:-$HOME/osoe-corpus}
SOS_SOURCE=${SOS_SOURCE:-$OSOE_CORPUS/tests-sos}
JOBS=${JOBS:-$(nproc)}
say() { echo "$(date -u +%H:%M:%S) sos.sh: $*"; }

if [ ! -d "$SOS_SOURCE/test/unit" ]; then
    say "fetching tests-sos into $OSOE_CORPUS"
    OSOE_CORPUS=$OSOE_CORPUS timeout -k 30 600 sh "$REPOSITORY/tools/corpus/fetch.sh" tests-sos ||
        { say "the fetch failed: no SOS programs"; exit 1; }
fi
OSHCC=$KIT_BUILD/tools/wrapper/oshcc
cd "$REPOSITORY" || exit 1
if [ ! -f "$KIT_BUILD/CMakeCache.txt" ]; then
    say "configuring the release build here, for oshcc"
    cmake --preset release > "$DIR.configure.txt" 2>&1 ||
        { say "the configure failed:"; tail -5 "$DIR.configure.txt"; exit 1; }
fi
say "building the library here"
cmake --build "$KIT_BUILD" --target osoe -j"$JOBS" > "$DIR.library.txt" 2>&1 ||
    { say "the library's build failed:"; grep -E 'error|Error' "$DIR.library.txt" | head -5; exit 1; }
[ -x "$OSHCC" ] || { say "no $OSHCC"; exit 1; }

find "$DIR" -mindepth 1 -delete
for d in unit spec-example apps include; do
    cp -R "$SOS_SOURCE/test/$d" "$DIR/$d" || exit 1
done
cd "$DIR" || exit 1
# One program, built from SOURCE beside it: its line, built or not.
build_one() {
    local src=$1 p=${1%.*} compiler= extra=
    case $p in
        unit/mt_lock|spec-example/hybrid_mpi_*) return 0 ;;   # a helper; MPI programs
        unit/mt_lock_test) extra=unit/mt_lock.c ;;
        spec-example/shmem_ctx) extra=-fopenmp ;;
    esac
    case $src in *.cpp) compiler=OSOE_CC=g++ ;; esac
    # shellcheck disable=SC2086
    if env $compiler "$OSHCC" -O1 -Iinclude "$src" $extra -o "$p" -lm -pthread > "$p.build" 2>&1; then
        echo "$p: built"
    else
        echo "$p: build failed"
    fi
}
export -f build_one
export OSHCC
say "building the programs, $JOBS at once"
printf '%s\n' unit/*.c unit/*.cpp spec-example/*.c apps/*.c | grep -v '\*' |
    xargs -P "$JOBS" -I{} bash -c 'build_one "$1"' _ {} | sort > build.txt
say "$(grep -c ': built$' build.txt) built, $(grep -c 'build failed' build.txt) not"

[ "$KIT_LAUNCHER" = osoe-launch ] && exit 0
# Every node's host key taken on first contact, as slurm-setup.sh takes them,
# which may still be running beside this.
SSH="ssh -o StrictHostKeyChecking=accept-new -o BatchMode=yes"
status=0
for n in $(grep -vxF "$(hostname -s)" "$NODES_FILE"); do
    $SSH -n "$n" mkdir -p "$DIR" && rsync -a --delete -e "$SSH" "$DIR"/ "$n:$DIR"/ ||
        { say "copying to $n failed"; status=1; }
done
[ "$status" -eq 0 ] && say "copied to every node of $NODES_FILE"
exit "$status"
