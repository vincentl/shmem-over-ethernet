#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine's kit fixes, in the osoe VM: R2 - which plugin slurmd -C crashes
# in. The installed plugin directory, copied less a set of plugins, bound over
# the installed one in a private mount namespace (nothing outside changes),
# and slurmd -C run there: its status, its node line, and a backtrace where it
# crashes. Under the VM's lock.
# usage: vm-r2-probe.sh
set -u
T=/tmp/kit-fixes/r2
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
PLUGINS=$(dirname "$(dpkg -L slurm-wlm-basic-plugins | grep '/select_cons_tres.so$')")
echo "# $(date -u +%FT%TZ) $PLUGINS: $(ls "$PLUGINS" | grep -c 'gpu') gpu plugins: $(ls "$PLUGINS" | grep gpu | tr '\n' ' ')"
rm -rf "$T"; mkdir -p "$T"
cat > "$T/in-namespace.sh" <<'NAMESPACE'
# In a private mount namespace: COPY bound over PLUGINS, then slurmd -C, and a
# backtrace if asked.
COPY=$1; PLUGINS=$2; TRACE=$3
mount --bind "$COPY" "$PLUGINS" || exit 1
/usr/sbin/slurmd -C > /tmp/kit-fixes/r2/out 2> /tmp/kit-fixes/r2/err
status=$?
echo "  status $status; stdout: $(head -1 /tmp/kit-fixes/r2/out | cut -c1-120)"
sed 's/^/  stderr | /' /tmp/kit-fixes/r2/err | head -4
if [ "$TRACE" = trace ] && [ "$status" -ne 0 ]; then
    env -u DEBUGINFOD_URLS gdb -q -batch -iex 'set debuginfod enabled off' -ex run -ex bt \
        -ex 'info sharedlibrary' --args /usr/sbin/slurmd -C 2>&1 |
        grep -E '^#|SIGSEGV|gpu_|rocm|nvidia|nvml|libslurm' | head -30 | sed 's/^/  gdb | /'
fi
NAMESPACE
variant() {   # NAME TRACE EXCLUDED...: slurmd -C with the plugins matching any EXCLUDED pattern left out
    local name=$1 trace=$2
    shift 2
    local copy=$T/$name
    mkdir -p "$copy"
    for f in "$PLUGINS"/*; do
        local b skip=
        b=$(basename "$f")
        for pattern in "$@"; do case $b in $pattern) skip=1 ;; esac; done
        [ -n "$skip" ] || cp -a "$f" "$copy/"
    done
    echo "== $name: $(ls "$copy" | wc -l) of $(ls "$PLUGINS" | wc -l) plugins, without: $*"
    sudo unshare -m --propagation private bash "$T/in-namespace.sh" "$copy" "$PLUGINS" "$trace" 2>&1
    rm -rf "$copy"
}
echo "== as installed"
/usr/sbin/slurmd -C > "$T/out" 2> "$T/err"; echo "  status $?; stdout: $(head -1 "$T/out" | cut -c1-120)"
variant without-rsmi trace gpu_rsmi.so
variant without-nvidia-rsmi no gpu_rsmi.so gpu_nvidia.so
variant without-gpu-plugins trace 'gpu_*.so'
variant without-gpu-and-gres-gpu no 'gpu_*.so' gres_gpu.so
variant without-nrt-nvidia-rsmi no gpu_rsmi.so gpu_nvidia.so gpu_nrt.so
variant without-generic-rsmi no gpu_rsmi.so gpu_generic.so
echo "  outside the namespace afterwards: $(ls "$PLUGINS" | wc -l) entries"
rm -rf "$T"
echo "# done $(date -u +%FT%TZ)"
