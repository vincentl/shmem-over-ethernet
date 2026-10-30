# The off-network cluster: osoe at hundreds of nodes

*Written by the lead on 2026-10-30 as the 0.2 tag's last item (X17 of
`docs/prompts/v0.2-open-items.md`), once the tree was frozen for the tag, for
the author's plan after 0.2 (N4, `docs/decisions/2026-10-20.md`): a bare-metal
cluster of hundreds of nodes, off the network, with an agent of its own testing
osoe there and perhaps developing it. The agent starts cold, with the tree and
this file. The author opens the session with: "Read
docs/prompts/off-network-cluster-brief.md and do what it says."*

## What this session is for

osoe is designed to stay usable at hundreds of nodes with 256 PEs each, and the
largest jobs across separate machines so far are 256 PEs on eight nodes and 512
on two (the README's opening). Its ceilings are far past what has run: most are
checked by their arithmetic alone (the README's "Ceilings, and how far each is
tested"; `docs/ceilings.md`). This cluster is the first that can answer the
questions only scale answers, listed below, each with how to measure it.
Measure first; change code only where a run cannot go on without it or a defect
shows, and keep each change small. The session's product is one Markdown file,
written for the worst case, that it is the only thing that comes back (the last
section).

## Read first

In this order, each for what it says, not whole:

1. `CLAUDE.md`: the working agreements - names spelled out as words, how a
   rename is audited, when to hand work to a subagent.
2. `README.md`: the opening and its Status; "Running a program with `srun`",
   "Looking inside a job" and "When a job fails or hangs"; "Scaling with nodes,
   not PEs", to its last heading, "Ceilings, and how far each is tested"; the
   Limits; and under "Reference", the environment variables, "How a job starts"
   and "How a job ends".
3. `docs/ceilings.md`: every ceiling, where the source sets it, the largest run
   of it, and the widest gaps.
4. `docs/fast-link.md` and its kit, `docs/fast-link/`: the runbook and scripts
   the measurements below reuse - `common.sh` first, which starts every job -
   and `docs/fast-link-results.md`, the three sessions on eight Google Cloud
   nodes: the numbers to set this cluster's beside.
5. The records of what is asked here: `docs/decisions/2026-10-20.md` - A2 (the
   progress helpers' default), A6 (the receive pool) and N4 (this brief);
   `docs/decisions/2026-10-19.md` - the mesh's schedule, and the neighbour table
   at 64 logical nodes; `docs/decisions/2026-10-28.md` - the third session.
6. The design notes, `docs/design/shmem-ethernet-design.md`, by section as a
   question needs them: §6 (the bootstrap: the rendezvous and the mesh), §3.4
   and §3.8 (connections, the reply pool), §5.8 (the large collectives'
   shapes), §10 (the open questions).
7. `docs/decisions/README.md`: the decisions in force. A finding that would
   change one is a question for the author, not a change.
8. `docs/pi-cluster.md` §8: how the corpus's programs are built with `oshcc`.

Not to read through: `docs/reviews/`, `docs/design/evidence/` and
`docs/prompts/done/` are history.

## The off-network facts

**The build fetches nothing.** CMake finds its packages through pkg-config and
downloads nothing - no `FetchContent`, `ExternalProject` or `file(DOWNLOAD)`
anywhere in the tree. The full build needs a C11 compiler (GCC or Clang),
CMake 3.25 or newer, pkg-config, liburing 2.6 or newer and Linux 6.7 or newer;
Python 3 only for the models and the kit's `summarize.py` (the README's
Requirements). Confirm it first, on the cluster: the `release` preset
configured and built with no network, and `build/release/tools/probe/probe` on
every node, which must say `ok` for every facility. A kernel older than 6.7,
or one whose `kernel.io_uring_disabled` refuses io_uring to the user, cannot
run a job of more than one node at all: say so in the file's first lines and
stop.

**What must be carried in**, from a machine with the network, before the
session:

1. **The tree, with its history**, so that every run names its commit: `git
   bundle create osoe.bundle --all` there, `git clone osoe.bundle osoe` here.
   The kit records `git rev-parse HEAD`; a copy without `.git` must set
   `KIT_COMMIT`.
2. **The reading corpus.** `tools/corpus/fetch.sh` fetches public OpenSHMEM
   programs by URL, each pinned in `tools/corpus/manifest.tsv`: among them
   `tests-sos`, Sandia OpenSHMEM's test suite; `osu-omb`, OSU's benchmarks
   7.5.2, a tarball; and `openshmem-npbs`, `prk`, `isx`, `osb` (with `ssca1`),
   `openshmem-examples` and `bale`. Fetch every entry there, into a directory
   of its own - `OSOE_CORPUS=$PWD/osoe-corpus tools/corpus/fetch.sh` - and
   carry it whole, the git entries' `.git` directories and the tarball's
   `.osoe-pin` file with it (`tar -cf osoe-corpus.tar osoe-corpus`, and on a
   Mac `COPYFILE_DISABLE=1` before it, so that macOS adds no `._` files).
   Unpack it here as `~/osoe-corpus`, where the kit's `sos.sh` looks, and link
   the checkout's `corpus/` to it (`ln -s ~/osoe-corpus corpus`), where
   `docs/pi-cluster.md` §8 looks. Then run `OSOE_CORPUS=~/osoe-corpus
   tools/corpus/fetch.sh` once here: an entry at its pin says so and needs no
   network, and one that is not tries the network and fails - which here means
   it was not carried in.
3. **liburing's source**, if the site's is missing or older than 2.6, and
   **CMake**, if the site's is older than 3.25: each built or unpacked under
   your home, as "The kit on the site's own Slurm" says.
4. **The OpenSHMEM 1.6 specification**, if wanted: it is not in the repository
   (`docs/reference/README.md`).

Nothing else is fetched: `dev/` makes the development VMs, which this cluster
does not need.

## The questions only this cluster answers

Each with what is known and how to measure it. Every job runs as the kit runs
its jobs (`docs/fast-link/common.sh`'s `launch`: `OSOE_NO_PROGRESS_TIMEOUT=60`,
`SHMEM_DEBUG=1`, a timeout and `osoe-stat --gc` after it), in rounds that meet
the cases in another order each round (`rotate`), the median of the rounds
reported with their spread; and each table says the commit, the nodes, the PEs
a node, the helpers and every setting that differed from the defaults.

1. **Start-up at hundreds of nodes.** On eight Google Cloud nodes the slowest
   PE's `shmem_init` took 102 ms at a PE a node, 122 at 8 and 117 at 32, the
   fastest about 70 (H11, `docs/fast-link-results.md`); 254 logical nodes on
   one machine made their mesh in 0.65 to 0.75 s (H23,
   `docs/decisions/2026-10-19.md`); the model,
   `tools/modelcheck/mesh_timing.py`, gives the mesh 71 ms at 1,024 nodes and
   0.26 s at 4,097 over a link of 100 µs each way, and 0.11 and 0.45 s with the
   leads arriving as the rendezvous's tables reach them (the README's "What
   does not scale"). Measure: `scale.sh`'s start-up probe
   (`build/release/kit/startup`: PE 0's own `shmem_init`, the slowest and the
   fastest) at 1, 8 and 32 PEs a node, at node counts doubling from 8 to the
   whole cluster, three starts each; each lead's `SHMEM_DEBUG` line of how long
   its mesh took; both under `OSOE_MESH_DATA_WIRES=on-demand` as well as the
   default `eager`; and the model run at this cluster's hop, as `SHMEM_DEBUG`'s
   link line gives it. What to watch: the questions below, and a lead a second
   late - a handshake the kernel dropped, which osoe's dial deadline of a
   second retries (T46, `docs/decisions/2026-10-20.md`) - from the listen
   backlog (the job's nodes up to 4,096, cut to `net.core.somaxconn`) or the
   kernel's per-CPU `net.core.netdev_max_backlog`.
2. **The kernel's neighbour table on a flat network.** Each node needs an entry
   for each peer on its subnet, n - 1, and Linux holds 1,024 by default
   (`net.ipv4.neigh.default.gc_thresh3`; `gc_thresh1` and `gc_thresh2` 128 and
   512): past it the kernel drops what it cannot address and nodes count each
   other vanished, which osoe does not check (`docs/decisions/2026-10-19.md`).
   No separate machines have met it; tier 3 met it on one kernel at 64 logical
   nodes. Measure: whether the nodes share one subnet (`ip -4 route`), the three
   settings (`/proc/sys/net/ipv4/neigh/default/`), and before and after a job of
   the whole cluster each node's entries (`ip -4 neigh show | wc -l`) and the
   kernel's own counts in `/proc/net/stat/arp_cache` - `forced_gc_runs` and
   `table_fulls`, hexadecimal, a line a CPU - which any user can read where the
   kernel's log is closed (`dmesg` is, to a user, on Debian 13). On a flat
   network of more than about a thousand nodes, the settings are the site's to
   raise above the node count; until then the largest job that starts is the
   answer, with its first failure's lines.
3. **Träff's reduction past 256 nodes.** It has run on 70 logical nodes and on
   eight machines, where it ran a 1 MiB reduction 2.4 times faster than the tree
   the chooser took elsewhere; from 257 to 512 nodes it is tested only through a
   lower ceiling's tests (the author's A13, `docs/decisions/2026-10-11.md`), and
   past 512 only the cost model has it - 1.19 times the best tree at 513 nodes
   and 1.18 at 1,024 on the Pi's link by the event simulation, at a lane longer
   by 128 KiB for each doubling (the README's "Time"). Measure: the kit's
   `window.sh` (`shapes.c`: reductions and broadcasts of 64 KiB to 4 MiB) with
   `CASES="link tree"` at 128, 256, 257, 512 and 513 nodes and the whole
   cluster, at 1 and 8 PEs a node; and where `link` does not take Träff's, a
   link set by hand that the model says takes it (`OSOE_LINK_HOP_NS`,
   `OSOE_LINK_MESSAGE_NS`, `OSOE_LINK_BYTES_PER_SECOND`: the measured hop and
   bandwidth with a message cost of 0, say). Which shape a job took follows from
   its link, as `SHMEM_DEBUG` gives it, by `tools/modelcheck/shape_timing.py`,
   the model the C's chooser is tested against (`shmem.shape.*`); set each
   measured time beside the model's.
4. **P35's cap within the exchange's room of 64.** `collect`'s places: the
   leads' exchange of the nodes' totals has room for 64 nodes, and the default,
   `link`, takes it to 16 (P35), each node's total through the tree or the scan
   past it; it was fastest at 8 and 16 logical nodes (H24) and on eight machines
   (H25), and past eight machines the 16 is untested (the README's
   `OSOE_COLLECTIVE_PLACES` row). Measure: the kit's `collect.sh`, every way
   forced and `link`, at 8, 16, 24, 32, 48 and 64 nodes and at the whole
   cluster, 8 to 32 PEs a node: where a way the chooser does not take beats the
   one it takes, the 16 or the costs are what to move - a question for the
   author, with the numbers.
5. **The receive pool at many peers (A6).** Each owner of a node's wires
   receives into one pool, `OSOE_WIRE_BYTES` × `OSOE_WIRE_BUFFERS`, 16 KiB × 64
   by default, however many peers send to it. A pool that runs dry is not an
   error: the kernel ends a receive with `ENOBUFS`, the owner arms it again, and
   `transport.am.receive_enobufs` counts it - with no notice, so only the
   counters show it (`shmem/src/ring.c`, `received`). On eight machines 256
   buffers cut the re-arms about five times in a fan-in and raised no rate; the
   author kept 64 and asked for the pool to be measured here (A6). Measure: the
   kit's `fan-in.sh` - every PE of the other nodes putting into one PE - at node
   counts doubling to the whole cluster, 1, 4 and 16 PEs a node, 64 and 256
   buffers, 0 and 2 helpers; and an `alltoall` of the whole cluster
   (`osoe-bench`), reading each node's counters with `osoe-stat` beside the
   rate.
6. **A2 under the site's binding.** Two progress helpers are the default on a
   job of three nodes or more whose nodes' PEs fit their CPUs
   (`OSOE_PROGRESS_HELPERS=auto`, A2 and P36). On eight machines under Slurm's
   binding (H26), with one CPU a PE two helpers bought the streams 1 to 5%, and
   with a core a PE 21 to 22%, at half the mailbox's rate. Measure: the kit's
   `helpers.sh` at 0 and 2 helpers and the site's usual PEs a node, its steps
   bound as the site's own jobs are - `KIT_SRUN` with the `--cpu-bind` and
   `--cpus-per-task` they use, since a step of whole nodes may be bound to
   nothing unless it asks (the runbook's §5) - and each step's allowed CPUs
   recorded (`grep Cpus_allowed_list /proc/self/status` from a task), as §5
   does under `binding.sh site`, which this cluster does not run.
7. **The ceilings' widest gaps that this cluster can close**
   (`docs/ceilings.md`, "The widest gaps"), each run to as far as the cluster
   goes, its largest value and what happened there - the line printed, word for
   word - recorded:
   - **nodes**: every question above at the whole cluster, eager and on demand;
     its connection area in `/dev/shm`, 250 MiB a node at 500 nodes (the README's
     "Memory");
   - **PEs a node past 256**, if the nodes have the cores: H12 ran 256 a node
     on two machines with streams of puts and five collectives alone, so gets
     and the mailbox past 64 a node have not run, nor `osoe-pacer` past 4 a
     node across machines; and the team blocks there - three whole-job
     `shmem_team_split_2d` grids fill the default pool at 256 PEs a node
     (`OSOE_TEAM_BLOCKS`);
   - **PEs a job**: the largest the cluster holds - the design's scale is 500
     nodes of 256, 128,000 PEs, and 512 have run - with `/dev/shm`, each PE's
     memory and start-up at it, and `alltoall`, whose cost grows as P(N - P)
     blocks a node;
   - **the site's limits**: open files (a lead holds two sockets a peer node and
     a descriptor a node-mate; `shmem_init` raises the soft limit to the hard
     one), locked memory (io_uring's rings on Linux 6.14 and later) and
     `vm.max_map_count` (two maps a node-mate, which osoe does not check), each
     as a step sees it, and the job size at which it binds by the README's
     arithmetic.

Not asked here, though the kit has them: the floor and the ladder (§4, §6,
§16), the zero-copy send (§7, off by default since A3), latency tuning (§15,
which needs root) and the link's measurement (§19, P38). Run §4's floor once,
between two nodes, as the yardstick for the rest: `PARTS="floor iperf3"` where
the site has `iperf3`, since its `echo-uring` part met its timeout on the first
two sessions' clusters and has been left out since (`docs/fast-link/floor.sh`).

## The kit on the site's own Slurm, without `sudo`

The kit was written for machines made for it, as a user with `sudo` on every
node. On a site's Slurm none of its root steps run: use the site's Slurm as it
is, change none of its configuration, and ask the site, through the author, for
anything below that must be root's.

| What the kit does as root | Where | Without root |
|---|---|---|
| installs the packages (`apt-get`) | `docs/fast-link.md` §1; `session-3.sh` | the site's compiler, CMake and pkg-config, by its modules; liburing 2.6 or newer, where the site's is older, built from its carried-in source under your home (its `./configure --prefix=$HOME/.local`, `make` and `make install`) and found through `PKG_CONFIG_PATH=$HOME/.local/lib/pkgconfig`; then check with `ldd` on a node that the library and the probe find it at run time, and set `LD_LIBRARY_PATH` for the jobs where they do not |
| sets up Slurm: munge's key, `slurm.conf`, the daemons | `slurm-setup.sh` | not run: the site's Slurm is used as it is. Each kit job is an allocation of its own (`salloc -J osoe-kit -N... --exclusive`), so the site's partition, account and time limit go in `KIT_SALLOC` (`--exclusive -p ... -A ... -t ...`) |
| `TaskPlugin=task/none`, so that nothing binds a step | `slurm-setup.sh` | the site's plugin binds: a step of whole nodes is unbound under `--cpu-bind=none`, as the kit asks, but `on`'s and `spawn`'s one-task jobs, less than a node, are held to the CPUs Slurm gives them (`common.sh`) - record each step's allowed CPUs, and give those jobs whole nodes (`--exclusive`), a small change to `common.sh` with its diff in the file |
| rewrites the binding and restarts the daemons | `binding.sh` | not run: the site's binding is question 6's case, as it is |
| `RemoveIPC=no` for systemd-logind | `slurm-setup.sh` | a login that ends on a node deletes the user's names in `/dev/shm` about ten seconds later, and a job then still in `shmem_init` can wait for ever (the README's Limits, "A login that ends on a node"): reach the nodes only through Slurm - `srun --overlap` into a job for `osoe-stat` - and if a login on a node is needed, hold it open until the job has started; or ask for `RemoveIPC=no`, or lingering (`loginctl enable-linger`) for the user |
| an unlimited locked-memory limit for the user and for `slurmd`'s steps | `slurm-setup.sh` | read what a step gets (`srun ... bash -c 'ulimit -l; ulimit -Hl'`); `shmem_init` raises the soft limit to the hard one, and on Linux 6.14 and later a ring the hard limit refuses ends the job by name; ask for more if a run meets it |
| interrupt coalescing and busy polling (`ethtool -C`, `sysctl -w`) | `tuning-node.sh`, §15 | not run: not asked here |
| `perf` by `sudo -n` | `profile.sh`, §13 and §21 | `perf` of the user's own processes needs `kernel.perf_event_paranoid` at 2 or below, and of their kernel's share 1 or below; at Debian's 3 there is no profile, and the counters `osoe-stat` reads stand for it |
| network namespaces for tier 3 | `tools/launch/tier3.sh`; `verify.sh`'s `tier3-single` | not run: real nodes stand for them; leave `tier3-single` out of `verify.sh`'s parts |
| the neighbour table's settings, raised for a run | `docs/decisions/2026-10-19.md`, H23's runs | the site's to set (question 2) |

And what needs no root but assumes machines of the kit's own:

- **The same path on every node.** `build.sh` and `sos.sh` copy the tree and
  the SOS programs to every node by `ssh` and `rsync`. On a shared file system,
  which a site usually has, build there once and run nothing that copies; where
  the site refuses `ssh` to a node the user holds no job on, run each node's
  part as a step instead. Build where the nodes' own libraries are - on a
  compute node, or a login node of the same distribution - since the library
  links the liburing it was built against.
- **An allocation a job.** On a busy site each of the kit's jobs may wait in
  the queue; the kit's sections then take as long as the queue does. Running
  the kit inside one allocation of the nodes needs `launch` and `beside` to
  start a step of that allocation rather than an allocation of their own -
  a small change to `common.sh`, its diff in the file.
- **Ports.** Every node must reach every other on osoe's rendezvous ports
  (`OSOE_PORT_RANGE`, 17000 to 26999 by default) and on the mesh's - any port
  the kernel chooses, or `OSOE_MESH_PORT_RANGE`'s, for a site whose firewall
  opens ports by range.
- **The facts.** `facts.sh` reads what any user may and says why for any fact
  it cannot; add what this brief asks of it - `gc_thresh1` to `3`,
  `vm.max_map_count`, the hard limits (`ulimit -Hn`, `-Hl`) and
  `/proc/net/stat/arp_cache` - by a small change with its diff in the file, or
  by hand beside it.

## How to work

- On a branch of your own, `off-network-cluster`, from the commit you were
  given; commit as you go, each message one sentence of what and why, in `git
  log`'s style. There is no network to push to.
- Names are words (`CLAUDE.md`); a change to the library comes with a test that
  fails without it, and the README stays true of the tree at every commit.
- A protocol, a default or a decision in force is the author's: describe the
  change and the evidence for it as a question, and do not make it. A protocol
  the project changes it models first (`tools/modelcheck/README.md`), which is
  not this session's to start.
- A ceiling met is a result, not a failure to work round: record its line, then
  go past it only by the setting the line names, and say so.
- A run killed by its timeout leaves names in `/dev/shm`: the kit's `launch`
  removes them with `osoe-stat --gc` on every node; by hand, the same.
- The site's nodes are shared with other users: whole nodes for every timed job
  (`--exclusive`), and nothing timed beside another of your own.

## What comes back: the return contract

One file, `docs/prompts/off-network-cluster-response.md`, written in the tree
and carried out alone. Assume nothing else comes back - no `out/` directory, no
log, no commit, no attachment - so the file must stand alone for a reader who
has the tree but not the cluster. Keep it current as the session goes, each
section written as its runs finish, so that a session that ends early still
leaves the account.

- **Markdown only**, plain text: tables, lists and code blocks; no images, no
  links to files that do not come back, no "see the log". Where a raw line
  matters - a refusal, a fatal, a notice - quote that line, word for word.
- **Self-contained tables**: each says what it measured and in what unit, the
  commit, the nodes and PEs a node, the helpers, every setting that differed
  from the defaults, the rounds, and the median with the spread. Numbers from
  this cluster beside the eight Google Cloud nodes' or the model's, where one
  exists, each labelled.
- **Names and paths**: write the home directory as `~` and the tree as
  `<repo>`, the site's nodes by a pattern such as `node[001-512]`, and no
  account's name.
- **Code changes**: each small - a few dozen lines - with its reason, its diff
  inline in a `diff` block, the test that shows it with its command and count,
  and whether it changes the wire, a layout or a default; a larger change is
  described and proposed, not made.

The file, in this order:

1. **In short**: what ran, what did not and why, and the answer to each of the
   seven questions in a sentence or two.
2. **The cluster**: one table of the facts - the nodes and their count, the
   CPU, cores and memory a node, `/dev/shm`'s size, the kernel, liburing's
   version, the interface and its speed and MTU, the network's shape (one
   subnet or routed), Slurm's version, its task plugin and how it binds, the
   limits a step sees (`ulimit -l`, `-Hl`, `-n`, `-Hn`), and the settings
   `facts.sh` reads with those above.
3. **The tree**: the commit given, the branch's commits, and every change as a
   diff.
4. **Each question**, one section each: what was run - the command lines and
   the settings - the table, what it shows, against what, and what it does not
   show.
5. **The ceilings met**: a table of each ceiling the runs reached, the value
   reached, what happened - the line, word for word - and what got past it, if
   anything did.
6. **Failures**: every job that failed or hung, its command, its first error
   line and the PE and node that printed it, whether it happened again, and
   where it hung `osoe-stat --summary`'s view.
7. **Findings**: anything stale, wrong or missing in the README, the design
   notes, `docs/ceilings.md`, the kit or this brief, each with its file and the
   words that are wrong.
8. **Questions for the author**: each with what it is about, the evidence, the
   options and one recommendation, so that it can be answered from the file
   alone.
9. **What was not done**, and what the next session should run first.
