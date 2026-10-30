# B10's evidence (2026-10-21): `RemoveIPC=no` and lingering, tried

The item (`docs/prompts/v0.2-open-items.md`, B10): the README's "Limits", "Launching" says that
systemd-logind deletes a user's files in `/dev/shm` about ten seconds after the user's last login
session ends, that a PE still in `shmem_init` may need a name there, and that `RemoveIPC=no` in
`logind.conf` or lingering (`loginctl enable-linger`) "should prevent it; neither has been tried
here". The author allowed, in the development VM only, a throwaway account and a restart of logind
(`docs/decisions/2026-10-20.md`, "The author's answers", item 8). A measurement: no library code,
test or design note is changed.

## What it found

**Both remedies keep a starting job's names. Without either, logind removes them ten seconds after
the user's last login session ends, and a PE that has not yet opened them never finds them.** In the
development VM (Debian 13, systemd and logind 257.13), a job of four PEs, one of them calling
`shmem_init` 25 s late, was started right after its user's only login session had ended. At logind's
defaults the job's names left `/dev/shm` 10.2 to 10.3 s after the session ended, when logind stopped
the user's manager, in all four such runs. The three PEs that had called `shmem_init` waited for the
fourth, which found no name when it came at 25 s, and no PE was through `shmem_init` when the
launcher's 40 s timeout ended the job. With lingering enabled for the user, and with `RemoveIPC=no`
in `/etc/systemd/logind.conf` and logind restarted, the same job had every PE through at 25 s and
finished, in both runs of each.

Two cases besides, which show where the removal reaches:

- **A job the login itself started, in the background, was not caught at the defaults.** Its
  processes keep the login's session in logind's list, closing, until they are gone; the user is not
  removed, and the names stay, for as long as the job lives (`class-*/a-login/user-state.txt`). A
  login held open for the whole job (`d-held`) is no different. So the removal catches a start that
  the ended login did not itself start - Slurm's, which is the Pi cluster's case - and only while
  the start is slow.
- **A job past its start was not touched.** The four PEs of `running/f-running` were through
  `shmem_init` within a millisecond, no sample at 0.2 s found a name of theirs, and logind removed the
  user at 10.1 s with the job holding; the job finished. A running job has nothing in `/dev/shm` for
  the removal to take, as the README says.

Confidence: high for this machine and this systemd - the baseline stalled in all four of its runs,
and each remedy completed in both of its. Not tried: the Pi cluster itself (Raspberry Pi OS, whose
systemd may not be this one), a login by sshd in place of `su` (below), a start slow for another
reason than a late PE, `UserStopDelaySec` at other than its 10 s.

## What ran

The development VM `osoe` (aarch64, 4 CPUs, Debian 13 "trixie", Linux 6.12.95, `systemd 257
(257.13-1~deb13u1)`, `class-background/environment.txt`), alone on it under `/tmp/osoe-vm.lock` for
each of three invocations (3.7, 3.7 and 0.8 minutes; `experiment.log` of each says no other work was
running at its start), the library of `main` at `efcf1fa5` built with the release preset's
configuration, 0 warnings - only `osoe`, `osoe-launch` and `osoe-stat` were built, `build/`. Logind's
configuration was the shipped one: `/etc/systemd/logind.conf` with every line commented (sha256
`a0f5aec3...`) and one drop-in, unattended-upgrades', `InhibitDelayMaxSec=30`; read from logind over
D-Bus, `RemoveIPC` true, `UserStopDelaySec` 10 s, `KillUserProcesses` off.

- **The account**: `osoe-b10`, made by `useradd --create-home --shell /bin/bash`, uid 1000, no
  password and no key, removed by `userdel --remove` at the end of each invocation.
- **A login session** is `su -l osoe-b10 -c login-body.sh`, run by the experiment's own transient
  service (inside another login session, `pam_systemd` opens none). Its PAM stack opened a session
  that logind lists (`check/login.txt`: `c7 1000 osoe-b10 ... background`), with the account's
  `systemd --user` beside it as a `manager` session. A `su` session is of class `background`; the
  second series set `XDG_SESSION_CLASS=user`, the class a login by ssh would have (not measured
  here), and got the same results. The session is held two seconds and ends.
- **The job** is `osoe-launch -n 4 -t 40` of `late-pe.c`: rank 1 sleeps 25 s before `shmem_init`
  (`-e 1:B10_DELAY_SECONDS=25`), the others call it at once and wait inside it, the control segment
  and their heaps in `/dev/shm`; then a barrier and a ring of puts, checked. A start is normally over
  in a millisecond, so the late PE stands for a slow start such as job 828's on the Pi
  (`docs/decisions/2026-09-27-night.md`, item 6).
