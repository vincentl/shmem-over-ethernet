# Slurm environment fixtures

One task's environment each, replayed into the bootstrap by
`shmem/tests/test-bootstrap.c` (tier 1, `shmem.bootstrap.*`; design §6.1, §8.2).
Only the `SLURM_` names a task sees are kept, less those that name a user, a
group or a path (`SLURM_JOB_USER`, `SLURM_JOB_UID`, `SLURM_JOB_GID`,
`SLURM_JOB_GROUP`, `SLURM_SUBMIT_DIR`, `SLURM_SUBMIT_HOST`).

- `captured-*`: from a real `srun`, with the Slurm version and command line in
  the header. `tools/launch/capture-slurm-fixtures.sh` makes them.
- `written-*`: written from Slurm's documentation, for distributions no
  capture here covers yet - marked **provisional** until a capture replaces
  them, since a fixture written from the documentation can only repeat what its
  writer understood.
- `osoe-*`: `osoe-launch`'s own names, for the rule that decides between them.

The `captured-*` files were captured on the development VM (`dev/lima/create.sh`
makes it). Its host name and its address on Lima's private network, which
`srun` had put in them, are replaced after the capture, by `vm-0` and
`192.0.2.1` (an address reserved for documentation, RFC 5737): they name a
development machine, not a cluster, and nothing needs them. The bootstrap reads
neither address variable, and of the node names only `SLURM_STEP_NODELIST`,
which the fixture's `# expect:` line echoes in `hosts=` and so spells the same
way. A new capture on that VM needs the same replacement by hand;
`capture-slurm-fixtures.sh` keeps what it saw.
