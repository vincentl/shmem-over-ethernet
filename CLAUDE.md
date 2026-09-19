# osoe — working agreements

Project-wide rules live in the design notes (`docs/design/shmem-ethernet-design.md`)
and in the per-slice briefs under `docs/prompts/`. This file holds only the
conventions for *how the work is carried out* that those documents do not cover.

## Delegate straightforward work to Sonnet subagents

Use the Agent tool with `model: "sonnet"` for work that is well-specified and
mechanical, so the main session's context is spent on design instead. Typical
candidates:

- Searching the tree or the `/corpus/` reading corpus for a pattern, a call
  site, or a prior art example.
- Running a build or a test preset in the VM and reporting only what failed.
- Bulk mechanical edits with a stated pattern — renames, header ordering,
  adding a ctest timeout and label to every new test.
- Reading a long file or a specification section and answering a specific
  question about it.
- Checking a finished change against a checklist: the `OSOE_`/`SHMEM_` prefix
  split, `shmem.exports`, the `pshmem_` strong / `shmem_` weak pairing, ctest
  labels and timeouts.

Give the subagent enough context to work without the conversation, and ask it
to report findings rather than paste files back.

**Keep in the main session** anything load-bearing: protocol and data-structure
design, the atomics rules of §3.16, decisions that change the design notes,
model-checking a new protocol, and the judgement calls a slice brief asks to be
stopped on. A subagent's report is evidence, not a verdict — verify anything
surprising before acting on it.