- **How a job is started.** `service`: right after the login has ended, as a transient service of the
  account (`systemd-run --uid`), outside the login, as Slurm's daemon starts one. `login`: from the
  login, detached (`setsid nohup`), the login ending two seconds later. `held`: in the login, which
  lasts as long as the job.
- **The configurations.** As shipped. Lingering enabled (`loginctl enable-linger`, afterwards
  `disable-linger`). `RemoveIPC=no`: the shipped `#RemoveIPC=yes` line of `logind.conf` made
  `RemoveIPC=no`, `systemctl restart systemd-logind`, the property read back as false; afterwards the
  file restored by `cp -a` from a copy, its checksum compared, logind restarted.
- Each run samples, every 0.2 s, the account's names in `/dev/shm` (`names.txt`) and logind's state of
  the account (`user-state.txt`), and keeps the journal's logind, user-manager and unit lines
  (`journal.txt`).

The runs, in the order each series ran them (`summary.tsv`). Times are seconds after the login
session ended, except `d-held`'s, after the job's start; `a-login`'s job started 2.0 s before its
login ended. "Completed" is exit 0 with four PEs through `shmem_init` and four rings ok.

| Run | logind | Names left `/dev/shm`, series 1 / 2 | Late PE calls `shmem_init` | Result, both series |
|---|---|---|---|---|
| `a-service` | as shipped | +10.2 / +10.3, by logind | +25.0 | **stalled**: 0 of 4 PEs through; ended by the launcher's timeout, exit 1 |
| `a-login` | as shipped | +23.0 / +23.1, by osoe | +23.0 | completed |
| `d-held` | as shipped, the login held open | +25.1 / +25.2, by osoe | +25.0 | completed |
| `b-service` | lingering enabled | +25.1 / +25.1, by osoe | +25.0 | completed |
| `c-service` | `RemoveIPC=no`, logind restarted | +25.2 / +25.1, by osoe | +25.0 | completed |
| `e-service` | as shipped again, `logind.conf` restored and logind restarted | +10.3 / +10.3, by logind | +25.0 / +25.1 | **stalled** |
| `f-running` (`running/`, class user) | as shipped | none sampled, by osoe | +0.0 (no PE late; every PE holds 30 s) | completed |

Who removed the names is read from the time. Logind's removal comes ten seconds after the session
ended, before the late PE calls `shmem_init`; osoe removes its own names once every PE is through, so
a removal a tenth of a second after the late PE's call is osoe's.

## A stalled run

`class-user/a-service` (the other three alike), in seconds after the login's session scope ended, at
01:22:47.30 by the VM's clock:

| s | |
|---|---|
| 0.00 | `session-c3.scope: Deactivated successfully`; `Removed session c3` (`journal.txt`) |
| 0.01 | the job is started (`Started b10-job-a-service.service`) |
| 0.11 | the node's names appear: the control segment, and the heaps and statics of the three PEs that have called `shmem_init` (`names.txt`) |
| 10.09 | `Stopping user@1000.service`; the user manager's session 23 is removed at 10.14 |
| 10.26 | the names are gone; logind lists the account no longer at 10.34 (`user-state.txt`) |
| 25.03 | rank 1 calls `shmem_init` (`job.err`) |
| about 30 | the lead's notice: "has waited 30 s for local PEs to join the node (call shmem_init): 3 of 4 have; still waiting for local id 1 (rank 1)" |
| about 40 | `osoe-launch: timeout after 40 s`; `osoe-stat --summary` shows rank 0 in `shmem_init`, waiting for PE 1 |

This is the Pi's probe of 2026-09-27 (`docs/design/evidence/pi-cluster/2026-09-27/agent-placement/removeipc-hang/probe.txt`):
the names go when the user manager stops, ten seconds after the last session. The late PE's wait has
no end of its own - no wait gives up by default, and `OSOE_NO_PROGRESS_TIMEOUT` was not set - so the
launcher's timeout ended it, fifteen seconds in.

## The job the login started

`class-*/a-login`: the job began inside the login's scope, and the login ended two seconds later.
`user-state.txt` reads `state=closing sessions=2` from then until the job ended, 23 s on - the login's
session, with the job's processes in it, and the user manager's - and `sessions=1` after;
`journal.txt` has `session-c9.scope: Deactivated successfully` when the job ended. Nothing was removed
while the job lived. That holds as long as logind leaves a logged-out session's processes alone
(`KillUserProcesses` off, as shipped here).

## Undone

Each invocation's trap undid it (the `undo` of `experiment.sh`; `verify-after-undo.txt` in each
directory): the account, its group and its home gone, lingering off, `logind.conf` back with the same
checksum, owner, mode, size and date, `RemoveIPC` true again in the running logind, `/etc`'s account
files with the checksums read before, nothing of the account's left in processes or files. After the
last invocation `experiment.sh cleanup` removed `/tmp/b10`, `/tmp/b10-out`, `/var/tmp/b10-saved` and
the journal file named below, and `confirm-undo.sh` compared the VM with what was read before anything
was changed: `undo-confirmed.txt`, all 28 checks ok. `running/saved/` holds the last invocation's
copies.

