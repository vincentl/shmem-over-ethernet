set -x
cd /tmp/omx && gcc -O -o sib sib.c && ./sib
cat /proc/sys/kernel/yama/ptrace_scope
ulimit -l; ulimit -n; ulimit -Hn
grep -i huge /proc/meminfo | head -3; mount | grep -i huge
zcat /proc/config.gz 2>/dev/null | grep VA_BITS || grep VA_BITS /boot/config-$(uname -r)
cat /proc/sys/vm/max_map_count
