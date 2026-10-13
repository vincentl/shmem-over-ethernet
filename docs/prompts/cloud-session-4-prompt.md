# Cloud session 4: the small items

*Written by the lead session on 2026-10-13 for the author's fourth cloud session. The environment's
setup script is unchanged from the second session's (its brief's appendix). The author opens the
session on the repository's `main` with **Claude Opus 5.5 at high effort** and sends: "Read
docs/prompts/cloud-session-4-prompt.md and do what it says."*

## What this session is for

The first three sessions (`docs/prompts/cloud-session-1-response.md` to `cloud-session-3-response.md`,
all merged) showed that this sandbox runs osoe's full build whole and closed 20 items between
them. About $80 of the author's cloud credit is left, until 5 November. This session takes what the
third left: **the small items** - the diagnostics, the small protocol items and the small tests -
in batches, as many as the credit allows, and ends with a **response file that is the complete
account**, as the third's was.

## Read first

- **The first session's brief**, `docs/prompts/cloud-session-1-prompt.md`: its "Read first",
  "Rules the lead keeps that the repository does not spell out" and "Branch, commits, pushes"
  apply as written, with the branch `cloud-session-4`.
- **The third session's response**, `docs/prompts/cloud-session-3-response.md`: the sandbox as it
  last found it, its findings (D24 is its finding 7, P10 its finding 6), and its list of what
  remains, which is this session's scope.
- Each item's text in `docs/prompts/v0.2-open-items.md`, its source, and the code it names; read
  the text again before starting an item, since the list is the authority. As before, do **not**
  edit the open items, `docs/prompts/task-v0.2-to-the-tag.md`, `docs/prompts/README.md` or
  `docs/decisions/`: the response lists what closed and what the records need, and the lead
  applies them at the merge. **The lead is working on the kit** (B11, `docs/fast-link/`) while
  this session runs: do not edit `docs/fast-link/` or `docs/fast-link.md`.

## The items, in batches by the file they touch

Each item with a test that fails without it (shown, as before), the README true, a commit per
batch and a manifest entry per item. If an item turns out to need a decision of the author's, or
a change to the wire or a layout the item does not name, stop it, say why, and go on.

1. **The settings' diagnostics** - D16, D24, D27, D29.
2. **Start-up's messages** - D10, D11, D14, D15, D25, D28.
3. **`fatal.c`'s and `abort.c`'s** - D18, D19, D20, D21, D22.
4. **`osoe-stat`'s** - D2, D3, D5, D6, D7, D8.
5. **`osoe-launch`'s** - D23; and **D30**, the library's lines split between PEs.
6. **The small protocol items** - P8, P9, P10, P16, P17, P21, P22, P25, P26, P27; and P13, which
   is §3.16's ground: name the memory order and why, and change nothing else.
7. **The small tests** - T18, T19, T22, T23, T31, T33, T34, T37, T40, T41, T43 (for T43, the
   third session found `shmem.landing-default.L16` and `.L17` running 16 and 17 PEs on one node
   without trouble).

Opus for anything near the protocols, the atomics or the models; Sonnet subagents, precisely
instructed, for the mechanical edits and the runs, their results read before acting on them. Run
the models you change and `selftest.py`, not the whole model suite, unless a change reaches the
engine. Before the response: the static checks, the release suite whole, and each preset's tests
for what changed (the `debug`, `asan` and `tsan` presets' where a change reaches the library's
threads or memory).

## The manifest, and the response that ends the session

**The manifest**, `docs/prompts/cloud-session-4-manifest.md`: the per-item log, with every command
and count, current after every item, committed and pushed each time.

**The response**, `docs/prompts/cloud-session-4-response.md`, written last, is **the complete
account of the session - not a short version**. The author reads it rather than the chat, and the
lead merges from it, so whatever you would tell the author in your final message belongs in it,
in full, and nothing should be in the chat alone. It holds:

- **In short** - what was done, what was not, and why.
- **The environment** - anything that changed since the third session's report.
- **Every item** - one section each: its outcome (done, partial, skipped, stopped) and why; its
  commits; the files and functions it changed and what each change does; whether it changes the
  wire or a layout, and which; how it was shown - the test, its failing-without-the-change
  evidence, the commands and their pass counts; and anything about it left open.
- **Where the tests stand at the end** - each preset and tier run, with counts, and what was not
  run.
- **What needs the author** - every decision, with the options and a recommendation.
- **What the lead's merge needs** - the items to close, with ids; any item's text to change; the
  record and README lines that wait for the merge; and what to re-run in the development VM,
  which is aarch64, with the exact commands.
- **Decisions taken and deviations** from this brief.
- **Findings** - anything stale, wrong or missing met on the way, proposed and not fixed, each
  with where it is.
- **What remains**, by id, for a next session.

The manifest may hold more detail; the response must not need it to be understood. Commit and push
the response as the last thing; your final chat message may be a single line pointing to it. If
the author stops the session early, write the response before anything else. Leave enough credit
to write it well.
