# Innova IBD Headers-First Scheduler v1

## Status

**DESIGN / NOT IMPLEMENTED**

- Reference tree: Innova Core `59d03e4`.
- Decision: `REPLACE IBD SCHEDULER`, as closed in
  `ibd-scheduler-architecture-decision.md` §13.5.
- Runtime baseline: `doc/forensics/ibd-gate-b3-head-result.md`.
- Scope: experimental IBD block selection and scheduling behind a flag. This
  document changes no production behavior.

## 1. Objective and non-objectives

During `IsInitialBlockDownload()`, select active historical block requests from
an ordered chain window anchored to the connected/common frontier. INV may
report availability or trigger header discovery, but cannot define the work
queue. Preserve enough parallelism to approach the observed healthy 500–800
connected blocks/s while preventing a full pipeline from masking a blocked
front.

This does not change consensus, PoW/PoS rules, historical validity, block or
transaction serialization, `ProcessBlock`, `AcceptBlock`, `SetBestChain`,
chainstate/database, wallet, staking, version/verack, any P2P message format,
the block wire format, or normal post-IBD relay. No protocol-version bump is
required. The current scheduler remains compiled as the control.

## 2. Verified current-tree facts and replacement boundary

The full-node path starts historical sync with `PushGetBlocks` in the version
handler (`main.cpp`), admits block INVs through
`TryAdmitBlockInvOrDefer`, and flushes `mapAskFor` to `getdata` in
`SendMessages`. Its budgets are a request cap, not an ordered download window.
Recovery in `CStalledSyncRecoveryState::ShouldRecover` requires the pipeline to
be empty.

The tree already supports the necessary wire messages:

- `getheaders` is served to full peers and returns up to 2000 headers by walking
  the active `pnext` chain. It is not SPV-only.
- `headers` is accepted by full nodes, checks batch continuity, requires the
  first unknown header to attach to `mapBlockIndex`, checks inferred PoW and
  future time, and then directly requests blocks. Only SPV mode creates index
  entries and continues a 2000-header response.
- `CNode::PushGetHeaders` and `CGetHeadersSyncState` already provide send dedup
  and request lifecycle telemetry.

The smallest safe replacement boundary is therefore:

> Add a full-node, scheduler-private ordered header graph and replace only the
> IBD request-selection/admission/recovery state machine with a window selector
> over that graph. Feed selected hashes through the existing ownership,
> `getdata`, expiry, receive, and `ProcessBlock` paths. Leave the current
> INV/getblocks scheduler intact and selectable when the experiment is off.

Do not put header-only entries into `mapBlockIndex` in v1. Current code treats
presence there as “accepted full block”: `AcceptBlock()` rejects a hash already
in `mapBlockIndex` (`main.cpp:6895`) and `AddToBlockIndex()` refuses the
duplicate (`main.cpp:6548`), and `CBlockIndex` construction performs
full-block-derived PoS, stake-modifier, disk, and DAG work. A separate graph
avoids changing that meaning or the storage schema.

## 3. Information model

### 3.1 What a serialized Innova header proves

`CBlock` header serialization contains version, previous hash, merkle root,
time, bits, and nonce; it omits transactions and block signature. A header is
eligible for the scheduling graph only when:

1. its hash is not operator-invalidated;
2. it is the expected first child of a known anchor or the next element of a
   contiguous message (`hashPrevBlock == previous hash`);
3. its time is not beyond `FutureDrift(GetAdjustedTime())`;
4. header fields and height obey cheap context-independent bounds already used
   by the current handler; and
5. where the current code can identify a PoW header, `CheckProofOfWork` passes.

“Header-valid” in this design means safe as provisional scheduling metadata,
not fully consensus-valid. Before the DAG fork, PoS identity and proof require
transactions/signature absent from `headers`; after the DAG fork, merge-parent
commitments are in the coinbase and also absent. `ProcessBlock` remains the
only authority. A received full block that fails validation invalidates or
quarantines the matching provisional node and descendants from scheduling.

### 3.2 Scheduler-private header graph

Introduce conceptually `CIbdHeaderNode`:

- `hash`, `prev`, `height`, and the six serialized header fields;
- parent pointer/key and children;
- `HEADER_PROVISIONAL`, `BLOCK_ACCEPTED`, `HEADER_QUARANTINED` state;
- announcing/supporting peer IDs with bounded cardinality and timestamps;
- first-seen time;
- optional comparable work/trust hint, never used to bypass full validation.

Introduce `CIbdHeaderGraph`:

