#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Round nine's kit fixes, in the osoe VM: R2 - which packages' absence lets
# slurmd -C run. For each set of packages, their plugins taken out of a copy of
# the plugin directory bound over the installed one and their other files under
# /usr/lib covered by /dev/null, in a private mount namespace; then slurmd -C.
# And apt's simulation of the removals the kit could advise. Under the lock.
set -u
T=/tmp/kit-fixes/r2p
exec 9>/tmp/osoe-vm.lock
flock -w 43200 9 || { echo "no lock"; exit 1; }
PLUGINS=$(dirname "$(dpkg -L slurm-wlm-basic-plugins | grep '/select_cons_tres.so$')")
rm -rf "$T"; mkdir -p "$T"
cat > "$T/in-namespace.sh" <<'NAMESPACE'
T=$1; PLUGINS=$2
mount --bind "$T/plugins" "$PLUGINS" || exit 1
while read -r f; do mount --bind /dev/null "$f"; done < "$T/libraries"
/usr/sbin/slurmd -C > "$T/out" 2> "$T/err"
echo "  status $?; stdout: $(head -1 "$T/out" | cut -c1-100)"
NAMESPACE
without() {   # PACKAGE...: slurmd -C as if these were not installed
    rm -rf "$T/plugins"; mkdir -p "$T/plugins"; cp -a "$PLUGINS"/. "$T/plugins/"
    : > "$T/libraries"
    for p in "$@"; do
        for f in $(dpkg -L "$p"); do
            [ -f "$f" ] && [ ! -L "$f" ] || continue
            case $f in
                "$PLUGINS"/*) rm -f "$T/plugins/$(basename "$f")" ;;
                /usr/lib/*) echo "$f" >> "$T/libraries" ;;
            esac
        done
    done
    echo "== without $*: $(ls "$T/plugins" | wc -l) plugins, $(wc -l < "$T/libraries") library files covered"
    sudo unshare -m --propagation private bash "$T/in-namespace.sh" "$T" "$PLUGINS" 2>&1 |
        grep -v 'Segmentation fault'
}
without librocm-smi64-1
without librocm-smi64-1 librocm-smi-dev
without slurm-wlm-rsmi-plugin
without slurm-wlm-rsmi-plugin slurm-wlm-plugins librocm-smi64-1 librocm-smi-dev liboam1 liboam-dev
echo "== apt's simulations"
echo "  apt-get remove slurm-wlm-rsmi-plugin: $(apt-get -s remove slurm-wlm-rsmi-plugin 2> /dev/null | awk '/^Remv/ { print $2 }' | tr '\n' ' ')"
echo "  apt-get remove --autoremove slurm-wlm-rsmi-plugin: $(apt-get -s remove --autoremove slurm-wlm-rsmi-plugin 2> /dev/null | awk '/^Remv/ { print $2 }' | tr '\n' ' ')"
echo "  outside afterwards: $(ls "$PLUGINS" | wc -l) plugin files, $(ls /usr/lib/*/librocm_smi64.so.1.0 2>&1)"
rm -rf "$T"
