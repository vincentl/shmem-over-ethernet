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

## Names are words, not contractions

Spell names out — in file names and in C identifiers alike, including variables,
functions, types and macros. `connection_area`, not `connarea`; `reservation`,
not `resv`; `capacity`, not `cap`.

The test is not length. It is whether the name is a word someone would say out
loud while explaining the code. Short is fine when the short form *is* the word,
and which words those are depends on the layer:

- **At the API layer, the specification's spelling wins.** `pe`, `ctx`, `amo`,
  `rma`, `nbi`, `team`, `quiet` are what OpenSHMEM calls these things, and the
  code implementing those routines must not invent a second vocabulary for them.
  A reader arrives holding the specification.
- **Below the §4.10 boundary — the transport, the AM layer, `base/` — use ours,
  and spell it out.** Nothing down there is named by the specification, so
  `osoe_transport_context`, not `osoe_tp_ctx`. The design notes' own terms are
  words and stay as the notes spell them: `am` (active message), `rmw`, `cas`,
  `futex`, `numa`, `mtu`.
- **Universal C spellings stay everywhere**: `len`, `ptr`, `fd`, `argv`, and
  `src`/`dst`, which are the parameter names of every `memcpy`-shaped
  signature there has ever been.

Everything else is spelled out, however long that gets.
`osoe_connection_payload_capacity` beats `osoe_conn_payload_cap`, and the extra
characters cost nothing that matters. A contraction that saves typing and costs
a reader a pause is a bad trade — and one that reuses a fragment for two
different things, as `osoe_connarea_conn` did, is worse than merely long.

**File names are kebab-case; C identifiers are snake_case.** A dash is not legal
in an identifier, so the two spellings of one concept differ only in the
separator: `connection-area.c` holds `osoe_connection_area_open`. A reader
should get from file to symbol without guessing.

**Short locals are fine in a short scope.** `i`, `n`, `k` for loops and indices,
and a one- or two-letter name for something used within a few lines of its
declaration where the type makes it obvious, beat ceremony. The rule is about
names a reader meets away from their declaration: anything in a header, anything
crossing a function boundary, anything outliving a screen.

## Mechanical renames

Order substitutions **longest-first**, and remember that a catch-all applied
after a specific one rewrites its own output: `osoe_conn` -> `osoe_connection`
turned an already-correct `osoe_connection_shared` into
`osoe_connectionection_shared`. That compiled and passed all four presets,
because it was wrong *consistently* — a rename is one of the few changes whose
defects a green test suite cannot see.

So audit afterwards, and **check that the audit command can find something you
know is there before believing an empty result**. An empty result from a broken
grep looks exactly like a clean tree. The first audit of that rename reported
clean because the pattern itself never matched anything.

## Review documents are named for the work they reviewed

`docs/reviews/archive/` holds the record of each finished adversarial review;
`docs/reviews/adversarial-review-prompt.md` is how one is run. A review's files
carry the phase, step and slice they were run against — for example
`phase1-step2-slice1-design-soundness.md` — so that a finding can be traced to
the state of the tree that produced it. The first round predates the convention
and keeps its names (`design-soundness.md`, `core-api.md`, `roce-transport.md`,
`watch-notification-options.md`); it was run against the design alone, before
there was an implementation to name.