- hash → node;
- roots anchored only at an accepted `mapBlockIndex` entry;
- a selected ordered branch and best provisional header;
- bounded competing branches and deterministic eviction;
- ancestor/descendant and “node at height on branch” helpers.

Branch choice cannot pretend that pre-DAG header-only chain trust is fully
known. V1 preference, in descending order, is: branch containing the accepted
active frontier; accepted descendants; independently supported contiguous
extension; longer contiguous extension; deterministic hash tie-break. A single
peer may supply work, but uncorroborated competing branches are lower confidence
and must not displace an accepted-prefix branch merely by flooding headers.
Full-block validation promotes the branch incrementally. Exact branch-scoring
and memory bounds require deterministic tests before runtime selection.

### 3.3 Per-peer state

Add conceptually `CIbdHeaderPeerState`, keyed by `NodeId` rather than raw
`CNode*`:

- `best_known_header` (graph node/hash) and announcement time;
- `last_common_header` anchored at an accepted block;
- `last_unknown_hash` for an INV whose graph entry is not yet known;
- header-sync state: locator/stop, request time, response/continuation state,
  capability result (`UNKNOWN`, `HEADERS`, `LEGACY`), and failure count;
- bounded availability: highest supported header plus exceptional exact hashes;
- ordered hashes currently assigned to the peer;
- front-stall attribution/time and delivery-quality observations.

Existing `nBestKnownHeight`/`hashBestKnownBlock` remain compatibility and UI
telemetry. They are not sufficient for ordered selection because they contain
no ancestor relation. The current tree has no equivalent of Blackcoin’s
`pindexBestKnownBlock` or `pindexLastCommonBlock`; the fields above are the
minimum equivalents.

### 3.4 Global scheduler state

Add conceptually `CIbdHeaderScheduler`:

- mode: `OFF`, `OBSERVE`, or `SELECT`;
- header graph and peer-state map;
- accepted/common frontier hash and height;
- selected branch ID/tip;
- active window `[frontier + 1, frontier + W]`;
- global selected/in-flight hash set derived from the existing ownership
  ledger, not a second ownership authority;
- front hash, owner, first-blocked time, reassignment generation, and progress
  epoch;
- refill counters and telemetry snapshots.

`W`, global active cap, per-peer cap, refill batch, header memory bound, and
stall thresholds are experimental configuration with safe clamps. Blackcoin’s
1024 and 16/peer are semantic examples, not Innova defaults. Initial values are
**MEASUREMENT REQUIRED** from healthy traces: enough outstanding useful work to
cover service/RTT/connect latency at 500–800 blocks/s without admitting an
unbounded suffix. The current 512 global / 128 peer pressure values are control
data, not automatically the new values.

## 4. Ordered-window invariant and selection

The hard invariant is:

> Every active experimental IBD request names a node on the selected contiguous
> header branch within the current window, whose predecessor path reaches the
> accepted/common frontier through graph nodes in that same branch.

The selector walks height order from `frontier + 1` to the window end. It skips
accepted blocks, current owners, and quarantined nodes. It returns exact hashes
up to global/per-peer capacity. Assignment prefers peers whose
`best_known_header` descends from the candidate (or that explicitly announced
the hash), then existing delivery quality, stable outbound status, low useful
pressure, and round-robin tie-break. Each hash is globally single-owner unless
a later, separately reviewed duplicate-request policy is introduced.

Later window blocks may be requested and remain in flight while an earlier
block is missing. This preserves throughput and tolerates bounded cross-peer
reordering. The window does not advance past the connected/common frontier;
receiving a later block does not make unrelated future INVs eligible.

## 5. Target state machine

All graph, selection, and active-chain reads below occur under `cs_main`.
Ownership calls acquire `cs_mapAlreadyAskedFor` internally; code must preserve
the existing order `cs_main` → `cs_mapAlreadyAskedFor` and must not scan
`vNodes` while holding the ownership lock. Socket wire timestamps retain
`cs_vBlockInFlightWire`. Peer lifetime snapshots use `cs_vNodes` outside the
ownership critical section. No new scheduler mutex is required for Stage 0/1;
if profiling later proves `cs_main` contention, lock splitting is a separate
measured change.

