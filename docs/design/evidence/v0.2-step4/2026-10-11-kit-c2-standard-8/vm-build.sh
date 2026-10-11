#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The kit for c2-standard-8, in the osoe VM: the tree (git archive of the
# branch kit-c2-standard-8, TAR) unpacked into /tmp/kit-c2/src and built by the
# kit's own build.sh under osoe-launch, as the runbook's dry run builds it - the
# release preset and the kit's probes - then the scripts' syntax. Under the
# VM's lock; the status is the first failure's.
# usage: vm-build.sh TAR COMMIT LOGDIR
set -u
TAR=$1; COMMIT=$2; LOGDIR=$3
T=/tmp/kit-c2
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
echo "# lock taken $(date -u +%FT%TZ); $(cut -d' ' -f1-3 /proc/loadavg)"
rm -rf "${T:?}/src"
mkdir -p "$T/src" "$LOGDIR"
tar -C "$T/src" -xf "$TAR" || exit 1
cd "$T/src" || exit 1
printf 'one\n' > "$T/one-node"
KIT_COMMIT=$COMMIT KIT_LAUNCHER=osoe-launch OUT="$T/build-log" \
    bash docs/fast-link/build.sh "$T/one-node" > "$LOGDIR/build.txt" 2>&1
status=$?
echo "build.sh: $status" | tee -a "$LOGDIR/build.txt"
[ "$status" -eq 0 ] || exit "$status"
echo "warnings in the release build: $(grep -c 'warning:' build-release.txt)"
for s in docs/fast-link/*.sh; do bash -n "$s" || { echo "$s: syntax"; exit 1; }; done
python3 -m py_compile docs/fast-link/summarize.py || exit 1
echo "the scripts parse"
echo "# done $(date -u +%FT%TZ)"
