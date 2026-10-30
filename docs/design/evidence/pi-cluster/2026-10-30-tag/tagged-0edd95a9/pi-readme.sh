#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# T11 and T12 from the Mac: the tree to be tagged (git archive of main) and pi-readme-remote.sh
# copied under ~/osoe/build/release-run, the script run on the head node in one ssh session, and
# its outputs brought back to readme/.
set -u
HERE=<repo>/build/scratch/pi-tag-0edd95a9
OUT=$HERE/readme; mkdir -p $OUT
PI="ssh -o ConnectTimeout=20 -o ServerAliveInterval=30 pi-login"
$PI 'mkdir -p osoe/build/release-run' || exit 1
scp -q <repo>/build/scratch/tag-checks/tree-tag.tar pi-login:osoe/build/release-run/tagged.tar || exit 1
scp -q $HERE/pi-readme-remote.sh pi-login:osoe/build/release-run/pi-readme-remote.sh || exit 1
$PI 'bash osoe/build/release-run/pi-readme-remote.sh'
rsync -a -e 'ssh -o HostKeyAlias=pi-login.local' --include='*.txt' --include='*.out' --include='*.sh' --include='*.c' --exclude='*' \
    'pi-login:osoe/build/release-run/readme/' $OUT/