| From → to | Trigger | State read | State mutated | Scope / locks |
|---|---|---|---|---|
| `OFF → HEADER_DISCOVERY` | flag selects observe/select and IBD is true | active tip, peers | anchor graph; initialize peer states; queue `getheaders` | global+peer; `cs_main`, then message queue |
| `HEADER_DISCOVERY → HEADER_SYNC` | suitable connected peer | locator from accepted tip/common node, peer sync state | mark header request; call `PushGetHeaders` | peer; `cs_main`; internal header-state lock |
| `HEADER_SYNC → MODEL_READY` | contiguous anchored headers accepted into graph | header batch, graph, peer state | nodes/support, peer best, continuation locator | global+peer; `cs_main` |
| `HEADER_SYNC → LEGACY` | empty/unsupported/timed-out header discovery after bounded retries | request age/capability | mark peer legacy; permit fallback discovery | peer; `cs_main` |
| `MODEL_READY → WINDOW_READY` | selected branch extends accepted frontier | graph, active chain | last common, branch, window endpoints | global+peer; `cs_main` |
| `WINDOW_READY → ASSIGNED` | capacity exists | ordered candidates, availability, quality, owner ledger | claim owner as QUEUED; enqueue exact hash | global+peer; `cs_main` then `cs_mapAlreadyAskedFor` |
| `ASSIGNED → IN_FLIGHT` | existing `SendMessages` flushes `getdata` | queued owner, peer cap | `MarkBlockInFlight`, owner IN_FLIGHT, wire sentinel | peer+global; existing locks |
| `IN_FLIGHT → RECEIVED` | `block` message | sender/hash ownership | existing clear/late accounting; scheduler receipt observation | peer+global; existing ownership/wire locks, then `cs_main` |
| `RECEIVED → ACCEPTED` | unchanged `ProcessBlock` accepts | block index/active chain | existing chainstate; promote graph node | global; `cs_main` |
| `RECEIVED → BUFFERED` | selected block arrives before selected predecessor | selected path and parent owner | existing bounded orphan handling; mark received-on-path | global; `cs_main` |
| `ACCEPTED/BUFFERED → ADVANCE` | active accepted frontier moves; orphan descendants connect | active tip, selected branch | common frontier, progress epoch, window; clear stall | global+peer; `cs_main` |
| `ADVANCE → ASSIGNED` | refill threshold crossed | new window/capacity | exact-hash assignments | global+peer; locks as above |
| `IN_FLIGHT → EXPIRED` | existing pending-wire 1 s or wire+60 s deadline | wire timestamp, owner | existing balanced release/quality/late expectation | peer+global; existing locks |
| `ANY → FRONT_STALLED` | exact condition in §8 | front status/owner/progress, later activity | front-stall record and owner attribution | global+peer; `cs_main` |
| `FRONT_STALLED → REASSIGN` | evidence-based stall policy permits | alternate eligible peers, owner generation | release exact owner once; claim/enqueue on alternate | global+peer; `cs_main` then ownership lock |
| `FRONT_STALLED → DISCONNECT` | repeated attributed front stalls or peer failure policy | peer history and outstanding hashes | `fDisconnect`; ordinary cleanup frees owners | peer; existing cleanup locks |
| `ANY → PRUNE_BRANCH` | full block invalid, accepted chain chooses another branch, or bounds exceeded | graph descendants/support | quarantine/evict nodes; cancel only now-invalid queued work | global+peer; `cs_main`, ownership release outside iteration |
| `SELECT → OFF/POST_IBD` | IBD ends or flag disabled at restart | active requests and current mode | stop new experimental selection; normal relay resumes | global; `cs_main` |

Changing the flag at runtime is not required in v1; startup selection avoids
unsafe mid-flight scheduler handoff. Existing requests drain or are cleaned by
normal ownership rules.

### 5.1 Control-plane progress invariant

Stage-1 runtime evidence exposed a prerequisite not represented by the original
state table:

> During IBD, saturation of either outbound or inbound block-data queues must
> not indefinitely delay a bounded, solicited ordered-chain control-plane
> response. Control priority is bounded in turn and cannot indefinitely delay
> block-data progress.

The control plane is the `getheaders` request lifecycle, expected `headers`
ingestion, provisional graph/branch updates, authoritative anchor, continuation
locator, and scheduler frontier metadata. The data plane is block payload
receive, ownership, `ProcessBlock`, orphan handling, database work, and active
chain connection.

Current `vSendMsg` and per-peer `vRecvMsg` are strict FIFO queues shared by both
planes. `ProcessMessages()` selects one complete ordinary message per peer
visit. The Stage-1 capture measured complete-block-frame-to-dispatch delay up to
146.4 seconds while `cs_main` acquisition wait peaked at only 15 microseconds.
Also, `net: to ... headers` records serialization into `vSendMsg`, not a socket
write, so the retained capture cannot split source send-queue delay from
receiver receive-queue delay.

