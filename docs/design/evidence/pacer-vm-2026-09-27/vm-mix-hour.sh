#!/bin/bash
# vm-mix-hour.sh: an hour of mix on the VM, the four smoke maps' largest
# (0,0,0,1,1,2), from phase 4's verified tree (64d8d39) in wt/pacer4-verify,
# release; after verify8.sh, under the VM's lock.
until grep -q DONE <repo>/build/scratch/pacer/mutate4.txt 2>/dev/null; do sleep 30; done
exec 9>/tmp/osoe-vm.lock; flock -w 14400 9 || exit 1
T=<repo>/build/scratch/wt/pacer4-verify/build/release
O=<repo>/build/scratch/pacer/vm-mix-hour.txt
echo "$(date +%T) start" > $O.summary
$T/tools/launch/osoe-launch -n 6 -m 0,0,0,1,1,2 -t 3900 -- $T/tools/pacer/osoe-pacer \
    --seconds 3600 --fold-nodes mix > $O 2>&1
echo "$(date +%T) exit $?: $(grep '^osoe-pacer failures=' $O)" >> $O.summary
echo DONE >> $O.summary
flock -u 9
