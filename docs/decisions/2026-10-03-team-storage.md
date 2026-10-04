# 2026-10-03: team blocks become a pool found by name (step D, part 1)

The first part of the team redesign's step D, built on
`docs/prompts/phase1-step7-slice3-prompt.md` (the brief), Part 1, from
`docs/design/team-resources-proposal.md` (the proposal) §3, D3 and D5. Nothing
about numbering changes: a team's number still comes from world PE 0's counter
and is never given back. What changes is where a node keeps a team and how it
finds it: the per-number blocks of the control segment become a pool, a team's
block is found by its *name* through a map, and the progress engine, the
internode seam and the wire know a team by its block, not its number. It changes
no protocol - a message means the same team before and after, so
`tree_barrier_model.py` stays valid - and the record says where each piece went.

## What was built

1. **`OSOE_TEAM_BLOCKS`**, the teams a node can hold at once: default 1,024, 8 to
   65,536, read and validated by every PE in the bootstrap, compared among a
   node's PEs in `open_control` as `OSOE_MAX_TEAMS` is, and free to differ
   between nodes. In `SHMEM_INFO`'s text and the README's table, *checked* among
   a node's PEs.
2. **The segment, layout 29** (`node.h`, `node.c`): the header's per-number team
   blocks are a pool of `OSOE_TEAM_BLOCKS`, still the header's tail, the
   predefined teams' the first three at the index of their number, so the node
   scope still names their fields. After the threads' records, appended: the
   **map**, a word per name - `OSOE_MAX_TEAMS` names for each local PE - holding
   a block index plus one, and the **bitmap** of free blocks. Four directory
   fields, appended; the directory is 288 bytes (`node.c`, `tools/stat/segment.c`).
3. **The block's name** (`struct osoe_team_name`, a cache line of each block): the
   local id of the team's local lead, its number, its holders and its
   generation. In this part the local lead writes it as it claims the block;
   holders are the team's local members and the generation 1; nothing gives a
   block back. Exported as `team.name.*`.
4. **The claim** (`osoe_node_team_claim`): a compare-and-swap on the bitmap, a
   failed claim taking nothing, **from the top of the pool down**. A created
   team's local lead on each node claims its block, names it, writes the record,
   lowers the node's low-water mark of published blocks and stores the map entry
   with release, all before the split's sync, after which each member acquires the
   map entry to find its block (`team.c`, `take_number`; `node.h`'s argument for
   the record, which the sync carries, unchanged).
5. **The seam** (`internode.h`): every `team` argument is a block index,
   node-local. The generic internode phase places a team's slots and counters in
   the internal region by the number its block's name holds, the same on every
   node (`internode-generic.c`, `number_of`); its up-calls find a descriptor
   through the block's number.
6. **The engine** (`progress.c`, `progress-host.c`, `internode-am.c`): its
   per-team arrays are sized by `OSOE_TEAM_BLOCKS` and indexed by block; it learns
   a team, with its tree, its number and the local id of its local lead on each
   neighbour's node, from the record (`team_of_record`); the arrival scan walks
   the predefined teams' three blocks and those at and above
   `team_blocks_published_from`, the published blocks' low-water mark - the walk
   it made over numbers below a high-water mark, over block indices.
7. **The wire** (`am.h`): a barrier message names its team on the node it goes
   to - the number in `team`, the receiving node's lead local id in `lead`, until
   now `reserved` - and the receiver's map resolves the name to its block
   (`barrier_team`); a name with no block ends the job naming the sender, the name
   and the node, and is never learnt from the message.
8. **A dry pool** ends the job at the next split, naming the setting and the node
   - temporary: the number is claimed job-wide by then, so the split cannot fail
   alike on every parent PE until the second part's reduction.
9. **`osoe-stat`** walks the pool: a team is shown by its record's name and the
   number its block's name holds, with its block and generation, and each local
   PE's published epoch for that number - the pairing the dump's comment
   claimed and did not do. The JSON's `number` is the team's number, with `block`
   and `epochs` beside it.
10. **The README** (the `OSOE_TEAM_BLOCKS` row; the limit of teams a node holds,
    with its temporary end) and **the design** (§5.7's account of the pool; §3.12's
    of the arrival scan).

## The author's answers (the brief's "a test that must change in Part 1")

- `shmem.allgather`'s white-box check read its team's block by number, which on
  two of its nine maps would have been another team's: it now finds the block by
  the number its name holds (`osoe_test_team_block`, `invariants.h`). The
  author: "By block index".
