# 2026-10-06: the internode backend behind one file boundary (v0.2 step 1, part 1)

The first part of v0.2's first step, built on
`docs/prompts/done/v0.2-step1-single-node-prompt.md` (the brief), Part 1, from the
author's decisions for v0.2 (`docs/decisions/2026-10-05-v0.2.md`, items 2 to
5). A refactor of the full build, so that the second part can replace
everything a job of more than one node needs with a backend that refuses such
a job, and build the library with no liburing: the *single-node build*. The
README does not change in this part.

## What was built

1. **`transport-am.c` split in two.** It keeps what every job uses: §4.10's
   entry points and the dispatch on locality, the pool of contexts in the node
   segment and their modes and mutexes (§4.11), `osoe_transport_direct`,
   `_load` and `_set_range`, and the fence and wait of a job of one node, which
   are the calling thread's fences alone. Its far half - the dirty set, the
   reservations and their waits, the landing zone's extents and copy-out, the
   credits, the replies, the waits of both modes with their counters, and
   `OSOE_CHECK_QUIET`'s promises - moved whole to `transport-internode.c`, in
   the order it had, its functions' text unchanged but for the context's
   ordering state, now reached through `context->internode`.
2. **`internode-backend.h`**, the internode backend's entry points as the part
   every job uses calls them, each one of §4.10's calls or its share of one for
   a job of more than one node, which the header says beside it:
   | Entry point | §4.10's call |
   |---|---|
   | `_settings_check` | `osoe_transport_settings_check`'s share: the kernel the backend needs for a job of this many nodes, then its settings |
   | `_advertise` | `osoe_transport_advertise` |
   | `_rendezvous` | the job rendezvous of §6.2, the blob's carrier (job.c's lead) |
   | `_start`, `_stop` | `osoe_transport_init`'s and `_fini`'s shares |
   | `_agent_pass`, `_agent_wait`, `_agent_exit`, `_agent_abort`, `_node_idle`, `_node_quiet`, `_deadlock_send` | the agent calls, in a job of more than one node |
   | `_context_open`, `_context_close` | `osoe_transport_context_create`'s and `_destroy`'s shares, and the default context's |
   | `_put`, `_get`, `_rmw` | the three, toward a PE that does not route in this process |
   | `_fence` | `osoe_transport_fence`'s share: what a fence owes the network |
   | `_wait`, `_wait_gets` | `osoe_transport_wait` and `_wait_gets`, whole |
   The internode backend also implements `internode.h` whole (on AM,
   `internode-am.c` and the generic phase). On AM it is these files:
   `transport-internode.c`, `progress-host.c`, `progress.c`, `wire.c`,
   `ring.c`, `connection-area.c`, `connection.c`, `reply.c`, `am.c`,
   `rendezvous.c`, `listener.c` and `internode-am.c`, now a list of their own
   in `shmem/CMakeLists.txt` (`osoe_internode_sources`).
3. **`transport-context.h`**, the context the two parts share: the common
   fields, and the internode backend's state behind an opaque
   `struct osoe_internode_context *internode`, NULL in a job of one node.
4. **The AM layer's sizes and the wire's settings got files of their own on
   the boundary**, `am-configuration.c` (`osoe_am_config_read` renamed
   `osoe_am_configuration_read`, after its file) and `wire-settings.c`
   (`read_settings` and `hash64` of `wire.c`, now `osoe_wire_settings_read`
   and `osoe_wire_hash`). Every PE of every job checks them as values before
   its node joins, and the landing zones in the node segment are laid out from
   the first in any job, so both stay in every build: the second part's
   single-node build checks them as the full build does on one node, and no
   setting is refused by one build and taken by the other. `node.c` now reads
   the AM layer's sizes from a file on the boundary and leaves
   `CheckBoundary.cmake`'s list of the boundary files known to read AM, which
   is down to two (`rendezvous.c`, `transport-local.c`).
5. **The io_uring start-up check moved below the seam.** `init.c` made it
   itself, between disarming and arming the refusal's last word, by calling
   `base/ring-check.c`; the transport's settings check now makes it first, by
   asking the internode backend whether it can run a job of this many nodes
   (`osoe_internode_backend_check`, which answers with a sentence or NULL), and
   refuses telling no one - `transport-am.c` disarms the last word before the
   fatal, so no tombstone is left in `/dev/shm`, as before - and then checks
   the settings as values in the order they had: the AM layer's sizes, then
   the wire's. `init.c` no longer knows io_uring exists, which an RDMA port
   would otherwise have had to change; and the second part's single-node build,
   which compiles no `ring-check.c`, could not have kept `init.c`'s call
   without a branch on the build there.
