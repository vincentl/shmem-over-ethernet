# reviews: adversarial reviews of osoe

A review is run by a lead, who sets fresh Claude agents to investigate and
writes the synthesis of what they found. `adversarial-review-prompt.md` is the
prompt a lead is given: it asks five questions of the tree - is it OpenSHMEM
1.6, is it correct, does it scale as the README says, will it run elsewhere, is
it ready to be public - and requires each finding to give its location, a
failure scenario, evidence, a severity and a confidence. In rounds four and five
a refutation pass of fresh agents also tried to knock down every finding rated
serious before the synthesis was written.

`archive/` holds the finished reviews, named for the work they reviewed
(`CLAUDE.md` gives the rule; the first round predates it), so that a finding can
be traced to the tree that produced it: five rounds - the first against the
design alone, the second against the first steps built, the third before the
first release, the fourth and fifth at merge points of its task - and three
reviews of single slices. Start with a round's synthesis, whose first section is
its verdict: `archive/phase1-step7-slice3-review.md` is round five's. What the
later rounds led to is in `docs/decisions/` (`2026-10-02-release-fixes.md`,
`2026-10-04-review-fixes.md` and `2026-10-04-step-d-review-fixes.md`).
`evidence/` holds the programs, models, logs and notes behind the first two
rounds' reports, kept as the record of what ran.
