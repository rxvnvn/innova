# Stage 1c — provisional header graph performance

Status: profiling/design evidence; Stage 2 not implemented.

## Finding

The original per-header insertion path scanned the complete node map while
linking children and rebuilt active-path state for every header. A 2000-header
response therefore incurred quadratic work. Stage 1c adds `InsertBatch`, which
links the batch once, derives heights in forward order, and updates the active
path once.

## Synthetic scaling

The deterministic graph test reports batch insertion times (milliseconds):

| headers | time |
|---:|---:|
| 100 | 0.235 |
| 200 | 0.474 |
| 400 | 1.025 |
| 800 | 2.194 |
| 1600 | 4.715 |
| 3200 | 10.003 |
| 6400 | 21.213 |

This is approximately linear (the small super-linear component is ordered-map
lookup), not quadratic. Batch and single insertion retain equivalent graph
semantics, including quarantine and re-anchor behavior.

## Runtime interpretation

The Stage 1b capture's 10.4 s graph insertion was consistent with the former
quadratic path. The same capture also recorded ProcessBlock calls up to 12.4 s;
cs_main wait telemetry was negligible, so this was execution time rather than
lock contention. It occurred on the legacy INV-driven data path, where orphan
and disconnected work can accumulate; it is not evidence that ordinary single
block validation is intrinsically 12 s. A Stage 2 capture must measure orphan
cascade counts and ConnectBlock/SetBestChain components directly.

The observer had 16,000 headers while the connected tip was 7. With the
observation window of 512, this provides substantial lookahead. At 500–800
connected blocks/s, 512 blocks are consumed in roughly 0.64–1.02 seconds;
continuation responses of 2000 headers replenish several seconds of healthy
consumption. The requirement is therefore that lookahead not exhaust, rather
than an unconditional millisecond dispatch deadline.

## Concurrency decision

No additional concurrency mechanism is selected at Stage 1c. The existing
single message-handler thread and non-preemptible ProcessBlock remain. The
headers graph now has proportional ingestion cost; Stage 2 must verify that
legacy orphan cascades disappear when requests are ordered and that lookahead
remains above the active window. A separate consensus/control thread is not
introduced.


## Reproducible loopback launch contract

The preserved loopback source datadir at
`runtime-artifacts/ibd-header-priority-20260808T145300Z/source-datadir`
contains:

```ini
server=1
listen=0
staking=0
gen=0
upnp=0
discover=0
dnsseed=0
```

Therefore the source launch must be explicit and self-contained: `server=1`
requires runtime RPC credentials even when the source is used only as a local
P2P header/block supplier.

The rerun on commit `e8bdce0` used the preserved working copy:
`runtime-artifacts/stage1c-rerun-20260808T160000Z/source-datadir` and the
fresh observer datadir:
`runtime-artifacts/stage1c-rerun-20260808T160000Z/observer-datadir-contractrun2`.

Exact source command:

```bash
src/innovad -daemon \
  -datadir=/home/user/innova/runtime-artifacts/stage1c-rerun-20260808T160000Z/source-datadir \
  -server=1 \
  -rpcuser=stage1c \
  -rpcpassword=stage1cpass \
  -rpcport=19845 \
  -rpcallowip=127.0.0.1 \
  -rpcbind=127.0.0.1 \
  -listen=1 \
  -staking=0 \
  -dnsseed=0 \
  -discover=0 \
  -upnp=0 \
  -port=19844 \
  -bind=127.0.0.1
```

Required preflight before launching the observer:

1. Source log reaches `Done loading`.
2. Source listener is present on `127.0.0.1:19844`.
3. Source RPC answers on `127.0.0.1:19845`.

Preflight checks used:

```bash
tail -120 runtime-artifacts/stage1c-rerun-20260808T160000Z/source-datadir/debug.log
ss -ltn sport = :19844
curl --silent --user stage1c:stage1cpass \
  --data-binary '{"jsonrpc":"1.0","id":"stage1c","method":"getblockcount","params":[]}' \
  -H 'content-type: text/plain;' \
  http://127.0.0.1:19845/
```

Observed preflight result:

- `Done loading` present for startup `08/08/26 13:43:13`.
- Listener present on `127.0.0.1:19844`.
- RPC returned `{"result":7830779,"error":null,"id":"stage1c"}`.

Exact observer command:

```bash
src/innovad -daemon \
  -datadir=/home/user/innova/runtime-artifacts/stage1c-rerun-20260808T160000Z/observer-datadir-contractrun2 \
  -listen=0 \
  -staking=0 \
  -dnsseed=0 \
  -discover=0 \
  -upnp=0 \
  -connect=127.0.0.1:19844 \
  -ibdheadersobserve=1
```

## Rerun result on e8bdce0

The rerun reached eight real 2,000-header continuation rounds:
`2000 -> 4000 -> 6000 -> 8000 -> 10000 -> 12000 -> 14000 -> 16000`.

Measured `frame_complete -> priority_selected` delays (microseconds):

- samples: `35462, 91063, 1866900, 4664556, 8791179, 14322659, 21078919, 29050404`
- p50: `6727868`
- p95: `29050404`
- max: `29050404`

Measured `dispatch_begin -> graph_insert_complete` durations (microseconds):

- samples: `32228, 35307, 37468, 38980, 42136, 43596, 45547, 47692`
- p50: `40558`
- p95: `47692`
- max: `47692`

Graph state at the end of the rerun:

- graph tip: `16000`
- connected tip: `6`
- minimum graph lookahead over the run: `2000`
- continuation rounds: `8`
- duplicates: `0`
- disconnected: `0`
- quarantined: `0`

All observed classified `request`, `receive`, and `connect` events remained
`IN_PREDICTED_WINDOW` for the represented graph segment. The observer continued
well beyond the former 4,000-header stall, and graph insertion stayed
sub-millisecond-to-tens-of-milliseconds rather than seconds. The remaining
multi-second-to-tens-of-seconds delay is now attributable to the single active
message-dispatch path rather than intrinsic header-graph complexity.
