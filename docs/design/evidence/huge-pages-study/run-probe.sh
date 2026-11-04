#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Runs the probe under each setting, restoring the VM's defaults after.
set -u
cd /var/tmp/osoe-huge/probe || exit 1
gcc -O2 -o probe shmem-huge-probe.c || exit 1
S=/sys/kernel/mm/transparent_hugepage/shmem_enabled
restore() { echo never | sudo -n tee $S >/dev/null; sudo -n mount -o remount,huge=never /dev/shm; }
trap restore EXIT
echo "== 1. defaults: shmem_enabled=$(cat $S | tr -d '\n'), /dev/shm $(mount | grep ' /dev/shm ' | sed 's/.*(//')"
./probe advise; ./probe none
echo "== 2. sysfs shmem_enabled=advise, /dev/shm as mounted"
echo advise | sudo -n tee $S >/dev/null; cat $S
./probe advise; ./probe advise memfd; ./probe none memfd
echo never | sudo -n tee $S >/dev/null
echo "== 3. /dev/shm remounted huge=advise, sysfs never"
sudo -n mount -o remount,huge=advise /dev/shm; mount | grep ' /dev/shm '
./probe advise; ./probe none
echo "== 4. /dev/shm remounted huge=within_size"
sudo -n mount -o remount,huge=within_size /dev/shm; mount | grep ' /dev/shm '
./probe none
echo "== 5. /dev/shm remounted huge=always"
sudo -n mount -o remount,huge=always /dev/shm; mount | grep ' /dev/shm '
./probe none
restore; trap - EXIT
echo "== restored: shmem_enabled=$(cat $S | tr -d '\n'); $(mount | grep ' /dev/shm ')"
grep -i "ShmemHuge\|HugePages_Total" /proc/meminfo
