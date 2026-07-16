#!/usr/bin/env bash
# Innova CPU mining RPC lifecycle integration test.
# Exercises the asynchronous setgenerate/getgenerate contract on two isolated
# regtest nodes. This script intentionally never targets mainnet.

set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
source "$SCRIPT_DIR/mining_helpers.sh"
INNOVA_ROOT="$(cd "$SCRIPT_DIR/../.." && pwd)"
INNOVAD="$INNOVA_ROOT/src/innovad"

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m'

RPCUSER="cpuminingtest"
RPCPASS="cpuminingpass"
PORT_SLOT=$(( $$ % 5000 ))
P2P_BASE="${CPU_MINING_TEST_P2P_BASE:-$((20000 + PORT_SLOT * 2))}"
RPC_BASE="${CPU_MINING_TEST_RPC_BASE:-$((35000 + PORT_SLOT * 2))}"
KEEP_TEST_DIR="${CPU_MINING_TEST_KEEP_DIR:-0}"

TEST_DIR=""
NODE1_PID=""
NODE2_PID=""
CLEANUP_DONE=0
PASSED=0
FAILED=0

log()  { echo -e "${BLUE}[CPU-MINING]${NC} $*"; }
pass() { echo -e "${GREEN}[PASS]${NC} $*"; PASSED=$((PASSED + 1)); }
fail() { echo -e "${RED}[FAIL]${NC} $*"; FAILED=$((FAILED + 1)); }
warn() { echo -e "${YELLOW}[WARN]${NC} $*"; }

node_dir() {
    echo "$TEST_DIR/node$1"
}

node_p2p_port() {
    echo $((P2P_BASE + $1))
}

node_rpc_port() {
    echo $((RPC_BASE + $1))
}

rpc() {
    local node="$1"
    shift
    "$INNOVAD" -datadir="$(node_dir "$node")" -regtest \
        -rpcuser="$RPCUSER" -rpcpassword="$RPCPASS" \
        -rpcport="$(node_rpc_port "$node")" "$@"
}

normalize_scalar() {
    tr -d '"[:space:]'
}

json_field() {
    local json="$1"
    local field="$2"
    FIELD="$field" python3 -c '
import json
import os
import sys
try:
    value = json.load(sys.stdin).get(os.environ["FIELD"], "")
    if isinstance(value, bool):
        print(str(value).lower())
    elif value is not None:
        print(value)
except Exception:
    pass
' <<< "$json" 2>/dev/null
}

json_first_txid() {
    python3 -c '
import json
import sys
try:
    txs = json.load(sys.stdin).get("tx", [])
    print(txs[0] if txs else "")
except Exception:
    pass
' 2>/dev/null
}

coinbase_total_atomic() {
    python3 -c '
import json
import sys
from decimal import Decimal
try:
    tx = json.load(sys.stdin)
    total = sum((Decimal(str(vout.get("value", 0)))
                 for vout in tx.get("vout", [])), Decimal(0))
    print(int(total * Decimal(100000000)))
except Exception:
    pass
' 2>/dev/null
}

is_uint() {
    [[ "$1" =~ ^[0-9]+$ ]]
}

get_height() {
    rpc "$1" getblockcount 2>/dev/null | normalize_scalar
}

status_tuple() {
    local node="$1"
    local generated
    local info
    local cpumining
    local active
    local requested

    generated="$(rpc "$node" getgenerate 2>/dev/null | normalize_scalar)" || return 1
    info="$(rpc "$node" getmininginfo 2>/dev/null)" || return 1
    cpumining="$(json_field "$info" cpumining)"
    active="$(json_field "$info" cputhreads)"
    requested="$(json_field "$info" requestedcputhreads)"
    printf '%s|%s|%s|%s\n' "$generated" "$cpumining" "$active" "$requested"
}