Before Stage 2, implement and validate bounded `PRIORITY DISPATCH` as specified
in `doc/forensics/ibd-header-dispatch-starvation-audit.md`: one solicited IBD
headers response per peer/pass may bypass not-yet-started bulk block messages at
both message-boundary FIFOs. Complete framing/checksum, the 2,000-header limit,
one outstanding request per peer, bounded scans, SPV behavior, and graph
mutation under `cs_main` remain unchanged. No separate thread or consensus path
is introduced.

## 6. Headers-first and legacy dual capability

### Preferred path

On version/verack during IBD, request headers explicitly from suitable peers
using an accepted-chain locator. Continue when a response contains 2000
contiguous accepted graph headers, using the last graph node as a scheduler
locator even though it is not a `CBlockIndex`; this requires a locator builder
over graph ancestors. Record which peers support the selected branch.

Verified interoperability constraint for that locator: `CBlockLocator::
GetBlockIndex` (`main.h:2010`) returns the first locator hash that exists in
`mapBlockIndex` **and** `IsInMainChain()`, and otherwise falls back to
`pindexGenesisBlock`. A locator whose hashes are all scheduler-graph-only
therefore degenerates to “start from genesis” on the server side
(`main.cpp:9350-9356`). The continuation builder orders the newest provisional
active-branch hash first, then progressively older provisional ancestors, with
the accepted main-chain anchor last as the terminal fallback. Remote
`GetBlockIndex` therefore resolves at the newest provisional hash it recognizes;
only a peer recognizing none of them falls back to the anchor. Placing the
anchor first would repeatedly resolve at the anchor and reproduce the initial
batch. Stage 1a fixes this ordering as a deterministic graph query; live legacy
peer behavior remains an observation target for Stage 1.

### Legacy fallback

Old `/Innovai:4.x/`, `/Innovai:5.0.0/`, and current nodes already understand
`getheaders` server-side, so user-agent strings must not be capability gates.
Capability is behavioral. If a peer does not yield a usable header response
after bounded retries, retain `getblocks/inv` only as discovery:

1. send `getblocks` from the accepted/common `CBlockIndex` locator;
2. record returned block hashes and announcer availability;
3. request headers from a headers-capable peer toward the unknown hash, or
   build a bounded **legacy candidate chain** only by receiving full blocks in
   predecessor order;
4. admit a legacy exact hash into the active window only after it maps to the
   selected ordered graph, or when it is exactly the immediate frontier block
   and `ProcessBlock` can establish the next accepted anchor;
5. never append arbitrary legacy INVs to the ordered active queue.

Thus a legacy peer may serve any selected exact hash it demonstrably has, but
cannot choose the historical work order. In an all-legacy network, immediate
frontier bootstrap through getblocks/inv remains possible and each accepted
block advances the anchor; throughput of that degraded compatibility mode is a
measurement target, not permission to re-enable unordered bulk admission.

## 7. INV, relay, and fork isolation during deep IBD

During deep IBD, INV is availability/discovery input only:

| Announcement | Action |
|---|---|
| new tip hash | record announcer/height hint; request headers toward it; do not directly schedule unless it enters the selected window |
| side-chain hash | record bounded availability; request headers if branch policy warrants; do not reshape selected historical window |
| stale/known block | mark known/ignore for scheduling |
| far-ahead unknown hash | record bounded last-unknown/support; request headers; defer from active scheduling |
| hash already on selected branch | update peer availability; request only if inside the active window, missing, unowned, and peer assignment selects it |

Post-IBD relay remains unchanged. Near-tip direct fetch is not needed for v1
deep IBD and is `POST-IBD ONLY` until separately tested.

## 8. Stall semantics

These are distinct events:

- **Request expiry:** local pending-wire failure at 1 s or no delivery by
  first-wire-send + 60 s. Preserve `59d03e4`; it releases ownership and records
  late-delivery attribution. It is not the only frontier repair.
- **Peer disconnect:** transport/liveness or repeated attributed scheduler
  failure. Existing `CNode::Cleanup` must balance all queued/in-flight gauges
  and owners.
- **Window-front stall:** the earliest selected hash not yet accepted blocks
  advancement, an eligible alternate supplier exists, the current owner has
  had a real opportunity to deliver, and useful frontier progress has not
  occurred for a measured interval, even though later window work may be
  queued, in flight, received, or buffered.
