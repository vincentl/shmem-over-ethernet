# Round six, question 4: will osoe run anywhere else?

Review of `main` at `375973a`, run 2026-10-04 and 05 against
`docs/reviews/adversarial-review-prompt.md`, at the merge point of the team
redesign's step E (`docs/decisions/2026-10-04-team-lanes.md`,
`docs/decisions/2026-10-04-team-scans.md`, design §5.7 and §5.8): the release
candidate. The previous round's answer to this question is
`phase1-step7-slice3-portability.md`, at `d8a651d`, and its fixes are
`docs/decisions/2026-10-04-step-d-review-fixes.md`. `main` has since moved by one
commit of text (`b2804cd`, the merge point's record), which this review does not
judge. Labels: *documented* (a source says so: the tree, a vendor's published
text), *observed in source*, *measured* (run here, on this tree), *recalled*
(from memory, not checked).

## Machine and method

The `osoe-x86` Lima VM: Debian 13, kernel 6.12 **arm64**, 4 cores, 6 GiB, its
cores with `lse`, `lrcpc` and `ilrcpc`, Apple's Rosetta registered for x86-64
binaries. Every run in it is an aarch64 run unless it says Rosetta. The review
tree was copied to the VM's `/tmp/q4e` and built four ways, each build and run
under `/tmp/osoe-vm.lock`, every command in the foreground:

- **x86-64**, cross-built with `x86_64-linux-gnu-gcc` 14.2 against
  `liburing-dev:amd64` 2.9 (`RelWithDebInfo`, the `release` preset's type), and
  run under Rosetta with the previous rounds' futex2 shim
  (`build/scratch/review-r3/x86/shim/$LIB/libfutex2-shim.so`). Rosetta is an
  x86-64 user space and memory order on Apple cores, not an x86-64 kernel and
  not an Intel or AMD memory system, and it refuses io_uring.
- **An aarch64 build that orders memory as x86-64 does**, new this round:
  `-mcpu=cortex-a76 -fsigned-char -include x86tso.h`. The header (Appendix)
  redefines the C11 calls the library uses so that every load is an acquire
  load (`ldapr`, which may pass an earlier `stlr`, as a load passes an earlier
  store through x86's store buffer), a `seq_cst` store and every
  read-modify-write end with a full fence (as x86's `xchg` and locked
  instructions are full fences), and `char` is signed, as on x86-64. Unlike
  Rosetta it runs natively, so it brings x86's one reordering and its `char` to
  the multi-node path - step E's whole protocol - which no x86-64 process has
  run. It was shown to reorder as x86 does before anything was believed of it
  (below).
- **aarch64 LL/SC**, `-mno-outline-atomics`: every atomic a
  load-exclusive/store-exclusive loop, as on a Pi 4's Cortex-A72.
- **The `release` preset as the README's quick start runs it**, its six commands
  verbatim.

Besides: `lane_model.py` copied to the Mac and run under the model checker's
x86-TSO mode with every remote put buffered; an audit of every translation
unit's argument evaluation order, x86-64 against aarch64, from GCC's GIMPLE
(round five's Q1 class); the PID-namespace fix and a small `/dev/shm` emulated
with `unshare`, nothing changed outside the namespaces. Nothing ran on the Pi
cluster. No rate limit or interruption touched a result. The VM's copy was
removed when done.

## Verdict

**Step E runs wherever step D ran, and on x86-64's memory order and `char`,
which this round brought to the network path for the first time. In this
question's area the tree can be tagged 0.1.0 as it is: nothing blocks, and
nothing must change first.** Four README sentences want touching, all minor, and
the release checklist's x86-64 item should run one more build so that it sees
step E at all.

What convinced me:

- **Step E put no architecture into the library** (*observed in source*,
  *measured*). Its rows and lanes are placed by the internal region's own
  128-byte line, never the compiler's `OSOE_CACHELINE` (64 on x86-64, 128 on
  aarch64); its grant (24 bytes), lane word (8) and piece header (16) are
  fixed-width and statically asserted, and the assertions hold on both targets;
  the exports are the same 3,595 names on both.
- **Its protocol is message passing**, with no new store-then-load shape: a
  grant or lane word is a put the target's agent copies and then a fenced add,
  read after an acquire load of the counter; the reader's clear is ordered
  before the next write by the reader's own next message. `lane_model.py` keeps
  all 30 verdicts under x86-TSO with every put buffered.
- **The x86-ordered aarch64 build passes everything**: the whole suite, 716 of
  716, every multi-node test among them; step E's lane, scan and pipeline tests
  25 times each (1,200 runs); and 13 minutes of the stress tester across
  logical nodes, every run `failures=0` - its collectives either side of the
  class line at 128, 1,024 and 16,384 bytes, its teams, its threads, and five
  minutes of `mix` at 16 PEs. On the same machine its litmus shows x86's
  reordering where the default build shows none, and none where x86 has none.
- **The LL/SC build passes the same suite** (716 of 716) and step E's tests ten
  times each.
- **On x86-64 under Rosetta every failure is Rosetta's or the cross build's**:
  369 of 716 pass, the 347 others are io_uring refused (336), seccomp refused
  (9), and two harness artefacts. Round five's five order-of-evaluation
  failures pass, and an audit of all 205 translation units, which finds every
  one of round five's 18 calls on its old file, finds no such call now - nor in
  the fixes branch's new test.
- **Refusals are by name**: a `/dev/shm` too small, a node of the other side of
  step E (a new rendezvous magic), a leftover from another PID namespace.

Ranked:

- **Q1, minor**: the README's two x86-64 passages describe the tree of
  28 September, and the release checklist's x86-64 item, run as written, can
  see nothing of step E, whose every test spans nodes.
- **Q2, minor**: the README's container counts have drifted - about 340 tests
  fail with io_uring refused, not "about 320", and about 560 in a default
  `/dev/shm`, not "about 540".
- **Q3, minor**: the quick start's four-PE ring needs about 1.05 GiB of
  `/dev/shm`, so on a 2 GB board (a Raspberry Pi 5 or 4 of 2 GB) its first run is
  refused - by name, with the remedies - and the quick start does not say so.
- **Q4, minor**: the README tells a ported program to wake a waiter with
  `shmem_p`, whose wake design §4.9-§4.10 calls best effort - "on x86 the wake
  can be lost", bounded by the 100 ms sweep.
- **Q5, minor notes**: the PID-namespace message where pids do not repeat
  exactly; step E's two models under sequential consistency only; the
  ThreadSanitizer registrations on the fixes branch on x86-64.

## The previous round's findings, checked as claims

| Round five | What was asked | On this tree | Evidence |
|---|---|---|---|
| Q1, fix before release | the split tests read the handle beside the call that writes it | **holds**: `check_made` and `check_failed` are macros taking the status first; the five tests that failed on x86-64 pass there (`team-split.exhaust.L4`, `.too-many.L4`, `.dry.L2`, `.reuse.L4`, `.stale-handle.L2`), and the order audit (below) finds all 18 calls on `d8a651d`'s file and none at `375973a` | *measured* |
| Q2, fix before release | a node's PEs in one PID namespace | **holds**: README 202-215 and the "provided" of Failures (867-869); `shm-object.c:63-75`'s new refusal measured where the pids repeat, the old message where they do not, as the README says (Q5 here) | *measured* |
| Q3, fix before release | a container's 64 MiB `/dev/shm` | **holds in substance**: README 194-200 gives the cause and the remedy; its counts have drifted with step E's tests (Q2 here) | *measured* in part |
| Q4, minor | lock-free assertion; D's models under SC; the README's x86-64 sentence | the first two on the release checklist's after-release list; **the sentence not updated** (Q1 here) | *observed in source* |
| For step E | E's slots on the region's own line, not `OSOE_CACHELINE` | **holds**: `internal-region.h` and `internode-generic.c` name neither `OSOE_CACHELINE` nor `_Alignas`; every row, counter and lane is placed by `OSOE_INTERNAL_LINE_BYTES` | *observed in source* |
| For step E | `OSOE_SMALL_COLLECTIVE_BYTES` in the agreement, a new magic | **holds**: `rendezvous.c:32` (`osoeup-4`), `:38-70` (the field and a zero word, the 56-byte header asserted on both targets), `:162-164` | *observed in source*, *measured* (built) |
| For step E | run E through the three builds, its grants through TSO, the one-node suite under Rosetta | **done**: below | *measured* |
| Round four Q3, after release | the x86-64 network path never run | **still never run on x86-64**; x86's memory order now reaches it through the x86-ordered build | *measured* |

## New findings, ranked

### Q1. The README's x86-64 passages describe the tree of 28 September, and the checklist's x86-64 item, run as written, sees nothing of step E (minor; high)

- **Where**: README 129-133 ("An x86-64 build of an earlier tree passed most of
  the one-node tests...") and 832-840 ("On x86-64 the tree of 28 September, before
  this one's release fixes, ... more than 300 in release"); release checklist,
  "x86-64 on the tagged tree: it builds and passes its one-node tests ... so that
  the README's sentence on architectures is true of it".
- **Evidence** (*measured*, below): at `375973a` the x86-64 build has 0 warnings
  and passes 369 of its 716 tests under Rosetta, every failure Rosetta's or the
  cross build's. Both passages are true and dated. But all 16 of step E's
  `shmem.lanes.*` tests span logical nodes, and Rosetta refuses io_uring, so
  they are among the 336 refused: the item as written checks the build and
  nothing of the lanes, the grants or the scans' new path. A large collective
  on one node never reaches the internode phase.
- **Fix**: at the tag, the two passages with the tagged tree's numbers (369 of
  716 here, every failure in the six classes below); and the item extended to
  run the x86-ordered aarch64 build's suite and a `mix` across logical nodes
  ("What the checklist's x86-64 item should run", below). Neither is a code
  change.

### Q2. The README's container counts have drifted with step E's tests (minor; high on the io_uring count, medium on the other)

- **Where**: README 191-192 ("the test suite's multi-node tests, about 320 of
  them, fail at `shmem_init` or at opening a ring") and 197 ("about 540 of the
  suite's tests fail").
- **Evidence**: Rosetta refuses io_uring with `ENOSYS`, as Podman's profile does,
  so the x86-64 run counts the io_uring refusals of this tree: 324 at
  `shmem_init`, one inside `shmem.differential`'s runner, 11 opening a ring -
  336, where round five counted 319 of the same classes (*measured*): step E's
  16 lanes tests and one more of its multi-node registrations. The 64 MiB count
  was not re-run: round five's 536 plus the 26 tests added since, nearly all
  starting a PE at the default heap, is about 560 (*not measured*).
- **Fix**: "about 340" and "about 560", or a share ("half the suite", "four
  in five of its tests") that does not drift with every slice.

### Q3. The quick start's four-PE ring needs about 1.05 GiB of `/dev/shm`, so its first run is refused on a 2 GB board (minor; high)

- **Where**: README 78-113 (the quick start, new since round five): "On Debian 13
  or Raspberry Pi OS Trixie ... run it on this machine as four PEs"; Requirements
  (117-141) name no memory.
- **Evidence** (*measured*, a private tmpfs on `/dev/shm` in a mount namespace):
  at 960 MiB - the default `/dev/shm` of a 2 GB machine, half its memory - the
  README's `osoe-launch -n 4 -- ./ring` is refused at once: "/dev/shm has 958
  MiB free, but this node's control segment and 4 heaps of 261 MiB and as many
  copies of the program's 1 MiB of statics need 1051 MiB: lower
  SHMEM_SYMMETRIC_SIZE, run fewer PEs per node, or enlarge /dev/shm (a
  container's --shm-size) - or, since pages cost only once written,
  OSOE_SHM_CAPACITY_CHECK=0 reports this without refusing". At 1,100 MiB it runs.
  Raspberry Pi 5 and Pi 4 boards are sold with 1 and 2 GB as well as 4 and 8
  (*recalled*).
- **The library is right**: loud, at once, with three ways out. The quick start
  promises four lines and status 0.
- **Fix**: a clause - the four-PE run needs about 1.1 GB of `/dev/shm`, half the
  memory by default; on a smaller machine prefix `SHMEM_SYMMETRIC_SIZE=64m`.

### Q4. The README advises `shmem_p` to wake a waiter, whose wake the design calls best effort - "on x86 the wake can be lost" (minor; high on the text, the loss itself not measured)

- **Where**: README 950-958 ("A wait is woken by the library's own operations -
  atomics, puts, signals ... should store with `shmem_atomic_set`, `shmem_p` or a
  signal"); design §4.9 (3338-3345: "Exact for AMOs, best effort for puts ... on
  x86 the wake can be lost. The safety-sweep deadline bounds that loss") and §4.10
  (3694-3697); `range_watch_model.py`'s `plainput`; `region-wait.c:59-74`, which
  counts such a loss at two-byte widths only as a late wake.
- **Mechanism** (*observed in source*): the watch is two-sided (§3.16). The
  waiter's stores are locked or `seq_cst` (`watch.c:74-76`); a put's writer side
  is a `memcpy` and then the bitmap's `seq_cst` load
  (`transport-local.c:61-74`, `progress-host.c:206-212`, `watch.c:106-117`), with
  no fence between, so the load may be satisfied before the copy is visible - on
  x86 through the store buffer, and on aarch64 too, where a plain store may pass a
  later `ldar` - and a waiter registering in that instant sleeps until its 100 ms
  sweep. The specification has a waited-for word updated by an atomic operation
  (1.5's change 17), so a conforming program never meets it; the README's advice
  names `shmem_p`, which is a put.
- **Fix**: drop `shmem_p` from that list, or say that a put's wake is best effort
  and can wait for the sweep.

### Q5. Minor notes

- **The PID-namespace refusal where pids do not repeat exactly.** Round five's
  emulation on this tree (*measured*): a two-PE launch in a fresh PID namespace,
  killed two seconds in, left `osoe.2.0.0.0` and its heap and statics; a second
  launch in a fresh namespace with the same process tree was refused with the new
  message - "its creator's pid 4 this process's own: another PID namespace left it
  (a node's PEs must share one); if no job of these ids runs, rm -f
  /dev/shm/osoe.<job id>.*" - and after that `rm` a third ran. With one process
  fewer in the second namespace, rank 0 was pid 3 and the recorded creator, pid 4,
  was its own rank 1: the old message, "is still running: is another job using
  these job and step ids?", naming a cause that is not the cause. The README says
  as much ("saying another PID namespace left them, where the pids repeat"), and
  the start-up check that compares namespaces is on the after-release list.
  Identical containers repeat their process trees, so the new message is the one
  they meet.
- **Step E's two models run under sequential consistency only**, as step D's
  four do (on the after-release list). `lane_model.py` keeps all 30 verdicts
  under TSO with its puts buffered (below); `scan_pipeline_model.py` writes its
  slots with `nd` steps, which the engine never buffers, so a TSO run of it
  equals its SC run. Registering `lane_model.py`'s TSO cases costs 3 s.
- **The fixes branch's ThreadSanitizer registrations on x86-64.** The branch
  (`review-six-fixes`, read, not judged) registers the lanes tests and the new
  `shmem.lanes.threads` under tsan, one PE a logical node. On x86-64, tsan's
  shadow begins at `0x100000000000` and its metadata at `0x300000000000`
  (*recalled*: compiler-rt's x86-64 mapping since its v3 runtime), the library's
  first two heap candidates (`node.c:53-58`), so a tsan build there takes the
  third, `0x010000000000`, inside tsan's low application range - round three
  reasoned the same, and no x86-64 tsan build has run. The new test itself is
  portable: it compiles for both targets with no order hazard, assumes no core
  count or affinity, and its 120 s bound is some sixty times its run.

## Step E on x86-64's memory order: the x86-ordered aarch64 build

**Why a build, and why it can be believed.** x86-TSO allows one reordering, a
load passing an earlier store to another word. Every aarch64 run before this
round hid it for the code's `seq_cst` loads: GCC compiles them to `ldar`, which
never passes an earlier `stlr` (RCsc), where x86 compiles a release store and a
`seq_cst` load to two plain `mov`s. Round five's RCpc build moved the *acquire*
loads to `ldapr` and left the `seq_cst` ones as `ldar`; this build moves them
too, and keeps the stores and read-modify-writes that x86 makes full fences full
fences. It was checked on the machine first, with a store-buffering litmus after
the project's own (`docs/design/evidence/fence-litmus/`), 2,000,000 rounds each,
through the same C11 calls the library makes (*measured*):

| Store, then load of the other word | x86-64 | Default aarch64 build | x86-ordered build | Rosetta, x86-64 code |
|---|---|---|---|---|
| release store, `seq_cst` load | allowed | 0 both-zero (`stlr; ldar`) | **527** (`stlr; ldapr`) | **185** |
| `seq_cst` store, `seq_cst` load | forbidden | 0 | 0 | 0 |
| `seq_cst` fetch-add, `seq_cst` load | forbidden | 0 | 0 | 0 |
| release fetch-add, `seq_cst` load | forbidden | 0 | 0 | 0 |

So the build shows x86's reordering where x86 has it, at a rate like Rosetta's,
and only there. In the library it holds 320 `ldapr`, no `ldar`, 268 `stlr`, 131
`dmb ish` and no outline helper (*measured*, `objdump`). It is weaker than x86 for
plain accesses (aarch64 may reorder plain stores with one another), so a failure
here could be aarch64's own; a pass means no x86-only ordering failure among what
ran.

**What ran** (*measured*, all natively in the VM, logical nodes over loopback
TCP):

| Run | Result |
|---|---|
| The release suite, `-j4` | 716 of 716, 132 s |
| Step E's and the collectives' tests (lanes, scan, pipeline, reduce, broadcast, collect, allgather, internal region, team split and storage, internode), 137 tests 4 times | 548 of 548 |
| `shmem.lanes.*`, `shmem.scan.*`, `shmem.pipeline.*`, 48 tests 25 times | 1,200 of 1,200, 81 s |
| `osoe-pacer collectives`, 6 PEs on 3 nodes, at `OSOE_SMALL_COLLECTIVE_BYTES` 128, 1,024, 16,384 | 60 s each: 1,019, 1,377 and 1,292 rounds, 36,933, 49,776 and 46,692 calls, `failures=0` |
| `osoe-pacer teams`, 8 PEs on 4 nodes | 90 s: 90,076 rounds, 2,233,438 collectives, 30,025 renewals, `failures=0` |
| `osoe-pacer threads`, 8 PEs on 4 nodes | 90 s: 60,755,200 operations, `failures=0` |
| `osoe-pacer mix`, 16 PEs on 4 nodes | 305 s, `failures=0` |

Heaps at 64 MiB for the stress tester, so that sixteen PEs fit the VM's
`/dev/shm`. The `-fsigned-char` in the same build covers the other ABI difference
a run can show: x86-64's `char` is signed, aarch64's unsigned; nothing failed.

**Not covered**: the real thing - an x86-64 kernel, Intel's and AMD's memory
systems, x87 `long double` between nodes (by reading the same combine code that
passes under Rosetta within a node, its 16 bytes travelling whole, nothing
comparing them bytewise), and `_Atomic` objects read or written by operator,
which the header cannot reach (`shmem.memory-orders` cannot see them either) and
which stay `ldar` and `stlr` here.

## x86-64 under Rosetta

- **Build** (*measured*): 0 warnings, 18 s at `-j4`; 745 tests registered, 716
  not models. Every `_Static_assert` holds, the up header's 56 bytes, the grant's
  24, the lane word and the piece header among them.
- **Suite** (*measured*, two halves, `-j4`, classified by each test's own
  output): **369 passed, 347 failed**: 324 refused at `shmem_init` for io_uring
  ("Function not implemented"), one more inside `shmem.differential`'s runner,
  11 opening a ring themselves (`env.probe` and `env.echo-uring` among them), 9
  wrapping the job in a seccomp filter, which Rosetta answers with `EINVAL`, then
  `shmem.static-link`, whose nested build makes an aarch64 archive, and
  `tools.pacer.installed.0-0-1-1`, which runs `ldd` on another architecture.
  None is x86-64's. Round five counted 355 and 335 of 690.
- **Step E there**: `shmem.small-collective-bytes.*` and
  `shmem.job.disagree.small-collective-bytes` pass; every `shmem.lanes.*` test is
  among the io_uring refusals. A one-node job never reaches the lanes.
- **Exports** (*measured*): 3,595 defined dynamic symbols on both targets, the
  same names - 1,794 strong `pshmem_`, 1,787 weak `shmem_`, no `osoe_` - as at
  step D: `osoe_internode_reduce_enter` stays hidden.
- **The README's team-block figure** (*measured*, `sizeof` on both targets): a
  block is 1,024 bytes on aarch64 and 576 on x86-64, as README 636-637 says.

## aarch64 LL/SC

`-mno-outline-atomics`: 116 `ldaxr`/`stlxr` pairs, no LSE instruction, no
helper call (*measured*). The release suite 716 of 716 in 119 s; the lanes, scan
and pipeline tests 10 times each, 480 of 480; `osoe-pacer collectives` at 128
bytes, 60 s, 1,456 rounds, `failures=0`. On Apple cores, not a Cortex-A72's
timing, as before.

## Step E's protocol under x86-TSO, by reading and by the model

By reading (*observed in source*), no part of step E orders a store before a
later load of another word without a locked instruction between them:

- **The grant** (`internode-generic.c:1150-1175`, `:856-886`): a put of its 24
  bytes and a fenced add, both applied by the child's node's agent - a copy, then
  a locked add on x86. The child loads the counter acquire (`grant_in`, `:831`),
  then copies the words: message passing.
- **The lane word** (`:956-970`, `:897-911`): the child's put before its first
  fenced add; the parent copies it after the acquire load that showed the add.
- **A reader clears what it read** (`:838-854`): plain stores by the reader,
  which precede its next message, published to its node's agent by a release or
  `seq_cst` operation; the next write of the cleared words is the agent applying
  a message that message caused - store, then release, then the agent's acquire,
  then its store: no load waits on the clear.
- **The watch** (`watch.c:52-78`) is unchanged; step E widened the range one
  watch covers to the grant counter (the lanes record, "The loop's one watch").
  Its two-sided stores are still locked or `seq_cst`, which the x86-ordered
  build keeps full fences.

The model agrees (*measured*, a copy on the Mac): `lane_model.py` with every
remote put a buffered store (`serial=False`, as the agent's copy is) and
`explore(..., tso=True)`: all 30 cases keep their verdicts, line for line - 18
`ok` with liveness (the designs, and the `release_lane_late` case with more
pieces than the window, which holds by design), 12 `VIOLATION` by the property
they fail by under SC (`no_grant`, `stale_grant`, `one_lane`, `lane_early`,
`release_lane_early`, `node_small`, `release_lane_late`) - at 1.2 to 1.6 times
the SC state counts, in 3.4 s.

## Order of evaluation, the class round five found

GCC evaluates a call's arguments right to left for x86-64 and left to right for
aarch64, an order C11 leaves unspecified (round five, Q1). The audit
(`order.py`, in this review's scratchpad) compiles every translation unit for
both targets with `-fdump-tree-gimple` and reports a *hazard* - a variable, or an
element or member of one, read into a temporary, then a call that may write it
(one passed its address, or any call for a variable not the function's own),
then the temporary used - that one target has and the other does not.

- **Checked against what is known to be there** (*measured*): on `d8a651d`'s
  `test-team-split.c` it reports 17 hazards, x86-64 only, covering all 18 of
  round five's calls; on `375973a`'s, none.
- **The tree** (*measured*): none in 205 translation units - the library, base,
  the tests, the tools - nor in the fixes branch's `test-lanes-threads.c`. It
  reports 109 one-target reads before a call, every one before a helper that
  writes nothing the read names (`value`, `pattern`, `strerror`,
  `__errno_location`, `random_name`, `slot`, `number_of`, `osoe_node_of` and
  their kind). A second pass, on functions whose sequence of calls differs
  between the targets, finds 16, every moved call a pure helper (`kind_name`,
  `child_index`, `offset_detail`, `want_of`, the pacer's oracles).

## Kernels, Slurm, build and install, and trust: what step E changed

- **Kernels**: no new facility. The diff since `d8a651d` adds no system call, no
  io_uring operation and no futex use; a grant is a put and a fenced add on the
  transport as it was. The probe's list stands (*observed in source*).
- **`/dev/shm`**: a PE's internal region at the default 256 numbers is 6.03 MiB
  at 4 nodes, against 12.6 at 64 numbers before step E (the scans record), so a
  node's need fell; the capacity check counts it, and refused by name in every
  run here that met it (Q3).
- **Slurm**: no change to the bootstrap's Slurm path. `OSOE_SMALL_COLLECTIVE_BYTES`
  is read by every PE, compared among a node's PEs and agreed at the rendezvous,
  as `OSOE_COLLECTIVE_PIPELINE` is; a heterogeneous job is refused as before.
- **Mixed builds**: a node from before step E and one after refuse each other by
  name (*observed in source*): the parent reads 56 bytes of up header
  (`rendezvous.c:159-164`), which an old child's 48 and the start of its first
  node record supply, and the magic differs ("node N sent a rendezvous report
  this build cannot read: the nodes are running different osoe libraries"); an
  old parent reads 48 and meets `osoeup-4`.
- **Architectures across nodes**: the region's offsets are the region's own
  line on both targets, the grant and lane word fixed-width, so the README's
  paragraph on a mixed job (841-848: it would start, its `long double` values
  wrong) is still exact; step E adds nothing a mixed job would trip on. Its rule
  that a job's nodes share an architecture stands.
- **Build and install**: step E changed the build files only in comments and by
  `perturb.c`. The quick start's six commands, run verbatim on a fresh copy:
  configure and build with 0 warnings, the ring's four lines and status 0 on one
  node and on two logical nodes, and the probe passing (*measured*).
- **Trust**: what step E takes from a peer is checked. A grant's epoch, identity
  and bytes must match the collective, and a lane index from a grant or a lane
  word must be below `OSOE_MAX_THREADS_HIGHEST`, or the job ends naming both
  leads (`internode-generic.c:868-883`, `:904-910`, `:1884-1887`, `:1900`); and
  the agent that applies a put into the internal region refuses an offset past
  the region's end (`progress-host.c:67-82`), so an index within that bound but
  past the peer's own lanes ends the job too, with a message that blames the
  settings (*observed in source*). A peer that holds the job's secret is trusted
  otherwise, as the README's Trust paragraph says.

## What the checklist's x86-64 item should run

1. **As written**, in `osoe-x86`: the release suite of the tagged tree under
   Rosetta with the futex2 shim. Expect 369 of 716 here (more with the fixes'
   tests), every failure in the six classes above; write the README's two
   passages from it (Q1).
2. **Added**: the x86-ordered aarch64 build of the tagged tree, `CMAKE_C_FLAGS`
   `-mcpu=cortex-a76 -fsigned-char -include <dir>/x86tso.h` (the Appendix's
   header): its release suite at `-j4`, the lanes, scan and pipeline tests
   repeated, and `osoe-pacer mix` on 16 PEs over 4 logical nodes. It is the only
   run that brings x86's memory order to step E, and with the litmus above it is
   ten minutes. The header could live in the tree beside
   `cmake/CheckMemoryOrders.cmake`, which already reads the same calls.
3. **After the release**, as round four left it: an x86-64 kernel. Any Debian 13
   x86-64 host; `dev/lima/create.sh` run on an Intel Mac, whose template pins no
   architecture, makes one; on Apple silicon, a Lima VM with `--arch=x86_64`
   runs one emulated, slowly (*recalled*).

## What is sound, and what convinced me

- **Step E put no architecture into the library**: its layout is the region's
  own line, its wire words fixed-width and asserted on both targets, its exports
  unchanged, its one new call hidden.
- **Its protocol holds under the weaker orders**: message passing with no new
  two-sided shape; `lane_model.py`'s verdicts unchanged under x86-TSO; the whole
  suite and 13 minutes of the stress tester with x86's reordering in every load,
  the lanes tests 1,200 times over, and the suite again with every atomic an
  LL/SC loop.
- **On x86-64 it behaves**: every one-node test passes but those Rosetta cannot
  run, round five's five among the passes; no order-of-evaluation hazard
  remains in any translation unit, by an audit shown to find the ones that were
  there.
- **The previous round's fixes in this area hold as written**: the split
  macros, the PID-namespace paragraph and refusal, the container paragraph's
  cause and remedy.
- **Refusals are by name**: a `/dev/shm` too small, a build from the other side
  of step E, a leftover from another namespace.

## Runs the lead could make elsewhere

1. **The x86-ordered build on the Pi 5**, whose Cortex-A76 has RCpc: from the
   cluster's tree, `cmake -B build/x86order -DCMAKE_BUILD_TYPE=RelWithDebInfo
   "-DCMAKE_C_FLAGS=-mcpu=cortex-a76 -fsigned-char -include $PWD/x86tso.h"`,
   `cmake --build build/x86order -j4`, then `srun -K1 --cpu-bind=none -N4
   --ntasks-per-node=4 build/x86order/tools/pacer/osoe-pacer --seconds 600 mix`
   and the same with `collectives` under `OSOE_SMALL_COLLECTIVE_BYTES=128`.
   `objdump -d build/x86order/shmem/libosoe.so.0.1.0 | grep -cP '\tldar'` should
   be 0. `failures=0` carries this review's result onto real nodes and gigabit's
   timing; a failure would be the first x86-only defect, or the Pi's own.
2. **An x86-64 host**, for an hour: `cmake --workflow --preset release`. Expect
   no failure. Any in `shmem.lanes.*`, `shmem.internode.*`, `shmem.multinode.*`
   or `tools.pacer.*` would be the first x86-64 message between nodes going
   wrong.
3. **Docker on any Linux host**, for Q2's counts: the release preset in a
   container at its defaults, then with `--shm-size=4g`.

## Sources

The tree at `375973a`: `README.md`; `docs/design/shmem-ethernet-design.md` §3.16,
§4.9, §4.10, §5.7, §5.8, §6.2; `docs/decisions/2026-10-04-team-lanes.md`,
`2026-10-04-team-scans.md`, `2026-10-04-step-d-review-fixes.md`;
`docs/release-checklist.md`; `docs/prompts/phase1-step7-slice4-prompt.md`;
`docs/reviews/archive/phase1-step7-slice3-portability.md` and
`phase1-step7-slice3-review.md`, `phase1-release-portability.md`;
`shmem/src/{internal-region.h,internode-generic.c,rendezvous.c,progress-host.c,
transport-local.c,watch.c,region-wait.c,shm-object.c,node.c,perturb.c,
init.c}`; `base/{cacheline.h,order.h}`; `shmem/tests/{test-team-split.c,
invariants.h,CMakeLists.txt}`; `dev/lima/{osoe.yaml,create.sh}`;
`tools/modelcheck/{engine,lane_model,scan_pipeline_model,range_watch_model}.py`;
`docs/design/evidence/fence-litmus/`. The fixes branch `review-six-fixes`:
`shmem/tests/test-lanes-threads.c` and its registrations, read only.

External: compiler-rt's ThreadSanitizer x86-64 address map (*recalled*);
Raspberry Pi board memory sizes (*recalled*); Lima's `--arch` (*recalled*).

Logs, probes and scripts, outside the repository, in this session's scratchpad
(`.../scratchpad/q4e/`): `probe/` (`x86tso.h`, `sb.c`, `sizes.c`,
`ring.c`), `scripts/` (`build.sh`, `run.sh`, `pacer.sh`, `order.py`,
`calls.py`, `nsrun.sh`, `slow1.sh`), `runs/` (the x86-64 suite in two halves),
`classify.py` (round five's), `tso/` (the TSO copy of `lane_model.py` and both
outputs). The VM's copy (`/tmp/q4e`), with its other logs, was removed when done.

## Appendix: `x86tso.h`

```c
/* Force-included (-include) into an aarch64 build for an RCpc core
 * (-mcpu=cortex-a76): the C11 atomics then order memory as x86-64 orders them. */
#ifndef REVIEW_X86TSO_H
#define REVIEW_X86TSO_H
#include <stdatomic.h>

#define REVIEW_X86TSO_FULL() __atomic_thread_fence(__ATOMIC_SEQ_CST)

/* Every load acquire (ldapr): a seq_cst load may pass an earlier stlr, as a mov
 * passes an earlier mov through x86's store buffer. */
#undef atomic_load_explicit
#define atomic_load_explicit(PTR, MO)                                              \
    __extension__({                                                                \
        __auto_type review_x86tso_p = (PTR);                                       \
        __typeof__((void)0, *review_x86tso_p) review_x86tso_r;                     \
        __atomic_load(review_x86tso_p, &review_x86tso_r,                           \
                      (MO) == memory_order_seq_cst ? memory_order_acquire : (MO)); \
        review_x86tso_r;                                                           \
    })

/* A seq_cst store is an xchg on x86-64: a full fence after it. */
#undef atomic_store_explicit
#define atomic_store_explicit(PTR, VAL, MO)                                        \
    __extension__({                                                                \
        __auto_type review_x86tso_p = (PTR);                                       \
        __typeof__((void)0, *review_x86tso_p) review_x86tso_v = (VAL);             \
        __atomic_store(review_x86tso_p, &review_x86tso_v, (MO));                   \
        if ((MO) == memory_order_seq_cst)                                          \
            REVIEW_X86TSO_FULL();                                                  \
        (void)0;                                                                   \
    })

/* Every read-modify-write is a locked instruction on x86-64, whatever its order. */
#define REVIEW_X86TSO_RMW(EXPR)                                                    \
    __extension__({                                                                \
        __auto_type review_x86tso_r = (EXPR);                                      \
        REVIEW_X86TSO_FULL();                                                      \
        review_x86tso_r;                                                           \
    })

#undef atomic_fetch_add_explicit
#define atomic_fetch_add_explicit(P, V, MO) REVIEW_X86TSO_RMW(__atomic_fetch_add((P), (V), (MO)))
#undef atomic_fetch_sub_explicit
#define atomic_fetch_sub_explicit(P, V, MO) REVIEW_X86TSO_RMW(__atomic_fetch_sub((P), (V), (MO)))
#undef atomic_fetch_or_explicit
#define atomic_fetch_or_explicit(P, V, MO) REVIEW_X86TSO_RMW(__atomic_fetch_or((P), (V), (MO)))
#undef atomic_fetch_and_explicit
#define atomic_fetch_and_explicit(P, V, MO) REVIEW_X86TSO_RMW(__atomic_fetch_and((P), (V), (MO)))
#undef atomic_fetch_xor_explicit
#define atomic_fetch_xor_explicit(P, V, MO) REVIEW_X86TSO_RMW(__atomic_fetch_xor((P), (V), (MO)))
#undef atomic_exchange_explicit
#define atomic_exchange_explicit(P, V, MO) REVIEW_X86TSO_RMW(__atomic_exchange_n((P), (V), (MO)))
#undef atomic_compare_exchange_strong_explicit
#define atomic_compare_exchange_strong_explicit(P, E, D, S, F) \
    REVIEW_X86TSO_RMW(__atomic_compare_exchange_n((P), (E), (D), 0, (S), (F)))
#undef atomic_compare_exchange_weak_explicit
#define atomic_compare_exchange_weak_explicit(P, E, D, S, F) \
    REVIEW_X86TSO_RMW(__atomic_compare_exchange_n((P), (E), (D), 1, (S), (F)))

#endif
```