Found on the way:

- **Restarting logind started the manager of the VM's own account.** Logind starts the manager
  (`user@501.service`) of every lingering user when it starts. `<user>` lingers (since the VM was
  made, 2026-09-19), but its manager had been dead since 2026-10-07 00:49:30 (`Main process exited,
  code=killed, status=9/KILL`), so the restart brought it back, with rootless containerd, buildkitd
  and stargz-snapshotter under it: 220 MB in an 8 GiB VM that sanitizer jobs strain. It was stopped by
  hand between the first and second series (`systemctl stop user@501.service`); the script now stops
  any such manager that was not running when it began (`stop_started_user_managers`, logged in the
  second and third `experiment.log`). It reads `inactive` now where it had stood failed since
  2026-10-07; the next restart of logind, or a boot, starts it again.
- **Journald made `/var/log/journal/<machine-id>/user-1000.journal`** (8 MiB) for the account's uid.
  `cleanup` removed it (`running/verify-again.txt` lists it before; `undo-confirmed.txt` checks its
  absence).
- **The VM's own account lingers**, so its files in `/dev/shm` - the other sessions' jobs - are not
  subject to the removal here, with or without these runs.

## Which script ran

The scripts here are the last versions. Three invocations ran three versions, which differ in lines
added between them; the modes they share behave alike.

- **`class-background/`** (the first series, `run`) ran before `B10_RUNS` and `B10_SESSION_CLASS`,
  `stop_started_user_managers`, `cleanup`'s removal of the journal file, the `running` mode, and the
  optional hold in `late-pe.c` and `job.sh`. Its runs are in `/tmp/b10-out` itself.
- **`class-user/`** (the second, `run` with `B10_RUNS=/tmp/b10-out/user-class B10_SESSION_CLASS=user`)
  ran before the `running` mode and the hold. Its `summary.tsv` has no header line - the script wrote
  it before it had made the directory, the second line of `experiment.log` - and its columns are
  those of `class-background/summary.tsv`; the script makes the directory first now.
- **`running/`** (`running`, with the same two variables) ran the last version, which compiles
  `late-pe.c` again for the hold before the run.

`build.sh` built the tree, `login-body.sh` and `job.sh` are the account's side of a run, and
`experiment.sh` is the controller. They name `/tmp/b10` and `/tmp/b10-out` in the VM; the host's side
was

```sh
git -C <repo> archive --prefix=b10/ main | limactl shell osoe -- bash -c 'cd /tmp && rm -rf b10 && tar xf -'
# the files of this directory into the VM's /tmp/b10-out, then, as the VM's own account:
limactl shell osoe -- sh -c 'cd /tmp/b10 && setsid nohup bash /tmp/b10-out/build.sh > /tmp/b10-out/build.log 2>&1 < /dev/null &'
# and, for each invocation (this one the second series'):
limactl shell osoe -- sudo systemd-run --unit=b10-experiment --collect -p TimeoutStopSec=300 \
    -E B10_RUNS=/tmp/b10-out/user-class -E B10_SESSION_CLASS=user \
    -p StandardOutput=append:/tmp/b10-out/user-class.log -p StandardError=inherit \
    bash /tmp/b10-out/experiment.sh run
```

It records what ran and is not one to run as written (`docs/decisions/README.md`, "The tree names no
one's machine"). The lock file had to be opened for reading by root: `fs.protected_regular` refuses
root the creating open of a file another user made in `/tmp`, which `exec 9> lock` is. Two earlier
invocations failed before they changed anything - the first at that open, the second because
`preflight` ended on a `grep` that found nothing and so returned 1 - and are not among the three.

## Files

| | |
|---|---|
| `experiment.sh` | the controller: `check`, `run`, `running`, `undo`, `verify`, `cleanup`; its head says what each does |
| `login-body.sh`, `job.sh`, `late-pe.c` | the account's side: the login session's command, one job, the test program |
| `build.sh`, `build/` | the build under the lock; its configure and compile output, and status |
| `confirm-undo.sh`, `undo-confirmed.txt` | the VM against what was read before: 28 checks |
| `class-background/`, `class-user/` | the two series, each with `summary.tsv`, `environment.txt`, `experiment.log`, `verify-after-undo.txt`, and a directory a run (`check`, `smoke`, `a-service`, ...) holding `login.txt` (what logind made of the session), `configuration.txt`, `names.txt`, `user-state.txt`, `journal.txt`, `job.err` (the PEs' lines and the launcher's), `job.status`, `leftover-names.txt` |
| `running/` | the job past its start (`f-running`), the same files; `saved/`, the copies the undo restores from; `verify-again.txt`, the check made again after the last invocation, before `cleanup` |