- **Global no-progress:** no frontier advance across the whole scheduler and no
  peer can currently supply the front. Trigger header refresh/peer discovery,
  not repeated unordered getblocks admission.
- **Slow-but-live peer:** delivers within its measured distribution and does
  not repeatedly own the blocked front. Keep it and assign non-front work as
  capacity permits.

The exact front-stall trigger is:

```
front = first selected-window node not accepted
blocked = front has an active owner
later_active = any later selected-window node queued/in-flight/received
alternate = eligible peer supports front and is not current owner
stall = blocked && alternate && no_frontier_advance_since(progress_epoch)
        && owner_wire_age >= FRONT_EVIDENCE_INTERVAL
```

`later_active` is telemetry and proof that the pipeline may be busy; it is not
required for recovery. `FRONT_EVIDENCE_INTERVAL`, escalation, and disconnect
thresholds are **MEASUREMENT REQUIRED**. Derive them from healthy front-block
wire-to-receive distributions under direct and impaired profiles. Do not copy
Blackcoin’s 2 s value and do not silently replace the 60 s generic expiry.

First response is exact-hash reassignment if ownership can be safely released
and an alternate exists. Later work remains in flight. Repeated stalls
attributed to the same peer may disconnect it after a separately measured
threshold; progress clears attribution and may relax the threshold. If no
alternate exists, keep the request live until generic expiry/transport failure
and refresh header/availability knowledge—do not churn the same owner.

## 9. Ownership and late delivery

Reuse, do not duplicate:

- `TryAssignBlockRequestOwner` / `TryAssignBlockRequestOwnerLocked` for QUEUED;
- `CNode::MarkBlockInFlight` for the QUEUED→IN_FLIGHT transition;
- `CNode::ClearBlockInFlight` plus
  `ReleaseBlockRequestOwnerOnReceive` on receive;
- `ReleaseBlockRequestOwner`, `EraseAlreadyAskedForIfUnowned`, timeout-owner,
  alternate-announcer, and late-delivery ledgers;
- `CNode::ExpireBlockInFlight` for pending-wire and 60 s expiration;
- `CNode::Cleanup` for disconnect/shutdown reconciliation;
- `PushBlockGetData` so socket first-send stamps continue to populate
  `mapBlockInFlightWireUs`.

The scheduler selects and claims; it does not decrement gauges itself except
through these existing primitives. Reassignment is generation-safe: release
only the old peer/hash identity, then claim the new owner. A late block from
the former owner must never erase the current owner. The current two-step
receive calls are retained until tests prove whether one can be consolidated;
their identity checks are the `30be277` invariant. Every terminal path must be
tested for exactly one active/gauge decrement.

`mapAlreadyAskedFor` remains request-timing/dedup bookkeeping, not the source
of truth for ownership or window membership. It is erased only when the hash is
unowned.

## 10. Blackcoin concept classification

The reference is Blackcoin More `26.x`; this is conceptual adaptation, not
copy/paste.

| Concept | Classification | Innova v1 treatment |
|---|---|---|
| `FindNextBlocksToDownload` | `ADAPTABLE WITH INNOVA STATE` | ordered walk over separate `CIbdHeaderGraph`, accepted-block checks, existing owner ledger |
| `pindexBestKnownBlock` | `ADAPTABLE WITH INNOVA STATE` | per-peer provisional graph node; never masquerades as accepted `CBlockIndex` |
| `pindexLastCommonBlock` | `DIRECTLY ADAPTABLE` | accepted ancestor shared by peer branch and local active chain |
| ordered candidate chain | `ADAPTABLE WITH INNOVA STATE` | separate provisional graph because legacy PoS/DAG validation requires full block |
| bounded download window | `DIRECTLY ADAPTABLE` | same invariant; numeric width measurement-required |
| per-peer in-flight limit | `DIRECTLY ADAPTABLE` | retain semantic cap; tune for Innova throughput |
| global duplicate exclusion | `DIRECTLY ADAPTABLE` | existing single-owner ledger is stronger for ordinary full blocks |
| window-front detection | `DIRECTLY ADAPTABLE` | earliest unaccepted selected node, independent of pipeline emptiness |
| staller attribution | `ADAPTABLE WITH INNOVA STATE` | owner ledger + wire age + peer availability; thresholds re-derived |
| disconnect/reassignment | `ADAPTABLE WITH INNOVA STATE` | exact release/reclaim through existing primitives; disconnect only after measured escalation |
| request timeout/backoff | `CONCEPT ONLY` | preserve Innova wire+60 s; design separate front evidence/escalation from data |
| near-tip direct fetch | `NOT NEEDED` | exclude from deep-IBD v1; reconsider after IBD scheduler passes |
| multiple compact-block owners | `INCOMPATIBLE` for v1 | Innova full-block ownership remains exclusive; Blackcoin constant is correctly `MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK = 3`, not 2 |

