#!/bin/bash
# SPDX-License-Identifier: LGPL-3.0-or-later
# usage: launch.sh NODE PART - cluster-verify.sh PART on NODE, detached there.
scp -q -o StrictHostKeyChecking=accept-new ~/cluster-verify.sh "$1": &&
ssh -o StrictHostKeyChecking=accept-new "$1" "setsid nohup bash cluster-verify.sh $2 ~/verify-out > /dev/null 2>&1 < /dev/null &" &&
echo "$1: $2 started $(date -u +%H:%M:%S)"