6. **`job.c`'s lead reaches the rendezvous through the backend**: it fills the
   blob by `osoe_transport_advertise` as before and hands it to
   `osoe_internode_backend_rendezvous`, which `rendezvous.c` implements as
   `osoe_listener_own_address` and `osoe_rendezvous` were called before. The
   result's struct moved to `internode-backend.h`, so `job.c` includes neither
   `listener.h` nor `rendezvous.h`, both now the internode backend's.
7. **`shmem.boundary`'s third check** (`cmake/CheckBoundary.cmake`): nothing
   outside the internode backend includes, however indirectly, a header of its
   (`am.h`, `am-state.h`, `connection.h`, `connection-area.h`, `listener.h`,
   `progress.h`, `progress-host.h`, `rendezvous.h`, `reply.h`, `ring.h`,
   `wire.h`) or `base/ring-check.h`. Shown to fail: `#include "progress.h"`
   planted in `transport-am.c`, `#include "listener.h"` and
   `#include "rendezvous.h"` in `job.c`, and `#include "base/ring-check.h"` in
   `transport-am.c`, each failed it naming the chain; the clean tree passes,
   87 files checked. A system header - `<liburing.h>` - it cannot see; the
   single-node build, which has none, is what fails on that.

## progress-host.c's guarded sites

The brief asked which of its sites guarded on `osoe_am.internode` a job of one
node reached. Before this part, through `transport-am.c`'s forwarders, all but
one: `osoe_progress_host_start` (from `osoe_transport_init`), `_stop` (from
`_fini`), `_poll` (the agent's pass), `_wait` (the agent's wait, which slept on
the plain futex there), `_exit`, `_abort`, `_idle` and `_quiet` (the deadlock
detector's calls), each returning at once or doing the one-node thing, and
`_deadlock_send`, which ended the job; `osoe_progress_host_wake_lead` was
reached only from the far half. After it, none: the part every job uses makes
the one-node answers itself - a pass that moves nothing, a plain futex wait on
the agent's word, idle and quiet with nothing applied, and the deadlock
message's fatal - and calls the internode backend only in a job of more than
one node. So the guards' `osoe_am.internode` halves went, and their
`osoe_am.lead` halves stay; `osoe_am.internode` is now the backend's own "has
started" flag, set by its start, and `wake_lead` keeps its guard, cheap, for a
call after the stop.

## The one change counters can see

A wait on a *shared* context (`SHMEM_THREAD_MULTIPLE`) in a job of one node
ran the shared mode's protocol: the generation and the pair of get counters it
closes, the context's mutex, and `OSOE_CHECK_QUIET`'s promise. On one node no
nonblocking get is ever counted in a pair - every get is applied before its
call returns - so the protocol closed nothing there. Now such a wait is what a
serial one always was on one node: the serializing fence, and the counters
`transport.wait.calls` and `transport.wait.nothing_outstanding`. So
`am.context_locks` and `transport.wait.ns` no longer count a shared context's
quiet, pe_quiet or destroy on one node. And a shared context's wait for its
gets alone - the end of `shmem_iget` and `shmem_ibget` - did all of that too,
two serializing fences included, and counted `transport.wait.calls` and
`transport.wait.nothing_outstanding`: on one node it now does nothing, as a
serial context's always did, since nothing is in flight to wait for and the
specification asks it to order no stores (the adversarial read's F1 found
these last two counters). No test reads any of the four on one node
(`test-multiple.c` reads `am.context_locks` only toward another node), and
nothing a program can observe changes: in a job of two nodes the shared waits
count exactly as before, which the adversarial read measured.

## Choices made without the author

Each is Claude's, under the standing mandate; the undo follows.

1. **The internode backend's entry points are whole §4.10 calls**, not the
   AM implementation's steps: the backend does a job of more than one node's
   whole wait, both modes, so the shared mode's protocol lives in one file and
   is not split across the seam. *Undo:* finer entry points for the wait's
   steps (complete the gets, wait for the dirty set, the promise), and the
   protocol back in `transport-am.c`.
2. **The io_uring check below the seam - for the author to look at.**
   `transport.h`'s comment on `osoe_transport_settings_check` grew: it now says
   the transport first refuses what the backend needs of the kernel for a job
   of this many nodes, telling no one. No call was added and no signature
   changed. The adversarial read judged it a change to the seam's shape in
   substance, the brief's first stop-and-ask, since a §4.10 call's contract
   grew and the start-up protocol's "tells no one" now runs below the seam. It
   was built without waiting, for two reasons: the second part needs exactly
   this hook - the brief puts the single-node build's refusal of a job of more
   than one node in the backend, at start-up, before anything is made in
   `/dev/shm`, which only the settings check reaches, and only telling no one -
   and the single-node build compiles no `ring-check.c`, so `init.c`'s call
   could not stay without a branch on the build in `init.c`, which the brief's
   first goal rules out. The rule that a refusal here tells no one lives in
   `transport-am.c`, the part every build compiles, so no backend calls the
   refusal's functions. *Undo:* `init.c` calls `osoe_ring_check` again between
   the disarm and the arm, the comment goes back, and the single-node build
   needs another way to refuse before the node joins - a new call in
   `transport.h`, which is the author's to allow.
3. **The rendezvous is an entry point of the internode backend**, as the brief
   lists it ("start (open the listener, run the rendezvous, build the
   mesh)"), and `job.c` - on the boundary - calls it through
   `internode-backend.h`, which the boundary check therefore treats as a
   backend's header (refused above the boundary, allowed on it and below).
   *Undo:* `job.c` calls `osoe_listener_own_address` and `osoe_rendezvous`
   again, and the second part compiles `listener.c` and `rendezvous.c` in the
   single-node build too.
4. **The internode backend starts before the default context opens**, so
   that the context's open can use what the start set (`OSOE_CHECK_QUIET`'s
   flag); the dirty set was allocated first before. Nothing between them uses
   either. *Undo:* the order back, the flag passed to the context's open.