The stale “2” in older forensic evidence remains historical; the source-audited
value is 3 and does not justify duplicate Innova full-block requests.

## 11. Existing IBD mechanism disposition

| Mechanism | Disposition | Reason |
|---|---|---|
| predecessor-path Policy D | `REMOVE FROM NEW IBD PATH` | path is intrinsic to selection; keep code only for control if implemented there |
| frontier admission exemption | `REMOVE FROM NEW IBD PATH` | exact front is always selectable inside the window; retain unchanged in control path |
| deferred future INV queues | `REMOVE FROM NEW IBD PATH` | bounded availability replaces work-queue semantics; retain for control/post-IBD behavior as applicable |
| future-supply diversification | `SIMPLIFY` | distribute selected ordered hashes, not arbitrary future announcements |
| alternate-announcer recovery | `SIMPLIFY` | availability helps choose an alternate exact owner; no re-announce loop |
| ORPHAN_LIMIT-driven getblocks | `REMOVE FROM NEW IBD PATH` | selected path should bound historical orphans; keep safety cap and control behavior |
| pipeline-empty semantic recovery | `REMOVE FROM NEW IBD PATH` | replace with window-front/global-progress states; retain control implementation |
| special IBD relay admission | `REMOVE FROM NEW IBD PATH` | relay cannot enter work queue directly |
| orphan pool and benign reorder repair | `KEEP` | bounded out-of-order arrival remains normal |
| quality ranking | `SIMPLIFY` | use delivery quality after branch/availability/cap eligibility |
| current IBD definition | `REQUIRES EXPERIMENT` | use as flag boundary initially; observe transitions and avoid changing it in v1 |
| normal relay/direct fetch | `POST-IBD ONLY` | untouched after scheduler exit |

## 12. Parallelism and refill

Use a global window wider than the active request cap so selection can see a
front sentinel and refill without waiting for header discovery. Keep independent
per-peer caps and a global cap. Refill whenever an assignment leaves QUEUED,
an in-flight request terminates, a peer disconnects, or the accepted frontier
advances; coalesce wakes using the existing pipeline-wake pattern.

Assignments should stripe consecutive hashes across eligible peers while
respecting quality and availability, but correctness comes from exact global
exclusion, not round-robin alone. A slow front block can be reassigned while
later hashes remain active. Do not cancel useful later requests merely to make
the pipeline look empty.

Required sizing measurement:

- healthy direct trace: service-to-wire, wire-to-receive, receive-to-connect,
  refill cadence, average serialized block size, and active cap at 500–800/s;
- B3 impairment: same distributions plus loss and front-block recovery;
- CPU/disk connect saturation: ensure a larger window does not increase
  accepted-but-unconnected memory without throughput gain.

Parameters pass only if healthy throughput remains comparable to the known
ceiling and B3 useful work rises structurally. No final numeric values are
claimed by this design.

## 13. Flag, stages, and source-level implementation map

Use a startup enum flag rather than a boolean, conceptually
`-ibdheaderscheduler=off|observe|select` (exact spelling may change only before
Stage 0 tests). Default `off` at first.

### Stage 0 — state and deterministic tests

Add `src/ibdheaderscheduler.h/.cpp` containing the scheduler-private graph,
peer/global state, ordered selector, pruning, and pure state transitions. Add
`src/test/ibdheaderscheduler_tests.cpp`; register it in `src/makefile.unix`
(`obj/test/ibdheaderscheduler_tests.o` in the `test_innova` `$(TEST_OBJS)`
list, alongside `obj/test/p2p_sync_tests.o`, `ibdforensic_tests.o`, and
`ibdsemantic_tests.o`, plus a `check-ibdheaderscheduler` target mirroring
`check-ibdforensic`) and in the existing Boost suite. No message handler calls
the selector.

First tests: contiguous insertion; unknown parent rejection; branch isolation;
pre-DAG provisional semantics; accepted-anchor promotion; selected-window path
invariant; global exclusion; last-common advancement; peer removal; invalid
branch quarantine; busy-pipeline front stall; exact reassignment generation;
late old-owner receipt preserving new owner; balanced cleanup.

