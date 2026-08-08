# Server-side `getblocks` / `hashContinue` audit: why the serving peer ends up replying `INV size = 1`

Target: tree `30be277` (HEAD). Question under audit:

> After long-running IBD, the local node logs `Prefetch: Requesting next batch at 1/1 blocks`
> and `GETBLOCKS_CONTINUE` with `nExpectedBatchSize == 1`. The claim to test is that the
> local node is *not* explicitly deciding to request one block — the serving peer (millions
> of blocks ahead) *generated* an `INV` containing only one block, and `nExpectedBatchSize`
> merely reflects the size of that previous `INV` batch. Why does the peer reply with
> `INV size = 1` instead of `INV size ≈ 1000`?

Method: static code archaeology over `src/main.cpp`, `src/net.cpp`, `src/net.h`,
`src/main.h`, plus git history of the relevant lines. No production code was modified.

---

## 1. Verdict

**STRONGLY SUPPORTED** — with one refinement to the question's framing.

* The batch size is remote-determined. The client never chooses a batch of one: the
  downloader reacts to whatever `INV` it receives.
* The serving peer does **not** have any logic that "shrinks" or "truncates" a response
  down to one. Its `getblocks` response size is a **deterministic function of the gap
  between the client's resolved locator and the peer's tip** (capped at 1000). It can be
  exactly `1` only in three narrow, code-proven ways (Sections 4, 6, 7).
* The server's own anti-spam gate (`Evaluate()`) can only emit *no* `INV`, never an
  `INV` of size 1. This **disproves** the earlier hypothesis that "repeat-suppression →
  `INV=1`" (Section 5).
* The refinement: "a peer millions of blocks ahead" describes the *start* of the session.
  By the time the 1-item batches dominate, the client's locator has reached within ~1
  block of that peer's moving tip. The flood of `1/1` events is the natural, by-design
  consequence of chasing a tip that advances while a single slow peer is the one being
  asked (Section 9).

A residual, evidence-limited point: the split of the 15,535 observed `1/1` events among
the three 1-item sources (Section 7 continuation `INV`, Section 4 tip-path response, or
single-block relay `INV`) cannot be proven from the stored session, because the stored
telemetry records the batch *count* but not the *origin* of each `INV`. Closing that gap
is the recommendation in Section 10.

---

## 2. Scope, constraints and method

Files read (exact regions):

| File | Region | Content |
|---|---|---|
| `src/main.cpp` | 9100–9274 | `getblocks` handler: parse, resolve, predict, walk, record |
| `src/main.cpp` | 9192–9217 | non-ALLOW early return (no `INV` emitted) |
| `src/main.cpp` | 8420–8469 | `ProcessGetData`, `hashContinue` continuation `INV` |
| `src/main.cpp` | 8883–8893 | `inv` handler: `nExpectedBatchSize` assignment |
| `src/main.cpp` | 9973–10029 | block handler: batch counter, prefetch trigger, `1/1` log |
| `src/main.cpp` | 10031–10082 | pipeline-drained `GETBLOCKS_CONTINUE` |
| `src/main.cpp` | 9022–9038 | known-last-block continuation (`INV_CONTINUATION`) |
| `src/main.cpp` | 7554–7570 | orphan-limit `PushGetBlocks(pindexBest, 0)` |
| `src/main.cpp` | 10820–10845 | `SendMessages` flush of `vGetBlocksInventoryToSend` → `inv` |
| `src/net.cpp` | 505–510 | token-bucket constants |
| `src/net.cpp` | 512–544 | `CGetBlocksRequestInfo` / `CGetBlocksResponseInfo` |
| `src/net.cpp` | 593–645 | refill, cost, cooldown, payload estimate |
| `src/net.cpp` | 647–789 | `Evaluate()` full decision logic |
| `src/net.cpp` | 791–819 | `RecordResponse()` |
| `src/net.cpp` | 6457–6529 | `ConsumeGetBlocksResponse`, `ExpireGetBlocksOutstanding` |
| `src/net.h` | 1535–1543 | `PushGetBlocksInventory` (bypasses `setInventoryKnown`) |
| `src/net.h` | 1047–1077, 1210, 1370 | timeouts, `hashContinue` member + reset |
| `src/net.h` | 2141–2150 | `CanAdvanceBlockSync` / `ShouldContinueKnownBlockInventory` |
| `src/main.h` | 2010–2024 | `CBlockLocator::GetBlockIndex` |

