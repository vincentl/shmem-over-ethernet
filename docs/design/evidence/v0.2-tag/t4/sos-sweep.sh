#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# sos-sweep.sh: T4 of the 0.2 tag's checks - SOS's test suite (corpus/tests-sos/test), every C test of
# unit/, spec-example/ and apps/ and the C++ ones, built with the tag tree's oshcc (build.sh) and run
# under osoe-launch on four logical nodes of two PEs, the map 0,0,1,1,2,2,3,3, at
# OSOE_PROGRESS_HELPERS=H. Adapted from merge-d/sos-sweep.sh by way of round ten's
# review-ten/conformance/sos.sh; nothing is built in the repository. The caller holds the VM's lock.
#   sos-sweep.sh H
set -u
H=${1:?helpers}
HERE=$(cd "$(dirname "$0")" && pwd)
R=<repo>
TREE=${T4_TREE:-/var/tmp/t4-tree}
W=${T4_WORK:-/var/tmp/t4-sos-h$H}
OUTDIR=${T4_OUT:-$HERE}
O=$OUTDIR/sos-h$H.txt
OSHCC=$TREE/build/release/tools/wrapper/oshcc
LAUNCH=$TREE/build/release/tools/launch/osoe-launch
SOS=${T4_SOS:-$R/corpus/tests-sos/test}
MAP=0,0,1,1,2,2,3,3
export OSOE_PROGRESS_HELPERS=$H
: > "$O"
rm -rf "$W" && mkdir -p "$W"
echo "== T4 SOS, map $MAP, OSOE_PROGRESS_HELPERS=$H, tree $(cat "$TREE/.t4-commit"), $(date -u +%FT%TZ), load $(cut -d' ' -f1-3 /proc/loadavg)" >> "$O"
env | grep -E '^(OSOE|SHMEM|SMA)_' >> "$O"
ls /dev/shm | sort > "$W/shm-before.txt"
build() {   # name compiler-prefix sources-and-flags...
  local p=$1 cc=$2; shift 2
  if ! env $cc $OSHCC -O1 -g -I$SOS/include "$@" -o "$p" -lm -pthread > "$p.build" 2>&1; then
    local why=$(grep -m1 -E "error:|undefined reference" "$p.build" | sed -E 's/^[^:]*:[0-9]+:[0-9]+: //; s/^[^:]*: //' | cut -c1-120)
    printf '%-44s BUILD FAILED  %s\n' "$dir/$p" "${why:-$(tail -1 $p.build | cut -c1-120)}" >> "$O"
    return 1
  fi
}
run() {     # name map
  local p=$1 map=$2 n tag status t0 t1 diag launcher last
  n=$(echo "$map" | tr ',' '\n' | wc -l); tag=$(echo "$map" | tr ',' '-')
  t0=$(date +%s.%N)
  timeout 300 $LAUNCH -n $n -m $map -t 240 -- ./"$p" > "$p.$tag.out" 2>&1 < /dev/null
  status=$?
  t1=$(date +%s.%N)
  diag=$(grep -m1 -E "^osoe: PE [0-9]+: " "$p.$tag.out" | sed -E 's/^osoe: PE [0-9]+: //' | cut -c1-140)
  launcher=$(grep -m1 -E "^osoe-launch: " "$p.$tag.out" | cut -c1-140)
  last=$(grep -v -E "^osoe|^osoe-launch" "$p.$tag.out" | grep -v '^\s*$' | tail -1 | cut -c1-50)
  printf '%-44s %-16s exit %-3s %6.1fs %s | %s\n' "$dir/$p" "$tag" "$status" "$(echo "$t0 $t1" | awk '{print $2 - $1}')" "${diag:-${launcher:--}}" "$last" >> "$O"
}
for dir in unit spec-example apps; do
  echo "== $dir" >> "$O"
  mkdir -p $W/$dir && cp -R $SOS/$dir/. $W/$dir/
  cd $W/$dir
  for src in *.c *.cpp; do
    [ -e "$src" ] || continue
    p=${src%.*}
    case $p in
      mt_lock) continue ;;                      # mt_lock_test's helper
      mt_lock_test) build $p "" $src mt_lock.c || continue ;;
      shmem_ctx) build $p "" -fopenmp $src || continue ;;
      hybrid_mpi_*) continue ;;                 # need MPI
      *) if [ "${src##*.}" = cpp ]; then build $p OSOE_CC=g++ $src || continue
         else build $p "" $src || continue; fi ;;
    esac
    run $p $MAP
    case $p in
      rma_coverage) build ${p}_pshmem "" -DTEST_PSHMEM $src && run ${p}_pshmem $MAP ;;
      query_thread) build ${p}_funneled "" -DENABLE_THREADS $src && run ${p}_funneled $MAP ;;
    esac
  done
done
ls /dev/shm | sort > "$W/shm-after.txt"
{
  echo "== summary (helpers $H, map $MAP)"
  echo "programs built and run: $(awk '$2 ~ /^0-/ {print $1}' "$O" | sort -u | wc -l)"
  echo "exit 0 on the map: $(awk '$2 ~ /^0-/ && $4 == 0' "$O" | wc -l)"
  echo "build failures: $(grep -c 'BUILD FAILED' "$O")"
  echo "runs failing: $(awk '$2 ~ /^0-/ && $4 != 0' "$O" | wc -l)"
  echo "slowest runs: $(awk '$2 ~ /^0-/ {print $5, $1}' "$O" | sort -rn | head -3 | tr '\n' ';')"
  echo "/dev/shm entries new since the start: $(comm -13 "$W/shm-before.txt" "$W/shm-after.txt" | wc -l)"
} >> "$O"
# The failures, with what each printed, for the report.
F=$OUTDIR/sos-h$H.failures.txt
: > "$F"
awk '$2 ~ /^0-/ && $4 != 0 {print $1, $2}' "$O" | while read -r name tag; do
  d=${name%%/*}; p=${name#*/}
  {
    echo "=== $name $tag"
    grep -E "^osoe|^osoe-launch" "$W/$d/$p.$tag.out" | head -8 | cut -c1-300
    echo "--- last lines"
    tail -4 "$W/$d/$p.$tag.out" | cut -c1-200
  } >> "$F"
done
grep 'BUILD FAILED' "$O" | while read -r name rest; do
  d=${name%%/*}; p=${name#*/}
  { echo "=== $name BUILD FAILED"; grep -m3 -E "error:|undefined reference" "$W/$d/$p.build" | cut -c1-300; } >> "$F"
done
# Keep every program's output and build log beside this record.
rm -rf "$OUTDIR/sos-h$H" && mkdir -p "$OUTDIR/sos-h$H"
( cd "$W" && find . \( -name '*.out' -o -name '*.build' \) -print0 | tar --null -T - -cf - ) | tar -C "$OUTDIR/sos-h$H" -xf -
echo DONE >> "$O"