### Stage 1 — observation mode

Modify `src/init.cpp` to parse/help/validate the enum. Modify `src/main.cpp`
`headers`, `inv`, `block`, version/verack, and active-tip-advance loci to feed
the model, request/continue headers, and record predicted selections. Modify
`src/net.h/.cpp` only for peer-state lifecycle hooks and telemetry. Observation
must issue no block request and alter no admission decision. Compare predicted
next set with blocks that actually connect under the control scheduler.

Implemented Stage-1 ownership and isolation facts:

- `-ibdheadersobserve` is a startup boolean and defaults off. The process-owned
  `CIbdHeadersObserver` and `CIbdHeaderGraph` are mutated only under `cs_main`;
  neither has an internal mutex or retains a `CBlockIndex*`.
- Suitable full peers are actively queried during IBD. Continuation uses the
  tested newest-provisional-to-oldest locator with the connected anchor last;
  a 2000-header response immediately advances that locator.
- Responses to observer-originated `getheaders` are consumed only by the
  observer. They deliberately bypass the legacy full-node `headers` block-fetch
  loop, because allowing that loop to create `getdata` would make observation
  alter request selection. Unsolicited legacy `headers` retain their old path.
- Connected-tip advancement re-anchors the graph. An advance along the active
  provisional path retains its suffix; a reorg or unknown new anchor resets
  stale provisional state. The authoritative chain remains `pindexBest`.
- The observation window is 512 solely for capture comparison, not a production
  scheduler parameter. Existing request, receive, and active-connect events are
  classified against it without feeding the result back into networking.
- SPV request, indexing, and continuation branches remain separate and
  unchanged. Header failure, silence, divergence, or disconnect cannot block
  the existing INV/getblocks scheduler.

### Stage 2 — experimental selection

Stage 2 is blocked until Stage 1b priority dispatch passes focused tests and an
observation-only saturated-pipeline capture proves bounded header
frame-to-dispatch latency across repeated continuation rounds.

Under `select && IsInitialBlockDownload()`:

- bypass `TryAdmitBlockInvOrDefer`, deferred refill, frontier exemption,
  continuation/prefetch getblocks, headers-direct bulk `vGetData`, and
  pipeline-empty recovery as block-selection sources;
- retain their compiled control branches for `off` and use getblocks/inv only
  for the legacy discovery rules in §6;
- call the ordered selector from the existing `SendMessages`/pipeline wake
  cadence;
- claim with existing ownership APIs, enqueue exact hashes, and flush through
  existing `mapAskFor`/`PushBlockGetData`/wire-stamp code;
- feed receive/accept/disconnect/timeout results back to scheduler state.

Likely reused unchanged: `CBlockLocator`, `PushGetHeaders`, getheaders server,
`CheckProofOfWork`, `FutureDrift`, `SyncPeerScore` inputs, all owner/late/expiry
primitives, `PushBlockGetData`, `ProcessBlock`, orphan recursive connection,
chainstate and disk code, and existing forensic/latency counters.

Likely modified: `src/main.cpp`, `src/main.h`, `src/net.cpp`, `src/net.h`,
`src/init.cpp`, `src/makefile.unix`, test registration, and telemetry modules.
New: `src/ibdheaderscheduler.h/.cpp` and tests. Do not create a new validation
path or block database table in v1.

### Stage 3 — controlled A/B

Run with identical binaries/config except mode:

1. healthy CONTROL (`off`) and healthy EXPERIMENT (`select`);
2. B3 moving-tip RTT 13.9 s / ~10% drop;
3. static-tip impaired profile if still diagnostic;
4. mixed-peer regtest with headers responders, deliberately silent-header
   legacy peers, disconnects, side branches, and one withheld front block;
5. fresh live-mainnet characterization after controlled gates, not as a
   prerequisite for this decision.

Do not remove or default-enable the old path before these pass.

## 14. Telemetry

Emit one structured event/counter family common to observe/select:

- header request/response/timeout/continuation and capability transitions;
- graph nodes/branches/bytes, accepted/provisional/quarantined counts;
- selected tip/support, last common, frontier, window end;
- candidates scanned, accepted/missing/owned/quarantined/skipped;
- assignment peer/reason, per-peer/global capacity, duplicate exclusions;
- front hash/owner/wire age, later-active count, stall start/clear/reassign/
  disconnect and reason;
- INV disposition (`availability`, `header-trigger`, `window-request`,
  `deferred`, `ignored`);