History consulted: `2e96aa3` (suppression layer + `doc/getblocks-server-rate-control.md`),
`690a674` (bounded-request state, `hashContinue` lineage), `581a59a` (500 → 1000 limit),
`051912e` / `6885e9f` (DAG-expanded traversal removal).

Constraints honored: no changes to `main.cpp`/`net.cpp`/`net.h`/IBD source; no commits;
no pushes. `git status --short` shows only the pre-existing excluded files.

---

## 3. Request side: locator resolution and prediction

On `getblocks` (`main.cpp:9100-9123`) the server:

1. deserializes `locator` and `hashStop`;
2. resolves `pindexLocator = locator.GetBlockIndex()` — the **first hash of the locator
   that exists in the server's main chain** (`main.h:2010-2024`);
3. resolves `pindexStop` only if `hashStop` is in the main chain;
4. computes a *prediction* of the response for the rate-control layer
   (`main.cpp:9125-9163`):
   `nPredictedMainItems = min(1000, max(0, pindexBest->nHeight - nResolvedHeight))`,
   plus an optional `+1` when the stop lies ≤1000 ahead and stake-age conditions indicate
   a tip `INV` will be appended (`main.cpp:9131-9142`).

Two points matter for everything that follows:

* The prediction is **only** used by `Evaluate()` for costing and identical-detection
  (`net.cpp:758-759`). It is not the response. The actual response is built by the walk in
  Section 4.
* `nResolvedHeight` is the *first* locator hash found, so a client whose locator tip is
  many blocks behind a peer that shares a common ancestor will resolve low and be offered
  ~1000 blocks. The response is large exactly until the locator reaches within 1000 of the
  peer's tip.

---

## 4. Response assembly: the deterministic walk and its stopping conditions

The response is built in `main.cpp:9220-9265`:

```cpp
CGetBlocksResponseInfo response;
CBlockIndex* pindex = pindexFirst;      // pindexLocator ? pindexLocator->pnext : NULL
int nLimit = 1000;
for (; pindex; pindex = pindex->pnext)
{
    hashBlock = pindex->GetBlockHash();
    if (hashBlock == hashStop)      { /* maybe tip INV */ break; }        // main.cpp:9230-9247
    if (PushGetBlocksInventory(inv)) response.Add(hashBlock, nHeight);    // main.cpp:9249-9253
    if (--nLimit <= 0)              { pfrom->hashContinue = hashBlock; break; } // main.cpp:9254-9264
}
```

Every matched hash goes through `PushGetBlocksInventory` (`net.h:1535-1543`), which
bypasses `setInventoryKnown` and appends to `vGetBlocksInventoryToSend`. The flush in
`SendMessages` (`main.cpp:10820-10845`) swaps that vector, marks each item known, and
batches it into `inv` messages of ≤ 1000 items. **There is no random truncation, no
adaptive shrinking, and no cap reduction anywhere in this path.**

The walk has exactly four stopping conditions. Each has a deterministic output size:

| # | Stop condition | Line | Output |
|---|---|---|---|
| S1 | `hashBlock == hashStop` (protocol stop) | 9230 | 0 … 1 main items + **possibly a single tip `INV`** (stake-age gate, 9236–9246) |
| S2 | `--nLimit <= 0` (1000-item cap) | 9254 | exactly 1000 items, `hashContinue` set |
| S3 | chain end (`pindex` becomes NULL, locator ≥ tip) | 9227 loop | **0 items (silent)** |
| S4 | empty start (`pindexFirst == NULL`, locator == tip) | 9125–9126 | 0 items (silent) |

The full set of achievable response sizes from this walk:

* **`≈1000`** — the peer is ≥ 1000 ahead of the resolved locator (S2). This is the bulk-IBD
  response.
* **`0`** — the client's locator has reached the peer's tip (S3/S4), or `hashStop` is ≤ 1
  above the locator and the stake-age tip `INV` gate fails (S1). An empty response is
  *silent*: no `inv` is queued, so the client's single-flight cycle only closes via the
  15 s timeout (`ExpireGetBlocksOutstanding`, `net.cpp:6492-6529`;
  `GETBLOCKS_RESPONSE_TIMEOUT = 15`, `net.h:1057`).
* **`1`** — exactly two distinct code paths:
  * **tip path**: resolved locator == tip − 1 → the walk emits exactly the tip block
    (S3 after one iteration);
  * **stop path**: `hashStop` lies exactly one above the resolved locator **and** the
    stake-age gate passes → a single tip `INV` (S1, `main.cpp:9236-9246`).

