#!/bin/sh
# Run inside the VM as a user with sudo. A private mount namespace gets a 64 MiB
# /dev/shm; nothing outside the namespace sees it.
T=${1:?}; E=$(cd $(dirname "$0") && pwd); U=$(id -un)
gcc -std=c11 -D_GNU_SOURCE -O1 -g -I$T/shmem/include $E/heap-overcommit.c -o /tmp/omx/heap-overcommit -L$T/build/debug/shmem -losoe -Wl,-rpath,$T/build/debug/shmem || exit 1
sudo unshare -m sh -c "mount -t tmpfs -o size=64m,mode=1777 tmpfs /dev/shm && sudo -u $U env SHMEM_SYMMETRIC_SIZE=48m OSOE_WATCHDOG=0 $T/build/debug/tools/launch/osoe-launch -n 3 -m 0,1,1 -t 30 -- /tmp/omx/heap-overcommit; echo launcher rc=\$?; df -h /dev/shm | tail -1"