status_matches() {
    local node="$1"
    local expected_running="$2"
    local expected_threads="$3"
    local tuple
    local generated
    local cpumining
    local active
    local requested

    tuple="$(status_tuple "$node")" || return 1
    IFS='|' read -r generated cpumining active requested <<< "$tuple"
    [ "$generated" = "$expected_running" ] &&
        [ "$cpumining" = "$expected_running" ] &&
        [ "$active" = "$expected_threads" ] &&
        [ "$requested" = "$expected_threads" ]
}

assert_status() {
    local node="$1"
    local expected_running="$2"
    local expected_threads="$3"
    local label="$4"
    local tuple

    tuple="$(status_tuple "$node" 2>/dev/null || true)"
    if status_matches "$node" "$expected_running" "$expected_threads"; then
        pass "$label"
        return 0
    fi
    fail "$label (expected $expected_running/$expected_threads, got ${tuple:-unavailable})"
    return 1
}

tracked_pid() {
    case "$1" in
        1) echo "$NODE1_PID" ;;
        2) echo "$NODE2_PID" ;;
        *) return 1 ;;
    esac
}

set_tracked_pid() {
    case "$1" in
        1) NODE1_PID="$2" ;;
        2) NODE2_PID="$2" ;;
        *) return 1 ;;
    esac
}

clear_tracked_pid() {
    case "$1" in
        1) NODE1_PID="" ;;
        2) NODE2_PID="" ;;
        *) return 1 ;;
    esac
}

pid_is_ours() {
    local node="$1"
    local pid="$2"
    local cmdline

    is_uint "$pid" || return 1
    kill -0 "$pid" 2>/dev/null || return 1
    [ -r "/proc/$pid/cmdline" ] || return 1
    if ! cmdline="$(tr '\000' ' ' < "/proc/$pid/cmdline")" 2>/dev/null; then
        return 1
    fi
    [[ "$cmdline" == *"-datadir=$(node_dir "$node")"* ]]
}

os_cpu_worker_count() {
    local node="$1"
    local pid
    local comm
    local name
    local count=0

    pid="$(tracked_pid "$node")"
    if ! pid_is_ours "$node" "$pid"; then
        echo 0
        return
    fi
    for comm in "/proc/$pid/task/"*/comm; do
        [ -r "$comm" ] || continue
        IFS= read -r name < "$comm" || true
        [ "$name" = "innova-cpuminer" ] && count=$((count + 1))
    done
    echo "$count"
}

wait_for_process_exit() {
    local node="$1"
    local pid="$2"
    local timeout="${3:-30}"
    local ticks=$((timeout * 10))
    local i

    for ((i=0; i<ticks; i++)); do
        if ! pid_is_ours "$node" "$pid"; then
            wait "$pid" 2>/dev/null || true
            return 0
        fi
        sleep 0.1
    done
    return 1
}

stop_tracked_node() {
    local node="$1"
    local pid
    pid="$(tracked_pid "$node")"
    is_uint "$pid" || return 0

    if pid_is_ours "$node" "$pid"; then
        rpc "$node" setgenerate false >/dev/null 2>&1 || true
        rpc "$node" stop >/dev/null 2>&1 || true
    fi
    if ! wait_for_process_exit "$node" "$pid" 20 && pid_is_ours "$node" "$pid"; then
        kill "$pid" 2>/dev/null || true
        wait_for_process_exit "$node" "$pid" 5 || true
    fi
    if pid_is_ours "$node" "$pid"; then
        kill -KILL "$pid" 2>/dev/null || true
    fi
    wait "$pid" 2>/dev/null || true
    clear_tracked_pid "$node"
}