Both 1-item paths require the client's locator to be at (or one below) the peer's current
tip. Neither is reachable while the client is genuinely millions of blocks behind — in
that regime the response is ~1000 (S2).

---

## 5. The server gate: `Evaluate()` and what each decision actually emits

`Evaluate()` (`net.cpp:647-789`) runs before the walk. Constants (`net.cpp:505-510`):

```cpp
GETBLOCKS_TOKEN_BUCKET_CAPACITY    = 30000   // milliTokens
GETBLOCKS_TOKEN_REFILL_PER_SECOND  = 1000
GETBLOCKS_COST_ITEMS               = 250     // items per cost step
GETBLOCKS_REPEAT_BASE_COOLDOWN_MILLIS = 2000
GETBLOCKS_REPEAT_PENALTY_INTERVAL  = 128
GETBLOCKS_REPEAT_DISCONNECT_THRESHOLD = 512
```

Cost model (`net.cpp:619-624`): `1000 + ceil(nItems / 250) * 1000` milliTokens. Bucket
starts full, refills 1000 mT/s, so a 1000-item response (5000 mT) is affordable roughly
every 5 s of idle; a 1-item response costs 2000 mT.

Decision construction (`net.cpp:653-694`): progress is declared if any of — locator
advanced, stop advanced, next range reached, or the client issued *useful `getdata`*
against the last response. A non-progressing identical/same-response request is
"repeated range" (`net.cpp:716-718`) and gets:

* cooldown `2000 << min(nConsecutiveNonProgressing / 16, 4)` ms (`net.cpp:626-631`);
* a penalty of 5 misbehavior points every 128th consecutive non-progressing request for
  strict inbound peers (`net.cpp:726-734`);
* `DISCONNECT` at 512 consecutive (`net.cpp:736-741`);
* otherwise `SUPPRESS` while the cooldown has not elapsed (`net.cpp:742-745`);
* finally, a full `ALLOW` still goes through the token bucket and becomes `RATE_LIMIT`
  when the bucket is short (`net.cpp:758-776`).

The critical fact for this audit is in the caller (`main.cpp:9192-9217`):

```cpp
if (decision.action != GETBLOCKS_SERVER_ALLOW)
{
    ... // RATE_LIMIT counter, SUPPRESS/RATE_LIMIT/DISCONNECT logs
    return true;                       // <-- no inv is ever queued
}
```

So the three gate outcomes other than `ALLOW` all produce **zero** items and **no `inv`
message** on the wire. Consequence:

> Suppression, rate-limiting, and disconnection can never be the origin of an `INV` of
> size 1. Their observable on the client is "no response" → the 15 s getblocks timeout,
> not a 1-item batch.

This is why the earlier "repeat-suppression → `INV=1`" theory is **DISPROVEN** by code.
Suppression suppresses to *nothing*, never to one. When a peer repeats the same locator
and gets suppressed, the client sees silence and retries via the timeout path
(`net.cpp:6511-6528` clears the per-peer dedup identity so the retry is not blocked).

---

## 6. State-machine reconstruction and arrow classification

Client-side single-flight cycle (`net.h` 2100–2122, `net.cpp:6457-6529`): one outstanding
`getblocks` per peer; any `inv` while outstanding closes it heuristically
(`main.cpp:8921-8922`); otherwise it expires after 15 s.

Server-side per-peer state (`CGetBlocksServerState`, `net.cpp:558-591`): last locator tip,
resolved height, stop, response hashes, token bucket, consecutive identical /
non-progressing counters, and a `hashContinue` continuation marker.

The reconstructed loop (left = client→server, right = server→client):

```text
[client] locator L, stop 0
   |-- getblocks(L, 0) ---------------------------->
   |                                           [server] Evaluate(L)
   |   ALLOW, bucket ok                     -->
   |        walk: pnext(L)..tip, cap 1000
   |        <------------------------------------ INV(n = gap, capped 1000)
   |        (if gap == 0: nothing; client times out at 15 s)
   |        (if gap == 1: INV(1) — tip path)      <-- "E2"
   |        (if gap >= 1000: INV(1000), hashContinue = last)   <-- "E1"
   |   nExpectedBatchSize = n
   |   request blocks via getdata (windowed)
   |   ... hashContinue block getdata'd -->
   |                                      [server] sends block + INV(1) {tip}   <-- "E3"
   |   INV(1) -> nExpectedBatchSize = 1 -> prefetch 1/1 -> getblocks(tip, 0)
   |        <------------------------------------ INV(0 or 1): next tip / silent
   [server] repeated same locator, no progress, no useful getdata
   |        <------------------------------------ (nothing) SUPPRESS/RATE_LIMIT
```

