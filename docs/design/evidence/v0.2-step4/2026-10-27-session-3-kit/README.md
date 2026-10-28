# The third session's kit, dry-run in the development VM

The record is `docs/decisions/2026-10-27-session-3-kit.md`; the chain is
`docs/fast-link/session-3.sh` (`docs/fast-link.md` §21), H12's section
`docs/fast-link/many-pes.sh` (§20). Everything here ran in the `osoe` VM under its lock,
four vCPUs, on 2026-10-28 UTC; `~` is the VM's home, `<repo>` the checkout.

- `vm-dry-run.sh`, `vm-dry-run.log`: the tree of `f6b46f95` built, then `dry-run.sh`'s
  `session-3` - the chain whole on five logical nodes at its own settings, H12's 256 PEs a
  node at 64 - with `/dev/shm` and the memory sampled every 10 s, and the static checks after
  it. `dry/` is what it brought back: `sections.txt` and `summary.txt`, the chain's lines and
  whole; for every section its log, its table (`SECTION-summary.txt`) and each output
  directory's `run.txt` (`SECTION-run.txt`); `setup-verdicts.txt`, `sos-build.txt`,
  `samples.txt`, `static.txt`.
- `vm-recheck.sh`, `vm-recheck.log`, `recheck/`: the tree of `d5b61886`, after the dry run's
  fixes - H12's wake point alone at 64 PEs a node with `perf` by `sudo -n`
  (`profile-wake-pes64-h0-r1.txt`: the table, then the progress threads' callers), the kit's
  own dry run, every section at small sizes (`std.txt` and each section's table), and the
  static checks.
- `binding-check.sh`, `binding-check2.sh`, `binding-check3.sh` and their logs: the VM's own
  one-node Slurm (`task/affinity`, nothing in its configuration changed), a whole-node
  allocation's step as the kit starts its jobs, each task's allowed CPUs - with
  `--overcommit` and without, every CPU each; with `--cpu-bind=threads` or `cores`, its own;
  with `--cpus-per-task=2`, two each - which set H26's two steps.
