# Round eight's scale S4: helpers and the collectives on the Pi cluster

The raw output behind round eight's scale and performance S4
(`docs/reviews/archive/v0.2-step4-scale-and-performance.md`) - the review's
own short run of `osoe-bench`'s collective lines on the Pi cluster, four nodes
of four PEs, `main` at `c897948`, on the night of 2026-10-09: two rounds,
alternating, of each binding (Slurm's default and `--cpu-bind=none`) at no
helpers and at two (`OSOE_PROGRESS_HELPERS`), one file a run
(`coll-BINDING-hHELPERS-ROUND.txt`) and the run's order and exit statuses
(`run.txt`). The README's "6 to 35% slower" (Limits, transport and API) and
`docs/pi-cluster-results.md`'s "Helpers on the Pi" read these with Part 3's
run (`docs/design/evidence/v0.2-step4/2026-10-08-helpers/pi-7903b15/`) and the
merge point's (`docs/design/evidence/v0.2-step4/2026-10-10-merge-point/pi-vs-step3/`).
Copied here on 2026-10-11 from the review's scratch directory, where the ninth
review found them out of the tree (scale S10).