Arrow-by-arrow classification:

| Arrow | Transition | Evidence | Class |
|---|---|---|---|
| E1 | `gap ≥ 1000` ⇒ `INV(1000)` | walk S2, `main.cpp:9227-9265` | **PROVEN** |
| E2 | `gap == 1` ⇒ `INV(1)` (tip path) | walk S3 after one iteration | **PROVEN** |
| E3 | last-batch block requested ⇒ server sends **1-item continuation `INV`** | `main.cpp:8447-8457` | **PROVEN** |
| E4 | `gap == 0` ⇒ silent, client 15 s timeout | walk S3/S4; `net.cpp:6492-6529` | **PROVEN** |
| E5 | non-progressing repeat ⇒ `SUPPRESS`/`RATE_LIMIT`/`DISCONNECT`, **no `INV`** | `net.cpp:716-776`, `main.cpp:9192-9217` | **PROVEN** |
| E6 | *observed* tail `1/1` flood is dominated by E2 (+E3) rather than by relay `INV`s | no origin tag in stored telemetry | **UNKNOWN** (see §10) |
| E7 | suppression *produces* `INV=1` | contradicts `main.cpp:9217` | **DISPROVEN** |
| E8 | server randomly truncates a large batch to 1 | no such code; deterministic walk | **DISPROVEN** |
| E9 | client *chooses* a 1-block request | `nExpectedBatchSize` is set only from received `INV` | **DISPROVEN** |
| E10 | DAG-expanded traversal returns small responses | removed (`051912e`, `6885e9f`); zero occurrences in tree | **DISPROVEN** |

---

## 7. The `hashContinue` mechanism: the deliberate 1-item `INV`

`hashContinue` is written on the *server* side only, in two places:

* **Set** at the 1000-item limit (`main.cpp:9262`): `pfrom->hashContinue = hashBlock;`
  with the comment "When this block is requested, we'll send an inv that'll make them
  getblocks the next batch of inventory."
* **Cleared** after serving it (`main.cpp:8447-8457`), inside `ProcessGetData`:

```cpp
// Trigger them to send a getblocks request for the next batch of inventory
if (inv.hash == pfrom->hashContinue)
{
    vector<CInv> vInv;
    vInv.push_back(CInv(MSG_BLOCK, hashBestChain));   // exactly ONE item
    pfrom->PushMessage("inv", vInv);
    pfrom->hashContinue = 0;
}
```

* **Reset** to zero at connect (`net.h:1370`).

So at the end of every full 1000-item batch, the serving peer emits an `inv` containing
**exactly one block — its current tip**. The comment explicitly states the design intent:
the client should then `getblocks` the next batch. This `INV` is:
* sent immediately after the last block (not after other queued traffic);
* deliberately *not* deduplicated via `PushInventory`/`setInventoryKnown` (comment at
  `main.cpp:8450-8452`);
* the only `hashContinue` read site on the wire path (`main.cpp:8448`); there is no
  server-side skip/alter of the walk based on it.

Client consequence: this 1-item `INV` lands in the `inv` handler and, because it is
block-bearing, resets the batch bookkeeping — see Section 8. It produces exactly one
`1/1` prefetch log per completed 1000-batch (i.e. ≈130 in the observed runtime, matching
the ≈130 full batches; it cannot by itself explain 15,535 events — hence E6 is UNKNOWN).

History: the mechanism is inherited from the upstream Bitcoin/Blackcoin lineage; the
bounded-request state was shaped by `690a674`, and the batch limit was raised 500 → 1000
in `581a59a`. `hashContinue` is an "end-of-page marker + advance signal", not a pagination
cursor: it is never read to *skip* blocks in the walk.

---

## 8. Client side: `nExpectedBatchSize`, prefetch, continuation — the meaning of `1/1`

The `inv` handler sets the batch bookkeeping on **any** block-bearing `inv`, response or
not (`main.cpp:8883-8893`):

```cpp
if (nBlockCount > 0)
{
    pfrom->nBlocksReceivedInBatch = 0;
    pfrom->nExpectedBatchSize = nBlockCount;      // remote-determined
    pfrom->fPrefetchSent = false;
    pfrom->hashLastBlockInBatch = <last MSG_BLOCK hash>;
}
```

