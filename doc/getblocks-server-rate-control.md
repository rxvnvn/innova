# Server-side getblocks response control

## Scope and root cause

This change protects only the server-side handling of an incoming
`getblocks`. It does not change consensus, serialization, the wire format,
block download, outgoing stalled-sync recovery, SPV header download, or the
accepted peer versions.

The semantic execution path is:

```text
ThreadMessageHandler2 (src/net.cpp)
  -> CNodeSignals::ProcessMessages
  -> ProcessMessages (src/main.cpp)
  -> ProcessMessage("getblocks")
  -> CBlockLocator::GetBlockIndex (src/main.h)
  -> main-chain pnext walk, at most 1000 main-chain entries
  -> QueueDAGMergeParentInventories
  -> CNode::PushGetBlocksInventory (src/net.h)
  -> CNode::vGetBlocksInventoryToSend
  -> SendMessages
  -> PushMessage("inv")
```

Before this change, every request repeated that path. The getblocks-specific
queue intentionally bypasses `setInventoryKnown` filtering, because a
protocol response must be sent even if an item was announced earlier.
Consequently, the same locator could append the same 1000 entries again
before or after a previous response was flushed. `SendMessages` serialized
all queued entries into one or more `inv` messages. There was no
server-side duplicate detection, cooldown, or request-cost limit.

This is the proven amplification mechanism for the supplied counters:
31,879 requests caused almost the same number of large inventory responses,
while useful block traffic was small. It does not require malformed data.

## Existing protocol behavior

- The response contains at most 1000 main-chain entries.
- DAG merge-parent traversal can add side-block inventories before each
  main-chain entry, so the actual inventory count can exceed the base count.
- `hashStop == 0` never matches a real block and therefore requests up to
  the 1000-entry limit or the current tip.
- A known stop hash is excluded from the normal inventory range. The
  existing stake-age continuation inventory remains unchanged.
- An unknown locator resolves to genesis through
  `CBlockLocator::GetBlockIndex`; without control, repeated unknown
  locators therefore repeatedly request the chain start.
- Different locator tips that resolve to the same main-chain height can
  produce the same response.
- `hashContinue` is set only when the 1000-main-chain-entry limit is
  reached. When that block is later requested by `getdata`,
  `ProcessGetData` sends a tip inventory and clears `hashContinue`.
  It never deduplicated incoming `getblocks`.
- An early repeat could be processed after the previous inventory was
  queued. It therefore appended a second copy; no “response in flight”
  state existed.
- Protocol versions 43950 and 50000 use the same incoming handler and the
  same locator and inventory serialization. The minimum peer gate remains
  `MIN_PEER_PROTO_VERSION == 43950`.
- Full-node historical sync uses getblocks/inv. SPV and hybrid modes use the
  separate outgoing getheaders path, but any incoming getblocks is protected
  by the same per-connection server policy.
- Existing generic misbehavior handling disconnects/bans at the configured
  banscore (default 100). There was no getblocks-specific score before this
  change.

## Per-peer state and progress

Each `CNode` embeds one fixed-size `CGetBlocksServerState`. It contains no
unbounded container and is destroyed with the connection. It records:

- locator tip, resolved locator height, stop hash and resolved stop height;
- predicted first/last base-chain response hash and count;
- actual queued first/last hash, item count, height bounds, and estimated
  serialized bytes;
- current and previous request time;
- consecutive and cumulative identical/non-progressing requests;
- same-locator and same-response counts;
- response, suppression, rate-limit, useful-getdata, and saved-byte totals;
- cooldown and token-bucket state.

A request is progress when at least one of these is true:

- the resolved locator height increases;
- a changed, known stop moves forward;
- it starts after the previous response range;
- matching block `getdata` arrived for the previous response;
- after construction, the actual response differs from the previous one.

A locator hash change without resolved-height movement is not sufficient by
itself. This prevents unknown, fork, or cyclic locator values from evading
control. A matching block `getdata` immediately clears consecutive repeat
state, so normal latency between inventory and data requests is not treated
as abuse.

## Identical-response suppression

The handler computes a bounded base-chain signature before any DAG disk read
or inventory queue mutation:

```text
locator tip
resolved height
stop hash / stop height
current chain tip
predicted first hash
predicted last hash
predicted item count
```

If the request is identical, or a changed non-progressing request predicts
the same already-served range while the chain tip is unchanged, the full
response is not rebuilt during cooldown.

The first response is allowed. The initial cooldown is 2 seconds. It
increases every 16 consecutive non-progressing requests to 4, 8, 16, and a
maximum of 32 seconds. A continuously flooding peer extends its own
per-connection cooldown. A peer that stops and retries after the cooldown
can receive one controlled retransmission. Any proven progress clears the
repeat cooldown.