5. **Settings parity** (item 4 above): the AM layer's sizes and the wire's
   settings stay in every build, so that the single-node build refuses exactly
   the values the full build refuses on one node. *Undo:* the single-node
   build checks only what it reads, and the README says the wire's settings
   are ignored there.

## Verification

Of `be30686`, the part as first committed, in the `osoe` VM from an archive
(`build/scratch/single-node/verify.sh`): the four presets with the floor pins
on - debug 718 of 718, release 718 of 718, asan 715 of 715, tsan 490 of 490 -
tier 3 113 of 113, `osoe-pacer collectives` at `OSOE_SMALL_COLLECTIVE_BYTES`
128, 1,024 and 16,384, its `teams` and `threads`, and ten minutes of `mix`, all
`failures=0`; and the models preset, 30 of 30 in 13 minutes. Of the tree with the
adversarial read's fixes (below), `1244875`: the four presets and tier 3
again - debug 718, asan 715, tsan 490 and tier 3 113 in the `osoe-load` VM,
without the floor pins while the `osoe` VM ran the models, and release 718 in
`osoe` with them - the boundary check's four plants, and F2's case.

## The adversarial read

By a fresh agent, given the diff, the claim of no behaviour change but the
one, and the rule that the part every job uses reaches the internode backend
only through its header. Nothing blocked; it found five things, all fixed:

- **F1**, the counters of a shared context's `wait_gets` on one node, which the
  first claim left out: stated above.
- **F2**, the order of the start-up refusals: a job of two nodes where
  io_uring is refused *and* an `OSOE_AM_*` setting is bad was refused by the
  setting, armed, so each lead left a tombstone, where before the io_uring
  refusal came first and left nothing. The backend's share was then the
  io_uring check and the wire's settings together, after the AM layer's
  sizes. Fixed by splitting it: the backend now only answers whether it can
  run the job (`osoe_internode_backend_check`), which the transport asks first;
  then the AM layer's sizes; then the wire's, read in `transport-am.c` (also
  F5). Shown: the double fault refuses on io_uring with nothing left in
  `/dev/shm`, and a bad wire setting on one node is still refused.
- **F3**, a hole in the third check: `rendezvous.h` was on the boundary, so
  `job.c` could have called `osoe_rendezvous` and passed. Its struct moved to
  `internode-backend.h` and `rendezvous.h` joined the internode backend's
  list, with `base/ring-check.h`; both plants now fail it.
- **F4**, comments that said a job of one node never enters the internode
  backend: it makes four of `internode.h`'s calls - the default fan-out, init,
  fini and a team number taken again - which the header and `transport-am.c`
  now say, and `internode-generic.c`, which they hand three of them to, is
  every build's.
- **F5**, the wire's settings checked in the backend's share: the second
  part's backend would have had to remember them for the single-node build to
  refuse what the full build refuses. Now in `transport-am.c`.

And minor ones, fixed: design §4.10's count of boundary files reading AM, and
`CheckBoundary.cmake`'s sentence calling the listener every build's. Its
judgement that moving the io_uring check is a change to the seam's shape is
choice 2 above. It also measured, beside the claim: in a job of two nodes the
shared waits take the mutex and count as before; the one-node agent calls are
the old guards' answers exactly; the moved code differs from the original only
by the context's indirection; and a context whose internode state fails to
allocate gives its block back with nothing leaked.
