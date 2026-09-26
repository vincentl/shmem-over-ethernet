#!/bin/bash
# F1 phase E: do the timeouts follow pi-002 or the rank it holds? And does
# plain TCP with both exchange steps at once stall?
set -u
O=~/pi-results-f1; say() { echo "$(date +%T) $*" | tee -a $O/summary.txt; }
timeouts() { srun -N4 --ntasks-per-node=1 sh -c 'echo "$(hostname)=$(nstat -az TcpExtTCPTimeouts | awk "NR>1{print \$2}")"' 2>/dev/null | sort | tr '\n' ' '; }
for order in "pi-001 pi-002 pi-003 pi-004" "pi-002 pi-001 pi-003 pi-004" "pi-003 pi-004 pi-001 pi-002" "pi-004 pi-003 pi-002 pi-001"; do
  printf '%s\n' $order > ~/f1-hosts
  label=$(echo $order | tr -d 'pi-' | tr ' ' '_')
  before=$(timeouts)
  SLURM_HOSTFILE=~/f1-hosts timeout 900 srun -K1 -n4 --distribution=arbitrary sh -c 'echo "rank $SLURM_PROCID on $(hostname)" >&2; exec ~/f1probe 28672 32768 28672 32768' > $O/E-order-$label.txt 2>&1 < /dev/null
  say "E ranks on $order: $(grep block $O/E-order-$label.txt | awk '{print $2":slow="$8",max="$10}' | tr '\n' ' ')"
  say "    timeouts before: $before"
  say "    timeouts after:  $(timeouts)"
done
say PHASE-E2-DONE
