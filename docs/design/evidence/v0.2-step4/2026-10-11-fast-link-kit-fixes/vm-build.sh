#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine's kit fixes, in the osoe VM: the tree (git archive of the branch
# kit-fixes, TAR) unpacked into /tmp/kit-fixes/src and built by the kit's own
# build.sh under osoe-launch, as the runbook's dry run builds it - the release
# preset and the kit's probes, with the build's oshcc, in build/release/kit -
# then the changed probe compiled again with every warning an error, the check
# probe (make-check-probe.py) beside it, and the changed scripts' syntax. Under
# the VM's lock; the status is the first failure's.
# usage: vm-build.sh TAR COMMIT LOGDIR
set -u
TAR=$1; COMMIT=$2; LOGDIR=$3
HERE=$(cd "$(dirname "$0")" && pwd)
T=/tmp/kit-fixes
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
cp build-release.txt "$LOGDIR/build-release.txt"
echo "warnings in the release build: $(grep -c 'warning:' build-release.txt)"
OSHCC=build/release/tools/wrapper/oshcc
# -Wformat-truncation apart: thread_cpu's three snprintf of a task's path warn
# under it, in main's multistream.c as in this one (shown below).
for p in multistream fan-in get-pieces hol startup; do
    "$OSHCC" -O2 -Wall -Wextra -Wno-format-truncation -Werror -o "$T/$p-strict" "docs/fast-link/$p.c" \
        || { echo "$p: warnings"; exit 1; }
done
echo "the kit's probes compile with -Wall -Wextra -Werror, -Wformat-truncation apart"
"$OSHCC" -O2 -Wall -Wextra -o "$T/multistream-truncation" docs/fast-link/multistream.c 2>&1 |
    grep -c 'warning:' | sed 's/^/warnings in multistream.c, -Wall -Wextra: /'
"$OSHCC" -O2 -Wall -Wextra -o "$T/multistream-main" "$HERE/multistream-main.c" 2>&1 |
    grep -c 'warning:' | sed 's/^/warnings in main'"'"'s multistream.c (7d1d868), -Wall -Wextra: /'
python3 "$HERE/make-check-probe.py" docs/fast-link/multistream.c "$T/multistream-check.c" || exit 1
"$OSHCC" -O2 -Wall -Wextra -Wno-format-truncation -Werror -o build/release/kit/multistream-check \
    "$T/multistream-check.c" || { echo "the check probe did not compile"; exit 1; }
echo "the check probe built"
for s in docs/fast-link/*.sh; do bash -n "$s" || { echo "$s: syntax"; exit 1; }; done
python3 -m py_compile docs/fast-link/summarize.py || exit 1
echo "the scripts parse"
ls -la build/release/kit
echo "# done $(date -u +%FT%TZ)"
