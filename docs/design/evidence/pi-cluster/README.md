# Runs on the Raspberry Pi cluster

Raw outputs of each session on the four-node Pi 5 cluster (`docs/pi-cluster.md`
describes the cluster and the runbook), one directory per session, kept as
brought back: OSU tables, benchmark logs, program verdicts. The runbook and
the status page read the numbers from here.

| Session | Tree | What ran |
|---|---|---|
| `2026-09-24/` | `bc86e8f` (before pipelining, the allgather and team creation) | runbook §1-§4, §7, most of §8: probe, smoke, endings, NPB IS, ISx, PRK, ssca1, the 72 examples, all 19 OSU programs |
| `2026-09-26/` | `b8d7ad5` (main) | §5 floor, §6 bench, tier 1 and 2, the SOS suite, start-up times, PEs per node, the pipelined collectives at fan-outs 2, 3 and 4 and D of 1 to 8, the allgather's fcollect |
| `2026-09-26-late/` | `a4ca6c4` (main) | tier 2, SOS's context and scan programs, F1 with its levers and the TCP counters, F2's crossover sweep; `rto/`, the minimum-timeout experiment on F1; `netcfg-*`, each node's TCP and NIC settings |
| `2026-09-26-night-f1/` | `96b2e0f` (main) | F1 investigated: the trigger map by block size, per-socket TCP samples, socket and chunk knobs, plain TCP with osoe's exchange, node subsets, rank placements, the 16 KiB chunk's map |
| `2026-09-28-review/` | `f561e2d`, source-identical to `b0e0b53` | the round-three review's session: 137 of its 138 raw files, by the step that made them (`01-*` the cluster's facts, `02-*` to `04-*` the benchmark, floor and binding runs, `05-*` `mix`, `06-*` and `slurm-env-*` three Slurm layouts' task environments, `07-*` SOS, `08-*` the mailbox, `09-*` and `10-*` the later runs, `11-*` the end), trailing spaces stripped and the nodes' hardware addresses removed from `01-node-facts.txt`. The 138th, `ip link`'s output, is those addresses alone. Paths name the cluster's own `pi` account |