cleanup() {
    [ "$CLEANUP_DONE" -eq 1 ] && return
    CLEANUP_DONE=1

    stop_tracked_node 1
    stop_tracked_node 2

    if [ -n "$TEST_DIR" ] && [ -d "$TEST_DIR" ]; then
        if [ "$KEEP_TEST_DIR" = "1" ]; then
            log "Preserving test directory: $TEST_DIR"
        else
            rm -rf "$TEST_DIR"
        fi
    fi
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

write_config() {
    local node="$1"
    local peer
    local dir
    dir="$(node_dir "$node")"
    peer=$((node == 1 ? 2 : 1))
    mkdir -p "$dir"

    cat > "$dir/innova.conf" <<EOF
regtest=1
server=1
daemon=0
rpcuser=$RPCUSER
rpcpassword=$RPCPASS
rpcport=$(node_rpc_port "$node")
port=$(node_p2p_port "$node")
bind=127.0.0.1
listen=1
discover=0
dnsseed=0
listenonion=0
nobootstrap=1
idns=0
staking=0
stakingmode=0
nofinalityvoting=1
addnode=127.0.0.1:$(node_p2p_port "$peer")
debug=1
printtoconsole=0
EOF
}

start_node() {
    local node="$1"
    local dir
    local pid
    dir="$(node_dir "$node")"

    rm -f "$dir/innovad.pid"
    "$INNOVAD" -datadir="$dir" -regtest -daemon=0 \
        -pid="$dir/innovad.pid" >"$dir/stdout.log" 2>&1 &
    pid=$!
    set_tracked_pid "$node" "$pid"

    sleep 0.1
    if ! pid_is_ours "$node" "$pid"; then
        fail "node$node exited during startup (see $dir/stdout.log)"
        wait "$pid" 2>/dev/null || true
        clear_tracked_pid "$node"
        return 1
    fi
}

wait_for_rpc() {
    local node="$1"
    local i
    for ((i=0; i<120; i++)); do
        if rpc "$node" getinfo >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.25
    done
    return 1
}

wait_for_height() {
    local node="$1"
    local target="$2"
    local timeout="${3:-30}"
    local ticks=$((timeout * 10))
    local i
    local height

    for ((i=0; i<ticks; i++)); do
        height="$(get_height "$node")"
        if is_uint "$height" && [ "$height" -ge "$target" ]; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_for_peer() {
    local node="$1"
    local i
    local peers
    for ((i=0; i<120; i++)); do
        peers="$(rpc "$node" getconnectioncount 2>/dev/null | normalize_scalar)"
        if is_uint "$peers" && [ "$peers" -ge 1 ]; then
            return 0
        fi
        rpc "$node" addnode "127.0.0.1:$(node_p2p_port $((node == 1 ? 2 : 1)))" onetry \
            >/dev/null 2>&1 || true
        sleep 0.25
    done
    return 1
}

peer_addresses() {
    local node="$1"
    rpc "$node" getpeerinfo 2>/dev/null | python3 -c '
import json
import sys
try:
    for peer in json.load(sys.stdin):
        address = peer.get("addr", "")
        if address:
            print(address)
except Exception:
    pass
'
}

wait_for_no_peers() {
    local node="$1"
    local i
    local peers
    for ((i=0; i<100; i++)); do
        peers="$(rpc "$node" getconnectioncount 2>/dev/null | normalize_scalar)"
        if [ "$peers" = "0" ]; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

partition_nodes() {
    local node
    local peer
    local address

    # Remove the persistent addnode entries before disconnecting so the nodes
    # cannot reconnect while independently creating same-height siblings.
    rpc 1 addnode "127.0.0.1:$(node_p2p_port 2)" remove >/dev/null 2>&1 || true
    rpc 2 addnode "127.0.0.1:$(node_p2p_port 1)" remove >/dev/null 2>&1 || true

    for node in 1 2; do
        while IFS= read -r address; do
            [ -n "$address" ] && rpc "$node" disconnectnode "$address" >/dev/null 2>&1 || true
        done < <(peer_addresses "$node")
    done

    wait_for_no_peers 1 && wait_for_no_peers 2
}

reconnect_nodes() {
    rpc 1 addnode "127.0.0.1:$(node_p2p_port 2)" add >/dev/null 2>&1 || true
    rpc 2 addnode "127.0.0.1:$(node_p2p_port 1)" add >/dev/null 2>&1 || true
    rpc 1 addnode "127.0.0.1:$(node_p2p_port 2)" onetry >/dev/null 2>&1 || true
    rpc 2 addnode "127.0.0.1:$(node_p2p_port 1)" onetry >/dev/null 2>&1 || true
    wait_for_peer 1 && wait_for_peer 2
}

wait_for_known_block() {
    local node="$1"
    local hash="$2"
    local i
    for ((i=0; i<300; i++)); do
        if rpc "$node" getblock "$hash" >/dev/null 2>&1; then
            return 0
        fi
        sleep 0.1
    done
    return 1
}

wait_for_worker_identity_tip() {
    local node="$1"
    local start_line="$2"
    local hash_tip="$3"
    local timeout="${4:-10}"
    local ticks=$((timeout * 20))
    local log_file="$(node_dir "$node")/regtest/debug.log"
    local i

    for ((i=0; i<ticks; i++)); do
        if [ -f "$log_file" ] && awk -v start="$start_line" -v needle="$hash_tip" '
            NR > start && index($0, "CPUMiner[0]: Work identity") && index($0, needle) {
                found = 1
                exit
            }
            END { if (!found) exit 1 }
        ' "$log_file"; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

setup() {
    if [ ! -x "$INNOVAD" ]; then
        echo "ERROR: innovad not found at $INNOVAD" >&2
        return 1
    fi

    TEST_DIR="$(mktemp -d "${TMPDIR:-/tmp}/innova_cpu_mining_rpc.XXXXXX")" || return 1
    log "Using isolated datadir root $TEST_DIR"
    log "P2P ports: $(node_p2p_port 1), $(node_p2p_port 2); RPC ports: $(node_rpc_port 1), $(node_rpc_port 2)"

    write_config 1
    write_config 2
    start_node 1 || return 1
    start_node 2 || return 1
    wait_for_rpc 1 || { fail "node1 RPC did not start"; return 1; }
    wait_for_rpc 2 || { fail "node2 RPC did not start"; return 1; }
    pass "both isolated regtest nodes started"

    wait_for_peer 1 || { fail "node1 did not connect to node2"; return 1; }
    wait_for_peer 2 || { fail "node2 did not connect to node1"; return 1; }
    pass "two-node P2P link established"
}

stop_and_assert() {
    local node="$1"
    local label="$2"
    if ! rpc "$node" setgenerate false >/dev/null 2>&1; then
        fail "$label: setgenerate false failed"
        return 1
    fi
    # No polling here: the state must already be stopped when the RPC returns.
    assert_status "$node" false 0 "$label: synchronous stop-and-join"
}

test_initial_status() {
    log "Checking initial state"
    assert_status 1 false 0 "node1 starts with CPU mining disabled"
    assert_status 2 false 0 "node2 starts with CPU mining disabled"
}

test_thread_selection() {
    log "Checking default and explicit worker counts"

    if rpc 1 setgenerate true >/dev/null 2>&1; then
        assert_status 1 true 1 "setgenerate true defaults to one worker"
    else
        fail "setgenerate true failed"
    fi
    stop_and_assert 1 "default worker count"

    if rpc 1 setgenerate true 1 >/dev/null 2>&1; then
        assert_status 1 true 1 "setgenerate true 1 starts one worker"
    else
        fail "setgenerate true 1 failed"
    fi
    stop_and_assert 1 "one worker"

    if rpc 1 setgenerate true 2 >/dev/null 2>&1; then
        assert_status 1 true 2 "setgenerate true 2 starts two workers"
    else
        fail "setgenerate true 2 failed"
    fi
    if rpc 1 setgenerate true 2 >/dev/null 2>&1; then
        # The controller does not expose session IDs over RPC. Exact active and
        # requested counts prove that the same-N call did not append workers.
        assert_status 1 true 2 "same-N start does not duplicate workers"
    else
        fail "second setgenerate true 2 failed"
    fi
    stop_and_assert 1 "two workers"

    if rpc 1 setgenerate true 4 >/dev/null 2>&1; then
        assert_status 1 true 4 "setgenerate true 4 starts four workers"
    else
        fail "setgenerate true 4 failed"
    fi
    stop_and_assert 1 "four workers"
}

test_invalid_thread_counts() {
    local value
    local output

    log "Checking invalid worker counts"
    rpc 1 setgenerate false >/dev/null 2>&1 || true
    for value in 0 -2 1000000; do
        if output="$(rpc 1 setgenerate true "$value" 2>&1)"; then
            fail "setgenerate true $value unexpectedly succeeded: $output"
            rpc 1 setgenerate false >/dev/null 2>&1 || true
        else
            pass "setgenerate true $value is rejected"
        fi
        assert_status 1 false 0 "invalid $value leaves miner stopped"
    done
}

test_all_logical_capped() {
    local tuple
    local generated
    local cpumining
    local active
    local requested

    log "Checking -1 logical-CPU selection and cap"
    rpc 1 setgenerate false >/dev/null 2>&1 || true
    if ! rpc 1 setgenerate true -1 >/dev/null 2>&1; then
        fail "setgenerate true -1 failed"
        return
    fi

    tuple="$(status_tuple 1 2>/dev/null || true)"
    IFS='|' read -r generated cpumining active requested <<< "$tuple"
    if [ "$generated" = true ] && [ "$cpumining" = true ] &&
       is_uint "$active" && [ "$active" -ge 1 ] && [ "$active" -le 16 ] &&
       [ "$requested" = "$active" ]; then
        pass "setgenerate true -1 starts $active worker(s), capped at 16"
    else
        fail "setgenerate true -1 returned invalid status: ${tuple:-unavailable}"
    fi
    stop_and_assert 1 "-1 worker selection"
}

test_repeated_cycles() {
    local i
    local threads
    local ok=1
    local os_workers

    log "Running 20 start/stop lifecycle cycles"
    for ((i=1; i<=20; i++)); do
        case $((i % 3)) in
            0) threads=4 ;;
            1) threads=1 ;;
            2) threads=2 ;;
        esac

        if ! rpc 1 setgenerate true "$threads" >/dev/null 2>&1 ||
           ! status_matches 1 true "$threads"; then
            ok=0
            warn "cycle $i failed to reach running/$threads"
        fi
        if ! rpc 1 setgenerate false >/dev/null 2>&1 ||
           ! status_matches 1 false 0; then
            ok=0
            warn "cycle $i failed to stop synchronously"
        fi
    done

    if [ "$ok" -eq 1 ]; then
        pass "20 start/stop cycles completed without residual workers"
    else
        fail "one or more of 20 start/stop cycles failed"
        rpc 1 setgenerate false >/dev/null 2>&1 || true
    fi

    os_workers="$(os_cpu_worker_count 1)"
    if [ "$os_workers" = "0" ]; then
        pass "no innova-cpuminer OS threads remain after lifecycle cycles"
    else
        fail "$os_workers innova-cpuminer OS thread(s) remain after stop"
    fi
}

test_mined_block_and_coinbase() {
    local before
    local target
    local after
    local template
    local expected_total
    local block_hash
    local peer_hash
    local block
    local block_height
    local confirmations
    local coinbase_txid
    local coinbase
    local actual_total

    log "Mining and validating an accepted PoW block"
    rpc 1 setgenerate false >/dev/null 2>&1 || true
    before="$(get_height 1)"
    template="$(rpc 1 getblocktemplate 2>/dev/null || true)"
    expected_total="$(json_field "$template" coinbasevalue)"
    if ! is_uint "$before" || ! is_uint "$expected_total"; then
        fail "could not read pre-mining height or getblocktemplate coinbasevalue"
        return
    fi
    target=$((before + 1))

    if ! CPU_MINE_TIMEOUT=30 cpu_mine_blocks 1 rpc 1; then
        fail "CPU miner did not produce a regtest block"
        return
    fi
    after="$(get_height 1)"
    if is_uint "$after" && [ "$after" -ge "$target" ]; then
        pass "mined block accepted: height advanced from $before to $after"
    else
        fail "height did not advance after mining (before=$before after=$after)"
        return
    fi
    assert_status 1 false 0 "height helper leaves miner stopped"

    block_hash="$(rpc 1 getblockhash "$target" 2>/dev/null | normalize_scalar)"
    block="$(rpc 1 getblock "$block_hash" 2>/dev/null || true)"
    block_height="$(json_field "$block" height)"
    confirmations="$(json_field "$block" confirmations)"
    if [ "$block_height" = "$target" ] && is_uint "$confirmations" &&
       [ "$confirmations" -ge 1 ]; then
        pass "mined block is in node1's active chain"
    else
        fail "mined block is not confirmed in node1's active chain"
    fi

    if wait_for_height 2 "$target" 30; then
        peer_hash="$(rpc 2 getblockhash "$target" 2>/dev/null | normalize_scalar)"
        if [ "$peer_hash" = "$block_hash" ]; then
            pass "node2 accepted the same mined block over P2P"
        else
            fail "node2 has a different block at height $target"
        fi
    else
        fail "node2 did not receive mined height $target"
    fi

    coinbase_txid="$(printf '%s' "$block" | json_first_txid)"
    coinbase="$(rpc 1 getrawtransaction "$coinbase_txid" 1 2>/dev/null || true)"
    actual_total="$(printf '%s' "$coinbase" | coinbase_total_atomic)"
    if is_uint "$actual_total" && [ "$actual_total" = "$expected_total" ]; then
        pass "coinbase outputs total $actual_total base units, matching getblocktemplate"
    else
        fail "coinbase total mismatch (actual=${actual_total:-unknown}, template=$expected_total)"
    fi

    # Deliberately do not assert the 65/35 Collateral Node split here:
    # low-height regtest does not activate the current mainnet CN payment gates.
    # Mainnet/testnet payment validation requires a separate, explicitly
    # prepared Collateral Node state.
}

test_same_height_tip_replacement() {
    local common_height
    local common_hash_1
    local common_hash_2
    local sibling_height
    local height_1
    local height_2
    local sibling_1
    local sibling_2
    local merged_height
    local log_file
    local log_start

    log "Checking same-height competing DAG tip while a worker is active"
    rpc 1 setgenerate false >/dev/null 2>&1 || true
    rpc 2 setgenerate false >/dev/null 2>&1 || true

    common_height="$(get_height 1)"
    common_hash_1="$(rpc 1 getbestblockhash 2>/dev/null | normalize_scalar)"
    common_hash_2="$(rpc 2 getbestblockhash 2>/dev/null | normalize_scalar)"
    if ! is_uint "$common_height" || [ -z "$common_hash_1" ] ||
       [ "$common_hash_1" != "$common_hash_2" ]; then
        fail "nodes do not share a common tip before same-height test"
        return
    fi
    sibling_height=$((common_height + 1))

    if ! partition_nodes; then
        fail "could not create an isolated two-node partition"
        reconnect_nodes >/dev/null 2>&1 || true
        return
    fi

    CPU_MINE_TIMEOUT=30 CPU_MINE_POLL_INTERVAL=0.001 cpu_mine_blocks 1 rpc 1 || true
    CPU_MINE_TIMEOUT=30 CPU_MINE_POLL_INTERVAL=0.001 cpu_mine_blocks 1 rpc 2 || true
    height_1="$(get_height 1)"
    height_2="$(get_height 2)"
    sibling_1="$(rpc 1 getblockhash "$sibling_height" 2>/dev/null | normalize_scalar)"
    sibling_2="$(rpc 2 getblockhash "$sibling_height" 2>/dev/null | normalize_scalar)"

    if [ "$height_1" != "$sibling_height" ] || [ "$height_2" != "$sibling_height" ] ||
       [ -z "$sibling_1" ] || [ -z "$sibling_2" ] || [ "$sibling_1" = "$sibling_2" ]; then
        fail "could not create exactly two distinct same-height siblings (h1=$height_1 h2=$height_2)"
        reconnect_nodes >/dev/null 2>&1 || true
        return
    fi
    pass "isolated nodes created distinct siblings at height $sibling_height"

    if ! rpc 1 setgenerate true 1 >/dev/null 2>&1; then
        fail "could not start node1 worker before competing-tip delivery"
        reconnect_nodes >/dev/null 2>&1 || true
        return
    fi
    assert_status 1 true 1 "worker active before same-height competing tip arrives"
    log_file="$(node_dir 1)/regtest/debug.log"
    if [ -f "$log_file" ]; then
        log_start="$(wc -l < "$log_file")"
    else
        log_start=0
    fi

    if ! reconnect_nodes; then
        fail "nodes did not reconnect for competing-tip delivery"
        rpc 1 setgenerate false >/dev/null 2>&1 || true
        return
    fi
    if wait_for_known_block 1 "$sibling_2" && wait_for_known_block 2 "$sibling_1"; then
        pass "both nodes accepted the competing same-height DAG sibling"
    else
        fail "same-height sibling was not exchanged over P2P"
    fi

    if wait_for_worker_identity_tip 1 "$log_start" "$sibling_2" 10; then
        pass "worker rebuilt work after the competing DAG tip arrived"
    else
        fail "worker did not log rebuilt work containing the competing DAG tip"
    fi

    stop_and_assert 1 "same-height tip replacement"
    merged_height="$(get_height 1)"
    if wait_for_height 2 "$merged_height" 30; then
        pass "nodes converged after competing-tip worker rebuild"
    else
        fail "nodes did not converge after competing-tip delivery"
    fi
}

test_shutdown_and_restart() {
    local old_pid
    local height_before
    local height_after
    local restart_before
    local restart_after

    log "Checking shutdown while CPU mining is active"
    if ! rpc 1 setgenerate true 4 >/dev/null 2>&1; then
        fail "could not start four workers before shutdown"
        return
    fi
    assert_status 1 true 4 "four workers active before daemon shutdown"
    old_pid="$(tracked_pid 1)"
    height_before="$(get_height 1)"

    # Do not call setgenerate false: daemon shutdown itself must stop and join
    # every worker before wallet/chain teardown.
    rpc 1 stop >/dev/null 2>&1 || true
    if wait_for_process_exit 1 "$old_pid" 30; then
        pass "daemon exited cleanly while CPU mining was active"
        clear_tracked_pid 1
    else
        fail "daemon did not exit after shutdown during CPU mining"
        stop_tracked_node 1
    fi

    if ! start_node 1 || ! wait_for_rpc 1; then
        fail "node1 failed to restart after mining shutdown"
        return
    fi
    pass "node1 restarted using the same datadir"
    assert_status 1 false 0 "CPU mining remains disabled after restart"

    height_after="$(get_height 1)"
    if is_uint "$height_before" && is_uint "$height_after" &&
       [ "$height_after" -ge "$height_before" ]; then
        pass "chain height persisted across mining shutdown/restart"
    else
        fail "chain height did not persist across restart"
    fi

    wait_for_peer 1 || warn "node1 did not reconnect to node2 immediately"
    restart_before="$(get_height 1)"
    if CPU_MINE_TIMEOUT=30 cpu_mine_blocks 1 rpc 1; then
        restart_after="$(get_height 1)"
        if is_uint "$restart_before" && is_uint "$restart_after" &&
           [ "$restart_after" -gt "$restart_before" ]; then
            pass "CPU mining works after daemon restart"
        else
            fail "post-restart mining did not advance the chain"
        fi
    else
        fail "post-restart CPU mining failed"
    fi
    assert_status 1 false 0 "post-restart mining stops cleanly"
}

print_summary() {
    echo
    echo "========================================"
    echo " CPU MINING RPC TEST SUMMARY"
    echo "========================================"
    echo " Passed: $PASSED"
    echo " Failed: $FAILED"
    echo "========================================"
    [ "$FAILED" -eq 0 ]
}

main() {
    echo "Innova CPU mining RPC regtest integration"
    if ! setup; then
        fail "test setup failed"
        print_summary
        return 1
    fi

    test_initial_status
    test_thread_selection
    test_invalid_thread_counts
    test_all_logical_capped
    test_repeated_cycles
    test_mined_block_and_coinbase
    test_same_height_tip_replacement
    test_shutdown_and_restart
    print_summary
}

main "$@"
