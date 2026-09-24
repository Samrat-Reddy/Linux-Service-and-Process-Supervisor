# Testing and Demonstration

## 1. Automated tests

```
make
./tests/run_tests.sh
```

Expected output:

```
=== Warden functional tests ===

[1] termination detection and restart policy
  PASS  T1 normal exit detected
  PASS  T2 failure exit code decoded
  PASS  T3 signal death decoded
  PASS  T4 policy=onfailure honoured
  PASS  T5 policy=always restarts

[2] exponential backoff and restart-storm cap
  PASS  T6 backoff grows 1s then 2s
  PASS  T7 backoff reaches 8s

[3] control channel
  PASS  T8 status over FIFO
  PASS  T9 stop suppresses restart
  PASS  T10 start command works

[4] SIGKILL escalation on shutdown
  PASS  T11 SIGKILL after grace period

[5] memory limit enforcement
  PASS  T12 soft RSS limit enforced

[6] zombie prevention
  PASS  T13 no zombie processes

[7] error handling
  PASS  T14 missing config fails cleanly

=== 14 passed, 0 failed ===
```

Takes about 70 seconds, most of it waiting for the backoff sequence.

## 2. Test case table

| # | Test | Method | Expected result | CO |
|---|------|--------|-----------------|-----|
| T1 | Normal exit detected | service exits 0 | log records `exited status=0` | 2 |
| T2 | Failure exit code decoded | service exits 3 | log records `exited status=3` | 2 |
| T3 | Signal death decoded | service dereferences NULL | log records `killed by signal 11 (Segmentation fault)` | 2 |
| T4 | `onfailure` policy | clean exit, policy `onfailure` | not restarted | 2 |
| T5 | `always` policy | clean exit, policy `always` | restarted | 2 |
| T6 | Exponential backoff | service exits immediately, repeatedly | delays 1s, 2s, 4s, 8s, 16s | 2 |
| T7 | Restart-storm cap | same, continued | state becomes `failed`, restarts stop | 2 |
| T8 | Status over FIFO | `wardenctl status` | table of services with PID, CPU, RSS | 3 |
| T9 | Stop suppresses restart | `wardenctl stop web` | SIGTERM sent, service stays stopped | 3 |
| T10 | Start command | `wardenctl start web` | service launches again | 3 |
| T11 | SIGKILL escalation | service ignores SIGTERM, shutdown requested | SIGKILL after 5s grace | 3 |
| T12 | Soft memory limit | service leaks memory, 32MB limit | terminated at 80% of limit | 4 |
| T13 | Hard memory limit | same, inspect service log | `malloc refused ... (rlimit reached)` | 4 |
| T14 | Zombie prevention | services restart repeatedly | `ps` shows no `Z` state processes | 2 |
| T15 | Missing config | `./warden -c /tmp/nothere.conf` | error message, exit status 1 | 5 |
| T16 | Malformed config line | line with name but no command | line skipped, others still start | 5 |
| T17 | Nonexistent command | config points at missing binary | child exits 127, `exec failed` in service log | 2 |
| T18 | Daemon detaches | `./warden -d`, then `ps -eo pid,ppid,tty` | PPID 1, no controlling terminal | 1 |
| T19 | Demand paging | `./cowdemo 32` | ~8192 minor faults when touching 32MB | 4 |
| T20 | Copy-on-write | same run | 0 faults on child read, ~8192 on child write | 4 |

## 3. Manual demonstration — about eight minutes

### Step 1 — Build

```
make
```

Show the build is warning-free. Compile live rather than using a prebuilt
binary.

### Step 2 — Start in the foreground

```
cat warden.conf
./warden -c warden.conf
```

Point out in the scrolling log:

- each service starting with its PID
- `batch` exiting cleanly and **not** being restarted (`policy=onfailure`)
- `worker` exiting with status 3 and being restarted
- `crasher` dying from signal 11 and being restarted
- the backoff delay growing: 1s, then 2s, then 4s

### Step 3 — Control channel, from a second terminal

```
./wardenctl status
./wardenctl stop web
./wardenctl status        # web now shows 'stopped', not restarted
./wardenctl start web
./wardenctl restart web
```

### Step 4 — Restart storm

```
echo 'flapper always 0 ./testsvc flap' > /tmp/storm.conf
./warden -c /tmp/storm.conf -p /tmp/storm.cmd
```

Let it run about 35 seconds. The backoff climbs to 16s and then:

```
[flapper] RESTART STORM: 5 consecutive failures without staying up 30s, giving up
```

This is the point worth dwelling on — it is what separates a supervisor from a
`while true; do ...; done` loop.

### Step 5 — Zombie prevention

```
ps -eo pid,ppid,stat,comm | grep testsvc
```

While services are restarting repeatedly, no process is in `Z` state. Explain
that every child is collected with `waitpid()`, so the kernel releases its
process-table entry immediately.

To show the contrast, the Phase 1 snapshot can be modified to skip `waitpid()`;
zombies then appear within seconds.

### Step 6 — Memory limits

```
echo 'hungry always 32 ./testsvc leak' > /tmp/mem.conf
./warden -c /tmp/mem.conf -p /tmp/mem.cmd
```

After about eight seconds:

```
[hungry] RSS 30436kB exceeds soft limit (80% of 32MB), terminating
```

Then show the service's own log:

```
cat logs/hungry.log
```

which contains `malloc refused after 28MB (rlimit reached)` — the kernel's hard
limit working independently of the supervisor's polling.

### Step 7 — Demand paging and copy-on-write

```
./cowdemo 32
```

```
after malloc (untouched)   : 1 minor faults
after touching every page  : 8192 minor faults
child reading all pages    : 0 minor faults
child writing all pages    : 8193 minor faults
```

Four numbers, four concepts: address space reserved without mapping, pages
supplied on demand, pages shared after `fork()`, and private copies made on
write.

### Step 8 — Daemon mode

```
./warden -c warden.conf -d
ps -eo pid,ppid,sid,tty,comm | grep warden
```

PPID is 1, TTY is `?`, and the session ID is its own — the process has detached
from the terminal. Administer it with `wardenctl`, then:

```
./wardenctl shutdown
cat logs/warden.log
```

### Step 9 — System call tracing

```
strace -f -e trace=clone,execve,wait4,write ./warden -c warden.conf 2>&1 | head -40
```

Shows `clone()` (what glibc's `fork()` invokes), `execve()` replacing each
child's image, and `wait4()` (the `waitpid()` backend) returning status words.

## 4. Memory checking

```
valgrind --leak-check=full ./warden -c warden.conf
```

Run for a few seconds, then `./wardenctl shutdown` from another terminal so the
supervisor exits cleanly and Valgrind can report. Install with
`sudo apt install valgrind` if not present.

## 5. Evidence to capture for the report

- Terminal output from Step 2 showing all three termination types
- The restart-storm message from Step 4
- `ps` output from Step 5 showing no zombies
- Both memory-limit messages from Step 6
- The `cowdemo` output from Step 7
- `ps` output from Step 8 showing PPID 1
- A few lines of `strace` output from Step 9
- The full `./tests/run_tests.sh` result
