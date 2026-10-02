# Round three, question 4: will osoe run anywhere else?

Review of `main` at `b0e0b53`, run 2026-09-28 against
`docs/reviews/adversarial-review-prompt.md`. Everything before this round ran
on aarch64 under one kernel family: the author's Lima VM and the Raspberry Pi
cluster. Investigators: `x86` (the tree cross-built for x86-64 and run under
Rosetta on an aarch64 kernel, in a VM made for this review), `trust-kernel`
(the trust boundary fuzzed at the rendezvous port; the kernel facilities
checked and denied one by one), `slurm` (every Slurm shape the VM's Slurm
24.11 could run or `osoe-launch -s` and two network namespaces could emulate),
`install` (the build and install as a stranger; the readiness deliverable has
that part). The `trust-kernel` investigator's write-up was cut off by a
safety classifier after its runs; its scripts and raw outputs
(`build/scratch/review-r3/trust-kernel/`, the VM's `/tmp/trust-kernel-*.out`)
were read by the lead, who also re-read the listener, and that section is
the lead's. Labels: *documented*, *observed in source*, *measured*,
*recalled*; "lead-verified" means the lead re-read or re-ran it.

## Verdict

**It will run where the network is what osoe assumes, and it will tell the
user little when the network is not.** Under every Slurm shape that could be
run or emulated - block, cyclic, plane, arbitrary and uneven distributions,
`-c 2`, every `--cpu-bind` form, `--hint`, `--exclusive`, `--overlap`,
`--mem-bind`, `--overcommit`, `--mpi=pmi2` and `pmix`, batch and `salloc`
shells, concurrent steps, job arrays - osoe starts and the job is right
(*measured*). The host-list grammar agrees with Slurm's own `scontrol` on 32
forms and 19 of 19 round trips. The kernel facilities it needs are refused by
name when absent, and a one-node job runs where io_uring is disabled. What
fails a stranger is start-up on a network that is not fully open: a node
that cannot reach a peer - a firewall on the mesh's kernel-chosen ports, a
name resolving to the wrong address - waits forever and prints nothing
(N1); and the rendezvous port accepts, before any proof of the secret, one
connection that can end or stall the job's start (N2). x86-64 is in the
section of that name.

## Findings, ranked

### N1. A node that cannot reach another waits forever and prints nothing (fix before release; high; *measured* by `slurm`, mechanism lead-verified)

- **Location**: `shmem/src/listener.c:409-433` (`dial` returns -1 for any
  connect failure and discards the error), `:435-481`
  (`osoe_listener_connect_port` retries forever, sleeping 0.2 to 10 ms, with no
  notice; only `OSOE_NO_PROGRESS_TIMEOUT` ends it, and its message names the
  node number and port, not the address or the error), `:550-598` (the mesh's
  equivalent). Design §3.15 ("no wait gives up because time has passed") is
  respected; the silence is the defect.
- **Scenario**, each *measured* to hang silently until killed: an emulated
  firewall rejecting the mesh's ports (32768-60999); one node given an address
  for another where nothing listens; two network namespaces where one node
  resolves the other's name to its second address. With
  `OSOE_NO_PROGRESS_TIMEOUT=5` each ends in 5 s with "node 0 has not answered
  node 1 on its port N" or "waiting for another node to connect", naming
  neither host, address nor `ECONNREFUSED`. The README's "TCP must be open
  between the nodes, not only that range" is the only warning a stranger
  gets, and a cluster whose firewall opens a port range has no osoe setting
  that works: the mesh listens on a kernel-chosen port
  (`progress-host.c:372`; `OSOE_PORT_RANGE` covers the rendezvous only, and
  the README says so).
- **Severity**: fix before release: one notice after about 30 s, repeated
  each minute, naming the entry, address, port and last error, without
  giving up; after release, a way to bound the mesh's ports for firewalled
  sites.
- **Confidence**: high.

### N2. The rendezvous port accepts, before any proof of the secret, one connection that can end or stall a job's start (fix before release; high; *measured* by `trust-kernel`, lead-verified in source)

- **Location**: `shmem/src/listener.c:334-404` (`osoe_listener_accept`): a
  hello with the job's ids but a different task or node count is fatal
  ("every lead must be given the same", `:366-370`), and so is one "claiming
  to be node N" out of range (`:373-375`), both **before** the answer and
  before the secret is proved; after an accepted hello the confirmation is
  read with no time limit (`read_all(..., 0, ...)`, `:384`), so a caller that
  never confirms holds the accept loop while the real peer's connection waits
  in the backlog, for the job's life unless `OSOE_NO_PROGRESS_TIMEOUT` is set.
  The port is derived from the job, step and node ids, which `squeue` shows
  (design §6.2).
- **Scenario** (*measured*, `trust-kernel/fuzz_hello.py` and `drive_hello.sh`
  against node 0 of a two-node job started alone, `OSOE_NO_PROGRESS_TIMEOUT=5`
  so the runs end): random bytes, a wrong job, a wrong step and a wrong target
  node are dropped or answered NOT_THIS_NODE and node 0 keeps listening
  (sound). A hello with a huge task count ends node 0 at once, with and
  without `OSOE_JOB_SECRET` set. An accepted hello followed by silence holds
  node 0 "waiting for node 1 to send" until the timeout, with and without the
  secret. Without the secret, an accepted hello followed by a wrong
  confirmation is taken for node 1, and node 0 ends when "the rendezvous lost
  node 1"; with the secret, the caller is refused by name and node 0 keeps
  listening (sound). The listener is open only during `shmem_init`
  (rendezvous listener closed once the children are in, `rendezvous.c:313`;
  mesh listener closed once the lower nodes have connected, `wire.c:197`;
  *measured*), so the exposure is the start-up window.
- **What is assumed and where it is said**: the design states it (§3.4, §6.2:
  all PEs trust each other; other local users are untrusted, with mode 0600,
  `O_EXCL` and `st_uid` checked, `shm-object.c:25-45`; the network is trusted
  as Slurm's own traffic is; the secret "authenticates the hello, not the
  traffic after it"). The README says only that the secret "keeps other hosts
  out of the job's rendezvous" and that TCP must be open. Every length a peer
  sends after the handshake is checked before use (the second review's D4,
  re-read by the investigator: `wire.c`, `progress.c`'s parser, `reply.c`).
- **Severity**: fix before release: prove the secret before acting on a
  hello's counts and node claim (or make those refusals non-fatal for an
  unproved caller), bound the confirmation read by `HELLO_NS` as the hello
  is, and add a "Trust" paragraph to the README: a private cluster network is
  assumed; the secret protects the join only; after it a host on the network
  reads and writes every heap by design.
- **Confidence**: high.

### N3. `env.probe` failing does not mean "nothing else will work": a one-node job runs without io_uring (fix before release, README wording; high; *measured*)

- **Location**: README "Building" ("The `env.probe` test checks that the
  running kernel provides every io_uring and futex facility osoe relies on. If
  it fails, nothing else will work on that machine"); design §1 ("A one-node
  job on a machine whose `kernel.io_uring_disabled` sysctl forbids io_uring
  still runs").
- **Evidence** (*measured*, `trust-kernel/partb.sh`, io_uring denied as the
  sysctl at 2 denies it): the probe fails three checks; a one-node job of two
  PEs exits 0; a two-node job refuses by name: "osoe needs io_uring with an
  io_uring created SINGLE_ISSUER, R_DISABLED and CQSIZE for a job of 2 nodes
  (it fails with "Operation not permitted", which usually means the
  kernel.io_uring_disabled sysctl is set)". The design is right and the
  README is wrong for the single-node user, who is the likeliest first user
  on a laptop.
- Also *measured*: `OSOE_AM_HUGEPAGES=1` with no huge pages falls back with a
  warning, printed once per PE rather than once; the agent's slice is applied
  on 6.12 (`se.slice` 100,000 ns against the default 2,100,000, read from
  `/proc`), and `OSOE_AGENT_SLICE_US=0` leaves the kernel's; a soft and hard
  `RLIMIT_NOFILE` of 16, 12 and 9 still ran a 2 × 2 job.

### N4. Slurm: the multi-node fixtures were written from documentation, and three small start-up gaps (after release; `slurm`)

- **No multi-node Slurm environment has ever been captured.** All five
  `captured-*` fixtures under `shmem/tests/slurm-fixtures/` are one-node VM
  captures; the four-node ones are written from the documentation, and the Pi
  runbook's §3 capture step (block, cyclic, plane, uneven, relative, exclude)
  was never run. The record (`docs/decisions/2026-09-24-overnight.md`) calls
  them "provisional until the Pi cluster's replace them". The lead asked the
  `pi` investigator to capture block, cyclic and uneven 4 × 2 environments on
  the cluster this round; see the Pi report. The Debian `127.0.1.1` path (a
  node's own name resolving to loopback) never ran on hardware; the
  investigator ran it end to end in two namespaces and it works.
- **The 33rd `shmem_init` within a minute on a multi-node job fails**
  (*measured*): the 32 candidate rendezvous ports are held in `TIME_WAIT` by
  the job's own earlier sessions, and the message's advice ("choose a range
  this host does not use") is wrong for that cause. `SO_REUSEADDR` on the
  rendezvous listener would fix it; the comment against it
  (`listener.c:271-276`, "a listener that could share a port with a
  stranger's") does not hold on Linux, where a second listener still gets
  `EADDRINUSE` (*measured*). Re-initialization is allowed by the specification.
- **The rendezvous listener binds one specific address** (`job.c:147`,
  `rendezvous.c:283`) while children dial their own resolution of the parent's
  name: a multi-homed node whose name resolves differently on different nodes
  hangs silently (*measured*). Binding `INADDR_ANY`, as the mesh listener does,
  removes it; the README should say that a node's name must resolve to the
  same address everywhere.
- Minor: an IPv6-only name is refused without saying IPv4 is required; the
  host-list parser accepts `n[]` as zero hosts where Slurm refuses it; a stale
  exported `OSOE_PROCID` silently switches every `srun` task to osoe's own
  variable names (source only).

### N5. Kernel facilities: checked by name where it matters; the design's fallbacks hold (sound, with three notes)

From the investigator's reading (`tools/probe/probe.c`, `base/ring-check.c`,
`base/futex.c`, `init.c`, `agent.c`, `node.c`; the raw outputs above):

- **Checked at init, refused by name**: futex2 (`futex_wait`, `futex_wake`,
  the `FUTEX2_SIZE_U32` calls; 6.7), and for a multi-node job the io_uring
  the ring needs (`SINGLE_ISSUER`, `R_DISABLED`, an explicit CQ size, a
  registered provided-buffer ring, the `RECV`, `SEND` and `FUTEX_WAIT` opcodes;
  6.7). `shmem/tests/deny-futex2.c` shows the futex refusal.
- **Used with a fallback**: `pidfd_open` (5.3; falls back to `kill(pid, 0)`
  with one line, design §3.15); `sched_setattr`'s slice (6.12; a kernel that
  refuses or ignores it leaves the default, and the code cannot tell which -
  the README's "an older kernel ignores it" is an assumption, not a check);
  `sched_setaffinity` to the node's CPUs (a cpuset that shrank leaves the
  agent where it was); `MAP_HUGETLB` (falls back with a warning).
- **Page size**: every size comes from `sysconf(_SC_PAGESIZE)` or is aligned by
  the linker to `MAXPAGESIZE`, which is at least the running page; the Pi's
  16 KiB pages have run the whole suite (`docs/pi-cluster-results.md`). The
  heap-base candidates fit x86-64's 47-bit user space (`node.c`; the x86
  section has the run).
- **Notes**: `TCP_INFO`'s `tcpi_unacked` and `tcpi_last_ack_recv` (the
  silent-peer check) exist since 2.6 kernels and are guarded by a length
  check that disables the check silently on a shorter `tcp_info`; the listen
  backlog is capped by `net.core.somaxconn`, undocumented (scale deliverable,
  minor 9); seccomp profiles that deny `io_uring_setup` (recent container
  runtimes, *recalled*) produce the same `EPERM` refusal as the sysctl, which
  the message names.

## x86-64 (`x86`; every runtime result under Rosetta, an x86-64 user space on the arm64 kernel of Apple hardware in TSO mode, not an x86-64 kernel or an Intel or AMD memory system)

**Nothing in osoe breaks on x86-64 in what could be run; what needs fixing is
documentation and a test gap.** Rosetta refuses futex2 (syscalls 454 and 455),
io_uring, `pidfd_open`, `close_range` and `clone3` with `ENOSYS`, and seccomp
with `EINVAL`, while the aarch64 probe passes on the same kernel, so those are
Rosetta's limits. An `LD_PRELOAD` shim (`x86/shim/futex2-shim.c`) turning the
futex2 calls into the classic bitset futex let single-node jobs run; nothing
stands in for io_uring, so **the internode TCP path has never run on x86-64.**

- **Build**: release, debug, ASan with UBSan, strict `-std=c11` and a static
  archive all build with 0 warnings, as on aarch64; all 43 static assertions
  hold; clang for x86-64 and aarch64 over 269 files gives the same single
  warning (`test-reply.c:247`, a test).
- **Tests** (release, 622 registered, the 24 Python models skipped): 3 `env.*`
  fail on Rosetta; of 595 run, 306 genuinely passed and 285 failed, twice
  each with the same result: 282 io_uring refused on multi-node maps, 1
  seccomp refused, 2 cross-build artefacts in the harness (`static-link`
  passes redone by hand; `pacer.installed` is multi-node). Nothing failed
  because of x86-64. ASan with UBSan halting on error passed 309 with no
  report. The pacer on one node with 8 PEs ran every default exercise for 20 s
  and `threads` and `mix` for 60 s each with 0 failures; the README's ring,
  built with the x86-64 `oshcc` (`-no-pie` plus the linker script), is correct
  at 8 PEs.
- **Disassembly matches §3.16**: every two-sided store is an `xchg` or a locked
  instruction (the watch range and bit, every rmw, the wait word, the lead-wake
  mark, claim, raise and lower), the loads after them plain `mov`s, the release
  fence no instruction; GCC 14 emits `lock orq $0,(%rsp)` where §3.16 and
  `base/order.h` say `mfence` (both full barriers). The machine really reorders
  stores past loads: the project's store-buffering control showed 2,113 and 323
  both-zero outcomes in 200,000 rounds under Rosetta and 0 with the fence.
- **ABI facts hold**: LP64 identities, `char` signedness derived from the type,
  `long double` handled by `sizeof` with results copied, AMO alignment checked
  (no split locks), fixed-width wire structs. **Heap-base candidates are below
  2^47**: release maps at `0x100000000000`, ASan at `0x300000000000` (a TSan
  build on a real x86-64 kernel should take the third candidate; reasoned, not
  built). Liveness works on the `kill(pid, 0)` fallback, which Rosetta forced.

Findings:

- **X1, fix before release (README)**: the README names no architecture, and
  no x86-64 process has ever sent data over TCP; the io_uring engine, the
  connection area and the timing pins are untested on x86-64. Say "developed
  and tested on aarch64; x86-64 builds and passes its single-node tests; the
  network path is untested there".
- **X2, fix before release (one README line; the check after release)**: a job
  mixing x86-64 and aarch64 nodes passes the rendezvous, whose agreement has
  no ABI field, and its `long double` data is then silently wrong (x87 80-bit
  against binary128); `internode.h:77-80` admits nothing checks it.
- **X3, after release**: the suite cannot see the x86-only lost wake that the
  `seq_cst` RMW prevents: with the watch bit weakened to a plain store,
  `invariant.missed_wake` went non-zero in 3 of 3 runs (about 3.2 million
  notifies) against 0 in 3 of 3 baseline runs, yet every test and the pacer
  still passed; only `osoe-stat` reads that counter.
- Minor: the init refusal says "needs Linux 6.7 or later ... this kernel is
  6.12" for any futex2 error, and the probe reports `ENOSYS` as "U64 accepted";
  `shmem.global-exit.fatal.*` passes on multi-node maps even when every PE
  refused at init (a false pass); three tests assume the build compiler is the
  host's (`static-link`, `strict-c11`, `headers.c++*`); `pacer-shim.c` is
  generated by two independent targets; a 64-byte line may false-share under
  Intel's pairwise prefetcher (unmeasured).

Housekeeping: Lima's `rosettad` caching daemon went pathological during the
runs (load average 160) and was stopped; `sudo systemctl start rosettad` in
`osoe-x86` restores it. The VM can be deleted.

## Slurm, in detail (`slurm`)

- **Variables read**: exactly nine step-level ones (`SLURM_PROCID`,
  `SLURM_STEP_NUM_TASKS`, `SLURM_NODEID`, `SLURM_STEP_NUM_NODES`,
  `SLURM_LOCALID`, `SLURM_GTIDS`, `SLURM_JOB_ID`, `SLURM_STEP_ID`,
  `SLURM_STEP_NODELIST`), never mixed with the `OSOE_` names, no parsing of
  `TASKS_PER_NODE`; `SLURM_GTIDS` is still set by upstream Slurm's current
  `env.c` (*observed*). Every malformed value tried is refused naming the
  variable and the value (*measured*). Heterogeneous jobs and Slurm's own
  steps are refused by name (*documented*, `2026-09-24-overnight.md`).
- **Host lists**: osoe agrees with `scontrol show hostnames` on every form
  Slurm writes - 32 forms, 19 of 19 round trips, a 1,000-node list and 779
  random names (*measured*); it refuses multi-bracket names such as
  `r[1-2]n[1-4]` by name, which Slurm never writes in a step's list. Each node
  finds its own entry by `SLURM_NODEID` as an index, so fully qualified and
  short names cannot be confused.
- **Real `srun`, one node**: block, cyclic, plane, arbitrary, `-c 2`,
  `--cpu-bind` in several forms, `--hint`, `--exclusive`, `--mem-bind`,
  `--overcommit`, `--mpi=pmi2` and `pmix` all pass. **Emulated multi-node
  layouts**: cyclic, plane, arbitrary, uneven, rank 0 off node 0, a node whose
  ranks are in decreasing order all pass; `SHMEM_TEAM_SHARED` is right when
  non-strided, and so are its strided splits and reductions. **Shells**: a
  program run straight from a batch script or from a `salloc` shell is refused
  at once, naming `srun`. **Collisions**: three concurrent `--overlap` steps ×
  20 sessions each collide on nothing; array tasks get distinct job ids.
  **Names that do not resolve** are refused by name on the node that finds
  them.
- `-K1` (readiness R5a): without it a task exiting 1 at start-up left the
  other running at 92 s and a SIGKILLed task at 51 s; with it the step ended
  about a second after; it does not cover a task that exits 0 without calling
  `shmem_init`, which hung at 34 s.

## What is sound

- **Slurm**, as listed: the bootstrap reads the right nine variables, parses
  every host list Slurm writes, starts under every distribution and binding
  that could be run or emulated, and refuses what it does not support by
  name.
- **The kernel checks**: refusal by name for what is required, fallbacks for
  what is optional, a one-node job without io_uring.
- **The receive side after the handshake** checks every length, offset, token
  and count a peer sends (the second review's D4 closed; re-read).
- **The listeners are open only during start-up**, and with the secret a
  caller that cannot prove it is refused by name and does not count.
- **Local users are kept out** of the job's shared memory: mode 0600,
  `O_EXCL`, the owner checked.
- **Build and install** (readiness): the install is exactly what the README
  lists; the installed tools are relocatable; the exported surface is 1,794
  weak `shmem_` names over strong `pshmem_` twins and no `osoe_` symbol; the
  headers are clean under gcc 14 and clang 19 from C99 to C23 and C++11 to
  C++23; clang builds the tree with one benign warning in a test.

## Sources

`reports/slurm.md` and `slurm/`, `reports/install.md`, `reports/x86.md` and
`x86/`, `trust-kernel/` (scripts) and the VM's `/tmp/trust-kernel-drive.out`,
`-drive2.out`, `-partb.out` (raw outputs, read by the lead),
`shmem/src/listener.c:334-481` (lead-verified), design §1, §3.4, §3.15, §6.1,
§6.2, `docs/decisions/2026-09-24-overnight.md`, `docs/pi-cluster.md` §3.
