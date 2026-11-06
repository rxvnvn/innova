#!/usr/bin/env bash
# S1 SIGKILL harness driver: phase 1 commits the V2 reorg tip then blocks at the
# C->A boundary; this supervisor SIGKILLs the exact PID; phase 2 restarts on the
# same durable datadir and must reconcile + accept the next block.
set -u
W=${W:-${TMPDIR:-/tmp}/s1e-sigkill}
BIN=${BIN:-$(cd "$(dirname "$0")/../.." && pwd)/build/src/test/test_innova}
LOG=$W/driver.log
rm -rf "$W"
mkdir -p "$W"
: > "$LOG"

export RX_TEST_DATADIR="$W/datadir"
export RX_SIGKILL_WORKDIR="$W"
echo "PHASE1 start $(date -u +%FT%TZ)" >> "$LOG"

RX_REORG_HOLD_AFTER_C_MS=180000 RX_REORG_HOLD_MARKER_FILE="$W/commit-marker.txt" "$BIN" --run_test=atomic_sigkill_tests/sigkill_phase1_commit_then_die \
    > "$W/phase1.log" 2>&1 &
PID=$!
echo "PHASE1 pid=$PID" >> "$LOG"

ok=0
for i in $(seq 1 180); do
  if grep -q "REORG-HOLD-AFTER-C" "$W/phase1.log" 2>/dev/null; then ok=1; break; fi
  if ! kill -0 "$PID" 2>/dev/null; then break; fi
  sleep 1
done
if [ "$ok" -ne 1 ]; then
  echo "PHASE1 FAILED: no REORG-HOLD-AFTER-C marker" >> "$LOG"
  tail -20 "$W/phase1.log" >> "$LOG"
  wait "$PID" 2>/dev/null
  exit 1
fi
echo "PHASE1 marker seen, killing PID $PID with SIGKILL" >> "$LOG"
kill -9 "$PID"
wait "$PID" 2>/dev/null
RC=$?
echo "PHASE1 exit=$RC (137 == SIGKILL)" >> "$LOG"
if [ "$RC" -ne 137 ]; then echo "PHASE1 FAILED: not killed by SIGKILL" >> "$LOG"; exit 1; fi

echo "PHASE2 start $(date -u +%FT%TZ)" >> "$LOG"
"$BIN" --run_test=atomic_sigkill_tests/sigkill_phase2_restart_and_recover \
    > "$W/phase2.log" 2>&1
RC2=$?
echo "PHASE2 exit=$RC2" >> "$LOG"
grep -aE "SIGKILL-PHASE2|error: in|fatal error|No errors" "$W/phase2.log" >> "$LOG"
if [ "$RC2" -ne 0 ]; then echo "RESULT=FAIL" >> "$LOG"; exit 2; fi
echo "RESULT=PASS" >> "$LOG"
