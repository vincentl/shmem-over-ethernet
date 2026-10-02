# Round three, question 5: is osoe ready to be public?

Review of `main` at `b0e0b53`, run 2026-09-28 against
`docs/reviews/adversarial-review-prompt.md`. Investigators: `repo-hygiene`
(licences, third-party material, private content, the docs tree),
`readme-settings` (every setting and number in the README against the code),
`install` (the README followed as a stranger: install, `oshcc`, exports, the
profiling interface, headers), `diagnosability` (a job failed every way, and
what the user is told), `seam` (`transport.h` and `internode.h` as a contract),
and the lead's own baseline runs in a fresh VM. Working files:
`build/scratch/review-r3/` (`repo-hygiene/`, `readme-settings/`, `install/`,
`diagnosability/`, `seam/`, `load/`). Labels: *documented*, *observed in
source*, *measured*, *recalled*; "lead-verified" means the lead re-read or
re-ran it.

## Verdict

**Publishable after a day of mechanical fixes, none of them to the library's
logic.** The licence decision is implemented completely and consistently
(*measured*: every SPDX line in the tree is the one value, `COPYING` is
byte-identical to the canonical GPL-3 text, nothing carries a conflicting
notice, and the algorithms that look borrowed - SipHash, the host-list
grammar, the PRNGs - are original code that cites its sources). Nothing
private ships in the code or the public documents; what leaks is confined to
the review evidence and the finished briefs under `docs/`. The README's
settings tables are right to a degree that is unusual: about thirty variables
and thirty numbers checked, three rows incomplete and none wrong. The README
fails its intended reader in three places that matter: its first command runs
a machine-dependent performance pin that failed in a fresh VM (R2); two of
its scaling sentences and one of its RoCE paragraphs contradict the code or
the record (R3); and its advice to run with `srun -K1` defeats its promise
about `shmem_global_exit` (conformance finding CA-2). The seam holds where it
matters, with a short list of peripheral things a porter would touch.

## Findings, ranked

### R1. Private paths, a private page and a missing notice in the tracked tree (fix before release; high; *measured* by `repo-hygiene`, lead-verified for the counts)

- **The author's paths**: `/Users/<author>/...` in 61 tracked files, 218 lines,
  all under `docs/reviews/evidence/`, `docs/design/evidence/` and
  `docs/prompts/done/` (lead-verified: `git grep -l '/Users/<author>'` matches
  nothing outside `docs/`); four also carry an older
  `/Users/<author>/Desktop/Maker Space/...` path, and some raw `ls -l` output
  names the Unix account. Nothing in `README.md`, the design notes, the
  decisions or the Pi documents.
- **The docs index sends a stranger to a private page**: `docs/README.md:3`
  opens with "Start with the status page (Claude Docs, ...)", a page with no
  URL that a reader cannot open; the same pointer is at
  `docs/decisions/2026-09-26-overnight.md:164`.
- **The Slurm fixtures carry the development VM's identity**: five of the
  twelve `shmem/tests/slurm-fixtures/*.env` files hold `192.168.5.15` and
  `lima-osoe` (lead-verified; the fixtures are otherwise scrubbed exactly to
  their stated policy - no user, home, account or `SSH_CONNECTION`). Low
  sensitivity, cheap to fix.
- **Four verbatim specification examples**
  (`docs/reviews/evidence/phase1-step2-slice1/conformance/spec-example-{5,38,42,43}.c`)
  name their source but not the notice the specification's permission
  statement requires (`spec-1.6.txt:204-208`: the OSSS notice and that copying
  is by permission). Terms compatible; the notice is missing.
- **SPDX lines**: `dev/lima/create.sh` and `dev/lima/osoe.yaml`, the only two
  real files without one; and 154 historical programs under the two
  `evidence/` trees (61 `.c`, 37 `.py`, 53 `.sh`, 2 `.cpp`, 1 `.yaml`), none
  built or shipped, covered by `COPYING` but not by the project's own "every
  source file" rule.
- **Scenario**: a stranger cloning the public repository reads the author's
  home directory and account name, follows the docs index into a page that
  does not exist for them, and finds the project's own rule broken in its
  evidence.