Only first/last hashes, counts, heights, and request keys are retained; no
inventory vector is retained for comparison.

## Cost-aware rate limiting

The independent per-peer token bucket uses milli-token accounting:

- capacity: 30 tokens;
- refill: 1 token per second;
- cost: `1 + ceil(predicted_items / 250)` tokens;
- an empty/small request costs at least 1 token;
- a 1000-item response costs 5 tokens;
- actual DAG-added items charge any extra cost after construction.

This permits a burst of six maximum base responses. A sustained stream of
different large ranges then receives at most one large response per five
seconds. Small legitimate responses cost less. Suppression and rate limiting
return immediately from only the getblocks branch; `getdata`, block, inv,
ping, and all other handlers remain responsive. No sleep and no global
network-wide limiter is used.

## Escalation

For non-whitelisted inbound connections only:

1. repeats are counted and sampled in the log;
2. repeated responses are suppressed;
3. cooldown grows to 32 seconds;
4. 5 banscore is added every 128 consecutive repeated/non-progressing range
   requests;
5. the connection is disconnected at 512 such requests without useful
   getdata progress.

A single repeat, a timeout retry, being millions of blocks behind, or simply
having a high request rate with real locator progress is not enough for a
penalty. Whitelisted peers still receive resource control but not this
penalty/disconnect escalation.

The generic banscore implementation may turn accumulated protocol
misbehavior into its existing subnet ban at the configured threshold. This
change does not introduce a new IP ban rule.

## Multiple connections from one IP

The primary protection is deliberately per connection. No IP-level
aggregation was added. Aggregation by address would create false positives
for NAT and would require lifecycle, decay, and bounded-map rules outside
the smallest semantic fix.

Opening many connections can multiply the bounded first-response allowance,
but it no longer lets one connection generate an unbounded stream. Existing
connection/netgroup limits remain an additional layer. A future soft
IP signal can aggregate only identical response signatures and rates; it
must not ban based on connection count alone.

## Diagnostics

Enable complete normal-request logging with:

```text
-getblocksdiag=1
```

Without that option, normal requests are not logged. Abuse events are
sampled at the first few occurrences, powers of two, and penalty intervals
to avoid replacing bandwidth amplification with log amplification.

Events are:

```text
GETBLOCKS_REQUEST
GETBLOCKS_RESPONSE
GETBLOCKS_REPEAT
GETBLOCKS_SUPPRESS
GETBLOCKS_RATE_LIMIT
GETBLOCKS_DISCONNECT
```

Each event contains peer id/address/subversion/version, locator and stop
data, resolved height, predicted and actual response bounds/count/bytes,
request timestamps, progress delta, cooldown, action, useful getdata,
cumulative requests/responses/suppression/rate-limit counts, and estimated
saved bytes.

Analyze a log with:

```sh
python3 contrib/devtools/analyze_innova_debug_log.py debug.log
python3 contrib/devtools/analyze_innova_debug_log.py debug.log --peer 45.179.221.93
```

The analyzer combines `PEERSTATE` message counters with the new events and
reports, per connection:

- getblocks count and rate;
- identical and non-progressing request counts;
- outgoing inv and block counts/bytes and their byte ratio;
- allowed, suppressed, and rate-limited responses;
- estimated suppressed bytes;
- an anomaly ranking led by outgoing inventory bytes.

## Traffic model

The supplied episode (the original log file was not available for replay)
is:

| metric | before |
| --- | ---: |
| getblocks received | 31,879 |
| inv messages | 31,898 |
| inv bytes | 1,090,039,868 |
| block bytes | 16,734,876 |
| inv/block byte ratio | 65.14 |

For a continuous identical 50 requests/second stream, the policy test gives:

| metric | protected connection |
| --- | ---: |
| first response | 1 |
| identical responses suppressed before disconnect | 512 |
| disconnect time | about 10.24 seconds |
| banscore increments | 4 x 5 |
| maximum-base inv bytes allowed | about 36,003 |
| maximum-base bytes suppressed before disconnect | about 18.4 MB |

If the full 31,879-request sequence is replayed against the policy without
honoring disconnect, only the first identical response is allowed and
31,878 are suppressed. Using the observed average response size, that is
approximately 1.09 GB saved. The real network closes the abusive connection
much earlier.

For a worst-case stream that changes request keys and response ranges to
avoid identical suppression, the bucket permits six initial 1000-item
responses and then about one per five seconds. Over ten minutes this is
about 126 base responses or 4.54 MB, roughly 240 times less than the
observed inventory traffic, while useful block serving remains independent.

Exact replay totals depend on DAG parent inventories and require the
original `debug.log`; the analyzer and event schema are ready for that
validation.
