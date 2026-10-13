# Cloud session 3: the manifest

*The record of the third cloud session (`docs/prompts/cloud-session-3-prompt.md`),
2026-10-13, on the branch `cloud-session-3` from `main` at `aeabc5e`, for the lead's
review after the author pulls. Written after every item, each time committed and
pushed. Paths write the repository as `<repo>`. The response,
`docs/prompts/cloud-session-3-response.md`, is the complete account; this file is
the per-item log behind it.*

## The environment, since the second report

Unchanged from the second session's (`docs/prompts/cloud-session-2-manifest.md`):
Linux 6.18.44-fc-v114 on four vCPUs and 15.7 GiB, `/dev/shm` 16 GiB,
`kernel.io_uring_disabled = 0`, `vm.mmap_min_addr = 4096`, `ulimit -l` held at
8192 KiB for root with `CAP_IPC_LOCK`, liburing 2.9, `ethtool` installed, gcc 13
as `cc` with gcc-14 and clang beside it, cmake, ninja and Python 3. The kernel
still has no netem (`# CONFIG_NET_SCH_NETEM is not set`; `tc qdisc add dev lo
root netem delay 1ms` says "Specified qdisc kind is unknown"), and `tbf` is built
in. Nothing needed setting.

At `main` (`aeabc5e`): `cmake --preset release` and `cmake --build build/release
-j4` in 51 s, **0 warnings**; the static checks (`ctest --test-dir build/release
-R '^shmem\.(memory-orders|strict-c11|boundary|static-link|exports|settings-documented|license-lines|single-node\.configure|version\.)' -j4`)
**13 of 13**.
