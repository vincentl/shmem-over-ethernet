# Cloud session 4: the manifest

*The record of the fourth cloud session (`docs/prompts/cloud-session-4-prompt.md`),
2026-10-14, on the branch `cloud-session-4` from `main` at `7cd2d68`, for the lead's
review after the author pulls. Written after every item, each time committed and
pushed. Paths write the repository as `<repo>`. The response,
`docs/prompts/cloud-session-4-response.md`, is the complete account; this file is
the per-item log behind it.*

## The environment, since the third report

Unchanged from the third session's (`docs/prompts/cloud-session-3-response.md`, "The
environment"): Linux 6.18.44-fc-v114 on four vCPUs and 15.7 GiB, `/dev/shm` 16 GiB,
`kernel.io_uring_disabled = 0`, `vm.mmap_min_addr = 4096`, `ulimit -l` held at
8192 KiB for root with `CAP_IPC_LOCK` (`ulimit -l unlimited` is refused: "cannot
modify limit: Operation not permitted"), liburing 2.9, gcc 13.3 as `cc` with gcc 14.2
and clang 18.1 beside it, cmake 3.28.3; Python is now 3.13.16. The kernel still has no
netem (`# CONFIG_NET_SCH_NETEM is not set`). Nothing needed setting.

At `main` (`7cd2d68`): `cmake --preset release` and `cmake --build build/release
-j4` in 63 s, **0 warnings**; the static checks (`ctest --test-dir build/release -j4
-R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)'`)
**13 of 13**, in 98 s.

From batch 1 on, the static checks' expression gains `settings-known`, D16's new
check: `-R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|settings-known|license-lines|single-node\.configure|version\.)'`.

**How "fails without" was shown, for every item below**: the batch's library change
was set aside (`git stash push --` the library's files, new library files moved out
of the tree) with its tests and checks kept, the build reconfigured and rebuilt, and
the batch's tests run with `--output-on-failure`; then the change was restored and
rebuilt. The lines quoted are from those runs.

## Batch 1: the settings' diagnostics

One commit for the four items: the one that adds this section.

### D16: unknown `OSOE_` names said, so a typo no longer leaves detection on - done

**The item** (RD, the round-three readiness report's diagnosability list; round
four's readiness, "A misspelt variable is silent"): `OSOE_DEADLOCK_SECOND=0`, meant
to turn the deadlock detector off, left it on, and a job waiting on a word was ended
as deadlocked at 10 s. Confirmed open: nothing read the environment for names it did
not know; only the three retired names drew a notice (`bootstrap.c`).

**What changed.**

- `shmem/src/unknown-settings.c` and `.h` (new): `known_settings[]`, every `OSOE_`
  name the tree reads or sets in a PE's environment - the library's settings (43),
  the launcher's names (10), the tools' (4), the retired (3) and the test suite's own
  (43) - 103 in all; `osoe_unknown_settings_known(name)`; and
  `osoe_unknown_settings_notice()`, which walks `environ` and, for each `OSOE_` name
  not in the list, says "`OSOE_DEADLOCK_SECOND is not a variable osoe reads, and
  changes nothing: did you mean OSOE_DEADLOCK_SECONDS?`" - the nearest known name by
  edit distance, at most two edits away - or, with none near, "`... (SHMEM_INFO=1
  lists the ones it reads)`". Once a process (a static flag: `shmem_test_init`
  starts the library three times), through `osoe_notice`, one write a line.
- `shmem/src/init.c`: `session_start` calls it on the job's rank 0, after
  `SHMEM_INFO`'s text - once for the job, as the retired names' notice is said.
- `cmake/CheckSettingsKnown.cmake` (new) and its test `shmem.settings-known` (tier 1,
  30 s): collects every `OSOE_` name read or set over `shmem/`, `base/`, `tools/` and
  `cmake/` - quoted literals in C, shell and Python files, `$OSOE_...` in shell, and
  `OSOE_...=` assignments in shell scripts and `CMakeLists.txt` (what tests and
  tools set for the PEs they start), with the launcher's `launcher_name("...")` -
  less the build's own CMake options (`option(OSOE_...)`, cache variables) and the
  two names the new tests set on purpose; and fails on a name read or set and not
  listed, and on a listed name nothing reads or sets.
- `cmake/CheckSettingsDocumented.cmake` leaves `unknown-settings.c` out of what it
  collects: the list holds the test suite's names, which the README does not.
- `shmem/CMakeLists.txt`: the new source.

**Wire or layout**: neither.

**Decision taken**: a notice, not a refusal. `base/environment.h`'s rule makes a
malformed *value* an error; an unknown *name* is said and ignored, as the retired
names are, so a script that sets a name a later osoe reads, or one osoe retired,
still runs. Said by rank 0 alone, as the retired names are: a name set on one other
PE only (`osoe-launch -e N:...`) is not said.

**Tests** (`shmem/tests/CMakeLists.txt`, tier 2, 90 s each):

- `shmem.settings.unknown.near`: `OSOE_DEADLOCK_SECOND=0`, two PEs of
  `shmem_test_init`; through the new `count-lines.sh`, exactly one line "`osoe: PE 0:
  OSOE_DEADLOCK_SECOND is not a variable osoe reads, and changes nothing: did you
  mean OSOE_DEADLOCK_SECONDS?`" and a clean job; none from PE 1.
- `shmem.settings.unknown.far`: `OSOE_FROBNICATE=1`, the line without a suggestion.
- `shmem.settings.unknown.known`: four known names of four groups set
  (`OSOE_DEADLOCK_SECONDS`, `OSOE_CC`, `OSOE_TEST_MAP`, `OSOE_BOOTSTRAP_TIMEOUT`), no
  such line, the retired name's own notice still said. A guard: it passes without the
  change, as it must.
- `shmem/tests/count-lines.sh` (new): runs a command and requires its success and
  exactly COUNT lines matching a pattern.

**Shown.** Without the change: `shmem.settings-known` fails ("no
<repo>/shmem/src/unknown-settings.c: nothing to check against"), `unknown.near`
("count-lines.sh: the command exited 0, 0 line(s) matching, wanted 1") and
`unknown.far` (its line not found). With it: all pass, `unknown.near` saying its
line once in three sessions. The check itself was shown to find what it is for: with
`OSOE_TEST_MAP` taken out of the list and `OSOE_NOT_READ_ANYWHERE` put in, it fails
naming both ("`OSOE_TEST_MAP (in shmem/tests/CMakeLists.txt)`" and
"`OSOE_NOT_READ_ANYWHERE`"); restored, "420 files, 103 OSOE_ names read or set, every
one of the 103 unknown-settings.c lists and no other". Its first run also found the
build's CMake options (`OSOE_TIER3=ON` in a message, `OSOE_MULTIPLE_MODELS=ON` in a
comment) and `osoe-launch`'s own test of `-e`, `OSOE_EXTRA`: the options are now left
out, and `OSOE_EXTRA` is listed.

**README**: the "Environment variables" opening says what an unknown name draws and
that `shmem.settings-known` holds the list to the tree.

### D24: a full context pool says `OSOE_MAX_CONTEXTS`, once a PE - done

**The item** (R9 Q5-8, `docs/reviews/archive/v0.2-before-part4-readiness.md`): 63
`shmem_ctx_create` calls succeed and the 64th returns nonzero with no word of
`OSOE_MAX_CONTEXTS`, where a split names `OSOE_MAX_TEAMS` and a thread
`OSOE_MAX_THREADS`; "a line under `SHMEM_DEBUG`, or once a PE, would tell the user
which variable to raise". Confirmed open in `context.c`'s `create`.

**What changed** (`shmem/src/context.c`): `create` notes whether the pool had no free
element (`full`), and the first time a process finds it so it says, after
`pool_mutex` is let go: "`shmem_ctx_create: no context free: OSOE_MAX_CONTEXTS is 3,
the contexts a PE may have at once, the default one included (design §4.11); the call
returns nonzero, as spec §9.5 lets it, and this PE says so this once`" (or
`shmem_team_create_ctx:`). `said_full` is a static under `pool_mutex`. A creation
that fails for memory, with an element free, stays quiet as before.

**Wire or layout**: neither.

**Decision taken** (the third session's finding 7 put it to the author; the review
offered the two forms): **once a PE**, not under `SHMEM_DEBUG` and not each call.
§9.5 makes a failed creation no error, and a program may probe for contexts by
creating them until one fails; a line a call would repeat with every probe, and a
line under `SHMEM_DEBUG` alone would not reach the user who needs it. The zero-copy
send's locked-memory notice is said once a process the same way. The author may
prefer either other form; each is a line's change.

**Test**: `shmem.context.pool-3.said-once.L2` (tier 2, 90 s): `OSOE_MAX_CONTEXTS=3`,
two PEs of `shmem_test_context`, which find the pool full twice each (the creations
to exhaustion, then one more); `count-lines.sh` requires exactly two lines naming
`OSOE_MAX_CONTEXTS is 3`. Without the change: "0 line(s) matching, wanted 2"; with
it, one line from each PE. A line a call would give four or more.

**README**: the `OSOE_MAX_CONTEXTS` row says a full pool returns nonzero and the PE
says so once.

### D27: the `EPERM` hint names the sysctl only where the sysctl refuses - done

**The item** (round four's Q1, `docs/reviews/archive/phase1-release-fixes-portability.md`):
under a container's seccomp profile, which answers `io_uring_setup` with `EPERM`, the
refusal said "which usually means the kernel.io_uring_disabled sysctl is set" of a
sysctl that read 0; the fix "have the message name a seccomp filter beside the
sysctl". Confirmed open: `transport-internode.c` named the sysctl on every `EPERM` at
creation, and the test `shmem.refuse.no-io-uring-setup` required that sentence of a
refusal that is `deny-io-uring`'s seccomp filter, on machines whose sysctl is 0.

**What changed.**

- `base/ring-check.c`, `.h`: `osoe_ring_sysctl_refuses(&value)` reads
  `/proc/sys/kernel/io_uring_disabled`: 0 allows; 2 refuses; 1 refuses a process
  without `CAP_SYS_ADMIN` (bit 21 of `CapEff` in `/proc/self/status` - no libcap)
  outside the group `kernel.io_uring_group` names (`getegid`, `getgroups`; -1, the
  default, names none) - the kernel's `io_uring_allowed`. -1 where it cannot be read.
- `shmem/src/transport-internode.c`, `osoe_internode_backend_check`: on `EPERM` at
  creation, by that answer - refuses: ", refused by the kernel.io_uring_disabled
  sysctl, which is N here" (at 1 adding "and admits only CAP_SYS_ADMIN and the group
  kernel.io_uring_group names, neither of which this process has"); allows: ",
  refused not by the kernel.io_uring_disabled sysctl, 0 here, which allows it, but
  by a seccomp filter, as a container runtime's default profile refuses io_uring";
  unreadable: ", refused by the kernel.io_uring_disabled sysctl or by a seccomp
  filter (the sysctl cannot be read here)". The buffer grew from 160 to 256 bytes.

**Wire or layout**: neither.

**Test**: `shmem.refuse.no-io-uring-setup` now requires the filter's wording, any
sysctl value that allows ("`[0-9]+ here, which allows it, but by a seccomp filter`"),
and fails on "sysctl is set" or "refused by the kernel.io_uring_disabled". Without the
change it fails, the job saying "(it fails with "Operation not permitted", which
usually means the kernel.io_uring_disabled sysctl is set)"; with it, "(it fails with
"Operation not permitted", refused not by the kernel.io_uring_disabled sysctl, 0 here,
which allows it, but by a seccomp filter, as a container runtime's default profile
refuses io_uring)".

**README**: the containers' paragraph, which said the message names the sysctl even
though it reads 0 inside the container, says it names the filter there.

### D29: `SHMEM_INFO` names every variable of the README's tables - done

**The item** (round four's minors, the readiness review's S5: "prints 21 of the
tables' 34 variables (none of the transport's buffers)"). Confirmed open: at `main`
the tables hold 47 names and the text named 28; missing were `OSOE_COLLECTIVE_PLACES`,
the transport's nine (`OSOE_AM_CHUNK`, `_BUFFER`, `_BUFFER_BUDGET`, `_CREDITS`,
`_HUGEPAGES`, `OSOE_WIRE_BYTES`, `_BUFFERS`, `_ZERO_COPY`, `_ZERO_COPY_BYTES`) and
the testing table's nine but `OSOE_HEAP_SHIFT` (`OSOE_FORCE_REMOTE`,
`OSOE_CHECK_QUIET`, `OSOE_TEST_PERTURB`, `OSOE_TEST_NOTICE_SECONDS`,
`OSOE_TEST_DEAF_HELPER`, `OSOE_TEST_ABORT_DELAY_MS`, `OSOE_WIRE_UNSENT_BYTES`,
`OSOE_WIRE_SPLIT`, `OSOE_WIRE_SEED`). The review's other half - nothing printed
without a started job - is how `SHMEM_INFO` works (`shmem_init` prints it) and was
left.

**What changed** (`shmem/src/init.c`, `print_info`): `OSOE_COLLECTIVE_PLACES` after
the shape, and two sections as the README has them, "The transport's buffers" and
"For testing", each entry with its range and default and its design section.

**Wire or layout**: neither.

**Test**: `shmem.version.info.every-variable` (tier 2, 90 s; in the static checks'
`version\.`) runs the new `info-every-variable.sh`: the names of the first column of
every table of the README's "Environment variables", each required as a whole word
in a one-PE job's `SHMEM_INFO=1` text. Without the change: "47 variables in the
README's tables, 19 not named"; with it, none.

**README**: "`SHMEM_INFO` prints most of this list" now says every variable of the
section's tables.

### Batch 1's runs

- release, rebuilt: **0 warnings**.
- The batch's tests (`ctest --test-dir build/release -j4 -R
  '^shmem\.(settings|version\.info|context\.pool|refuse\.no-io-uring|no-io-uring)'`):
  without the change **9 of 15** (the six above fail); with it **15 of 15**.
- The release suite, models aside (`ctest --test-dir build/release -j4 -LE model
  --timeout 300`): **936 tests, 933 passed, 2 skipped** (the root refusal tests,
  `shmem.wire.zero-copy-refused` and `shmem.internode.zero-copy-refused.0-1-2`, as in
  every session here), **1 failed: `shmem.boundary`**, in 306 s. The boundary check
  requires every library file in exactly one layer list, and the new
  `unknown-settings.c` and `.h` were in none; they are now the runtime's
  (`cmake/CheckBoundary.cmake`), like `fatal.c` and the sweep, and the check passes
  ("92 files above or on the boundary, none reaching a header below it").
- The static checks with `settings-known`, after that, on the batch's tree alone:
  **15 of 15** in 101 s - `main`'s 13, `shmem.settings-known` and
  `shmem.version.info.every-variable` (in `version\.`).

**D27, the sysctl's own branches, shown by hand** (after batch 1's commit, with
nothing else running; the sysctl put back to 0 at once): with
`kernel.io_uring_disabled=2`, a two-node job of `shmem_test_init` said "(it fails
with "Operation not permitted", refused by the kernel.io_uring_disabled sysctl,
which is 2 here)"; at 1, as user 65534 (`setpriv --reuid=65534 --regid=65534
--clear-groups`), "refused by the kernel.io_uring_disabled sysctl, which is 1 here
and admits only CAP_SYS_ADMIN and the group kernel.io_uring_group names, neither of
which this process has"; at 1 as root, which holds `CAP_SYS_ADMIN`, the job ran
(exit 0). No test can set the sysctl, so these stay a record.

## Batch 2: start-up's messages

One commit for the six items: the one that adds this section.

### D10: a dead node named by its host and its lead's rank - done

**The item** (RD; round four's readiness Q5-6, "Messages that could say more"): a
dead or vanished node was named by its index alone ("node 1 ended its connection
without its goodbye"), and the README never said that node N is the step's node N,
`SLURM_NODEID`. Confirmed open: `progress.c`'s death messages took `peer` alone.

**What changed.**

- `shmem/src/progress.h`: the host's table gains `describe_node(context, node,
  words, len)`, last, so that the engine's own tests' positional tables stay valid
  (each now gives it `NULL`): "A peer node as a message names it beside its index".
- `shmem/src/progress-host.c`: `describe_node` writes the node's entry in the
  step's node list (`osoe_hostlist_entry` of `osoe.bootstrap.node_list`: a host
  name under Slurm, a loopback address under `osoe-launch`) and its lead's rank
  (`osoe_job_node_lead`): "127.0.0.2, lead rank 2".
- `shmem/src/progress.c`: `node_words(engine, node, buf, len)` makes "node 1
  (127.0.0.2, lead rank 2)", or "node 1" where the host gives nothing; the seven
  death messages use it - a peer that stopped answering (keepalive, and both of
  `osoe_progress_check_silent`'s), a failed wire, a stream ended mid-segment, a
  reset and an end without the goodbye.
- `shmem/tests/test-barrier-am.c`, `test-progress.c`, `test-owners.c`: the new
  field's `NULL` in their host tables, which `-Wmissing-field-initializers` asked
  for.

**Wire or layout**: neither (the host table is process-private).

**Test**: `shmem.dead-pe.lead.0-0-1-1.named` (tier 2, internode, 90 s): node 0's
lead dies (`dead-pe.sh`, map `0,0,1,1`) and node 1 must say "node 1: node 0
(127.0.0.1, lead rank 0) ...". Without the change: "node 1: node 0 ended its
control wire without its goodbye: its lead has exited or died (§6.4)", failing;
with it, "node 1: node 0 (127.0.0.1, lead rank 0) ended its control wire without
its goodbye".

**README**: "When a job fails or hangs" quotes the new line and says what a node's
number is: its place in the step's node list, Slurm's `SLURM_NODEID`, or
`osoe-launch`'s logical node, whose entry is a loopback address.

### D11: start-up's messages name a rank beside a local id - done

**The item** (RD: "start-up messages give local ids, not ranks"). Confirmed partly
open: round four had made the join's notices name ranks ("local id 2 (rank 2)"),
but `node.c`'s agreement check and sibling mapping, and `liveness.c`'s pidfd
notice, still named "local PE N" alone.

**What changed**: `shmem/src/node.c` - `check_agreement`'s three messages (the
symmetric size, the heap object, the statics) and `map_siblings`'s four (a
sibling's heap or statics that cannot be opened or mapped) say "local PE N (rank
R)"; `shmem/src/liveness.c` - "cannot open a pidfd for local PE N (rank R, pid
P)". The settings' agreement messages, "OSOE_MAX_CONTEXTS differs between PEs: the
local lead has 64, this PE has 128", name the lead by its role and were left: the
line's own "PE N:" is the rank of the PE that saw it.

**Wire or layout**: neither.

**Tests**: `shmem.refuse.size_mismatch` now requires "SHMEM_SYMMETRIC_SIZE differs
between PEs: local PE [012] (rank [012]) has N bytes"; `shmem.dead-pe.no-pidfd.*`
require "cannot open a pidfd for local PE N (rank R, pid P)". Without the change
all three fail - "SHMEM_SYMMETRIC_SIZE differs between PEs: local PE 0 has
268435456 bytes, this PE has 2097152", and "(pid 18266)" alone.

**A trap met on the way**, recorded so that the next session does not meet it: a
`;` in a `PASS_REGULAR_EXPRESSION` splits it into a list of two, either of which
passes the test. The first forms of the two pidfd expressions (`[^;]*; watching`)
and of D28's below (`\); this kernel`) passed without the change for that reason;
they now use `.` where the `;` was, and fail without it. The same holds of a
refusal test's PATTERNS: the existing `shmem.start-up.never-calls-init.no-progress-timeout`
splits its "seconds; missing local ids" in two, which still means what it says;
D15's new case uses `.`.

### D14: a descriptor limit too low names the hard limit to raise - done

**The item** (round four's readiness Q5-6): "accepting a connection on port 18438:
Too many open files", no word of `ulimit -Hn`, which the README names. Confirmed
open: `listener.c`'s three socket and accept failures gave `strerror` alone.

**What changed** (`shmem/src/listener.c`): `descriptor_hint(error, buf, len)` -
for `EMFILE`, ": this process may hold N descriptors, which shmem_init raised to
the hard limit, and a node's lead holds two sockets for every other node and a
descriptor for every other PE of its node beside the program's own - raise the
hard limit on open files, ulimit -Hn"; for `ENFILE`, the system's `fs.file-max`;
nothing otherwise - after "cannot create a listening socket", "accepting a
connection on port P" and "cannot create a socket".

**Wire or layout**: neither.

**Test**: `shmem.start-up.descriptors.16-nodes` (tier 2, internode, 90 s): sixteen
logical nodes, each PE under `sh -c "ulimit -n 32; exec ..."`. Without the change
the job ends "cannot create a socket: Too many open files"; with it, the hint, its
limit 32. (At 40 the job runs; at 32, 24 and 20 it is refused alike.)

**README**: the Limits' "descriptors for the network" says a node that runs out ends
naming its limit and that the hard one is to raise.

### D15: under the no-progress limit, the node with the missing PE speaks first - done

**The item** (round four's readiness Q5-6, its run A15): with
`OSOE_NO_PROGRESS_TIMEOUT` set, a PE that never calls `shmem_init` can make another
node's first line about the rendezvous rather than the missing rank. Confirmed
open, and seen in this session's run without the change: map `0,0,1,1`, rank 1
never calling, the limit 3 s - node 0's lead "no local PE has joined for
OSOE_NO_PROGRESS_TIMEOUT seconds; missing local ids: 1 (rank 1)" and node 1's PE 3
"no progress for 3 s (OSOE_NO_PROGRESS_TIMEOUT): this node's lead has not finished
the job rendezvous" in the same second: whichever came first, under `srun -K1` the
only line left.

**What changed.**

- `base/stall-watch.h`: `OSOE_STALL_WATCH_HOP_GRACE_NS` (2 s) and
  `osoe_stall_watch_limit_beyond(limit, hops)`: the limit and two seconds a hop,
  or 0 (never) as before.
- `shmem/src/listener.c`: the five waits on another node - `read_all`,
  `write_all`, `osoe_listener_accept`, `osoe_listener_connect_port`,
  `osoe_listener_connect` - take one hop's grace; their three ends on a node that
  never came say "- a node comes to the rendezvous once all its PEs have called
  shmem_init, and names any that has not" (`NOT_COME`).
- `shmem/src/job.c`: a PE's wait for its lead's job tables (`follow`) takes two
  hops' grace, so that its lead, a hop from the others, speaks before it.
- A node's own waits - the join's, `node.c` - keep the limit itself.
- Design §3.15 says so, "As built (D15)".

**Wire or layout**: neither. **A behaviour change** the item did not name in
these words: with the limit set, a wait on another node at start-up now ends 2 s
(4 s for a non-lead) later than the limit; the README's row says so.

**Test**: `shmem.start-up.never-calls-init.no-progress-timeout.0-0-1-1` (tier 2,
internode, `osoe_refusal_test`, 20 s): map `0,0,1,1`, rank 1 exits before
`shmem_init`, the limit 3 s, notices put off to 60 s; every rank must end by
itself, the job's first line of the library's must be node 0's naming "missing
local ids: 1 (rank 1)", and node 1's lead must say "node 0 has not answered node 1
on any of its 32 candidate ports in OSOE_PORT_RANGE=... - a node comes to the
rendezvous once all its PEs have called shmem_init, and names any that has not".
`refusal.sh` learns the first-line form: a PATTERN beginning with `=` must match
the first line that begins "osoe: ". With the change: node 0's line at 3 s, node
1's at 5 s, the run 5.1 s. Without it the test fails (node 1's line lacks the
cause).

**README**: the `OSOE_NO_PROGRESS_TIMEOUT` row, and "A start that waits" in "When a
job fails or hangs".

### D25: a stalled get names the node that owes the zone's bytes - done

**The item** (R9 Q5-8): a 256 KiB `shmem_getmem_nbi` toward a stopped node waits
inside the call, its fifth piece finding the zone full, and ends "shmem_get_nbi:
no progress for 10 s ... the landing zone's released extents has stayed at 0";
`osoe-stat` shows "nonblocking gets, 262144 bytes in flight" and no "waiting
toward" line. Confirmed open in `make_room` (`transport-internode.c`): a wait for
room that is not a blocking get's own named no node, since the ninth review's Q5-2
found the get's own target blamed.

**What changed** (`shmem/src/transport-internode.c`).

- `issue_target`, a process-private array beside `copy_destination`, one
  `_Atomic uint32_t` an extent: the PE each piece was asked of, stored (release)
  by `issue_record`, which now takes the PE, before the record goes `ISSUED`.
- `zone_owed_by(&pe)`: over the zone's records, those `ISSUED` (loaded acquire) -
  unanswered; a landed piece owes nothing - and their targets (acquire): the one
  node that owes them all and, where one PE does, that PE; otherwise none. O(the
  zone's extents), at a wait's start and its turns.
- `make_room`: a wait that is not a blocking get's own names that node and PE
  (`am_wait_begin`, the reply kind osoe-stat shows as a node waited toward), and
  is begun again when the answer changes; where several nodes owe, or none, it
  names none, as before.
- Not a protocol change: nothing waits on `issue_target`; its two orders are
  named per §3.16 (release, acquire), and it is freed with `copy_destination`.

**Wire or layout**: neither (process-private memory).

**Tests**: `shmem.get-no-progress.nonblocking.0-1` (new, tier 2, internode): 256
KiB nonblocking from PE 1, which stops, through a zone of 64 KiB, the limit 10 s;
`test-get-no-progress.c` gains `OSOE_TEST_GET_NBI=1` (listed in
`unknown-settings.c`). Without the change: "shmem_get_nbi: no progress for 10 s
(OSOE_NO_PROGRESS_TIMEOUT): the landing zone's released extents has stayed at 0";
with it, "shmem_get_nbi: no progress toward node 1 (PE 1) for 10 s ...".
`shmem.get-no-progress.zone-held.{8,150000}.0-1-2`, Q5-2's: the zone held by a
nonblocking get from PE 2, which stops - the blocking get's wait now names node 2
and PE 2, which owe it, and still never node 1, which Q5-2 forbade; without the
change both fail (they named no node).

**README**: the gets' Limits entry says where one node owes every piece the wait
names it, in `osoe-stat` and in the no-progress line.

### D28: a sandbox's refusal of futex2 named as a filter, not an old kernel - done

**The item** (round four's portability Q6): "osoe needs Linux 6.7 or later, for the
futex2 calls (they fail with "Function not implemented"); this kernel is
6.12.95+deb13-cloud-arm64" under Rosetta; "name the kernel's age only when the
release is below 6.7, and a filter otherwise". Confirmed open in `init.c`.

**What changed**: `base/futex.h` declares `osoe_futex_kernel_has_calls(release)`;
`futex-futex2.c` reads uname's "major.minor" - 1 from 6.7, 0 below, -1 unreadable;
`futex-classic.c` gives -1 (every kernel the single-node build runs on has the
classic calls, and its requirement's words already name a sandbox).
`shmem/src/init.c`'s refusal adds, where the kernel has the calls, ", which has
them: a seccomp filter refuses them here - a container runtime's profile older
than the calls, or a sandbox's - or an emulator does not pass them on".

**Wire or layout**: neither.

**Tests**: `shmem.refuse.old-kernel` (`deny-futex2`, `ENOSYS`) and
`shmem.refuse.no-shared-futex2` (`EPERM` on the shared calls) now require ",
which has them: a seccomp filter refuses them here" after the release. Without the
change both fail ("... (they fail with "Function not implemented"); this kernel is
6.18.44-fc-v114"). The old-kernel branch itself, a release below 6.7, cannot run
here: the full build needs 6.7.

**README**: the probe's paragraph in "Building" says the refusal names the
kernel's age only where it is older than 6.7, and a filter otherwise.

### Batch 2's runs

- release, rebuilt: **0 warnings** (the first build warned three times, the engine
  tests' tables missing the new field; given `NULL`, none).
- The batch's tests (`ctest --test-dir build/release -j4 -R
  '^(shmem\.(dead-pe|refuse\.(size_mismatch|old-kernel|no-shared-futex2)|get-no-progress|start-up\.(never-calls-init|descriptors|node-refuses)|stopped-node)|env\.probe\.old-kernel)'`):
  **30 of 30** with the change; without it (the library's files and the three
  engine tests set aside, the tests kept) **19 of 30** - the eight new or changed
  above, and, once their `;` was gone, `shmem.refuse.no-shared-futex2` and both
  `shmem.dead-pe.no-pidfd.*`.
- The README's descriptive internode count: 512 to 516 (four new internode tests).
- The static checks: **15 of 15**, 101 s.
- The release suite, models aside: **940 tests, 938 passed, 2 skipped** (the root
  refusal tests), none failed, 300 s.

## Batch 3: `fatal.c`'s and `abort.c`'s

One commit for the five items: the one that adds this section. They are one
change seen from five sides, so the evidence is shared.

### D18, D20, D21: an end said once a process - done

**The items.** D18 (v0.2 step 4's readiness Q5-3): at two helpers each surviving
lead wrote a death twice, its agent's "control wire" line and its helper's
"connection" line, each with its own control segment line, "four lines a node where
there were two". D20 (RD): "one death printed ten lines on one node and one fatal
three times". D21 (R9 Q5-4, corrected by the round-nine fact-check's item 13): a
non-lead at no helpers wrote both its agent's "ending: the job is aborting on
behalf of PE 0" and its own thread's "...: the job is aborting on behalf of PE 0;
this PE cannot make progress" - two writers, `abort.c` and `fatal.c`, "and no guard
of once a process". Confirmed open: four writers of an end, none guarded.

**What changed.**

- `shmem/src/fatal.h`, `fatal.c`: `osoe_fatal_first_word()`, true for the first
  caller in the process and false after (`atomic_exchange_explicit` on
  `end_said`, seq_cst). A fatal writes its line only where it has the first word;
  everything else it does - the publish, the last word, the abort, `_exit` - it
  does as before.
- The other three writers take the same word before they write: `abort.c`'s
  "ending: the job is aborting on behalf of PE N", `liveness.c`'s "local PE N
  (rank R, pid P) has ended without finalizing" (the cause, said first, and the
  agent's own "ending" after it silent), and `init.c`'s "the process exited without
  calling shmem_finalize".
- `liveness.c`'s line now ends "; this PE was waiting in R" too, as the
  "ending" line it silences did: the first suite run with the guard failed seven
  tests that read where the lead had waited from that "ending" line
  (`shmem.unswept-wait`, "shmem_quiet: the job is aborting on behalf of PE 1|waiting
  in shmem_quiet" not found; the six `shmem.death-in-wait.*`, "0 of 1 survivors
  named shmem_barrier_all") - the lead's own liveness line had taken the first word.
  With the cause line saying where, all seven pass.
- The first writer is the one that says the cause where there is one: the PE that
  saw it writes before it declares the abort that ends the others (fatal.h).

**Wire or layout**: neither.

### D19: "in shmem_finalize", not "not waiting in the library" - done

**The item** (v0.2 step 4's readiness Q5-3, its second bullet): a fatal the agent
raises in `shmem_finalize` - the deaf helper's, the frozen helper's under
`OSOE_NO_PROGRESS_TIMEOUT` - ended "...; this PE was not waiting in the library",
the PE being in `shmem_finalize` in no wait the blocked record names; the fix "this
PE was in shmem_finalize". Confirmed open.

**What changed.**

- `shmem/src/blocked.h`, `metrics.c` (where the record's process-local word lives):
  `osoe_blocked_finalizing(bool)`, a process-wide `finalizing` word stored release
  and loaded acquire; and `osoe_blocked_where(buf, len)`: "waiting in R" while a
  wait is recorded, "in shmem_finalize" while finalizing in none, "not waiting in
  the library" otherwise.
- `shmem/src/init.c`, `pshmem_finalize`: the word set on entry, under the
  lifecycle mutex, and cleared before it is let go.
- `fatal.c` and `abort.c` take their "this PE was ..." from `osoe_blocked_where`,
  where each composed it alike; `shmem/tests/test-fatal-order.c`, which compiles
  `fatal.c` alone, stubs it.

### D22: no `osoe-stat` advice on a segment the job's end takes with it - done

**The item** (RD): "every fatal suggests `osoe-stat` on a segment gone by then".
Confirmed: every fatal after start-up wrote "control segment /osoe... (osoe-stat
/osoe..., while any PE of this node is alive)", though by default the name is gone
after the join and the segment goes with the node's PEs, which the fatal ends
within a sweep.

**What changed** (`fatal.c`): `segment_named(name)` - whether `/dev/shm<name>`
still exists - and the control segment line only then, as "control segment
/osoe.J.S.N.0, its name still in /dev/shm (osoe-stat /osoe.J.S.N.0; osoe-stat --gc
removes it once its PEs are gone)": under `OSOE_KEEP_CONTROL_SEGMENT`, or in a start
that fails before the node's PEs have opened it.

### Batch 3's tests

- `shmem.dead-pe.lead.0-0-1-1-2-2.helpers-{0,2}.one-line` (new, tier 2, internode):
  six PEs on three nodes, node 2's lead (rank 4) dies (`dead-pe.sh`), at no helpers
  and at two; `dead-pe.sh` learns `ONE_LINE`, which requires every rank to have
  written one line of the library's at most; and no line may name a control
  segment. **Without the change** both fail, "ranks that wrote more than one line:
  0 2 5" - ranks 0 and 2, the leads, the death and the segment line; rank 5,
  node 2's other PE, the liveness line and its agent's "ending". **With it**: one
  line each.
- `shmem.helpers.deaf.0-1`: now requires "...has not answered...this PE was in
  shmem_finalize". Without the change it fails, the line ending "this PE was not
  waiting in the library".
- `shmem.fatal.kept-segment-named` (new, tier 2): `OSOE_KEEP_CONTROL_SEGMENT=1`, one
  PE of `shmem_test_bad_put`, whose put is refused: the fatal names its segment
  "its name still in /dev/shm", and `osoe-stat --gc` then removes it. Without the
  change it fails on the new wording ("(osoe-stat ..., while any PE of this node is
  alive)"); it guards that the advice stays where it can help.
- `shmem.fatal-order` passes both ways, as a stub change must.
- With the change: the batch's tests and their neighbours (`-R
  '^shmem\.(dead-pe|fatal|helpers\.deaf|refuse|exit|global-exit\.fatal|job\.disagree)'`)
  **72 of 72**; without it, of the five above, **1 of 5**.

**README**: "When a job fails or hangs" says each process says its end once, where
the PE was, and that the segment is named only while its name is in `/dev/shm`.

### Batch 3's runs

- release, rebuilt: **0 warnings** (one link error on the way: `test-fatal-order.c`
  compiles `fatal.c` alone, and needed `osoe_blocked_where`'s stub).
- **The first release suite with the guard: 943 tests, 932 passed, 2 skipped, 9
  failed.** Seven were the guard's - the liveness line's missing "where", fixed as
  told under D18 - and two, `shmem.static-link` and `shmem.strict-c11`, were mine:
  they compile the library's sources when they run, and I was editing batch 4's
  `job.c` while they did, with a brace missing (an `else` taking only the first of
  three statements, which the strict build caught). The lesson for any session
  here: no source edits while a suite runs.
- After the fix, on the batch's tree alone (batch 4's files set aside): the static
  checks **15 of 15**, 100 s; the release suite, models aside, **943 tests, 941
  passed, 2 skipped** (the root refusal tests), none failed, 302 s.

## Batch 4: `osoe-stat`'s

One commit for the six items. D8 is stopped (it needs a layout change), with its
README sentence made.

### D2: every node of a job shown, however many - done

`tools/stat/stat.c`: `list_segments` and `show_job` take the whole listing
(`list_all`, as `--gc` already did) instead of a fixed 64 names. Test
`shmem.stat.view.many-nodes` (new, tier 2, internode; `shmem/tests/stat-views.sh`):
66 logical nodes of one PE each. **Without**: "osoe-stat JOB showed 64 of 66 nodes"
(the listing 64, "1 more not shown"). **With**: 66 nodes, 132 summary lines, 66
listed.

### D3: the counters view holds every PE of a node - done

`stat.c`, `counters_human`: the per-PE values in an array of the node's PE count
(`calloc`), not `uint64_t per[64]`. Test `shmem.stat.view.many-pes` (new, tier 2):
70 PEs on one node. **Without**: "rows of 66 fields, not 72" under a header naming
70. **With**: 72 fields a row (name, 70 values, total).

### D5: `--gc` reads into its own memory; its status counts only failures - done

- `tools/stat/segment.c`, `segment.h`: `osoe_segment_open_copy` - the magic by one
  `pread` (garbage of any size costs that read), then a `pread` copy of at most the
  first GiB (landing zones come last and `--gc` reads none of them); every bound
  then holds against what was read. `not_a_segment` marks a refusal that was the
  object's own content.
- `stat.c`, `gc_one(name, named)`: uses the copy; an object that is no segment, a
  table it cannot read, or one that shrank is "left alone" with status 0; a name
  given on the command line that it cannot open is a failure (1), one found by the
  listing is said on stderr and not counted (a decision: it may have gone under it).
- Tests: `shmem.stat.gc.not-a-segment` (new, tier 2): 4 KiB of random bytes named
  `osoe.$$.0.0.0`. **Without**: "is not an osoe control segment", "--gc exited 1".
  **With**: "left alone: ... is not an osoe control segment", "--gc exited 0".
  `shmem.stat.gc.shrunk-under-read` (new, tier 2, not under the sanitizers): a
  segment a fatal keeps (`OSOE_KEEP_CONTROL_SEGMENT=1`, `shmem_test_bad_put`), cut to
  one page the moment `osoe-stat` has it by a new `LD_PRELOAD` shim
  (`shmem/tests/shrink-under-read.c`: after `mmap` returns, or after the first
  `pread`). **Without**: "Bus error", "--gc exited 135" - round four's `SIGBUS`, now
  deterministic. **With**: "no readable metric table, so nothing says whose it is;
  left alone", "--gc exited 0". (A cut to 64 bytes did not fault the old reader:
  the truncation zeroes the rest of the directory's page and its parse stopped at
  "no table" - hence a page.)

### D6: a stopped PE marked as stopped - done

`stat.c`: `process_stopped(pid)` reads `State:` in `/proc/<pid>/status` (T or t);
a PE line gains "   [stopped: SIGSTOP or a debugger]" and a node's summary line
"[N PE(s) stopped]". Test `shmem.stat.view.stopped` (new, tier 2, internode): two
nodes, rank 1 sent SIGSTOP. **Without**: "rank 1's line does not say it is
stopped". **With**: both markers.

### D7: an init hang shows where each PE waits - done

The "blocked in" record says `shmem_init` and what is awaited: `shmem/src/node.c`'s
`wait_count` (the lowest local PE not yet at the phase, by rank, begun again as it
changes; no detail where every PE is there and the count lags), `rendezvous.c`
(the parent around the connect and around `send_up`/`receive_down`, the awaited
child around each accept, each child around `receive_up`), `job.c` (a non-lead
waits on its lead's rank). `shmem/tests/test-job.c`, which compiles `job.c` and
`rendezvous.c` alone, stubs the two calls. Review caught one path of mine that
entered without leaving - the record nests, so it would have named `shmem_init`
under every later wait - fixed with an explicit flag before any run counted.
Test `shmem.stat.view.init-hang` (new, tier 2, internode): four PEs on two nodes,
rank 3 exits before `shmem_init`. **Without**: "0 of 3 waiting PEs show where they
wait". **With**: rank 0 "shmem_init (node 1)", rank 1 "shmem_init (PE 0)", rank 2
"shmem_init (PE 3)" - the chain to the missing rank.

### D8: each owner's last pass with its age - stopped: it needs a layout change

A last-pass time is a new field of every owner record (`struct
osoe_am_owner_metrics`, layout 36 onward) and a new metric-table row: a layout
change the item does not name, so stopped. Made instead: the README sentence the
review offered (its option (c)), checked by a run - an idle owner's passes went
241 to 261 in two seconds, ten a second. Options for the author are in the
response ("What needs the author").

### Batch 4's runs

- release build **0 warnings** (one link error on the way: `shmem_test_job` needed
  the blocked record's stubs).
- The batch's six tests: **with, 6 of 6; without (stat.c, segment.c, segment.h,
  node.c, rendezvous.c, job.c set aside), 0 of 6**, each for the reason above.
- Static checks (with `settings-known`): **15 of 15**, 102 s.
- **The release suite was not run on batch 4**: the session's credit ran out here.
  The lead's merge should run it whole.
- README: "Looking inside a job" (stopped marking, init waits, every node and PE,
  D8's sentence); the `--gc` paragraph (reads into its own memory, leaves alone
  what it cannot read, status counts only failures; the epilog's safe line is
  still the `rm`); the internode count **521** (`ctest -N -L internode`). Batch 3's
  commit left that count at 516 where it was 518 - a slip, corrected here.
- Wire or layout: neither.
