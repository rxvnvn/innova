#!/bin/sh
set -eu
daemon=$1
alert=$2
dir=$(mktemp -d)
trap 'rm -rf -- "$dir"' EXIT
run_node() {
    "$daemon" "-datadir=$dir" -regtest -daemon=0 -listen=0 -connect=0 -dnsseed=0 -discover=0 -upnp=0 -gen=0 -printtoconsole "$@"
}
# A diagnostic command exits through the same cleanup function as InitError.
run_node -printblocktree >"$dir/diagnostic.log" 2>&1
if run_node -testnet >"$dir/failure.log" 2>&1; then
    echo 'fatal startup unexpectedly succeeded' >&2
    exit 1
else
    status=$?
fi
test "$status" = 1
grep -q 'Cannot use -testnet and -regtest together' "$dir/failure.log"

# Use a local recipient so ctest never notifies the developer's desktop.
mkdir "$dir/bin"
cat > "$dir/bin/notify-send" <<'SINK'
#!/bin/sh
printf '%s\n' "$@" >> "$STATE_DIRECTORY/received"
SINK
chmod +x "$dir/bin/notify-send"
export STATE_DIRECTORY="$dir" PATH="$dir/bin:$PATH"
SERVICE_RESULT=exit-code EXIT_CODE=exited EXIT_STATUS="$status" "$alert" innovad-lab.service
SERVICE_RESULT=success EXIT_CODE=exited EXIT_STATUS=0 "$alert" innovad-lab.service
test "$(wc -l < "$dir/failures.log")" -eq 1
test "$(grep -c 'innovad-lab.service: exit-code (code=exited, status=1)' "$dir/received")" -eq 1
