# Design Notes

## 1. Architecture

Four threads share one service table.

```
                        ┌──────────────────────┐
        SIGCHLD ──────► │ signal handler       │
        SIGTERM ──────► │ write() one byte     │
                        └──────────┬───────────┘
                                   │ self-pipe
                                   ▼
  ┌────────────┐   ┌──────────────────────┐   ┌────────────────┐
  │ health     │   │ reaper thread        │   │ control thread │
  │ thread     │   │ waitpid(WNOHANG)     │   │ reads FIFO     │
  │ reads      │   │ applies restart      │   │ start/stop/    │
  │ /proc      │   │ policy               │   │ status         │
  └─────┬──────┘   └──────────┬───────────┘   └────────┬───────┘
        │                     │                        │
        └─────────────────────┼────────────────────────┘
                              ▼
                   ┌──────────────────────┐
                   │ services[]           │  guarded by table_lock
                   │ shared service table │  restart_cv signals work
                   └──────────┬───────────┘
                              ▼
                   ┌──────────────────────┐
                   │ main thread          │
                   │ restart manager      │
                   │ cond_timedwait       │
                   └──────────────────────┘
```

| Thread  | Blocks on                     | Responsibility                       |
|---------|-------------------------------|--------------------------------------|
| main    | `pthread_cond_timedwait`      | restarts services whose backoff expired |
| reaper  | `poll()` on the self-pipe     | collects dead children, applies policy  |
| health  | timed sleep                   | samples `/proc` every 2 seconds         |
| control | `poll()` on the command FIFO  | serves administrator commands           |

## 2. Why a self-pipe

`SIGCHLD` arrives asynchronously and can interrupt the supervisor at any
instruction, including halfway through updating the service table. A signal
handler may only call async-signal-safe functions — it cannot lock a mutex,
call `malloc()`, or use `printf()`. Doing the real work inside the handler
would either deadlock or corrupt state.

The handler therefore does exactly one thing: `write()` a single byte into a
pipe. `write()` is async-signal-safe. The reaper thread polls the read end of
that pipe and performs the actual reaping with ordinary locking, in normal
thread context. This converts an asynchronous signal into a synchronously
readable event.

Two further details matter:

- The write end is **non-blocking**, so a burst of signals that fills the pipe
  cannot stall the handler.
- `waitpid()` is called in a **`WNOHANG` loop**, not once per signal. Standard
  signals are not queued: if three children die simultaneously, the kernel may
  deliver a single `SIGCHLD`. Looping until `waitpid()` returns 0 or `ECHILD`
  guarantees no child is missed.

## 3. Locking discipline

Two locks and one semaphore:

| Primitive       | Protects                                |
|-----------------|------------------------------------------|
| `table_lock`    | the entire `services[]` array            |
| `log_lock`      | log file writes (inside `log.c`)         |
| `restart_slots` | bounds concurrent spawn operations to 2  |

**Lock ordering is `restart_slots` → `table_lock` → `log_lock`, never the
reverse.** This is what prevents deadlock. `service_request_start()` acquires
the semaphore *before* the mutex; a thread holding the mutex never waits on
the semaphore.

The restart manager therefore cannot simply start services while scanning the
table. It collects the due services under the lock, marks them so no other
thread claims the same one, releases the lock, and only then starts them.

**The race the semaphore addresses:** both the main thread (automatic restart)
and the control thread (administrator `start` command) can spawn services
concurrently. `fork()` under memory pressure is the expensive operation, so
the counting semaphore bounds how many may be in flight at once.

**The race `table_lock` addresses:** the health thread reads `s->pid` to sample
`/proc` while the reaper thread may be clearing that same field after the
process died. Without the lock, the health thread could read a PID that has
been recycled by the kernel for an unrelated process and sample the wrong one.

## 4. Restart policy

On child termination:

```
exited cleanly (status 0)  +  policy=onfailure  →  stop, do not restart
any termination            +  policy=never      →  stop, do not restart
manual stop requested                           →  stop, do not restart
shutting down                                   →  stop, do not restart
otherwise                                       →  schedule restart
```

Scheduled restarts use exponential backoff: 1s, 2s, 4s, 8s, 16s, capped at 16s.
After five consecutive failures without the service staying up for 30 seconds,
it is marked `failed` and no longer restarted. A service that does stay up for
30 seconds has its counter and backoff reset.

This is what prevents a restart storm. Without it, a service that crashes
immediately on a bad configuration would be relaunched thousands of times per
minute, exhausting the process table and filling the disk with logs — the
recovery mechanism becoming a denial-of-service attack on its own system.

## 5. Memory limits: two layers

| Layer | Mechanism | Behaviour |
|-------|-----------|-----------|
| Hard  | `setrlimit(RLIMIT_AS)` before `exec()` | kernel refuses allocations beyond the limit; `malloc()` returns NULL inside the service |
| Soft  | RSS sampled from `/proc`, 80% of the limit | supervisor sends `SIGTERM` and lets the restart policy handle recovery |

The soft limit acts first in practice, because it is based on memory actually
resident rather than address space reserved. The hard limit is the backstop for
a service that allocates faster than the 2-second sampling interval.

## 6. Known limitations

Worth stating rather than hiding:

- **A failed `exec()` is reported as exit status 127**, which a service could
  also return deliberately. A close-on-exec pipe from child to parent would
  distinguish them properly.
- **Configuration is read once at startup.** Reloading on `SIGHUP` is a natural
  extension.
- **`restart` in the control thread polls** for the old process to be reaped
  rather than waiting on a condition variable. Correct, but not elegant.
- **The service table is a fixed array of 16.** Dynamic allocation would remove
  the limit at the cost of more locking around resizes.

## 7. Course outcome mapping

| CO | Covered by |
|----|------------|
| **CO-1** User/kernel boundary, system calls, the shell's role | The supervisor performs the same `fork`/`exec`/`wait` sequence a shell does; `strace -f` shows every transition into kernel services |
| **CO-2** Process abstraction, lifecycle, creation, termination | `fork()`, `execvp()`, `waitpid()`, exit-status decoding with `WIFEXITED`/`WIFSIGNALED`, zombie prevention, orphan behaviour, `setpgid()` |
| **CO-3** IPC, pipes, FIFOs, signals, process groups | Self-pipe (anonymous pipe), control channel and reply channel (named FIFOs), `sigaction()` handlers, `SIGTERM`/`SIGKILL`/`SIGCHLD`, own process group per service |
| **CO-4** Virtual memory, demand paging, copy-on-write | `cowdemo` measures demand paging and COW via minor fault counts; `setrlimit(RLIMIT_AS)`; RSS and fault counters sampled from `/proc` |
| **CO-5** File abstraction, descriptors, file I/O | Configuration and log file I/O, `dup2()` redirection of service output, descriptor inheritance across `exec()`, `/proc` read through the ordinary file API |
| **CO-6** Threads, race conditions, mutexes, condition variables, semaphores | Four threads over shared state, `pthread_mutex_t` on the service table, `pthread_cond_t` for restart scheduling, counting semaphore bounding concurrent spawns, documented lock ordering |
