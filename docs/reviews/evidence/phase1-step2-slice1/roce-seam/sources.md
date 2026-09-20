# Sources checked for the roce-seam review (fetched 2026-09-20)

Everything here was read on the day of the review. "Observed" means the file was
downloaded and read; line numbers are those of the `master` branch that day and
will drift. Nothing below was measured on hardware.

## rdma-core (linux-rdma/rdma-core, master)

- `libibverbs/verbs.h`
  <https://raw.githubusercontent.com/linux-rdma/rdma-core/master/libibverbs/verbs.h>
  - l.156-159: `enum ibv_atomic_cap` has three values: none, HCA-scope, global.
  - l.331-345: `enum ibv_pci_atomic_op_size` (4-, 8-, 16-byte) and
    `struct ibv_pci_atomic_caps` with three size masks: fetch_add, swap,
    compare_swap. Reported in `ibv_device_attr_ex.pci_atomic_caps` (l.362).
  - l.1141-1142: `IBV_WR_ATOMIC_CMP_AND_SWP`, `IBV_WR_ATOMIC_FETCH_AND_ADD`;
    l.1345-1348: the `ibv_qp_ex` forms `wr_atomic_cmp_swp`, `wr_atomic_fetch_add`.
- `providers/mlx5/qp.c`
  <https://raw.githubusercontent.com/linux-rdma/rdma-core/master/providers/mlx5/qp.c>
  - l.48: the atomic operand is 8 bytes. l.57-58: the two IBV atomic opcodes map
    to `MLX5_OPCODE_ATOMIC_CS` / `MLX5_OPCODE_ATOMIC_FA`. l.898-910: the
    `ibv_post_send` path builds the atomic segment. l.1554-1584: the `ibv_qp_ex`
    path does too.
  - **Consequence:** the first round's OFI note ("rdma-core implements these only
    in rxe and ionic; I did not see mlx5 support") is out of date or was wrong:
    stock verbs on mlx5 carries the two 8-byte IBTA atomics.
- `providers/mlx5/verbs.c`, l.4040-4057: `get_pci_atomic_caps` fills
  `pci_atomic_caps` from the HCA's `atomic_caps.fetch_add_pci_atomic`,
  `...swap...`, `compare_swap_pci_atomic` fields.

## UCX (openucx/ucx, master)

- `src/uct/ib/mlx5/rc/rc_mlx5.inl`
  <https://raw.githubusercontent.com/openucx/ucx/master/src/uct/ib/mlx5/rc/rc_mlx5.inl>
  - l.755-772: plain FA/CS assert an 8-byte length.
  - l.774-832: masked CS and masked FA exist in **4-byte and 8-byte** forms
    (the opmod carries log2 of the operand size: 2 or 3).
  - l.1865-1935: how every UCT atomic is encoded as **one** WQE:
    ADD 64 = plain FA; ADD 32 = masked FA; AND, OR, SWAP = masked CS with a zero
    compare mask (so the "compare" always succeeds) and a swap mask choosing the
    bits to replace; XOR = masked FA with every bit a field boundary (add without
    carry); CSWAP = CS / masked CS.
  - **Consequence:** on mlx5 with extended atomics, the bitwise AMOs and SWAP are
    single-round-trip NIC operations, not read-then-CAS loops.
- `src/uct/ib/mlx5/rc/rc_mlx5_common.c`, l.1139-1195
  <https://raw.githubusercontent.com/openucx/ucx/master/src/uct/ib/mlx5/rc/rc_mlx5_common.c>
  - If the device has **PCI atomics**, UCX advertises only ADD and CSWAP (at the
    sizes the device reports) and sets `UCT_IFACE_FLAG_ATOMIC_CPU`.
  - Otherwise it advertises plain 64-bit ADD/CSWAP, and with extended atomics the
    full set at 32 and 64 bits, and sets `UCT_IFACE_FLAG_ATOMIC_DEVICE`.
- `src/uct/api/uct.h`, l.386-391: the two flags' meaning - consistent with CPU
  operations, versus consistent only with other atomics on the same device.
- `src/uct/ib/mlx5/dv/ib_mlx5dv_md.c`, l.2544-2587: the capability query
  (`atomic_size_qp`, `atomic_size_dc`, `atomic_operations`,
  `atomic_req_8B_endianness_mode`, `fetch_add_pci_atomic`,
  `compare_swap_pci_atomic`); l.598-630: UCX registers a **separate atomic
  memory key** for ranges with remote-atomic access, and l.351-356 notes the
  firmware wants such a region aligned to the largest atomic operand.
- `src/uct/ib/base/ib_device.h`, l.229-235, l.418-421: the per-device size masks
  and `uct_ib_device_has_pci_atomics`.

Not determined: which shipping ConnectX firmware sets the PCI-atomic capability
bits, and on which root complexes. The code path exists; its prevalence is unknown.

## PCI Express AtomicOps

- PCI-SIG "Atomic Operations" ECN (against Base 2.0; folded into 2.1), as mirrored at
  <https://www.yumpu.com/en/document/view/10780137/atomic-operations-ecn-pci-sig>:
  three requests - FetchAdd, Swap, CAS; FetchAdd and Swap at 32 and 64 bits, CAS
  at 32, 64 and 128 bits; Root Ports and Functions with memory BARs may be
  completers; a requester-enable bit in Device Control 2; an egress-blocking bit
  on routing ports.
- AMD ROCm, "How ROCm uses PCIe atomics"
  <https://rocm.docs.amd.com/en/docs-6.3.2/conceptual/pcie-atomics.html>: the same
  three operations and sizes; target address aligned to the operand size; every
  root port on the path must route AtomicOps; the CPU must support them.

## SHARP

- Graham et al., "Scalable Hierarchical Aggregation Protocol (SHArP)", COMHPC
  2016, abstract <https://dl.acm.org/doi/abs/10.5555/3018058.3018059>: 8-byte
  MPI_Allreduce on 128 hosts, 6.01 us in software to 2.83 us offloaded.
- The Next Platform, 2023-12-01
  <https://www.nextplatform.com/2023/12/01/if-you-want-to-sell-ai-to-enterprises-you-need-to-sell-ethernet/>:
  SHARP is a feature of the Quantum InfiniBand switches; Spectrum-4 Ethernet
  does not have it. So **on RoCE there is no SHARP to lose.**

## XRC and DC

- NVIDIA MLNX_OFED "Advanced Transport"
  <https://networking-docs.nvidia.com/mlnxofedswum/590590/advanced-transport>:
  XRC saves queue pairs for all-to-all process connectivity; DC lets reliable
  queue pairs connect and disconnect dynamically, reducing the system-wide
  count; DCT v0 (ConnectX-4) and v1 (ConnectX-5 on) do not interoperate; the
  mlx5 atomic reply-endianness create flag.
- Subramoni et al., "Designing MPI Library with Dynamic Connected Transport",
  ISC 2014 <https://link.springer.com/chapter/10.1007/978-3-319-07518-1_18>
  (abstract only): DC matches RC/XRC all-to-all performance at a lower memory
  footprint.

Not confirmed from a primary source: mlx5's maximum queue-pair count per
device. The per-NIC XRC arithmetic in the review uses the count alone and labels
the device limit *recalled*.
