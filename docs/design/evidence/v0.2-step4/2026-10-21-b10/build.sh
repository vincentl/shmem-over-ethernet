#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# B10: build what the experiment runs, in the osoe development VM, under the
# VM's lock. The tree is main's, copied into /tmp/b10 with
#
#   git -C <repo> archive --prefix=b10/ main \
#     | limactl shell osoe -- bash -c 'cd /tmp && rm -rf b10 && tar xf -'
#
# and the files of this directory are in /tmp/b10-out. Only the library, the
# launcher and osoe-stat are built (the release preset's own configuration and
# flags): the experiment runs no test of the suite. late-pe.c is compiled with
# the build tree's oshcc. The last line of /tmp/b10-out/build.status says how it
# ended.
set -u
exec 9> /tmp/osoe-vm.lock
if ! flock -w 1800 9; then
    echo "BUILD FAILED: no lock in 1800 s" > /tmp/b10-out/build.status
    exit 3
fi
cd /tmp/b10 || exit 2
if ! cmake --preset release > /tmp/b10-out/build-configure.txt 2>&1; then
    echo "BUILD FAILED: configure" > /tmp/b10-out/build.status
    exit 1
fi
if ! cmake --build build/release -j4 --target osoe osoe-launch osoe-stat \
    > /tmp/b10-out/build-compile.txt 2>&1; then
    echo "BUILD FAILED: compile" > /tmp/b10-out/build.status
    exit 1
fi
if ! build/release/tools/wrapper/oshcc -O2 -Wall -Wextra -o build/release/late-pe \
    /tmp/b10-out/late-pe.c > /tmp/b10-out/build-late-pe.txt 2>&1; then
    echo "BUILD FAILED: late-pe" > /tmp/b10-out/build.status
    exit 1
fi
echo "built $(date +%s)" > /tmp/b10-out/build.status
