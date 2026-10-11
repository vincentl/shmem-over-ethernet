# reviews: adversarial reviews of osoe

A review is run by a lead, who sets fresh Claude agents to investigate and
writes the synthesis of what they found. `adversarial-review-prompt.md` is the
prompt a lead is given: it asks five questions of the tree - is it OpenSHMEM
1.6, is it correct, does it scale as the README says, will it run elsewhere, is
it ready to be public - and requires each finding to give its location, a
failure scenario, evidence, a severity and a confidence. From round three a
refutation pass also tried to knock down every finding rated serious before the
synthesis was written - fresh agents from round four.

`archive/` holds the finished reviews, named for the work they reviewed
(`CLAUDE.md` gives the rule; the first round predates it), so that a finding can
be traced to the tree that produced it: nine rounds - the first against the
design alone, the second against the first steps built, the third before the
first release, the fourth to sixth at merge points of its task, the sixth on
the release candidate, the seventh and eighth at the merge points of v0.2's
first and last steps (`v0.2-step1-*`, `v0.2-step4-*`), and the ninth before
0.2's tag, Part 4 aside (`v0.2-before-part4-*`); three reviews of single
slices; and the adversarial reads of v0.2's parts, a fresh agent's read of a
part before it merged and again after its fixes where the first found
something serious (`v0.2-step2-adversarial-read.md` to
`v0.2-step4-part3-second-adversarial-read.md`, each named for its part and its
read), and of the changes after step 4's merge point, each named for its change
(`v0.2-step4-review-fixes-adversarial-read.md`,
`v0.2-step4-grant-fields-adversarial-read.md`,
`v0.2-single-node-configure-adversarial-read.md`,
`v0.2-loopback-keepalive-adversarial-read.md`,
`v0.2-blocking-gets-adversarial-read.md`,
`v0.2-zero-copy-memlock-adversarial-read.md` and its second,
`v0.2-zero-copy-refused-second-adversarial-read.md`,
`v0.2-helper-wake-adversarial-read.md`, and the fast-link kit's two,
`v0.2-fast-link-kit-adversarial-read.md` and
`v0.2-fast-link-kit-second-adversarial-read.md`, and the read of round
nine's changes, `v0.2-round-nine-fixes-adversarial-read.md`); and a fresh
agent's fact-check of the 0.2 release note's draft, line by line against its
sources (`v0.2-release-notes-fact-check.md`). Start with a round's synthesis,
whose first section is its verdict: `archive/v0.2-before-part4-review.md` is
round nine's, `archive/v0.2-step4-review.md` round eight's, and
`archive/phase1-step7-slice4-review.md` round six's, on the first release's
candidate. What the later rounds led to is in `docs/decisions/`
(`2026-10-02-release-fixes.md`, `2026-10-04-review-fixes.md`,
`2026-10-04-step-d-review-fixes.md`, `2026-10-05-step-e-review-fixes.md`,
`2026-10-06-single-node-review-fixes.md` and
`2026-10-10-step4-merge-point.md`), and what each part's read led to in that
part's record.
`evidence/` holds the programs, models, logs and notes behind the first two
rounds' reports, behind two reads of v0.2 step 4's first part
(`v0.2-step4-part0/`, `v0.2-step4-part0-fourth-read/`), and behind round
eight's scale S4 on the Pi (`v0.2-step4-scale-s4-pi/`), kept as the record of
what ran.
