#!/usr/bin/env bash
# Regression harness — fresh-IBD block-94 coinbase-spend acceptance (v5.0.3).
#
# Reproduces the production blocker found by the first fresh mainnet self-sync:
# a ZERO-DATADIR node connected genesis..93 and then permanently rejected canonical
# block 94 (the first block whose transaction spends a coinbase), because the V2
# authoritative maturity walk anchored at the not-yet-published spender.
#
# Pre-fix  : node stalls at height 93 (repeated "InvalidChainFound ... height=94").
# Post-fix : node passes height 94 and keeps advancing, with ZERO InvalidChainFound.
#
# Usage:
#   contrib/regression-block94-fresh-sync.sh ./src/innovad "66.70.182.1:14530 161.97.182.56:14530 207.180.228.107:14530" [seconds]
#
# Exit 0 => PASS (height >= 94, zero rejections); non-zero => FAIL.
set -euo pipefail

BIN="${1:?usage: $0 <innovad-binary> \"<peer1:port> <peer2:port> ...\" [seconds]}"
PEERS="${2:?peers required}"
SECS="${3:-180}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/b94-regress-XXXXXX")"
DD="$WORK/datadir"; mkdir -p "$DD"
CONF="$WORK/innova.conf"
PORT="$(shuf -i 40000-49000 -n1)"
cat > "$CONF" <<EOF
server=1
daemon=0
rpcuser=b94regress
rpcpassword=$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')
rpcbind=127.0.0.1
rpcallowip=127.0.0.1
rpcport=$PORT
listen=0
EOF

ARGS=()
for p in $PEERS; do ARGS+=( -addnode="$p" ); done

echo "[b94-regress] binary=$BIN peers='$PEERS' window=${SECS}s datadir=$DD"
timeout "$SECS" "$BIN" -datadir="$DD" -conf="$CONF" -debug=1 "${ARGS[@]}" > "$WORK/run.log" 2>&1 || true
LOG="$DD/debug.log"

ACCEPTED=$(grep -c "SetBestChain: new best" "$LOG" 2>/dev/null || echo 0)
REJECTS=$(grep -c "InvalidChainFound" "$LOG" 2>/dev/null || echo 0)
TOP=$(grep "SetBestChain: new best" "$LOG" 2>/dev/null | sed -E 's/.*height=([0-9]+).*/\1/' | sort -n | tail -1)
TOP="${TOP:-0}"
H94=$(grep -c "new best=00000000dca37031.*height=94" "$LOG" 2>/dev/null || echo 0)

echo "[b94-regress] accepted=$ACCEPTED rejections=$REJECTS top_height=$TOP height94_seen=$H94"
if [ "$TOP" -ge 94 ] && [ "$REJECTS" -eq 0 ] && [ "$H94" -ge 1 ]; then
    echo "[b94-regress] PASS: block 94 connected, chain healthy (zero InvalidChainFound)"
    echo "[b94-regress] artifacts: $WORK"
    exit 0
fi
echo "[b94-regress] FAIL: height=$TOP rejections=$REJECTS height94_seen=$H94"
echo "[b94-regress] artifacts: $WORK"
exit 1
