# Round five, question 4: will osoe run anywhere else?

Review of `main` at `d8a651d`, run 2026-10-04 against
`docs/reviews/adversarial-review-prompt.md`, at the merge point of the team
redesign's step D (`docs/decisions/2026-10-03-team-storage.md`,
`docs/decisions/2026-10-03-team-allocation.md`, design §5.7). The previous round's
answer to this question is `phase1-release-fixes-portability.md`, at `08b3da0`,
and its fixes are `docs/decisions/2026-10-04-review-fixes.md`. Labels:
*documented* (a source says so: the tree, a vendor's published text),
*observed in source*, *measured* (run here, on this tree), *recalled* (from
memory, not checked).

## Machine and method

The `osoe-x86` Lima VM: Debian 13, kernel 6.12.95 **arm64**, 4 cores, 6 GiB, its
cores with `lse`, `lrcpc` and `ilrcpc`, Apple's Rosetta registered for x86-64
binaries. Every run in it is an aarch64 run unless it says Rosetta. The review
tree was copied to the VM's `/tmp/q4d` and built three ways with the `release`
preset's build type (`RelWithDebInfo`), each build and run under
`/tmp/osoe-vm.lock`:

- **x86-64**, cross-built with `x86_64-linux-gnu-gcc` 14.2 against
  `liburing-dev:amd64` 2.9 and run under Rosetta with the previous rounds' futex2
  shim (`build/scratch/review-r3/x86/shim/$LIB/libfutex2-shim.so`; the probe,
  run under it, passes the futex2 waits and is refused io_uring with `ENOSYS`).
  Rosetta runs x86-64 code on Apple cores in their TSO mode: an x86-64 user
  space and memory order, not an x86-64 kernel and not an Intel or AMD memory
  system.
- **aarch64 RCpc**, `-mcpu=cortex-a76`, the Pi 5's own core. GCC 14 then
  compiles every `acquire` load to `ldapr`, which may pass an earlier `stlr` -
  for a release store followed by an acquire load of another word, the
  reordering x86's store buffer allows (design §3.16) - and inlines LSE. The
  library holds 253 `ldapr`, 56 `ldar` (its `seq_cst` loads), 237 `stlr` and no
  outline helper; the default build has 309 `ldar` and no `ldapr` (*measured*,
  `objdump`). It runs natively, across logical nodes included.
- **aarch64 LL/SC**, `-mno-outline-atomics`: every atomic a
  load-exclusive/store-exclusive loop, as on a Pi 4's Cortex-A72 - 112
  `ldaxr`/`stlxr` pairs, no LSE instruction, no helper call.

Besides: step D's four models, copied to the Mac and run under the model
checker's x86-TSO mode with their stores made buffered or serializing as the
code's are (below); containers emulated in the VM with namespaces (`unshare`),
nothing changed outside them - a private 64 MiB tmpfs on `/dev/shm`, the tree's
own `deny-io-uring setup` (which refuses `io_uring_setup` with `EPERM`, as
Docker's default seccomp profile does), and PID namespaces sharing `/dev/shm`;
no Docker, Podman or Apptainer ran. Apple clang 21 checked all 279 translation
units for both targets with the VM's Linux headers, the previous rounds' method.
Nothing ran on the Pi cluster.

Two interruptions, neither touching a result: an API rate limit paused the
review for two hours, after which the copy, its logs and the lock were checked
intact; and one kill aimed by `pgrep -f` matched the shell that issued it, as
the project's notes warn, and ended only that shell - the run it was meant to
cut short finished normally, no process of mine was left, the lock was free and
`/dev/shm` held no name of mine - and that run was redone with the namespace's
own pid 1 doing the killing. The copy (544 MB) was removed when done.

## Verdict

**Step D runs wherever the tree ran before it, and on the two weaker memory
orders it had never met. What does not travel is in one of its test files and
in two README sentences about containers; nothing here blocks release.**

What convinced me: step D's new shared-memory protocols are message passing and
read-modify-writes with unconditional wakes, with no new two-sided
(store-buffering) shape (*observed in source*). Its four models keep every
verdict under x86-TSO with the code's own store orders (*measured*). A build
whose every acquire load is `ldapr` passes the whole suite, step D's 76 team
tests five times over and 7.7 minutes of the stress tester's `teams` and `mix`
across four logical nodes; a build of LL/SC loops passes the same suite and
tests, and two minutes of `teams`. The x86-64 build
passes, under Rosetta, every one-node test it can run but five. Those five fail
because step D's own test reads a team handle before the call that writes it,
an order C leaves unspecified and GCC on x86-64 takes the other way (Q1). The
internal region's new lines and the wire's new field are
architecture-independent, the exported symbols are unchanged, and every
refusal met was by name.

Ranked:

- **Q1, fix before release (a few lines; high)**: on x86-64 with GCC, eight
  tests of the release preset fail by their own evaluation order, and those
  modes check nothing of the handle there.
- **Q2, fix before release: a README sentence; a start-up check after release
  (high on the mechanism, medium on exposure)**: with a PID namespace per PE,
  as Apptainer's `--containall` gives each rank, a PE that dies is never
  noticed and the job waits for ever. And where `/dev/shm` outlives a launch's
  namespace, `osoe-stat --gc` keeps a dead start's names for good.
- **Q3, fix before release: a clause (high)**: in a container's default
  64 MiB `/dev/shm` a one-node job at the defaults is refused, by name, and
  496 of the 690 tests fail - not the "about 300" of the README's new container
  paragraph.
- **Q4, minor**: no assertion that the shared 64-bit atomics are lock-free;
  step D's models check SC only, with stores stronger than the code's; the
  README's x86-64 sentence can now describe this tree.

Once step E is built, on this question: put E's slots on the internal region's
own line (below), and run E through the same three builds, its new protocol
(the grants) through the TSO mode, and the one-node suite under Rosetta.

## The previous round's findings, checked as claims

| Previous | What was asked | On this tree | Evidence |
|---|---|---|---|
| Q1, fix before release | a README paragraph for RHEL 10 and containers | **holds for io_uring** as written (README 148-154); **incomplete for containers**: their default `/dev/shm` (Q3 here) and their PID namespaces (Q2 here) | *measured* |
| Q2, fix before release | `OSOE_AM_HUGEPAGES=1` said to have no effect: README row, design §3.4, §6.2 | **holds**: README 931; design 465, 495-499, 5954-5955 | *observed in source* |
| Q3, after release | the x86-64 network path never run | **still never run**: this round's x86-64 runs are one node again, though the RCpc build now carries the weaker of the two orders across nodes natively (below) | *measured* |
| Q4, minor | `osoe-stat --gc` as root from a timer, not the epilog | **holds** (README 1018-1021); `--gc` judges liveness by pid, which fails across PID namespaces (Q2 here) | *observed in source*, *measured* |
| Q5, minor | a Clang user's `shmem.strict-c11` | **unchanged**: the same single warning, `test-reply.c:278`, for both targets; step D adds none in 279 files | *measured* |
| Q6, minor | futex2's refusal names the kernel's age for any error | **unchanged** (`init.c:289`) | *observed in source* |
| Q7, minor | the 33rd initialization within a minute | not re-run; `listener.c` unchanged by step D | *observed in source* |
| Q8 | notes | not re-run | - |
| X3 (round three) | the suite cannot see an x86-only lost wake | not re-run; step D adds no two-sided protocol, the shape that lost wake needs (below) | *observed in source*, the TSO models |

## New findings, ranked

### Q1. Step D's split tests read the team handle before the call that writes it: with GCC on x86-64, eight tests of the release preset fail, and those modes check nothing of the handle there (fix before release; a few lines; high)

- **Where**: `shmem/tests/test-team-split.c`, `check_made` (510) and
  `check_failed` (517), called 21 times as `check_made(shmem_team_split_strided(...,
  &made), made)` and `check_failed(shmem_team_split_2d(..., &x, ..., &y), x, y)`,
  lines 540 to 711, in the `exhaust`, `too-many`, `dry`, `stale-handle` and
  `reuse` modes. All 21 came with step D's part 2 (`532fba4`, by `git log -S`).
  An audit for a call whose arguments hold a nested call taking `&v` and then
  `v` finds these 21 and nothing in the library, the pacer or any other test
  (*observed in source*; the audit was checked to find the instances known to
  be there).
- **Mechanism**: C11 (6.5.2.2) leaves the order of a call's argument
  evaluations unspecified, and a call in one argument is indeterminately
  sequenced with the read of `made` in the next. GCC on x86-64 reads `made`
  first; GCC on aarch64 and Clang on both targets call first (*measured*, a
  seven-line program: `check(make(&t), t)` sees the old `t` from GCC for x86-64
  at `-O0` and `-O2`, and the new one from the other three). So on every
  machine the project has, the tests are right. On x86-64 with GCC,
  `check_made` gets the handle as it was before the split - `SHMEM_TEAM_INVALID`
  - and `check_failed` gets `SHMEM_TEAM_WORLD`, and both fail.
- **Evidence** (*measured*): under Rosetta, `shmem.team-split.exhaust.L4`,
  `.too-many.L4`, `.dry.L2`, `.reuse.L4` and `.stale-handle.L2` fail, each at
  `test-team-split.c:512` or `:519`, deterministically. A copy instrumented in
  the VM shows the split returning 0 and `check_made` seeing `team=(nil)`. With
  only the sequencing changed - the status taken first, then the handles read -
  all five pass on x86-64 with every expected line ("no two team numbers are
  free on every PE", the dry pool named in each split, the stale handle named
  as team 3, destroyed), the library untouched.
- **Effect**: three more registrations run these modes across logical nodes,
  `exhaust.0-0-1-1`, `reuse.0-0-1-1` and `reuse.0-1-2-0-1-0`, and tier 3's
  `team-split.reuse.0-1-2-0-1-0` does for each link shape. Rosetta refuses
  them at `shmem_init`; on an x86-64 host with io_uring they fail the same
  way, since the order is the compiler's and not the node map's (*observed in
  source*). So the README's first command on an x86-64 machine reports eight
  failures that are the tests', in every preset GCC builds. The hour on an
  x86-64 host that the previous round asked for (its Q3) would meet them first.
  And there, these modes check none of the handles they were written to check.
- **Fix**: before release, take the status first - `int status =
  shmem_team_split_strided(...); check_made(status, made);` - or make the two
  helpers macros that do. No GCC or Clang warning sees this shape; what guards
  it is a run on x86-64, which Rosetta gives for one node (Q4's last note).

### Q2. A node's PEs must share one PID namespace, and the README does not say so: with one each, a PE that dies is never noticed and the job waits for ever; where `/dev/shm` outlives a launch's namespace, `osoe-stat --gc` keeps a dead start's names for good (fix before release: a README sentence; a start-up check after; high on the mechanism, medium on exposure)

- **Where**: `shmem/src/liveness.c:109`, `:147`, `:201` (node-mates watched
  through `pidfd_open` of the pid each published, or `kill(pid, 0)`);
  `shm-object.c:62` (a leftover kept while its creator's pid answers);
  `tools/stat/stat.c:358`, `:1115`, `:1125` (`osoe-stat`'s liveness and
  `--gc`'s judgement, by `kill(pid, 0)`). The design states both assumptions:
  §3.15 (2200-2201) says the watching assumes a node's PEs share a pid
  namespace, and §6.2 (5886-5889) gives the review fixes' caveat about a launch
  in a pid namespace of its own with `/dev/shm` outliving it. The README states
  neither: not in "A PE or a node that dies ends the whole job" (634-635), not
  in "Names a failed start leaves" (995-1032), and not in the container
  paragraph (148-154).
- **Where strangers meet it**: Apptainer's standard way to run a parallel
  program is one container per rank, `mpirun` (for osoe, `srun`) running
  `apptainer exec IMAGE BINARY` (*documented*, Apptainer's MPI guide).
  Apptainer's containers keep the host's PID namespace by default, which is
  fine. Its `--containall` gives each a PID namespace of its own, beside
  `--contain`'s minimal `/dev`, whose list includes `/dev/shm` (*documented*,
  Apptainer's `exec` reference and admin guide; whether that `/dev/shm` is the
  host's they do not say). Docker gives each container a PID namespace of its
  own (*recalled*), and `--ipc=host` shares the host's IPC and `/dev/shm` with
  it (*documented*, Docker's run reference) - the usual remedy for Q3's small
  `/dev/shm`. Slurm's container page says HPC containers usually stop at the
  mount namespace (*documented*).
- **Evidence 1: a PE in a PID namespace of its own** (*measured*): two PEs of a
  one-node job started without a launcher, each under `unshare --pid --fork`
  with a shell as its namespace's pid 1, as a container's init or starter is,
  `/dev/shm` shared. PE 1 kills itself a second after `shmem_init` while PE 0
  waits in `shmem_barrier_all`. In one namespace (the control), PE 0 ends
  within the second: "local PE 1 (rank 1, pid 1155540) has ended without
  finalizing; ending the job (§3.15)". In a namespace each, both PEs are pid 2,
  and the job starts and runs; PE 1 dies (status 137) and PE 0 is still in the
  barrier when my 40 s bound ends it, without a line from the library. Its
  pidfd for "pid 2" watches itself, and the deadlock detector, rightly, proves
  nothing with a PE gone. (A first attempt with each PE as its namespace's
  pid 1 could not die, since a namespace's init ignores `SIGKILL` from inside;
  it did show a job running whole across two namespaces.) `osoe-stat` judges
  liveness by `kill(pid, 0)` too, so it cannot tell from either namespace
  (*observed in source*).
- **Evidence 2: `/dev/shm` outliving a launch's namespace** (*measured*):
  `osoe-launch -n 2` in a fresh PID namespace whose pid 1 is a shell, rank 1
  held back 5 s, and everything in the namespace killed two seconds in, as
  `docker kill` would. It left `osoe.2.0.0.0` with its heap and statics: job
  id 2, `osoe-launch`'s pid there, which every fresh namespace repeats (README
  327-328: an `osoe-launch` job's id is the launcher's process id; the
  launcher-free recipe's `OSOE_JOB_ID=$$` repeats likewise). `osoe-stat --gc`
  from the VM's own namespace, as a site's root timer would run it: "still
  has live PEs; left alone", since pids 4 and 5 there are kernel threads. The
  next launch, in a fresh namespace, is refused: "/dev/shm/osoe.2.0.0.0
  already exists, and its creator, pid 4, is still running: is another job
  using these job and step ids?", pid 4 being that launch's own rank 0.
  `osoe-launch` then removed the names, and the third launch ran. So the
  review fixes' caveat is real; where measured, it ends in this refusal at the
  control segment, before the job tables' and connection area's creator
  check is reached - loud, but naming a cause that is not the cause.
- **Fix**: before release, a README sentence beside the container paragraph
  and the "dies" limit: a node's PEs must share one PID namespace and one
  `/dev/shm`, since osoe watches node-mates, and judges what a failed start
  left, by process id - one container per node, or containers that keep the
  host's PID namespace (Apptainer without `--containall` or `--pid`); and
  `osoe-stat --gc` run in the namespace that made the names. After release,
  and cheap: each PE publishes its PID namespace's identity (the device and
  inode of `/proc/self/ns/pid`) in its slot, and the lead refuses a node whose
  PEs differ, by name. The segment records the lead's, and `--gc` leaves a
  segment of another namespace alone and says why.

### Q3. In a container's default `/dev/shm` a one-node job at the defaults is refused, and 496 of the 690 tests fail, not the "about 300" of the README's new container paragraph (fix before release: a clause; high)

- **Where**: README 148-154 (the review fixes' paragraph: there jobs on one
  node run, and the suite's multi-node tests, about 300, fail); README 557-559
  (start-up refuses heaps that would not fit `/dev/shm`); `node.c:979-998`
  (the refusal).
- **The platforms**: Docker gives a container 64 MiB of `/dev/shm` when
  `--shm-size` is not given (*documented* secondhand: Docker's reference text,
  as several guides quote it; the current CLI page lists the option without a
  default); Podman and Kubernetes pods alike (*recalled*).
- **Evidence** (*measured*, a private 64 MiB tmpfs on `/dev/shm`): the README's
  ring at the defaults is refused at one PE - "/dev/shm has 62 MiB free, but
  this node's control segment and 1 heaps of 268 MiB and as many copies of the
  program's 1 MiB of statics need 271 MiB: lower SHMEM_SYMMETRIC_SIZE, run
  fewer PEs per node, or enlarge /dev/shm - or, ..." - and at two (540 MiB);
  at `SHMEM_SYMMETRIC_SIZE=16m` it runs. The release suite, one test at a time
  as the preset runs it: with io_uring allowed, 194 passed and 496 failed,
  every failure that refusal (757 lines of it, no SIGBUS, no unnamed death).
  With io_uring refused too, as Docker's default profile refuses it, 154
  passed and 536 failed: 213 at `/dev/shm`, 310 at `shmem_init`'s io_uring
  check, 13 opening a ring.
- **The library is right**: it refuses by name, before anything runs, with
  three ways out. The README is wrong: it promises one-node jobs in containers
  and counts only io_uring's failures.
- **Fix**: before release, a clause: container runtimes give `/dev/shm`
  64 MiB unless asked, less than one PE's default heap, so give the container
  a larger `/dev/shm` of its own (`--shm-size`) rather than the host's
  (`--ipc=host`, Q2), or lower `SHMEM_SYMMETRIC_SIZE`; at the default size about
  500 of the 690 tests fail, and some 300 once it is raised. Beside it, the
  refusal's "1 heaps".

### Q4. Minor notes

- **No assertion that the shared 64-bit atomics are lock-free.** `node.h`
  declares 46 `_Atomic uint64_t` fields that processes share - step D added the
  handoff word, the published epoch row, and the free and posted bitmaps - and
  nothing asserts `ATOMIC_LLONG_LOCK_FREE == 2`, as the futex words' size and
  alignment are asserted (*observed in source*). C11 promises address-freedom
  only of lock-free atomics. Both supported architectures have it (in the three
  builds' disassembly every 64-bit atomic is one instruction or one exclusive
  pair), and 32-bit targets are refused at compile time already (`lock.c:83`,
  the previous round). One `_Static_assert` turns the cross-process assumption
  into a refusal.
- **Step D's models check sequential consistency only**, where 19 of the older
  models also run under x86-TSO, and they mark serializing the stores the code
  makes `release` or `relaxed` (the block's name, its record, the map entry,
  the handoff), so a TSO run of them as written would check a stronger program
  than the code. With the stores as the code makes them, all four keep every
  verdict under TSO (below). Register those cases, so that a later change of an
  order is checked where x86 lives.
- **The README's x86-64 sentence** (599-607) describes the tree of 28 September.
  With Q1 fixed it can describe this one: the same one-node tests pass, 355 of
  690 here, every other failure Rosetta's or the cross build's.
- **Step D's 37 `relaxed` accesses** are the correctness review's G2. From this
  question: on x86-64 a relaxed load and an acquire one compile alike, and on
  aarch64 the RCpc and LL/SC builds pass with them.

## x86-64

- **Build** (*measured*): 0 warnings, 39 s at `-j4`, 718 tests registered (690
  not models). Every `_Static_assert` holds with the 64-byte line, the team
  block, the handoff word's line and `team_posted_summary` among them. The
  library has 370 `xchg`, 106 locked instructions and one `lock orq
  $0x0,(%rsp)` for the `seq_cst` fence, and no `mfence`.
- **Suite** (*measured*, under Rosetta with the shim): 355 passed and 335
  failed, each classified by its own output. 308 were refused at `shmem_init`
  for io_uring ("Function not implemented"; one inside `shmem.differential`'s
  runner); 11 open a ring themselves; 9 wrap the job in a seccomp filter, which
  Rosetta answers with `EINVAL`; then `shmem.static-link`, whose nested build
  makes an aarch64 archive, `tools.pacer.installed.0-0-1-1`, which runs `ldd`
  on another architecture - and Q1's five. The multi-node names that passed are
  `shmem.job.*`, the TCP rendezvous, which needs no io_uring. At `08b3da0` the
  previous round counted 347 and 317 of 664.
- **Step D on x86-64** (*measured*): the fourteen one-node team tests the build
  can run passed five times each, 70 runs - `team-split.L1/L2/L4` and
  `.full.L4`, `team-threads.L1/L2/L4`, `team-levels.L1/L2/L4`,
  `team.L1/L2/L4`, `heap-shift.team.L4`. `osoe-pacer teams` ran 90 s on one
  node of 8 PEs, 286,943 rounds, 7.1 million collectives and 95,647 renewals (a
  strided team destroyed and split anew), with `failures=0`; `threads` ran 60 s
  with `failures=0`. That covers the pool's claims, the map, a block's life, the
  generation, the handles and, in `team-threads` and `team-levels`, the pending
  words under `SHMEM_THREAD_MULTIPLE`, on x86 code and TSO hardware.
- **Exports** (*measured*): 3,595 defined dynamic symbols for x86-64 and
  aarch64, the same names: 1,794 strong `pshmem_`, 1,787 weak `shmem_`, no
  `osoe_`, as before step D.
- **Handles** (*observed in source*, `team.c:565-591`): an address plus a
  generation's span, computed on a `uintptr_t` the code asserts is 64-bit,
  decoded by division and never read through. 2^32 generations × 1,021
  descriptors × 152 bytes stays below 2^63 past any heap address either kernel
  gives, so a handle neither wraps nor meets `SHMEM_TEAM_INVALID` (0) or the
  predefined 1 and 2.
- **Not covered**, as before: anything between nodes, an x86-64 kernel, Intel
  and AMD memory systems.

## aarch64 beyond the development machines

| Build | The release suite, `-j4` | Step D's 76 team tests, five times | `osoe-pacer`, 16 PEs on four logical nodes |
|---|---|---|---|
| RCpc, `-mcpu=cortex-a76` | 690 of 690, 133 s | 380 of 380 | `teams` 150 s: 55,330 rounds, 2,367,076 collectives, 18,443 renewals, `failures=0`; `mix` 312 s, `failures=0` |
| LL/SC, `-mno-outline-atomics` | 690 of 690, 139 s | 380 of 380 | `teams` 120 s: 77,971 rounds, 3,336,248 collectives, 25,990 renewals, `failures=0` |

(*measured*; heaps at 64 MiB so that sixteen PEs fit the VM's `/dev/shm`.)

- **What the RCpc run adds**: §3.16's rule that a two-sided protocol's loads are
  `seq_cst` was checked by reading and by a litmus, and round three confirmed the
  shipped library has no `ldapr`. This is the first run of the whole suite with
  every acquire load free to pass an earlier release store - what a Pi 5 user
  who builds with `-mcpu=native` gets, and x86's store-buffer reordering for
  those pairs. It found nothing.
- **It is not the whole of x86-TSO**: a release store followed by a `seq_cst`
  load is ordered on aarch64 in both builds (`stlr; ldar`) and not on x86
  (`mov; mov`). For that shape the TSO models and the reading below stand
  alone.
- **LL/SC** ran on Apple cores, not at a Cortex-A72's timing, as before.

## Step D's protocols under x86-TSO

By reading (*observed in source*), none of step D's protocols has a store whose
order before a later load of another word matters without a locked instruction
between them:

- **The posted bitmap** (`internode-am.c:84-127`, `:180-206`): the handoff's
  release store, two locked `or`s, a locked add to the lead's wake word, and an
  unconditional wake. The agent loads the wake word before it exchanges the
  summary and the bitmap words: message passing.
- **The pending words** (`team.c:1137-1217`): a CAS, then a `seq_cst` load of
  `held`. The keeper does a locked `or` on `held`, then a `seq_cst` store (an
  `xchg` on x86) clearing the word, and an unconditional wake: message passing
  through the word's modification order.
- **A block's life** (`node.c:1258-1402`): the namer's stores are relaxed and
  release, followed by the split's sync, a locked add, before anyone reads
  them. Leaving is locked RMWs throughout, but for the map entry's and the
  record's release stores, each followed at once by one.
- **The dry test** (`osoe_node_team_returning`): loads only, ordered against
  the leavers by the rounds' reductions.

The models agree (*measured*, copies on the Mac; the state counts are SC's
from the record and TSO's from this run):

| Model | Design under SC → TSO | Breakages under TSO |
|---|---|---|
| `team_block` | 122,306 → 323,300, ok with liveness | `no_holders`, `no_relearn`, `map_early`, `last_clears`, `free_first`: each VIOLATION, by the property it names under SC |
| `pool_dry` | member 2,075 → 6,432; parent 90 → 203; remote 62 → 83; split_first 52 → 74; two 13 → 16; each ok with liveness | `first` TRAPPED; `odd_only`, `zero_at_leave`, `first_round_final` VIOLATION (LEGIT); `departing` TRAPPED |
| `split_numbers` | every case the same state count as under SC (194,051 and so on), ok with liveness | `token` TRAPPED, `no_pending` and `release_first` VIOLATION, `higher_waits` STUCK twice, `lower_fails` VIOLATION |
| `posted_bitmap` | 4,627,960 → 5,301,807, ok with liveness (within its 6,000,000-state limit) | `summary_first`, `wake_first`, `bits_first`, `clear_after`: each STUCK |

How they were encoded: a store the code makes `release` or `relaxed` is
buffered (`serial=False`). In `team_block` these are the name's generation,
counters and holders, the record, the map entry, the handoff, and the leave's
record and map entry; in `pool_dry`, the claim's zeroing of the members and the
namer's four stores; `posted_bitmap` already buffered its handoff. A locked add
on a dummy word stands for each sync or reduction arrival the code makes by a
`seq_cst` RMW, which on x86 drains the store buffer: after the map entry's
store (the split's sync), and at each abstract sync and reduction. Without that
step, a ghost sync would let a member past the sync read a map entry still
buffered, which the code forbids. `split_numbers` needed no change: every store
on its path is `seq_cst` or a locked instruction, so its TSO states are its SC
states. The scripts are this review's (`tso/patch.py` in its scratchpad).

## Kernels, Slurm, build and install, and trust: what step D changed

- **Kernels**: no new facility. The pending words' waits are futex2 waits on a
  process-private 32-bit word, and the pool is allocated with `posix_fallocate`
  as the per-number blocks were. The probe's list stands (*observed in source*,
  the diff's new calls).
- **`/dev/shm`**: the pool of team blocks is allocated with the header at the
  segment's creation (`node.c:809-828`), so a full `/dev/shm` refuses by name;
  Q3's 690 tests met no SIGBUS (*measured*).
- **Slurm**: the bootstrap's Slurm path is unchanged. `OSOE_TEAM_BLOCKS` may
  differ between nodes and is checked among a node's PEs; `OSOE_MAX_TEAMS` is
  agreed at the rendezvous as before.
- **Mixed builds**: `OSOE_AM_WIRE_REVISION` (1) is folded into the handler
  checksum, so a binary from before step D and one after refuse each other: "the
  nodes are running different binaries (§3.13)" (`rendezvous.c:114-117`). A
  half-upgraded cluster is told so (*observed in source*). `osoe-stat` reads
  older layouts by design.
- **Architectures across nodes**: the internal region's new split lines are
  placed by `OSOE_INTERNAL_LINE_BYTES` (128), not the compiler's line
  (`internal-region.h:97`, `:113-121`, `:157-163`), so every node computes the
  same offsets. The barrier message's new `lead` is a fixed-width word, and a
  name from another node is bounds-checked before the map is read
  (`node.h:1167`) (*observed in source*). The README's rule that a job's nodes
  share an architecture is unchanged.
- **Build and install**: three builds, 0 warnings; Clang, nothing new (Q5 as
  before). Install was not re-run: step D changed no `install` line in any
  CMake file.
- **Trust**: nothing new beyond the bounds check; the Trust paragraph stands as
  the previous round measured it.

## For step E (kept apart)

- The proposal's E2 still has the small slot "rounded up to the cache line"
  (`docs/design/team-resources-proposal.md:362`). The previous round's note
  stands: the internal region is laid out by its own 128-byte line, the same on
  every node, and E's slots belong on it rather than on `OSOE_CACHELINE` (64 on
  x86-64, 128 on aarch64).
- E's `OSOE_SMALL_COLLECTIVE_BYTES` joins the rendezvous's agreement, a new
  up-header shape and so a new magic, as `rendezvous.c:58-61`'s assertion
  demands.
- E brings a number's cost from about 200 KiB of address space a PE to 15 KiB,
  which eases Q3's containers.

## What is sound, and what convinced me

- **Step D put no architecture into the library.** Its internal-region lines
  are placed by the region's own constant, its wire field is fixed-width and
  bounds-checked, and its handles are 64-bit integer arithmetic, asserted. The
  x86-64 and aarch64 builds export the same 3,595 names, and every static
  assertion holds on both lines.
- **Its protocols hold under the weaker orders.** All four models keep every
  verdict under x86-TSO, every breakage still failing. The suite passes whole
  with every acquire load an `ldapr`, and with every atomic an LL/SC loop. The
  team tests ran 380 times on each, and the stress tester's renewals - each a
  block given back and claimed again at a new generation - ran 18,443 times on
  the one and 25,990 on the other, without a failure.
- **On x86-64 it behaves**: every one-node test passes but Q1's five, which pass
  once the test sequences its call. The team tests ran 70 times, and 95,647
  renewals under the stress tester, on TSO hardware.
- **Refusals are by name**: a `/dev/shm` too small (757 times, no SIGBUS), a
  binary from the other side of step D, a pool too full to create.
- **The previous round's fixes in this area hold as written**: the huge-pages
  text, `--gc` from a timer, and the container paragraph's io_uring half.

## Runs the lead could make elsewhere

1. **x86-64**, any Debian 13 host or cloud VM, for an hour: `cmake --workflow
   --preset release`. Expect exactly Q1's eight failures, or none once it is
   fixed. Any failure in `shmem.multinode.*`, `shmem.internode.*` or
   `tools.pacer.*` would be the first x86-only defect - and this would be the
   first x86-64 message between nodes.
2. **A Pi 5 built for its own core**:
   `cmake -B build/native -DCMAKE_BUILD_TYPE=RelWithDebInfo
   -DCMAKE_C_FLAGS=-mcpu=native`, then `cmake --build build/native`. Check
   that `objdump -d build/native/shmem/libosoe.so.0.1.0 | grep -c ldapr` is
   not 0, run `ctest --test-dir build/native -LE '^model$'` on one node, and
   `srun -K1 --cpu-bind=none -N4 --ntasks-per-node=4
   build/native/tools/pacer/osoe-pacer --seconds 300 mix`. This confirms the
   RCpc result on Cortex-A76 cores; a failure would be the first.
3. **Apptainer on a Slurm cluster**: `srun -N1 -n2 apptainer exec IMAGE ./die`,
   then the same with `--containall` (`die.c` is in this review's scratchpad:
   PE 1 kills itself a second after init, PE 0 waits in a barrier). Expect the
   first to end within a second naming PE 1. Expect the second to hang if
   Apptainer's minimal `/dev/shm` is the host's, or to wait at the start with
   the lead's notice if it is not. Either confirms Q2 and settles which.
4. **Docker**, any Linux host: the release preset in a container at its
   defaults (expect about 536 of 690 failing), with `--shm-size=4g` (about
   310), and with that plus a seccomp profile allowing io_uring (all). This
   confirms Q3 and the README's corrected numbers.

## Sources

The tree at `d8a651d`: `README.md`; `docs/design/shmem-ethernet-design.md` §3.4,
§3.15, §3.16, §5.7 and §6.2; `docs/design/team-resources-proposal.md`;
`docs/decisions/2026-10-03-team-storage.md`,
`docs/decisions/2026-10-03-team-allocation.md` and
`docs/decisions/2026-10-04-review-fixes.md`;
`docs/reviews/archive/phase1-release-fixes-portability.md` and
`phase1-release-fixes-review.md`; `shmem/src/{node.h,node.c,team.c,internode-am.c,
internal-region.h,am.h,am.c,rendezvous.c,shm-object.c,liveness.c,init.c}`;
`tools/stat/stat.c`; `tools/launch/launch.c`;
`shmem/tests/{test-team-split.c,CMakeLists.txt,deny-io-uring.c}`;
`tools/modelcheck/{engine,team_block_model,pool_dry_model,split_numbers_model,
posted_bitmap_model}.py`. Beside it: the correctness review's G2
(`phase1-step7-slice3-correctness.md`), and the previous rounds' futex2 shim and
headers (`build/scratch/review-r3/x86/`).

External (*documented*): Apptainer's user guide (`apptainer exec`'s
`--containall`, `--contain` and `--pid`; "Apptainer and MPI applications") and
admin guide (`mount dev = minimal`); Docker's `docker container run` reference
(`--ipc`; `--shm-size`, whose 64 MiB default is quoted secondhand); Slurm's
"Containers Guide". The C11 standard's 6.5.2.2 is *recalled*.

Logs, probes and scripts, outside the repository, in this session's scratchpad
(`.../scratchpad/q4d/`): `runs/` (the x86-64 suite in two halves, the RCpc and
LL/SC suites, the team repeats, the stress tester's runs, the two 64 MiB suites,
the PID-namespace runs); `classify.py` and `classify2.py`; `probe/` (`split1.c`,
the instrumented `tts.c`, the sequenced `tts2.c`, `order.c`, `die.c`,
`ring.c`); `scripts/` (`build.sh`, `run.sh`, `pacer.sh`, `shm64.sh`, `rank.sh`,
`nsrun.sh`); `tso/` (the patched models and `patch.py`); `clang-pass.py` with
`cc-x86.json`. The VM's copy (`/tmp/q4d`) was removed when done.
