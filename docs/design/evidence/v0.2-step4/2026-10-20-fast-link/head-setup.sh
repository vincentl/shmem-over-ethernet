#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# Cluster session 2's setup on the head node (docs/fast-link.md §1): the
# packages here and on every node at once (each node's log in ~/apt-NODE.txt),
# the tree cloned from the bundle into ~/osoe, the node list in ~/NODES. Run in
# an ssh session with the agent forwarded: it reaches the nodes by ssh.
# usage: head-setup.sh
set -u
PACKAGES="build-essential cmake pkg-config liburing-dev git rsync ethtool iperf3 linux-perf"
install="sudo apt-get update -q && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q $PACKAGES && sudo DEBIAN_FRONTEND=noninteractive apt-get install -y -q --no-install-recommends slurm-wlm munge"
bash -c "$install" > ~/apt-head.txt 2>&1 || { echo "head: the packages failed"; tail -5 ~/apt-head.txt; exit 1; }
echo "head: packages in"
[ -d ~/osoe ] || git clone -q ~/osoe-main.bundle ~/osoe || exit 1
git -C ~/osoe log --oneline -1
for i in 1 2 3 4 5 6 7 8; do echo "slurm-node-0$i"; done > ~/NODES
for n in $(cat ~/NODES); do
    ( ssh -o StrictHostKeyChecking=accept-new "$n" "$install" > ~/apt-"$n".txt 2>&1
      echo "exit $?" >> ~/apt-"$n".txt ) &
done
wait
for n in $(cat ~/NODES); do echo "$n: $(tail -1 ~/apt-"$n".txt)"; done