- received→accepted latency, selected-path orphan count, useful/waste terminal
  outcome, connect rate, and tip gap with freshness marker.

Peer-height values must carry last-update age. A frozen peer height is a lower
bound, as demonstrated by B3; never chart it as a current remote tip without a
freshness qualifier.

## 15. Acceptance and falsification

Primary adversarial baseline is B3 at `59d03e4`: 241 s and 304 s flat tips,
128/128 pipeline, mean 0.22 block/s, median zero, 1.16% progressing samples,
gap at least 6315 and widening, orphan pool pinned at 750, 1847 orphan-limit
rejects, and approximately 4.9% useful received work.

The experiment passes only if:

- a missing/slow front is detected and recovered while later selected work
  remains active and suitable peers exist;
- there is no recurrence of minutes-scale flat/full tip with an indefinitely
  widening fresh gap in the controlled B3 profile;
- historical selected supply does not pin the orphan pool at 750 or produce
  thousands of `ORPHAN_LIMIT` rejects; benign transient reorder is allowed;
- measured useful-work ratio improves dramatically from 4.9%; report the
  value rather than inventing a threshold before data;
- healthy control-vs-experiment throughput remains comparable to the observed
  500–800 block/s ceiling, with CPU, memory, and lock contention reported;
- headers and legacy peers coexist, and legacy INVs never contaminate the
  selected window;
- ownership, late delivery, timeout, disconnect, and gauges pass deterministic
  and functional tests.

Falsify v1 if the provisional information model cannot keep a stable ordered
branch on real pre-DAG PoS history, if all-legacy operation cannot advance from
fresh startup, if useful throughput is materially serialized, or if exact
reassignment regresses ownership invariants. Such a result revises this design;
it does not restore unordered INV admission by default.

## 16. Unresolved questions

These require Stage 0/observation measurement but do not block Stage 0:

1. What deterministic branch-support rule best handles unvalidated pre-DAG PoS
   headers without letting one peer reshape the selected branch?
2. What graph memory/branch bounds cover mainnet history and hostile forks?
3. What window, global cap, per-peer cap, and refill batch sustain 500–800/s on
   healthy traces?
4. What front evidence interval and repeated-stall disconnect policy separate
   slow-but-live delivery from a true blocker under each network profile?
5. Do deployed legacy peers consistently resolve the ordered provisional
   locator at their newest recognized hash, or do any versions exhibit behavior
   requiring a compatibility fallback beyond the terminal accepted anchor?
6. How much selected-path out-of-order buffering is safe before backpressure is
   needed?
7. At what measured IBD boundary should relay isolation relax to existing
   near-tip behavior?

## 17. Stage 0 readiness

There is no prerequisite production experiment. Stage 0 can begin with pure
graph/selector state and tests, leaving wire behavior unchanged. The first
coding step is to add `CIbdHeaderGraph` with contiguous anchored insertion,
ancestor lookup, branch quarantine, and deterministic ordered-window tests;
peer assignment and runtime hooks follow only after that state model passes.


## Stage 1b runtime result (2026-08-08)

Bounded priority at both FIFO boundaries plus extraction-before-dispatch was
implemented without changing request selection. The graph advanced past the
old 4,000-header failure to 16,000 with correct anchor movement and no graph
integrity errors. However, complete-frame-to-dispatch latency grew to 31.616 s
because the single handler cannot preempt an already-running `ProcessBlock` or
header insertion. Thus the control-plane wall-clock invariant is not yet met
and Stage 2 remains blocked. See
`doc/forensics/ibd-header-dispatch-starvation-audit.md`.


## Stage 1c profiling result (2026-08-08)

The former per-header graph insertion path performed full-graph child scans and active-path rebuilding. `CIbdHeaderGraph::InsertBatch` now links and propagates a response in forward order and updates the active path once. The deterministic 100–6400 header benchmark is approximately linear (0.235 ms to 21.213 ms); semantics, quarantine, and re-anchor behavior are preserved. The Stage 1b multi-second graph insertion was therefore an intrinsic algorithmic cost and is corrected.

The remaining long `ProcessBlock` calls were observed with negligible `cs_main` wait and belong to the legacy INV-driven data path; Stage 2 must measure orphan-cascade and connection components directly. No second consensus or control thread is introduced. The required control-plane invariant is lookahead safety: the ordered graph must remain sufficiently ahead of the active download window, rather than meeting an unconditional millisecond dispatch deadline. See `doc/forensics/ibd-header-graph-performance-stage1c.md`.
