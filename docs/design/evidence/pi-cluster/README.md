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
