# design: how osoe works, and why

`shmem-ethernet-design.md` is the design notes: every protocol and data
structure, the reasons for it and what was rejected, in ten numbered sections -
the goals (§1), the layering (§2), the active-message layer (§3), the SHMEM
layer (§4), collectives (§5), bootstrap (§6), the specification's subset and the
order of work (§7), testing (§8), the invariants the code is held to (§9) and
the open questions (§10). The code's comments cite it by section, as `§4.10`;
start with §1, for the goals and the release point, and §2, for the layers, then
§4.10, the boundary that the layout of `shmem/src/` follows.

Beside the notes are three proposals, each saying at its head what became of
it: `osoe-pacer-proposal.md`, the stress tester's, `team-resources-proposal.md`,
the team redesign's, and `mesh-schedule-proposal.md`, a start-up schedule for
the connection mesh, postponed; and `successor-api-stage-setting.md`, a note on
what a smaller API after osoe might be, which binds nothing. `workloads/traces/`
holds virtual traces of the reference workloads of §8.6, read from the source of
public programs and not run, and its README gives their format. `evidence/`
holds the raw outputs that the notes and the records cite - model runs and
litmus tests, experiments, mutation matrices, and a directory for each session
on the Pi cluster - kept as the record of what ran; `docs/README.md` counts
them.
