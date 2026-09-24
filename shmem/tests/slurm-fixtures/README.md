# Slurm environment fixtures

One task's environment each, replayed into the bootstrap by
`shmem/tests/test-bootstrap.c` (tier 1, `shmem.bootstrap.*`; design §6.1, §8.2).
Only the `SLURM_` names a task sees are kept, less those that name a user or a
path (`SLURM_JOB_USER`, `SLURM_JOB_UID`, `SLURM_JOB_GID`, `SLURM_SUBMIT_DIR`,
`SLURM_SUBMIT_HOST`).

- `captured-*`: from a real `srun`, with the Slurm version and command line in
  the header. `tools/launch/capture-slurm-fixtures.sh` makes them.
- `written-*`: written from Slurm's documentation, for distributions no
  capture here covers yet - marked **provisional** until a capture replaces
  them, since a fixture written from the documentation can only repeat what its
  writer understood.
- `osoe-*`: `osoe-launch`'s own names, for the rule that decides between them.