- The reply pool's term for collective forwards stays degree x `OSOE_MAX_TEAMS`
  through this part - while numbers are job-wide a node's blocks match them one
  for one, so it is still the bound - and the second part moves it to the pool and
  re-sizes `shmem.reply-pool.overflow`. The author: "Move it to Part 2".

## Choices made without the author

- **Claims from the top of the pool.** The proposal says "as team numbers are
  claimed"; the direction is not part of that. From the top, a created team's
  block index is never its number on any node, so engine code indexing by number
  fails ordinary multi-node tests at once, with no test hook - the brief's
  "make the first claim skip a block", done for every claim. *Undo*: the
  highest-bit search in `osoe_node_team_claim` becomes the lowest, and the scan's
  low-water mark a high-water mark again.
- **The posted-block bitmap goes to the second part.** The brief's item 6 asks
  for a bitmap of posted blocks behind one summary word, so that a pass with
  nothing posted costs one load. In this part blocks map one to one to numbers
  and are never given back, so the walk above the low-water mark costs what the
  walk over numbers cost; the bitmap is a new lock-free publication, which the
  second part, where blocks are given back and the walk would grow, builds with
  a model first. *Undo*: none needed; the second part adds it.
- **The engine's own tests** register teams by hand and have no map: with no
  finder, a message's number is its block and its lead local id 0
  (`barrier_team`), so those tests run as they stand.
- **`shmem.job.disagree.team-blocks` is a test that the job runs**: nodes may
  hold pools of different sizes, so the brief's name stands for the positive
  case - the team split program's forty teams over two nodes, the second node's
  pool a tenth of the first's. The refusal is `shmem.team-blocks.local-disagree`,
  within a node.
- **The pool's pages are allocated with the header** at the segment's creation,
  as the per-number blocks were: a megabyte of `/dev/shm` a node at the default,
  so that a claim cannot meet a full `/dev/shm` by `SIGBUS`.

## Tests and verification

On a copy of the tree, with the test pins on, in the `osoe-x86` VM: `debug`
670 of 670, `release` 670 of 670, `asan` 669 of 669, `tsan` 453 of 453. In the
`osoe` VM: `tier3` 107 of 107; `osoe-pacer teams` for 300 s and `mix` for 600 s
on 0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3, no failure in either. The SOS team programs
(`unit/` and `spec-example/` with `team` in their names, on 0-1 and 0-0-1-1): 53
of 55 runs exit 0; the two that do not are `shmem_team_b2b_collectives`, which
collects 40 longs into a destination of 10 and is named for it - SOS's own,
known since 2026-09-26. `shmem_team_max` creates 61 teams at the default 64
numbers, every split after them failing alike, as numbers are still job-wide.

New tests: `shmem.team-blocks.local-disagree` and `shmem.team-blocks.invalid.*`
(the setting refused, within a node and by value); `shmem.job.disagree.team-blocks`
(forty teams over two nodes, the second node's pool a tenth of the first's);
`shmem.team-blocks.dry` (`OSOE_TEAM_BLOCKS=8`, the fatal naming the setting and
the node); `tier3.team-split.0-1-2-0-1-0`.

**Mutations**, each a line of the code the part stands on, run against the
team and gather tests (23 of them):

- *by-number*: the arrival scan hands the engine the team's number where its
  block index is meant. 18 of 23 fail, every multi-node `shmem.allgather` and
  `shmem.team-storage` map among them - with blocks claimed from the top of the
  pool, no created team's block is its number on any node.
- *own-lead*: a message names the team by the sending node's local lead
  rather than the receiving node's. 7 of 23 fail: the maps whose teams have
  different local leads on different nodes (0-1-2-0-1-2, 0-1-2-0-1-0, the
  five-node ones); the others pass, their leads coinciding, which is why the
  tier-3 map 0-1-2-0-1-0 was added.

Found while writing the second part, and fixed before the merge: two diagnostics of
`internode-generic.c` printed `number_of(number_of(team))`, the number of the
block whose index was the team's number - a wrong number in a fatal's text,
nothing more.

## Left for the second part

The per-PE free mask and the split's reduction, giving numbers and blocks back,
the generation's use, the handles, the reply pool's term, the posted-block
bitmap, and the tests that change by design: `shmem.team-split.exhaust`'s
numbers, `shmem.team-storage`'s record that outlives destroy, the pacer's
`teams` budget.