The block handler then increments the counter and triggers prefetch at 75% of the expected
batch (`main.cpp:9973-10029`):

```cpp
pfrom->nBlocksReceivedInBatch++;
int nPrefetchThreshold = (pfrom->nExpectedBatchSize * 3) / 4;
if (!pfrom->fPrefetchSent && nBlocksReceivedInBatch >= nPrefetchThreshold)
{ ... PushGetBlocks(pindexLast, 0, PREFETCH) ... }
printf("Prefetch: Requesting next batch at %d/%d blocks ...\n", ...);
```

For a batch of 1, the threshold is 0, so the *first* received block fires prefetch and
logs `1/1`. Therefore:

> `Prefetch: Requesting next batch at 1/1 blocks` is the client's *reaction* to having
> received a 1-item `INV`. It is never a client-side decision to request one block.

The prefetch locator is the **last block of the received batch** (`hashLastBlockInBatch`,
`main.cpp:9981-9986`), not the connected tip — so the batch stays ~1000 as long as the
peer is ≥1000 ahead. The batch collapses to 1 exactly when the received `INV` itself was
1-item (Sections 4/7) or when `hashLastBlockInBatch` is within 1 of the peer tip.

Other client senders (all pass locator = `pindexBest`, i.e. the *connected* tip):

| Source | Trigger | Lines |
|---|---|---|
| `PREFETCH` fallback | prefetch threshold, `pindexLast` missing | `main.cpp:10005-10027` |
| `CONTINUATION` | pipeline drained + 10 s cooldown + peer ahead | `main.cpp:10048-10082` |
| `INV_CONTINUATION` | last block of `INV` already known and peer can advance | `main.cpp:9022-9038`, `net.h:2147-2150` |
| `ORPHAN_LIMIT` | orphan store full during IBD | `main.cpp:7558-7560` |
| `VERSION` / `fStartSync` | initial sync | `main.cpp:8701-8710`, 10642–10650 |

The `CONTINUATION` path (`main.cpp:10031-10082`) is the one that logs
`GETBLOCKS_CONTINUE ... reason=empty-inflight` with `nExpectedBatchSize`; it fires only
when `setBlocksInFlight.size() <= 1 && getBlocksIndex.empty()` and the cooldown has
expired. With a 1-item `INV`, the pipeline is nearly always drained, so this path fires
repeatedly — each time re-asking with a locator at (or one below) the peer tip, which the
server answers with 1 item (tip path, E2) or silence (E4).

---

## 9. Runtime correlation and the alternative-explanations verdict table

Accepted runtime baseline (from prior forensic captures):

* ~130 full 1000-batches; 16,628 single-block batches; 15,535 `1/1` prefetch logs;
* 15,853 getblocks/getdata timeouts; 14,216 late arrivals (89.7%); problem peer ping ≈ 13.9 s;
* 23,002 `ORPHAN_LIMIT_IBD` rejections; bulk peaks 500–1000 blk/s decaying to single digits.

Code-level interpretation of the shape of that data:

1. The ~130 × 1000 batches are the deterministic S2 regime: client locator deep behind the
   peer, `INV(1000)` responses, one `1/1` prefetch per batch from the Section 7
   continuation `INV` (E3).
2. The 16,628 single-block batches dominate the tail: the client has reached the peer's
   tip and is *chasing a moving tip*. Every new block the peer connects advances its tip
   by one; the next `getblocks(locator=tip−1)` is answered with `INV(1)` (E2). Each such
   cycle involves one block request, which — at 13.9 s RTT against a 5 s request timeout —
   times out and arrives late (≈89.7% late), in turn filling the orphan store and driving
   `ORPHAN_LIMIT_IBD` (the 23,002 count and the `PushGetBlocks(pindexBest,0)` retry at
   `main.cpp:7558-7560`).
3. Server-side suppression accounts for some of the "silent" gaps between batches (E5 →
   E4), visible as the 15 s outstanding timeouts, but it cannot produce any `INV=1`.

Verdict table for every candidate explanation of `INV=1`:

