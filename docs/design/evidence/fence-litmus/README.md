# Fence, quiet and the two-sided rule on aarch64 (§3.16, §4.6)

Two litmus programs, run on the development VM (Apple silicon, cores with
`lrcpc`, GCC 14.2, Linux 6.12). The rules they informed are derived from the C11
model in §3.16 and §4.6; these show what a real aarch64 target does with the
weaker choices, and are to be rerun on a Pi 4 (Cortex-A72) and a Pi 5
(Cortex-A76).

- **`fence-litmus.c` — message passing, W→W (fence).** Writer: `data`,
  something, `flag`; reader waits for `flag`, reads `data`; a third thread
  contends `data`'s line. `fence-litmus.out`, 20 M rounds twice per variant:
  nothing between 889 and 32 reorderings; `stlr` to a thread-local word 0;
  `stlr` then `ldar` 0; `dmb ish` 0. The architecture permits the `stlr` case
  to reorder; these cores do not.
- **`store-buffer-litmus.c` — store buffering, W→R (quiet, and two-sided
  protocols).** Built with `-march=armv8.3-a`, so an acquire load is `ldapr`.
  `store-buffer-litmus.out`, 2 M rounds three times per variant: nothing
  between ~1.1 M both-zero per run; `seq_cst` store then `acquire` load
  (`stlr; ldapr`) 289, 4,449, 278; `seq_cst` store then `seq_cst` load
  (`stlr; ldar`) 0; plain store, `stlr` to a thread-local word, plain load
  ~1.1–1.4 M; plain store, `dmb ish`, plain load 0.

Build and run each as its header says.
