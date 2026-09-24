# Linux Service and Process Supervisor

A program that keeps background services running. It starts the services
listed in a configuration file, detects when one of them stops, reports how it
stopped, and starts it again if it should be restarted.

Operating Systems and Systems Programming course project.

---

## Build and run

```
make
./warden services.conf
```

While it runs, type commands:

```
status            show all services
stop web          stop a service
start web         start it again
quit              shut everything down
```

---

## Configuration

`services.conf`, one service per line:

```
<name>  <restart?>  <command>  [arguments...]
```

- `name` — the name used in commands and log messages
- `restart?` — `yes` to restart it automatically, `no` to leave it stopped
- the rest is the program to run and its arguments

Lines starting with `#` are comments.

---

## What the program does

1. Reads the configuration file.
2. Starts each service with `fork()` and `execvp()`.
3. Waits for a `SIGCHLD` signal, which the kernel sends when a child stops.
4. Collects the child with `waitpid()` and works out whether it exited
   normally or was killed by a signal.
5. Restarts it after a short delay if its policy says so, giving up after
   five failures in a row.
6. Reads `/proc` to show each running service's state and memory use.

---

## Operating system concepts used

| Concept | Where |
|---------|-------|
| Process creation | `fork()` in `start_service()` |
| Program execution | `execvp()` in the child |
| Process termination | `waitpid()` in `reap_children()` |
| Exit status decoding | `WIFEXITED`, `WEXITSTATUS`, `WIFSIGNALED`, `WTERMSIG` |
| Zombie prevention | every child is collected with `waitpid()` |
| Signals | `SIGCHLD` handler, `SIGTERM` to stop services, `SIGKILL` as a last resort |
| Signal safety | the handler only sets a `volatile sig_atomic_t` flag |
| Kernel process information | reading `/proc/<pid>/stat` |
| File I/O | configuration file and `warden.log` |
| Error handling | return values checked, `errno` reported with `strerror()` |

---

## Files

```
warden.c        the supervisor (about 400 lines)
testsvc.c       a dummy service used for testing
services.conf   example configuration
Makefile        build instructions
warden.log      activity log, created when the program runs
```

---

## Testing

`testsvc` can be told how to behave, which makes each case easy to trigger:

| Mode | Behaviour | Tests |
|------|-----------|-------|
| `ok` | runs, then exits normally | normal exit detection |
| `fail` | runs, then exits with status 3 | failure detection and restart |
| `crash` | runs, then causes a segmentation fault | signal death detection |
| `long` | keeps running until stopped | the `stop` and `start` commands |

The supplied `services.conf` uses all four, so a single run exercises
everything.
