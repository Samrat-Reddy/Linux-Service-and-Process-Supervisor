#!/bin/bash
#
# run_tests.sh - automated functional tests for the Warden supervisor.
#
# Run from the project root:  ./tests/run_tests.sh
#
# Each test starts a supervisor with its own configuration and control FIFO,
# exercises one behaviour, then checks the log for the expected evidence.

set -u

FIFO=/tmp/warden-test.cmd
LOG=/tmp/warden-test.log
PASS=0
FAIL=0

cleanup() {
    ./wardenctl -p "$FIFO" shutdown >/dev/null 2>&1
    sleep 1
    pkill -f "warden -c /tmp/wtest" >/dev/null 2>&1
    rm -f /tmp/wtest-*.conf "$FIFO"
}
trap cleanup EXIT

check() {
    local name="$1" pattern="$2"
    if grep -qE "$pattern" "$LOG"; then
        printf '  PASS  %s\n' "$name"
        PASS=$((PASS + 1))
    else
        printf '  FAIL  %s  (no match for: %s)\n' "$name" "$pattern"
        FAIL=$((FAIL + 1))
    fi
}

start_warden() {
    rm -f "$LOG"
    ./warden -c "$1" -p "$FIFO" -l "$LOG" -d
    sleep 1
}

stop_warden() {
    ./wardenctl -p "$FIFO" shutdown >/dev/null 2>&1
    sleep 2
}

echo
echo "=== Warden functional tests ==="
echo

# ---------------------------------------------------------------- T1..T4
echo "[1] termination detection and restart policy"
cat > /tmp/wtest-1.conf <<'EOF'
clean      onfailure  0   ./testsvc  ok     1
failing    never      0   ./testsvc  fail   1
crashing   never      0   ./testsvc  crash  1
persistent always     0   ./testsvc  ok     1
EOF
start_warden /tmp/wtest-1.conf
sleep 6
check "T1 normal exit detected"        "\[clean\] exited status=0"
check "T2 failure exit code decoded"   "\[failing\] exited status=3"
check "T3 signal death decoded"        "\[crashing\] killed by signal 11"
check "T4 policy=onfailure honoured"   "\[clean\] not restarting"
check "T5 policy=always restarts"      "\[persistent\] restarting in"
stop_warden
echo

# -------------------------------------------------------------------- T6
echo "[2] exponential backoff and restart-storm cap"
cat > /tmp/wtest-2.conf <<'EOF'
flapper    always     0   ./testsvc  flap
EOF
start_warden /tmp/wtest-2.conf
sleep 35
check "T6 backoff grows 1s then 2s"    "restarting in 2s"
check "T7 backoff reaches 8s"          "restarting in 8s"
stop_warden
echo

# -------------------------------------------------------------------- T8
echo "[3] control channel"
cat > /tmp/wtest-3.conf <<'EOF'
web        always     0   ./testsvc  long
EOF
start_warden /tmp/wtest-3.conf
./wardenctl -p "$FIFO" status | grep -q "web" \
    && { echo "  PASS  T8 status over FIFO"; PASS=$((PASS+1)); } \
    || { echo "  FAIL  T8 status over FIFO"; FAIL=$((FAIL+1)); }
./wardenctl -p "$FIFO" stop web >/dev/null
sleep 2
check "T9 stop suppresses restart"     "\[web\] sent signal 15"
./wardenctl -p "$FIFO" start web >/dev/null
sleep 1
check "T10 start command works"        "control: command 'start web'"
stop_warden
echo

# ------------------------------------------------------------------- T11
echo "[4] SIGKILL escalation on shutdown"
cat > /tmp/wtest-4.conf <<'EOF'
stubborn   always     0   ./testsvc  hang
EOF
start_warden /tmp/wtest-4.conf
sleep 1
stop_warden
sleep 5
check "T11 SIGKILL after grace period" "did not stop, sending SIGKILL"
echo

# ------------------------------------------------------------------- T12
echo "[5] memory limit enforcement"
cat > /tmp/wtest-5.conf <<'EOF'
hungry     never      32  ./testsvc  leak
EOF
start_warden /tmp/wtest-5.conf
sleep 12
check "T12 soft RSS limit enforced"    "exceeds soft limit"
stop_warden
echo

# ------------------------------------------------------------------- T13
echo "[6] zombie prevention"
cat > /tmp/wtest-6.conf <<'EOF'
churn      always     0   ./testsvc  ok    1
EOF
start_warden /tmp/wtest-6.conf
sleep 8
if ps -eo stat,comm | grep -E '^Z.*testsvc' >/dev/null; then
    echo "  FAIL  T13 zombie processes found"
    FAIL=$((FAIL + 1))
else
    echo "  PASS  T13 no zombie processes"
    PASS=$((PASS + 1))
fi
stop_warden
echo

# ------------------------------------------------------------------- T14
echo "[7] error handling"
./warden -c /tmp/definitely-missing.conf >/dev/null 2>&1
if [ $? -ne 0 ]; then
    echo "  PASS  T14 missing config fails cleanly"
    PASS=$((PASS + 1))
else
    echo "  FAIL  T14 missing config should exit non-zero"
    FAIL=$((FAIL + 1))
fi
echo

echo "=== $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