- **Severity**: fix before release, all mechanical (a `sed` over `docs/`, one
  paragraph in `docs/README.md`, five fixture lines, four notices, two SPDX
  lines; the 154 evidence files are the author's call).

### R2. The README's first command fails on a machine unlike the author's: `shmem.floor` pins a machine-dependent ratio (fix before release; high; *measured*, lead)

- **Location**: README "Building" (`cmake --workflow --preset release # configure,
  build, and test`); `shmem/tests/test-floor.c:81-83` (`FASTEST 3.0`, `INTERNAL
  1.5`, `STREAM 0.10`), `:323` (`s.median < STREAM` fails the test); design §8.4.
- **Scenario**: in a fresh Debian 13 VM on the same Mac (four cores, the same
  kernel), the release suite passed 597 of 598: `shmem.floor` failed because
  the library's stream over loopback was 0.069-0.223× the bare echo's across
  its seven pairs, median 0.0999, against "at least 0.10×"
  (`build/scratch/review-r3/load/release-test.log`). The pin's other two
  ratios were comfortably inside (fastest trips 1.5-1.7× against 3×; a put and
  quiet 1.00× a get against 1.5×). Run alone on the idle VM five times
  afterwards it passed each time, with the stream ratio's median at 0.103,
  0.250, 0.282, 0.311 and 0.321: marginal even idle, and below the pin beside
  the other tests of a `-j4` suite run, which is how the README's command
  runs it. The stream ratio depends on how fast the machine's loopback
  `memcpy` is against one progress thread's throughput: on the author's VM it
  measured 0.21-0.43× (design §1); on a faster x86-64 loopback it will be
  lower still (the `x86` investigator's run is the cross-check). A stranger who runs the README's first command sees a red
  test named "floor" with a "FAIL" line and no way to know it is a pin the
  author set on their own machine.
- **Evidence**: *measured* as above; *observed in source*, lead-verified. Not
  a library defect: the test says so itself ("pins not checked: an
  instrumented or unoptimized build measures itself") for the other presets.
- **Severity**: fix before release: either take the pin out of the default
  suite (keep it in a label the author runs), widen it to what loopback on
  other machines gives, or say in the README that `shmem.floor` is a
  performance pin set on the author's hardware and may fail elsewhere.
- **Confidence**: high.

### R3. Three README passages contradict the code or the record (fix before release; high; *observed in source*, lead-verified)

1. **"Toward RDMA"**: "XRC queue pairs, one per context and target node,
   matching today's one connection a node pair" - the second review's F12
   put XRC at about 2.1 million queue pairs per NIC at 4,096 × 256 and its
   synthesis concluded DC was the only option; neither was folded into the
   design, the decisions or the README, and an XRC initiator queue pair
   belongs to one process (*recalled*). And "collectives stay above the seam,
   so in-network reduction (SHARP and its kind) is out of reach" contradicts
   `internode.h:137-145`, which offers exactly that offload, and design §4.10,
   which puts the internode phase below the seam (the README's next clause
   half says so). Two sentences.
2. **The scaling table's team row and the Limits' team paragraph** omit that
   every split claims job-wide numbers, so a split made on each node spends
   one number per node (scale finding P1); the **budget row** says the
   buffers "shrink, saying so, to fit" where at the defaults they cannot and a
   job of more than 1,025 nodes is refused (P3); the **start-up row** and the
   large-reduction sentence are off (P-minor 1, 2).
3. **"One progress thread a node drives the network"** is stated but its
   consequence is not: the node's internode message rate and bandwidth are one
   thread's whatever the PE count, "line rate" was measured at 1 Gbit/s, and
   §3.12's four sharded threads for 256 PEs are not built (P2).

### R4. The README's settings tables: three rows incomplete, one convention broken, none wrong (fix before release for the first three; *observed in source*, lead-verified for F1)

From `readme-settings` (`reports/readme-settings.md`), which traced every
variable to its read site and every number to a constant or a record:

- **`OSOE_MAX_THREADS`** is read only by a node's first PE
  (`metrics.c:460`, called while the lead builds the control segment); every
  other PE copies the lead's value from the segment header (`node.c:888`) and
  never consults its own environment. The README says "the same on every PE
  of a node"; a user who sets it for one demanding rank with the launcher's
  `-e RANK:NAME=VALUE`, a flag the README documents, gets silence and no
  effect. The sibling `OSOE_MAX_CONTEXTS` is compared PE by PE and fails
  loudly (`node.c:840-847`). Say what the `OSOE_HEAP_BASE` row already says:
  "the node's first PE's value is the node's".
- **`OSOE_AM_BUFFER`** has an enforced floor of twice the chunk, refused by
  name (`connection-area.c:126-156`); the README gives only the ceiling.
- **`OSOE_WIRE_BYTES`, `OSOE_WIRE_BUFFERS`** have ranges (64 to 1,048,576; 2
  to 32,768 and a power of two, `wire.c:62-73`) the README does not state,
  unlike every other row of that table.
- Minor: `OSOE_JOB_SECRET` is checked job-wide exactly as the five rows marked
  *Checked* are (`listener.c:127-136`) but is not marked; `SHMEM_INFO` prints
  20 of the 31 reference rows and none of the transport-buffer section;
  `osoe-launch`'s tier-3 flags `-N`/`-H` and the `SUDO_*` variables it reads
  are undocumented (a development tool, never installed).
- **Sound**: every other default, range and *Checked* mark (about thirty
  variables), every numeric claim in the prose (about thirty), the installed
  file list, every documented flag of the four tools, the IPv6 refusal, the
  empty `shmemx.h`, the message a non-`oshcc` program gets for a static, and
  all three example command lines plus the launcher-free shell recipe, run
  verbatim in the VM.

### R5. The README's operational advice defeats its own promise about `shmem_global_exit` (fix before release; cross-reference: conformance CA-2)

`srun -K1`, which the README recommends so that a task failing at start-up
ends the rest, makes Slurm kill the step when any task exits nonzero. On
`shmem_global_exit(status)` with a nonzero status every other PE `_exit`s
within milliseconds, Slurm kills the caller mid-`exit`, its `atexit` handlers
never finish, its buffered output is lost and `srun` returns 143. *Measured*
by `conformance-a` in the VM's Slurm: a caller handler of 200 ms lost 3 of 3
runs; without `-K1`, or with handlers under 50 ms, every run returned 7. The
README promises the caller "its `atexit` handlers run and its streams are
flushed"; the test `global-exit.sh` always launches with `-k`, so nothing
catches it. Fix in the design (hold the other PEs' exit until the caller's
process has ended, within the abort grace) or in the README (state the limit
beside `-K1`).

### R5a. The README's requirements and first steps, followed as a stranger (fix before release for the first three; *measured*, `install` and `slurm`)

- **Python 3 is required and not listed.** `tools/pacer/CMakeLists.txt:37`
  makes Python 3 `REQUIRED` for the counted build's shim generator
  (lead-verified); the README's requirements name a C compiler, CMake,
  pkg-config and liburing. Without it, configure first prints "python3 not
  found: the concurrency models will not run as tests", as if it would carry
  on, then stops with "Could NOT find Python3" (*measured* by pointing
  `Python3_EXECUTABLE` at a missing path).
- **There is no version anywhere.** The project is `VERSION 0.0.0`,
  `SHMEM_VENDOR_STRING` is "osoe", `SHMEM_VERSION` prints `osoe, OpenSHMEM 1.6`
  where the README and `SHMEM_INFO` promise the library's version, no tool
  answers `--version`, and no commit hash is compiled in. The tag the release
  point calls for is the moment to give the library one.
- **The README's first `srun` example** (`srun -N2 --ntasks-per-node=4 ./ring`)
  lacks the `-K1 --cpu-bind=none` the README requires four paragraphs later
  (*measured* by `slurm`: without `-K1`, a task exiting 1 at start-up left the
  other running at 92 s, a task killed by SIGKILL at 51 s; with it the step
  ended about a second after the failure. `-K1` does not cover a task that
  exits 0 without calling `shmem_init`, which still hung at 34 s).
- **Start-up failures under `srun` leave objects in `/dev/shm`**, and the
  README tells Slurm sites nothing, while its `RemoveIPC=no` advice switches
  off the one mechanism that would clean them: a task exiting before
  `shmem_init` leaves the segment, the heaps and the statics objects, which
  `osoe-stat --gc` removes; survivors sent SIGTERM right after init, as `-K1`
  does, leave each node's `.conn.0` and `.job.0` in 8 of 8 tries, and `--gc`
  removes none of those (`gc_one`, `tools/stat/stat.c:952-1003`, knows the
  heap, statics and segment names; lead-verified). The library's own abort
  and a PE killed after init leave nothing (3 of 3 each). `osoe-launch` hides
  all of this by deleting leftovers itself (`launch.c:266-269`). The lead
  found four such sets in the shared VM's `/dev/shm` after the Slurm
  investigator's runs. A one-line epilog (`rm -f /dev/shm/osoe.${SLURM_JOB_ID}.*`)
  in the README now; `--gc` learning the two names after release.
- **The README's build command runs the whole suite serially**: the presets
  set no job count, so the build is serial (about two minutes of CPU) and the
  620 tests run one at a time, the long models among them (`abort_exit`
  eleven minutes, `get_completion` fifteen), with no time given. A stranger
  who follows "configure, build, and test" waits the better part of an hour
  with no indication that this is normal. Say so, or set `-j` in the presets,
  or leave the models to a label.
- Minor: the README does not say how to learn the job id `osoe-stat` needs
  (under `osoe-launch` it is the launcher's pid, never printed; `osoe-stat`
  with no argument lists live jobs, which is the route to document);
  `oshcc -show`/`--showme` work only as the first argument; the `tier3`
  preset, `BUILD_TESTING`, `BUILD_SHARED_LIBS`, `OSOE_TIER3` and
  `OSOE_MULTIPLE_MODELS` are undocumented; on a non-Linux host the presets
  fail with only `Could not use disabled preset "release"`; `pshmem.h`
  includes `<shmem.h>` with angle brackets and so needs `-I`; a C99 program
  calling a generic gets only "implicit declaration"; `shmem.h:6-7` carries a
  stale comment; no pkg-config file, CMake package, manual pages, installed
  licence files or uninstall; `CheckExports.cmake` is a pattern allowlist,
  not derived from the header, and checks no count (the count itself, 1,794
  weak names each with a strong `pshmem_` twin and no `osoe_` symbol, was
  confirmed by `nm -D`).

### R6. What a stranger meets in the docs tree (after release; judgement, `repo-hygiene`)

- 755 of the 1,199 tracked files (63% by count, about 9 of 12.5 MB) are
  review evidence and finished briefs. `docs/README.md` labels them history
  and says why they are kept; that is a deliberate choice, and it reads as a
  record, not noise, once the index's first paragraph stops pointing at a
  private page.
- `docs/decisions/*.md` are in the third person throughout, and the
  "Author"/"Claude" convention is explained where a stranger meets it
  (`docs/decisions/README.md`). `CLAUDE.md` sits at the root; the README never
  says the code was written with an assistant's help, while the decisions and
  prompts are candid about it. The author's call; a reader who finds the
  briefs after the README may feel the README should have said.
- `docs/design/successor-api-stage-setting.md` plans a different project; it
  is labelled non-binding.
- No `CONTRIBUTING`, `CHANGELOG`, `SECURITY.md`, code of conduct or
  `.gitattributes`; no version anywhere (the README says there is no tag yet).
  Minor for a first release; a `CHANGELOG` starts with the tag.
- A large `10.0.x` address dump in one evidence file duplicates what
  `docs/pi-cluster.md` discloses on purpose.

### R7. The seam, read as a RoCE contract (after release except the README sentence in R3; `seam`, lead-verified on the two headers' text)

The promise holds where it matters. A stub backend defining only the 31
functions the two headers ask of one (23 in `transport.h`, 8 in
`internode.h`) links against all 45 non-backend library objects and the 12
`base/` objects (*measured*, `seam/stub/`), needing only two AM symbols and
liburing besides. No CPU atomic touches a symmetric or internal-region word
above the seam - every remote word goes through `osoe_transport_rmw` and
`osoe_transport_load`, so a NIC could carry every atomic, node-mates'
included, as the README sketches. Every ordering the layers above rely on is
expressed with `OSOE_TRANSPORT_FENCED` on the same (context, PE) or with a
wait, in put-with-signal, the pipelined tree, Bruck's READY and DATA, the
scan, alltoall's barrier and the lock's release; nothing leans on AM's
single ordered stream. Nonblocking gets are a flag plus per-context
counters, the shape RDMA needs. The rendezvous is out of band in every build
and says so; queue-pair and key material has its slot, the 128-byte per-node
blob of `osoe_transport_advertise`.

What a porter would still touch, about 60-120 lines above the seam plus one
model re-run, most of it admitted by the record:

- `init.c:258-270` runs the io_uring preflight (`base/ring-check`) for every
  multi-node job, and `node.c:534-541` and `rendezvous.c:257` call two AM
  symbols (`osoe_am_config_read`, `osoe_am_table_checksum`); a backend-only
  build does not link until they move, and a RoCE build would refuse a node
  whose `kernel.io_uring_disabled` sysctl is set though it opens no ring.
  `shmem.boundary` checks includes only and cannot see it. About 25 lines;
  the checksum belongs in AM's own blob, which the second review's F10
  proposed and the tree half took.
- The metrics table (`metrics.h:81-100`, `metrics.c`, `node.h:407-470`) holds
  AM's 39 counter rows and gives a backend no way to add its own;
  `osoe-stat`'s connection table and the pacer's library counters read them
  by AM names and degrade gracefully.
- The deadlock detector reaches the engine only through `transport.h`
  (*measured*, `nm`) but its premises are AM's: every waiting thread asleep
  in the kernel where §4.10 expects a completion-queue spin, no backend
  threads in a PE, blocking calls counting themselves through
  `osoe_wait_sleeps`, an obligation `base/sweep.h` states and `transport.h`
  does not; and `node_quiet`'s "applied" is two-sided. All fail toward no
  detection, never toward ending a live job; `deadlock_model.py` should be
  re-run for a one-sided channel.
- Target-side wake is still undesigned behind `transport.h:433`'s "Notification:
  exact" (the second review's F8, not adopted); a polling backend needs a
  first-sight protocol against `watch.c`, which publishes the range but not
  the value the waiter saw, and it should be model-checked.
- Contract text: `transport.h:527-529` says a wait completes every nonblocking
  get the PE issued where `:346-347` and the code complete the context's own;
  "an RDMA backend sleeping on a completion channel is the same shape" holds
  only with io_uring, since the agent waits on a descriptor and a futex at
  once; "four waits" where AM has seven.

The second review's seam findings D5, D6, D7, F4, F5, F6 and F9 hold today;
F10 is half done; F8 and F12 are open.

## Diagnosability

From `reports/diagnosability.md` (138 cases, all *measured* in the VM on the
release build, maps `0,0,0,0` and `0,0,1,1` and others, almost all with
`osoe-launch -k` so the library's own reaction shows) and `reports/install.md`.

**"Loud, never hung" holds once `shmem_init` has returned, and not before.**

- **R8. A start-up failure on one PE or node ends that PE with a good message
  and leaves the rest waiting silently until the launcher kills them (fix
  before release; high).** Cases: a PE exiting before `shmem_init` (the others
  wait to the 30 s timeout, on one node and two); the `/dev/shm` capacity
  refusal (the lead checks before the control segment exists, `node.c:611-628`,
  so there is no abort word for the others to read); `OSOE_GET_LANDING_BYTES=1000`
  or `OSOE_MAX_THREADS=0` even when every PE has the same value (only the lead
  validates them, and PE 1 waits); a bad value set on one PE only;
  `-e 3:SHMEM_SYMMETRIC_SIZE=1g` on `0,0,1,1` (node 1 ends, node 0 waits, where
  the README says "a job whose PEs disagree ends naming the variable");
  `OSOE_JOB_SECRET` on one node only (only the refusing node ends, where design
  §6.2 says it "ends the job naming both"). The existing tests of these
  refusals run without `-k`, so the launcher kills the survivors and the suite
  cannot see the hang; `srun -K1` masks it the same way, and the README's
  first example has no `-K1`. With `OSOE_NO_PROGRESS_TIMEOUT` set the other
  node blames the ports. A three-node disagreement found by the rendezvous
  tree is fine: every node ends. Fix: publish the failure where the others
  can see it before the control segment exists (a word in the job object, or
  the launcher's pidfd path), and run those refusal tests with `-k`.
- After release: mismatched collectives among a node's PEs return 0 everywhere
  with no message (conformance C4.2); a divergent `shmem_malloc` sequence
  writes into other PEs' objects silently (C5); "on behalf of PE N" names the
  PE that noticed, not the culprit, and a dead node is named by index rather
  than host or rank; `osoe-stat` shows a SIGSTOPped node as a normal waiting
  one, and in an init hang every "blocked in" is `-`; unknown `OSOE_` names
  are ignored silently, so the typo `OSOE_DEADLOCK_SECOND=0` leaves detection
  on. Minor: one death printed ten lines on one node and one fatal three
  times; every fatal suggests `osoe-stat` on a segment gone by then;
  `OSOE_HEAP_BASE=0x1000` is told to set `OSOE_HEAP_BASE` and `-4096` is
  accepted; start-up messages give local ids, not ranks; `osoe-launch` never
  prints the job id and gives no diagnostics on timeout.
- **Sound**: every death after init ends the job within about 0.1 s with the
  rank and pid in the first line (`_exit(0)`, `exit` from a thread, SIGKILL,
  SIGSEGV, `abort`, either node's lead, a PE killed while the others compute,
  on both maps); deadlock detection found six shapes on both maps in 3.1-3.3 s
  at `OSOE_DEADLOCK_SECONDS=2` with a report pointing at the culprit and no
  false positive against a PE computing for 12 s; a node stopped 8 s with
  64 MiB of puts in flight was waited for, 4 of 4, three of them at
  `OSOE_KEEPALIVE=1,1,2`; about 25 bad settings refused by name with the valid
  range; exhaustion of teams, contexts, threads and a short collect
  destination behave as documented; landing-zone backpressure and
  `OSOE_AM_CREDITS=1` with 8 PEs gave 0 wrong bytes (the second review's D1
  gone); the pidfd fallback works; launcher timeout and Ctrl-C leave nothing
  (D13 fixed); `shmem_global_exit(7)` gives 7 everywhere with output flushed;
  `osoe-stat` pins a one-node deadlock's culprit at a glance and walks to a
  cross-node one with `--summary`; `osoe-stat` with no argument lists running
  jobs, which is how a user learns the job id.

Established earlier by `install`:

- **Installed correctly, and self-contained**: `cmake --install` puts exactly
  what the README says under the prefix (`libosoe.so.0.0.0` with its soname,
  the four headers and `mpp/`, `oshcc`, `osoe-stat`, `osoe-pacer`, the linker
  script under `lib/osoe/`) and nothing else (no `.a`, `.pc`, CMake package,
  licence files or manual pages); the installed `oshcc` builds the README's
  ring with a run path to the prefix, which runs under `osoe-launch` and under
  the VM's `srun -K1 --cpu-bind=none -N1 -n 4` (*measured*, `install`).
- **`oshcc -show`** works only as the first argument (`oshcc -O2 -show ...`
  fails as an unknown compiler flag); `--showme:compile` and `--showme:link`
  work; `OSOE_CC=clang`, `g++` and `clang++` build and run; `-static` is
  refused by name, and `--static` slips past the check and fails at link for
  want of a `libosoe.a`.
- **A program built without `oshcc`** runs, and passing a static's address to a
  put ends the job with the promised message, on one node and two.
- **Leftovers**: a task killed by `srun -K1` right after a fatal, and a task
  that exits before `shmem_init`, leave `osoe.<job>.0.{0,1}.{conn,job}.0`
  objects (264,064 and 904 bytes) in `/dev/shm` in 8 of 8 tries, and
  `osoe-stat --gc` removes none of them: `gc_one` knows the heap, statics and
  segment names only (`tools/stat/stat.c:952-1003`). The library's own abort
  and a PE killed after init leave nothing (3 of 3 each). The second review's
  D11 was closed for the objects it named and not for these two.

## What is sound

- **The licence** (*measured*, `repo-hygiene`): `COPYING` byte-exact against
  the canonical GPL-3 (674 lines, 35,149 bytes); `COPYING.LESSER` one byte
  longer than the canonical count with every line and heading matching (a
  trailing newline); every SPDX line in the tree the single value
  `LGPL-3.0-or-later`; no conflicting header or copyright line anywhere
  (MIT, BSD, Apache, GPL-2, CC0 grepped); the README's licence paragraph says
  the right thing about programs that link the library.
- **Third-party material**: SipHash-2-4, the Slurm host-list grammar, the
  pacer's PRNGs (splitmix64, MurmurHash3's finaliser, FNV-1a) and the API
  headers are original code with their origins cited, none copied from SOS or
  Slurm; `docs/design/workloads/traces/` avoids quoting corpus code because
  "licenses vary"; `corpus/`, `build/` and the specification PDF are
  git-ignored with zero tracked matches; `.claude/` has no tracked file.
- **No secrets** (keys, tokens, passwords) anywhere; the repository is small
  (about 12.5 MB tracked), with no generated files, no CRLF, every executable
  bit on a real shebang.
- **The README's tables and examples** (R4) and the seam (R7), for the reasons
  given there.
- **The baseline in a fresh VM**: the four presets and the tier-3 preset pass
  whole but for the pin of R2 (scale deliverable, "Measurements").

## Sources

`reports/repo-hygiene.md`, `reports/readme-settings.md`, `reports/seam.md` and
`seam/stub/`, `reports/install.md`, `reports/diagnosability.md`,
`reports/conformance-a.md` (CA-2), `load/release-test.log`, `README.md`,
`docs/README.md`, `docs/decisions/README.md`, design §3.12, §3.15, §4.10,
§8.4, `docs/reviews/archive/phase1-step2-slice1-roce-seam.md` (F8, F10, F12).