| # | Candidate explanation | Evidence | Verdict |
|---|---|---|---|
| V1 | "Peer replies `INV=1` instead of ≈1000" (the claim) | remote-determined; deterministic walk; E2/E3 produce 1-item `INV` | **SUPPORTED** (with the tip-chase refinement, §1) |
| V2 | Suppression/rate-limit shrink a response to 1 | returns before any `INV` is queued | **DISPROVEN** |
| V3 | Server randomly truncates large batches | no such code | **DISPROVEN** |
| V4 | Client explicitly requests 1 block | `nExpectedBatchSize` set only from received `INV` | **DISPROVEN** |
| V5 | DAG-expanded traversal returns 1 | removed from tree | **DISPROVEN** |
| V6 | 1-item `INV` = hashContinue continuation (batch boundary) | `main.cpp:8447-8457` | **PROVEN** (≈1 per batch; ≈130) |
| V7 | 1-item `INV` = tip-path getblocks response (client at tip−1) | walk S3 | **PROVEN** (dominant tail candidate) |
| V8 | 1-item `INV` = unsolicited single-block relay | any block-bearing `INV` resets the batch | **PLAUSIBLE** (indistinguishable in stored data) |
| V9 | which of V6/V7/V8 generated each of the 15,535 `1/1` logs | no origin tag captured | **UNKNOWN** |

---

## 10. Recommendation

The mechanism question is answered by code: the serving peer's `INV` size is
remote-determined and deterministic, the client is a pure reflector of that size, and
suppression can never yield `INV=1`. What the stored data cannot split is the *origin* of
each 1-item `INV` (V6 continuation vs V7 tip-path response vs V8 relay). That split is the
only remaining question, and it is a telemetry gap, not a code defect.

Minimal additional instrumentation (all diagnostic; no production behavior change):

1. **Tag `INV` origin at the `inv` handler** (`main.cpp:8883-8893`): record for each
   block-bearing `INV` whether it arrived while `HasOutstandingGetBlocks()` (response), is
   the Section 7 continuation `INV` (match `hashBestChain` while `fFrontierResponsePending`
   is unset and the previous `INV` was the last block of a batch), or is unsolicited
   (relay). Emit `INV_ORIGIN time_us=... n=... source=<response|continue|relay>`.
2. **Capture server-side response size at the flush** (`main.cpp:10820-10845`) into an
   existing counter family (e.g. `getblocks_response_inv_messages` in `ibdmetrics.h`) with
   a histogram bucket: `n=0`, `n=1`, `1<n<1000`, `n=1000`.
3. **Correlate** the 1-item `INV` origin against `nConsecutiveNonProgressingRequests` and
   `nResponsesSuppressed` (already in `CGetBlocksServerState`, `net.cpp:578-589`) to
   confirm that suppression intervals coincide with *silence*, not `INV=1`.
4. Re-run the tail-of-IBD scenario with `-ibdforensic=1` + `BLOCKREQTRACE` +
   `GETBLOCKS_*` counters as documented in
   `doc/forensics/ibd-forensic-instrumentation.md`, and confirm that the `1/1` flood tracks
   tip-chase responses (V7) with a small V6/V8 contribution.

If the runtime confirms V7 dominates, no server-side change is warranted: a tip-chasing
client against a slow peer is correct protocol behavior, and the real lever is the block
request timeout / multi-peer reassignment already covered by the closed `30be277` work.

---

## 11. Revision after adversarial review

The earlier interpretation that tip-path (V7) dominates the observed 1/1 events is no
longer considered sufficiently supported.

The new analysis introduces strong evidence that relay INV (V8) may dominate instead.

However, existing telemetry cannot attribute individual 1-item INV messages to their
origin.

Therefore the dominance question remains unresolved until INV-origin instrumentation is
added.

### Why V7 (tip-chasing) is now considered insufficient

Three independent lines of evidence, none present in the original Section 9 reading, argue
against a client sitting at `locator = tip−1` during the tail:

1. **The orphan count contradicts tip−1 operation.** `ORPHAN_LIMIT_IBD`
   (`main.cpp:7554-7570`) fires only when a received block's parent is unknown
   (`mapOrphanBlocks`). A client at `tip−1` holds the parent of every block it requests, so
   orphans near the tip require blocks to arrive faster than one RTT and cannot sustain
   23,002 store-saturations. The observed flood of orphans requires the client to keep
   receiving blocks ≥ 2 ahead of its *connectable* frontier, i.e. it is behind — not at the
   tip.

2. **The reported batch-size distribution is bimodal** (~1000 or ~1, with no meaningful
   intermediate sizes). A client approaching the peer tip through getblocks would receive a
   graduated series (`INV(999)` … `INV(1)`). Bimodality is the signature of two *separate*
   streams — getblocks responses (1000, or suppressed) and 1-item relay `INV`s — not of a
   response that gradually shrinks.

