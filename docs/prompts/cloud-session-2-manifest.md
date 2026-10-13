# Cloud session 2: the manifest

*The record of the second cloud session (`docs/prompts/cloud-session-2-prompt.md`),
2026-10-13, on the branch `cloud-session-2` from `main` at `225d205`, for the lead's
review after the author pulls. Written after every item, each time committed and
pushed. Paths write the repository as `<repo>`.*

## The environment, since the first report

The machine is the first session's (`docs/prompts/cloud-session-1-manifest.md`,
Part 1): Linux 6.18.44-fc-v114 on four vCPUs and 15.7 GiB, `/dev/shm` 16 GiB,
`kernel.io_uring_disabled = 0`, `unshare -rn` working, `ulimit -l` held at 8192 KiB
for root with `CAP_IPC_LOCK`, liburing 2.9 from the setup script. Nothing needed
setting. What changed is `ethtool`, which the setup script now installs
(`/usr/sbin/ethtool`), and with it tier 3 configures and builds:
`cmake --preset tier3` and `cmake --build build/tier3 -j4` in 1 min 32 s, **0
warnings**. **It does not run here**: `ctest --preset tier3 --test-dir build/tier3`
fails all 123 tests in 16 s, each at its first `tc qdisc add dev link0 root netem`
(`tools/launch/tier3.sh`) with "Error: Specified qdisc kind is unknown". This
kernel is built with `# CONFIG_NET_SCH_NETEM is not set` (`/proc/config.gz`) and
has no loadable modules (`/lib/modules` is absent); the token-bucket qdisc, `tbf`,
is built in and works in a namespace. So tier 3's shaped links - their delay and
their loss are netem's - need the development VM, and the lead runs this
session's tier-3 tests there; where a change's tests were run here over the same
namespaces with `tbf` in place of netem, its entry says so. At `main`, the release
preset builds without a warning and the static checks pass 13 of 13.

## Part 1: `collect`'s places four ways

### 1. T32's rest: `allgather_model.py`'s breakages name the property each must fail

**Done.** Commit: the one that adds this entry. Files:
`tools/modelcheck/allgather_model.py`.

- Every check's message now begins with the property it holds, as the first
  session made every other model's: DEST, FORWARD, RESULT, PLACE, RANGE, SCAN,
  and in the leads' scan scan_model.py's SLOT and VALUE - 32 messages, prefixed
  by a script whose every pattern had to match its stated count, and the lines
  the prefixes made longer than 100 columns rewrapped by hand.
- Every breakage names the property it must fail, and `report(res,
  expect=...)` compares: `no_ready` DEST, `no_data_wait` FORWARD, `short_steps`
  RESULT, `staged_one_slot` SLOT, `staged_early_copy` FORWARD,
  `bases_in_node_order` PLACE, and the ranged `range_before_data`,
  `range_before_ready`, `wrap_as_one` and `no_cut` RANGE. The ranged arithmetic,
  which searches with its own code, judges its three breakages by name with
  `engine.judge`, as `circulant_model.py`'s arithmetic does: `heap_tree` SCAN,
  `no_cut` and `wrap_as_one` RANGE.
- The docstring says so; FORWARD's definition now covers a share copied out of
  a staging slot, which is what `staged_early_copy`'s check is and what the
  docstring already named it.
- **Every verdict unchanged**: the whole model before and after, `python3
  allgather_model.py`, 66 lines each, every state count and verdict identical
  and only the prefixes differing; no line UNEXPECTED; exit 0; 7 min 17 s here.
- **Without the change a misnamed breakage passes**: a copy with `no_ready`
  expecting RESULT and `heap_tree` expecting RANGE prints `<-- UNEXPECTED: fails
  DEST, where it must fail RESULT` and `fails SCAN, where it must fail RANGE`,
  and exits 1; under the old `expect_ok=False` both were accepted.
- `selftest.py`: exit 0.
- **Left**: `engine.report`'s `expect_ok`, the older spelling the first session
  kept for this model, now has no caller. Removing it changes the engine, which
  the brief keeps for a change that reruns the whole suite, so it stays; the
  next engine change can drop it.
- **For the VM**: `ctest --test-dir build/release -R "^model\.(allgather|selftest)$"`.
