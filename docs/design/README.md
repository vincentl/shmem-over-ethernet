# design: how osoe works, and why

`shmem-ethernet-design.md` is the design notes: every protocol and data
structure, the reasons for it and what was rejected, in ten numbered sections -
the goals (§1), the layering (§2), the active-message layer (§3), the SHMEM
layer (§4), collectives (§5), bootstrap (§6), the specification's subset and the
order of work (§7), testing (§8), the invariants the code is held to (§9) and
the open questions (§10). The code's comments cite it by section, as `§4.10`;
start with §1, for the goals and the release point, and §2, for the layers, then
§4.10, the boundary that the layout of `shmem/src/` follows.

Beside the notes are six proposals, each saying at its head what became of
it: `osoe-pacer-proposal.md`, the stress tester's, `team-resources-proposal.md`,
the team redesign's, `progress-helpers-proposal.md`, v0.2 step 4's,
`mesh-schedule-proposal.md`, a start-up schedule for the connection mesh,
built (H23), `blocking-get-proposal.md`, question 4's exploration of a
blocking get a piece at a time, decided and built as its (b), and
`collect-scan-proposal.md`, P30's design and model for `collect`'s places,
built by the second cloud session (its "Stage 2, as built"); and two documents on a smaller API after osoe, which bind nothing:
`successor-api-stage-setting.md`, the lessons building osoe taught and a
strawman, and `core-api-proposal.md`, the proposal that follows from them,
item by item with its alternatives and the questions put to the author.
`workloads/traces/` holds virtual traces of the reference workloads of §8.6, read from the source of
public programs and not run, and its README gives their format. `evidence/`
holds the raw outputs that the notes and the records cite - model runs and
litmus tests, experiments, mutation matrices, and a directory for each session
on the Pi cluster - kept as the record of what ran; `docs/README.md` counts
them.