3. **Block relay does not depend on the client's position.** On connecting a new block, a
   node announces it to every peer behind by ≤ 2000 blocks — including peers far behind
   (`AcceptBlock`, `main.cpp:7092-7103`). A peer "millions of blocks ahead" therefore emits
   a 1-item `INV` for *every* new network block regardless of how far the client lags. This
   is a mechanism for a sustained 1-item stream that requires no client progress at all.

### The relay-driven alternative (V8) in code

The V8 loop is fully supported by the current tree:

* relay `INV(1)` on new tip block — `main.cpp:7092-7103`;
* client batch reset to size 1 — `main.cpp:8883-8893`;
* relay block admitted/requested (budget-gated) — `main.cpp:424-514`, `main.cpp:9003`;
* arrival counted unconditionally during IBD; prefetch threshold `(3·1)/4 = 0` fires `1/1`
  on the first arrival — `main.cpp:9973-10029`;
* missing parent → orphan, and the client asks the same peer for the orphan's parent
  (walk-back) — `main.cpp:7608-7614`, `main.cpp:9008-9017`, `WantedByOrphan`
  (`main.cpp:2903`); the walk-back chain saturates the per-peer request window;
* orphan store saturates → `ORPHAN_LIMIT_IBD` (23,002) and `PushGetBlocks(pindexBest, 0)`
  with the frozen, deep locator — `main.cpp:7554-7570`;
* relay-block `getdata` does **not** count as "useful" for the server's suppression reset
  (`NoteBlockGetData` requires membership in the *last response* range, `net.cpp:839-863`),
  so repeated frozen-locator getblocks stay suppressed — `net.cpp:716-745`,
  `main.cpp:9192-9217` — and end in the 15 s getblocks timeout (`net.h:1057`,
  `net.cpp:6492-6529`).

This loop reproduces the full counter profile: 16,628 single-block batches and 15,535 `1/1`
logs from relay `INV`s; 23,002 orphan rejections from the walk-back; 15,853 timeouts /
89.7% late from `BLOCK_IN_FLIGHT_TIMEOUT = 5` (`net.h:2167`) against a 13.9 s RTT; ~130
full batches from the rare windows in which the window briefly freed and the server was
allowed to answer.

### What remains unresolved

V6, V7 and V8 all legitimately produce 1-item `INV`s, and the stored session
(`session-ses_02f1.md`) contains no per-`INV` origin tag and no locator-height capture
(`from height N` in `main.cpp:10002`; `local_height=`/`peer_height=` in `main.cpp:10052`).
The relative contribution of each source to the 15,535 `1/1` logs therefore cannot be
measured retrospectively. Resolving the dominance question requires the INV-origin
instrumentation listed in Section 10 (items 1–3), at which point V6 vs V7 vs V8 can be
attributed per message.

---

## Appendix: Explanatory Runtime Model

This appendix is not a reconstruction of the production implementation.

It is a simplified executable model whose purpose is to determine whether the observed
runtime behaviour can arise from the already verified mechanisms.

Agreement with runtime should not be interpreted as proof that production follows the same
causal chain. The model is evidence-guided, not evidence itself.

To decide whether the degraded steady state is a self-sustaining loop and, if so, which
request origin starves the BULK batch path, a discrete-event model of the *existing*
architecture was built and run: `contrib/forensics/degradation_model.py`. It attributes
every in-flight window slot to an origin (BULK / CONTINUATION / RELAY / ORPHAN_PARENT /
CROSS_PEER / OTHER) using the tree constants (window 128 `net.h:564`, in-flight timeout 5 s
`net.h:2167`, getblocks timeout 15 s `net.h:1057`, orphan cap 750 `main.h:102`, batch 1000
`main.cpp:9222`, continuation cooldown 10 s `main.cpp:10036`).

Model assumptions (disclosed):
* single slow serving peer (the premise of this audit), server transmit rate `--bw` in
  blk/s, one-RTT-delayed getblocks responses, per-response drop probability `--p-drop`
  (proxy for the ~10% never-arriving requests recorded at runtime);
* **no automatic re-ask of timed-out blocks** (`net.h:2165-2236`): a timed-out hash returns
  only when a new announcement re-lists it (continuation batch, relay INV, orphan-parent
  walk-back) — hence no `TIMEOUT_REISSUE` origin;
* no TCP backpressure, no multi-peer reassignment, relay stream = one tip block per
  `--net-interval` seconds.

### A.1 6-hour bandwidth sweep

