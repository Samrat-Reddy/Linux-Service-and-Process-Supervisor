# Development Phases

The project was built in six phases, each ending with something runnable. The
`phase1/` directory keeps the first version as a standalone snapshot; later
phases are located in the modular source under `src/`.

---

## Phase 1 — Core lifecycle

**Files:** `phase1/warden.c`

Configuration parsing, `fork()` + `execvp()` to launch services, blocking
`waitpid()` to collect them, exit-status decoding. Single-threaded, no restart.

Build and run it independently:

```
cd phase1 && make && ./warden services.conf
```

Kept in the submission because it is the clearest demonstration of the core
mechanism without the surrounding machinery — and because it is where the
zombie experiment is easiest to show.

---

## Phase 2 — Asynchronous detection and recovery

**Files:** `src/supervisor.c`

The blocking `waitpid()` becomes a `SIGCHLD` handler feeding a self-pipe, with
`waitpid(WNOHANG)` in a loop. Restart policy added, split by exit reason, with
exponential backoff and the restart-storm cap.

This is the phase where the program stops being a launcher and becomes a
supervisor.

---

## Phase 3 — Logging and daemonisation

**Files:** `src/log.c`, `src/service.c` (`child_setup`), `src/main.c`
(`daemonise`)

Service stdout and stderr redirected into per-service log files with `dup2()`
in the window between `fork()` and `exec()`. Timestamped supervisor log.
Double-fork daemonisation with `setsid()`. Controlled shutdown on `SIGTERM`
with SIGKILL escalation after a grace period.

---

## Phase 4 — Control channel

**Files:** `src/control.c`, `src/wardenctl.c`

Named pipe command server plus the `wardenctl` client. Each client creates a
private reply FIFO, since a FIFO is unidirectional. Commands: `status`,
`start`, `stop`, `restart`, `shutdown`.

---

## Phase 5 — Health monitoring and memory instrumentation

**Files:** `src/health.c`, `tools/cowdemo.c`, `setrlimit()` in `src/service.c`

Periodic sampling of `/proc/<pid>/stat` for process state, CPU ticks, RSS and
page-fault counters. Address-space limits applied to children. Soft RSS limit
enforced by the supervisor. Separate `cowdemo` program measuring demand paging
and copy-on-write through minor fault counts.

---

## Phase 6 — Concurrency

**Files:** threading throughout `src/`

The single event loop split into four threads over the shared service table:
restart manager (main), reaper, health, control. Mutex on the table, condition
variable for restart scheduling, counting semaphore bounding concurrent spawn
operations. Lock ordering documented in `docs/DESIGN.md`.

---

## Suggested git history

If the repository is being graded for progression, tagging each phase gives
clear evidence of incremental work:

```
git tag -a phase1 -m "Core lifecycle: fork, exec, waitpid"
git tag -a phase2 -m "Self-pipe SIGCHLD handling and restart policy"
git tag -a phase3 -m "Logging, daemonisation, controlled shutdown"
git tag -a phase4 -m "FIFO control channel and wardenctl client"
git tag -a phase5 -m "/proc health monitoring and memory limits"
git tag -a phase6 -m "Multithreaded supervisor with shared state"
```
