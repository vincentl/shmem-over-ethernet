#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# The 0.2 tag's Pi runs on the tree to be tagged (main 0edd95a9), after the rehearsal on 5cefc7fb
# has ended by itself: its outputs on the Pi kept, then part a, the README's commands and quick
# start (T11, T12), T10's timing, and the hour of mix (part d's first half; its threads hour ran in
# the rehearsal, on the same code but for the version).
set -u
OLD=<repo>/build/scratch/pi-tag-5cefc7fb
HERE=<repo>/build/scratch/pi-tag-0edd95a9
cd $HERE || exit 1
LOG=$HERE/sequence.txt; : > $LOG
until grep -q "^DONE" $OLD/rehearsal.txt; do sleep 30; done
echo "the rehearsal ended: $(tr '\n' ';' < $OLD/rehearsal.txt)" >> $LOG
rsync -a -e 'ssh -o HostKeyAlias=pi-login.local' 'pi-login:osoe/build/release-run/' $OLD/remote-release-run/ && echo "the rehearsal's outputs on the Pi kept" >> $LOG
echo "start $(date +%H:%M:%S)" >> $LOG
bash pi-a.sh $HERE/tree; echo "part a: $? $(date +%H:%M:%S)" >> $LOG
bash pi-readme.sh; echo "readme: $? $(date +%H:%M:%S)" >> $LOG
bash pi-t10.sh; echo "t10: $? $(date +%H:%M:%S)" >> $LOG
bash pi-mix.sh; echo "mix: $? $(date +%H:%M:%S)" >> $LOG
echo "DONE $(date +%H:%M:%S)" >> $LOG