| `--bw` | BULK slots | RELAY slots | ORPHAN_PARENT slots | conn (6 h) | gap | orphan-limit events | timeouts |
|---|---|---|---|---|---|---|---|
| 0.2 | 99.9% | 0.1% | <0.1% | 50 | 1352 | 148 | 319,866 |
| 0.5 | 99.9% | 0.1% | <0.1% | 17 | 1352 | 258 | 335,937 |
| 1 | 99.9% | 0.1% | <0.1% | 46 | 1324 | 542 | 360,421 |
| 2 | 99.8% | 0.1% | 0.1% | 112 | 350 | 313 | 287,682 |
| 5 | 98.3% | 1.1% | 0.5% | 214 (+207 relay) | **1** | 250 | 32,192 |
| 12 | 80.5% | 11.9% | 6.2% | 321 relay | **1** | 317 | 2,923 |

(`getblocks`: at `--bw ≥ 5` the response mix flips to 1-item/empty — 344 `1item` + 744
`empty` at bw=5; the last bulk slot is admitted at t≈6.5 h at bw=5 vs t≈5 min at bw=12.)

### A.2 Results

1. **What displaces BULK.** In the degraded regime (bw ≤ 2) *nothing* displaces BULK slot
   occupancy — it stays ≥ 99.8% for the full 6 h. What is displaced is *effective
   connection*: the batch keeps occupying the window while `h_conn` makes only staircase
   progress (see 4). RELAY + ORPHAN_PARENT exceed a few tenths of a percent only in the
   tip-chase regime (bw ≥ 5), where RELAY becomes the primary connector (207–321 tip-block
   connections) and the bulk batch collapses to 1-item/empty responses. So the "displacement"
   seen in the runtime's single-block flood is the client reaching the batch boundary, not
   non-bulk requests starving bulk *slots*.
2. **Per-origin window fractions.** Degraded: BULK 99.9%, ORPHAN_PARENT ≤ 0.1%, RELAY
   ≤ 0.1%, CONTINUATION 0 (every response ≥ 2 items). Tip-chase: BULK ≈ 80% (residual of
   the last batch), RELAY ≈ 12%, ORPHAN_PARENT ≈ 6%.
3. **Event sequence that eliminates BULK.** Staircase: batch blocks arrive → orphan store
   fills to the 750 cap → rejections (`ORPHAN_LIMIT_IBD`) → the missing parent is resolved
   by a re-listed ask → store flushes and `h_conn` jumps ≈ 1000 → repeat until `h_conn`
   crosses the batch boundary; only then do getblocks responses become 1-item/empty and
   BULK slots vanish.
4. **Self-sustaining loop: yes**, for bw ≲ 3 and RTT < 15 s. Mechanism: in-flight timeout
   5 s ≪ RTT 13.9 s means each serial path delivers at most one connection per RTT; ~10%
   drops create permanent gaps; each gap floods the orphan store to the cap; cap rejection
   discards the missing parent from the client's received set, and its re-ask is buried
   behind the server's transmit backlog; the client then re-lists `[locator+1..+1000]`
   forever at ≥ 99% BULK occupancy while `h_conn` makes only staircase progress. The gap
   closes only when the server rate reaches bw ≥ 5 blk/s (the client then outruns the
   backlog and reaches the tip).

### A.3 Hard failure cliff at `RTT > GETBLOCKS_RESPONSE_TIMEOUT`

| `--rtt` | connected tip (1 h, bw=2) |
|---|---|
| 12 | 1008 |
| 14.9 | 1008 |
| 15.1 | 0 |
| 16 | 0 |
| 20 | 0 |

When RTT exceeds the 15 s getblocks timeout, the response to `getblocks` itself times out
before arriving, so the client never learns the batch and `h_conn` stays 0. The runtime's
13.9 s ping sits just below this cliff, which is consistent with the rare 15 s getblocks
timeouts and the near-continuous batch downloads observed.

### A.4 Implication for Section 11

The model supports the Section 11 revision that the client is *behind* (not at `tip−1`)
during the degraded phase: at bw ≤ 2 the connected tip is stuck at 1008–2009 against a
moving tip (gap 350–1352) while the batch path occupies ≥ 99.8% of the window. The V7
tip-chase interpretation remains valid only for the *later* (bw ≥ 5) phase, in which the
single-block flood and `1/1` logs are reproduced as RELAY-driven connections. The INV-origin
telemetry of Section 10 items 1–3 remains the only way to split the two phases in the stored
runtime data.
