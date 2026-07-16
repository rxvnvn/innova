#!/usr/bin/env bash

# Start the asynchronous CPU miner with one worker and wait until the requested
# chain height is reached. Run in a subshell so the EXIT trap always stops and
# joins the miner, including timeout and signal paths.
cpu_mine_to_height() (
    local target_height="$1"
    shift
    local -a rpc_command=("$@")
    local current_height
    local remaining
    local timeout
    local started_at=$SECONDS
    local stopped=0

    if [[ ! "$target_height" =~ ^[0-9]+$ ]] || [ "${#rpc_command[@]}" -eq 0 ]; then
        echo "cpu_mine_to_height: expected TARGET_HEIGHT and an RPC command" >&2
        return 2
    fi

    _cpu_mining_stop() {
        if [ "$stopped" -eq 0 ]; then
            "${rpc_command[@]}" setgenerate false >/dev/null 2>&1 || true
            stopped=1
        fi
    }

    trap '_cpu_mining_stop' EXIT
    trap '_cpu_mining_stop; exit 130' INT
    trap '_cpu_mining_stop; exit 143' TERM

    if ! current_height=$("${rpc_command[@]}" getblockcount 2>/dev/null | tr -d '"[:space:]'); then
        echo "cpu_mine_to_height: cannot read the current block height" >&2
        return 1
    fi
    if [[ ! "$current_height" =~ ^[0-9]+$ ]]; then
        echo "cpu_mine_to_height: invalid block height: $current_height" >&2
        return 1
    fi
    if [ "$current_height" -ge "$target_height" ]; then
        return 0
    fi

    remaining=$((target_height - current_height))
    timeout=${CPU_MINE_TIMEOUT:-$((remaining * ${CPU_MINE_TIMEOUT_PER_BLOCK:-30}))}
    if [[ ! "$timeout" =~ ^[1-9][0-9]*$ ]]; then
        echo "cpu_mine_to_height: CPU_MINE_TIMEOUT must be a positive integer" >&2
        return 2
    fi

    if ! "${rpc_command[@]}" setgenerate true 1 >/dev/null 2>&1; then
        echo "cpu_mine_to_height: failed to start CPU mining" >&2
        return 1
    fi

    while [ $((SECONDS - started_at)) -lt "$timeout" ]; do
        if current_height=$("${rpc_command[@]}" getblockcount 2>/dev/null | tr -d '"[:space:]') &&
           [[ "$current_height" =~ ^[0-9]+$ ]] &&
           [ "$current_height" -ge "$target_height" ]; then
            return 0
        fi
        # Regtest can find consecutive blocks very quickly. A short bounded
        # poll interval minimizes overshoot around fork-height assertions.
        sleep "${CPU_MINE_POLL_INTERVAL:-0.01}"
    done

    echo "cpu_mine_to_height: timed out at height ${current_height:-unknown}, target $target_height" >&2
    return 1
)

# Mine COUNT new blocks relative to the height observed immediately before the
# miner starts. The RPC command may be a shell function and may include fixed
# arguments, for example: cpu_mine_blocks 5 rpc 0
cpu_mine_blocks() (
    local count="$1"
    shift
    local -a rpc_command=("$@")
    local start_height

    if [[ ! "$count" =~ ^[1-9][0-9]*$ ]] || [ "${#rpc_command[@]}" -eq 0 ]; then
        echo "cpu_mine_blocks: expected a positive COUNT and an RPC command" >&2
        return 2
    fi
    if ! start_height=$("${rpc_command[@]}" getblockcount 2>/dev/null | tr -d '"[:space:]'); then
        echo "cpu_mine_blocks: cannot read the current block height" >&2
        return 1
    fi
    if [[ ! "$start_height" =~ ^[0-9]+$ ]]; then
        echo "cpu_mine_blocks: invalid block height: $start_height" >&2
        return 1
    fi

    cpu_mine_to_height "$((start_height + count))" "${rpc_command[@]}"
)
