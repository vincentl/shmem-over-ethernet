#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# docs/fast-link.md §15: one node's latency tuning, as a site would make it
# (H27), on the interface its route to FIRST uses (facts.sh's):
#   on   interrupt coalescing off (ethtool -C: adaptive-rx and adaptive-tx off,
#        rx-usecs and tx-usecs 0) and busy polling on (net.core.busy_read and
#        net.core.busy_poll, BUSY microseconds), the settings before kept in
#        /tmp/osoe-kit-tuning.saved for off;
#   off  the settings kept put back, where on kept any.
# Each by sudo -n, which root does without; every line prefixed by the node's
# name, the last the settings as they then are.
# usage: tuning-node.sh on|off BUSY FIRST
set -u
state=$1 busy=$2 first=$3
saved=/tmp/osoe-kit-tuning.saved
ethtool=/usr/sbin/ethtool
[ "$(id -u)" -eq 0 ] && sudo= || sudo="sudo -n"
say() { sed "s/^/$(hostname) tuning: /"; }
address=$(getent ahostsv4 "$first" | awk 'NR == 1 { print $1 }')
interface=$(ip route get "$address" 2>/dev/null |
            awk '{ for (i = 1; i < NF; i++) if ($i == "dev") print $(i + 1) }' | head -1)
if [ -z "$interface" ] || [ "$interface" = lo ]; then
    interface=$(ip -o -4 route show default | awk '{ print $5 }' | head -1)
fi
# The coalescing settings ethtool -C takes, as ethtool -c says them, those the
# interface has: one it does not ("n/a") is neither kept nor set.
coalescing() {
    $ethtool -c "$interface" 2>&1 |
        awk '/^Adaptive RX:/ { if ($3 != "n/a") printf "adaptive-rx %s ", $3
                               if ($5 != "n/a") printf "adaptive-tx %s ", $5 }
             /^rx-usecs:/ { if ($2 != "n/a") printf "rx-usecs %s ", $2 }
             /^tx-usecs:/ { if ($2 != "n/a") printf "tx-usecs %s ", $2 }'
}
# Each NAME VALUE pair of $1 by its own ethtool -C, so that a parameter the
# driver refuses leaves the others set; each refusal said.
# shellcheck disable=SC2086
coalesce() {
    set -- $1
    while [ $# -ge 2 ]; do
        $sudo $ethtool -C "$interface" "$1" "$2" 2>&1 | sed "s/^/$1 $2: /" | say
        shift 2
    done
}
case $state in
on)
    [ -e "$saved" ] || {
        echo "coalescing $(coalescing)"
        echo "busy_read $(cat /proc/sys/net/core/busy_read)"
        echo "busy_poll $(cat /proc/sys/net/core/busy_poll)"
    } > "$saved"
    coalesce "adaptive-rx off adaptive-tx off rx-usecs 0 tx-usecs 0"
    $sudo sysctl -q -w net.core.busy_read="$busy" net.core.busy_poll="$busy" 2>&1 | say
    ;;
off)
    if [ -r "$saved" ]; then
        coalesce "$(sed -n 's/^coalescing //p' "$saved")"
        $sudo sysctl -q -w net.core.busy_read="$(sed -n 's/^busy_read //p' "$saved")" \
            net.core.busy_poll="$(sed -n 's/^busy_poll //p' "$saved")" 2>&1 | say
        rm -f "$saved"
    fi
    ;;
*)
    echo "usage: tuning-node.sh on|off BUSY FIRST" >&2
    exit 2
    ;;
esac
echo "$interface: $(coalescing)busy_read $(cat /proc/sys/net/core/busy_read)," \
     "busy_poll $(cat /proc/sys/net/core/busy_poll)" | say
