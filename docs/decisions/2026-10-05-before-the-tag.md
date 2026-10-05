# 2026-10-05: before the tag

The author's questions and answers the morning after the release point, before
the tag. Documents only: the code is the tree the release point verified
(`2026-10-05-release-point.md`).

1. **The Pi cluster's switch is gigabit where the nodes meet it.** The author
   asked whether the PoE switch, listed as having 100 Mbit/s ports, limits the
   cluster below the gigabit the README states. It does not: each node's link
   negotiates 1000 Mb/s, full duplex, with its port (read on all five nodes on
   2026-10-05; the captures of 2026-09-26 recorded the same for three compute
   nodes and left the fourth's blank), and node-to-node streams run at 111.5
   to 112.1 MiB/s, which a 100 Mbit/s port cannot carry - it caps a
   stream near 11 MiB/s. So the README's speeds and scaling stand, and
   `docs/pi-cluster.md` §1 now records each node's negotiated speed in every
   session. *Undo:* drop that line of the runbook.

2. **The mesh's start-up schedule waits for a system large enough to measure
   it.** The author asked whether anything spreads the start-up connections
   out in time, against a TCP storm. No clock does, but the order does: each
   lead accepts every lower node, then calls each higher one, one call at a
   time, with a listen backlog of the node count, and a model
   (`tools/modelcheck/mesh_timing.py`) has at most two SYNs reach a listener
   within a round trip at any node count. What the order costs is time - a
   chain of about n call times, 0.32 s at 1,024 nodes in the model - and
   `docs/design/mesh-schedule-proposal.md` proposes a deterministic schedule of
   slots that would take 51 ms there. The author's decision: postpone it until
   a large system can test it, and say in the README's limits that the start-up
   is a chain a larger system may need changed. The same limit now names
   Linux's neighbour table (`net.ipv4.neigh.default.gc_thresh3`, 1,024 entries
   by default), which a subnet of more than about a thousand nodes outgrows, at
   the default connection budget's ceiling of 1,025 nodes. *Undo:* take the
   proposal up as a slice, with a system to time it on; the README's sentence
   changes with it.

3. **What Claude Fable 5.1 did.** The README's "How osoe was made" said that
   Claude Sonnet and Claude Fable took parts of some sessions and of the tasks a
   session handed off. The author's correction: Fable did periodic reviews and
   deep planning. The README says so now.

## What is left for the author

- **The tag**, on the commit that carries this record, and **the repository
  published**: `main` and the tag, the whole history as decided
  (`2026-10-04.md` item 2). The working branches beside `main` were
  squash-merged into it and are not published.
- Optionally, **`RemoveIPC=no`** on the Pi's nodes, which needs sudo.
