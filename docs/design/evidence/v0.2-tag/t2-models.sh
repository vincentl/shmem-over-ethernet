#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T2, a check of the 0.2 tag: the six SHMEM_THREAD_MULTIPLE models, opt-in
# (-DOSOE_MULTIPLE_MODELS=ON), last run at 0.1.0's tag - dirty_set, get_completion,
# watch_list, landing_extent, copy_out_claim and pe_quiet - on the tree given, in the osoe VM.
#   t2-models.sh TAR NAME
set -u
TAR=$1 NAME=$2
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=/var/tmp/$NAME
SUMMARY=$HERE/$NAME.summary
exec 9>/tmp/osoe-vm.lock
flock -w 7200 9 || { echo "the VM's lock was not had in two hours"; exit 99; }
: > "$SUMMARY"
rm -rf "$TREE" && mkdir -p "$TREE" && tar -xf "$TAR" -C "$TREE" && cd "$TREE" || exit 2
cmake --preset release -DOSOE_MULTIPLE_MODELS=ON > configure.txt 2>&1 || { echo "configure failed" | tee -a "$SUMMARY"; exit 3; }
start=$(date +%s)
ctest --test-dir build/release -R '^model\.(dirty_set|get_completion|watch_list|landing_extent|copy_out_claim|pe_quiet)$' \
      -j2 --timeout 3600 --output-on-failure > models.txt 2>&1
echo "$(date -u +%FT%TZ) T2 on $(basename "$TAR"): $(grep -E 'tests passed' models.txt) in $(( ($(date +%s) - start) / 60 )) min" | tee -a "$SUMMARY"
grep -E 'Passed|Failed|Timeout' models.txt | sed 's/^ *[0-9]*\/[0-9]* //' | tee -a "$SUMMARY"
