# Warden — A Lightweight Service Supervisor for Linux

Process lifecycle management, failure detection, and automatic recovery using
POSIX system programming interfaces.

Operating Systems and Systems Programming course project.

---

## What it does

Warden keeps a set of configured background services running. It launches each
one, detects when it dies, works out *why* it died, and restarts it according
to a policy — with exponential backoff and a cap that stops a permanently
broken service from being restarted forever. It samples each service's CPU,
memory and page-fault counters from `/proc`, and accepts administrator
commands over a named pipe while running detached as a daemon.

Everything runs in user space. No kernel modification.

---

## Build

```
make
```

Produces four binaries:

| Binary      | Purpose                                            |
|-------------|----------------------------------------------------|
| `warden`    | the supervisor daemon                              |
| `wardenctl` | administrative control client                      |
| `testsvc`   | configurable dummy service used for testing        |
| `cowdemo`   | demand-paging and copy-on-write measurement        |

Requires only `gcc`, `make` and glibc. Tested on Ubuntu 24.04.

---

## Run

```
./warden -c warden.conf          # foreground, logs to terminal
./warden -c warden.conf -d       # background daemon
```

Options:

```
-c FILE   service configuration      (default warden.conf)
-l FILE   supervisor log file        (default logs/warden.log)
-p PATH   control FIFO path          (default /tmp/warden.cmd)
-d        detach and run as a daemon
```

Control it from another terminal:

```
./wardenctl status
./wardenctl stop web
./wardenctl start web
./wardenctl restart web
./wardenctl shutdown
```

Example status output:

```
SERVICE      STATE    PID      CPU%    RSS(kB)  RESTARTS FAULTS(min/maj)
web          running  424      0.0     1636     0        91/0
worker       backoff  -        0.0     0        3        0/0
crasher      failed   -        0.0     0        5        0/0
```

---

## Configuration

`warden.conf`, one service per line:

```
<name>  <policy>  <mem_mb>  <command>  [arguments...]
```

| Field    | Meaning                                                          |
|----------|------------------------------------------------------------------|
| `name`   | logical service name used in commands and logs                   |
| `policy` | `always`, `onfailure` (only on non-zero exit or signal), `never` |
| `mem_mb` | address-space limit via `setrlimit()`; `0` means unlimited        |
| command  | program and arguments, passed to `execvp()`                      |

Blank lines and `#` comments are ignored. A malformed line is reported and
skipped rather than aborting startup.

---

## Testing

```
./tests/run_tests.sh
```

Runs 14 functional tests covering termination detection, all three restart
policies, exponential backoff, the control channel, SIGKILL escalation, memory
limit enforcement, zombie prevention, and error handling. Expected result:

```
=== 14 passed, 0 failed ===
```

See `docs/TESTING.md` for the manual demonstration procedure.

---

## Source layout

```
src/warden.h        shared service table and module interfaces
src/main.c          option parsing, daemonisation, thread startup
src/config.c        configuration file parser
src/service.c       fork/exec, log redirection, resource limits
src/supervisor.c    self-pipe signal handling, reaping, restart policy
src/health.c        /proc sampling: CPU, RSS, page faults
src/control.c       FIFO command server
src/log.c           thread-safe timestamped logging
src/wardenctl.c     control client

tools/testsvc.c     dummy service with selectable failure modes
tools/cowdemo.c     demand paging and copy-on-write measurement

phase1/             Phase 1 snapshot, kept to show build progression
tests/run_tests.sh  automated functional tests
docs/               design notes, phase breakdown, test procedure
```

---

## Documentation

- `docs/DESIGN.md` — architecture, threading model, locking discipline
- `docs/PHASES.md` — what each development phase added
- `docs/TESTING.md` — test cases and demonstration script
