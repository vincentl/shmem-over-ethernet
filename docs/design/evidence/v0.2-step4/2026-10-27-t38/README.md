# T38's evidence (2026-10-27): the CPU a wake costs, as built and with the spin

The item (`docs/prompts/v0.2-open-items.md`, T38; the author's answer in
`docs/decisions/2026-10-24.md`, "The author's answers", item 2): measure the CPU a wake costs
under a trickle of puts, as built and with round nine's spin before the owner's flag; build a
test of it if the spin costs more than five times as built, otherwise make T38's guard a count in
the layout-42 slice. The record that reads these files is
`docs/decisions/2026-10-27-t38-wake-cost.md`. A measurement: no library code or test changed.

## What ran

The development VM `osoe` (lima, vz, aarch64, 4 vCPUs, 8 GiB; Debian's Linux 6.12.113, HZ 250,
`NO_HZ_FULL`, `VIRT_CPU_ACCOUNTING_GEN`) on an Apple M3 of four performance and four efficiency
cores, on mains power, low-power mode off. Alone on it under `/tmp/osoe-vm.lock`, the VM's clock 01:34 to
01:40 UTC on 2026-10-28 (the evening of 2026-10-27 at the desk).

- **The trees.** `main` at `94cd6e0b` with `measuring-program.patch` applied, packed with
  `COPYFILE_DISABLE=1 tar --no-mac-metadata --no-xattrs`, and extracted twice; one of the two
  mutated by round nine's `preflag-mutant.py` (copied here from the review's scratch, as it ran:
  a helper, and the lead's agent, that moved nothing in a pass polls its ring's completions, its
  wake word and its dirty bits for up to 200 us before it raises its sleeping flag, going round on
  any work). Both built with the `release` preset, no warning (`measure.sh build`).
- **The program** (`measuring-program.patch`, which adds `shmem/tests/test-wake-cost.c` and its
  target): four logical nodes of one PE each (`osoe-launch -n 4 -m 0,1,2,3`),
  `OSOE_DEADLOCK_SECONDS=0` (the detector's requests nudge every owner), at
  `OSOE_PROGRESS_HELPERS=0` and `2`. After a barrier and 50 ms, every PE - each its node's lead -
  samples each owner of its node: the CPU of the owner's thread from
  `/proc/self/task/<tid>/schedstat` (the tid from `transport.am.tid`; user and system time, the
  scheduler's own count of the thread's time on a CPU, taken at each switch), its
  wakes (`transport.am.wakes`, the futex completions on its ring that saw its wake word move) and
  its passes (`transport.am.passes`). It samples again after 500 ms of idling (the `idle` lines),
  and again after the trickle (the `trickle` lines): PE 0, alone on node 0, puts 8 bytes to nodes
  1, 2 and 3 in turn, a gap apart, then quiets, waits 50 ms and samples; the other PEs wait on a word
  of their own until PE 0 puts it, and sample then. Node 0's owners are woken by PE 0's puts, which
  the wakes count; the other nodes' owners are woken by the sockets, which they do not, so their
  lines give the CPU a put received.
- **The series** (`measure.sh run`): each a number of rounds of four jobs - as built and the spin,
  at no helpers and two - alternating, beside a number of busy loops started for the series.

| Directory | Rounds | Busy loops | Puts | Gap |
|---|---|---|---|---|
| `out-smoke/` | 1 | 0 | 300 | 2 ms |
| `out-alone/` | 5 | 0 | 300 | 2 ms |
| `out-gap10/` | 2 | 0 | 100 | 10 ms |
| `out-loaded1/` | 3 | 1 | 300 | 2 ms |
| `out-loaded/` | 3 | 4 | 300 | 2 ms |
| `out-loaded8/` | 3 | 8 | 300 | 2 ms |

Each file is one job, `TREE-hHELPERS-lLOADS-ROUND.txt`, every node's lines and the launcher's exit
status; every job exited 0. `summary.txt` is `summarize.py out-smoke,out-alone out-gap10
out-loaded1 out-loaded out-loaded8`: for each series, tree and helper count, node 0's CPU a wake,
the other nodes' CPU a put, a node's owners' idle CPU a second and node 0's passes a wake, each the
median, the range and the count.

- **The calibration** (`calibrate.sh`, `burst.c`, `calibrate.txt`), the library not involved: the
  CPU a fixed burst of arithmetic costs when it runs 2 ms apart, 300 times, and when it runs back to
  back, three runs each alone and beside 1, 4 and 8 busy loops.

## What it found

- **The spin costs two and a half times as built on an idle VM, and eleven to thirty-one times
  beside busy loops** (node 0's medians; the worst pairs 1.8 and 5.9 times), because the as-built
  cost moves sevenfold to twentyfold with the VM's load and the spin's does not.
  Node 0's CPU a wake as built: 90 to 178 us alone, 7 to 37 us beside one, four or eight busy
  loops; with the spin, 283 to 436 us alone, 204 to 263 us beside them. The highest as built,
  177.7 us (`out-gap10/built-h2-l0-1.txt`), and the lowest with the spin, 203.9 us
  (`out-loaded8/spin-h0-l8-1.txt`), are 1.15 times apart.
- **Passes a wake do not move**: 5.0 in both trees in every series at 2 ms, 5.2 in both at 10 ms
  and two helpers. The spin polls between a pass and the flag without passing.
- **The host slows a lightly loaded VM**: the calibration's burst cost 31 to 48 us a gap apart
  alone and 17 to 20 us beside one or four busy loops. That is a factor of about two of the
  library's sevenfold to twentyfold; the rest is not explained here.
