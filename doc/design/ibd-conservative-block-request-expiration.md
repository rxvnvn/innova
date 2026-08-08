# Conservative Block Request Expiration — Final Design

## Status

- **Status:** FINAL / NOT IMPLEMENTED
- **Basis:** ABRE v1 measurement campaign + economic replay (ABRE v1 §§13–14) + this
  closing sensitivity analysis on both recorded traces
- **Selected policy:** wire-origin conservative timeout, `T = 60 s`, per-hash expiry
  clocked from the getdata message's first socket `send()`, with a separate
  pending-wire bound. Fixed per-hash retry timeouts are removed.
- **Production behavior:** unchanged (design only)
- **Supersedes:** `ibd-adaptive-block-request-expiration-v1.md` (adaptive machinery is
  rejected; see §4)

## 1. Selected policy (one line)

Replace `ExpireBlockInFlight()`'s clock origin (currently `mapBlockInFlightSince` set at
`MarkBlockInFlight`, i.e. enqueue) with the wire-send time of the getdata batch, and the
hardcoded `BLOCK_IN_FLIGHT_TIMEOUT = 5` with a conservative `60 s` wire-origin deadline.
Do **not** build the adaptive estimators (slot0/perBlock), position correction, or
per-batch correlation map of ABRE v1.

## 2. Closing sensitivity analysis (offline replay of both traces)

Method: for every forensic entry, wire = per-batch `max(first_socket_send_us)`
(one getdata message == one batch). For a candidate `T`, a *delivered* block is
"false-expired" iff `recv − wire > T`; a *never-delivered* block is "detected" at
`wire + T` (detection latency ≈ `T`). Duplicate-rerequest work is predicted as the number
of delivered blocks the new policy would expire and re-request yet that arrive anyway.

### 2.1 IMPAIRED (5% loss / RTT 13.9 s; 7 966 entries, 980 batches, 6 836 delivered, 1 130 never)

Live wire-origin latency: `p50=14.08s p75=14.13s p90=49.05s p95=58.04s p98=72.12s p99=89.30s p99.9=156s max=219s`

| T | delivered falsely expired | % of 6836 | duplicate-rerequest predicted | never detected | head-never (frontier) |
|---|---|---|---|---|---|
| 5 s (status quo) | 6 836 | 100.0% | 6 836 | 1 130 @5s | 141 |
| 15 s | 929 | 13.6% | 929 | 1 130 @15s | 141 |
| 30 s | 889 | 13.0% | 889 | 1 130 @30s | 141 |
| **45 s** | **716** | **10.5%** | **716** | **1 130 @45s** | **141** |
| **60 s** | **242** | **3.5%** | **242** | **1 130 @60s** | **141** |
| 75 s | 112 | 1.6% | 112 | 1 130 @75s | 141 |
| 90 s | 68 | 1.0% | 68 | 1 130 @90s | 141 |
| 120 s | 25 | 0.37% | 25 | 1 130 @120s | 141 |

Histogram of live `recv−wire` (IMPAIRED): 86.4% in [10–15 s], 0 in [0–10 s), then a
long tail: [15–20]=11, [20–30]=29, [30–45]=173, [45–60]=474, [60–75]=130, [75–90]=44,
[90–120]=43, [>120]=25.

Key shape: the bulk arrives at ~1×RTT (14 s). The tail that survives past 5 s is
clustered 45–75 s. Between T=30 and T=60 the false-expiry rate collapses 13.0%→3.5%;
between T=60 and T=90 it only drops 3.5%→1.0%. Marginal gains beyond 60 s are small and
the detection-latency cost grows linearly. **60 s is the knee of the curve** (see §3).

### 2.2 CONTROL (healthy; 59 159 entries, 52 844 batches, 59 158 delivered, 1 never)

Live wire-origin latency: `p50=1.58s p90=2.10s p95=2.22s p98=2.35s p99=2.63s max=18.75s`

| T | delivered falsely expired | % | duplicate predicted | never detected |
|---|---|---|---|---|
| 5 s (status quo) | 110 | 0.19% | 110 | 1 @5s |
| 15 s | 51 | 0.09% | 51 | 1 @15s |
| 20 s | 0 | 0.00% | 0 | 1 @20s |
| 30–120 s | 0 | 0.00% | 0 | 1 |

CONTROL is fully covered by any T ≥ 20 s; 60 s has 3× margin.

### 2.3 Never-delivered / pending-wire edge

- IMPAIRED: 1 130 never-delivered, **141 head (seq 0)**, of which **14 batches** have a
  never-delivered head while their tails were delivered (true frontier blockers). All are
  detected at wire+T under every candidate; conservative T only delays their re-request.
- CONTROL: the single never-delivered block (batch 56132, seq 0) was actually sent on the
  wire (`first_socket_send_us` > mark) and simply never received — it is a genuine loss,
  not a pending-wire case. It will be detected at wire+T.

## 3. Classification of the 60 s value

**DATA-DERIVED KNEE, with an engineering margin.** Justification:

1. IMPAIRED false-expiry vs T drops steeply up to ~60 s (30 s→13.0%, 45 s→10.5%,
   60 s→3.5%) then flattens (75 s→1.6%, 90 s→1.0%). The marginal reduction per added
   second collapses after 60 s.
2. 60 s covers 100% of CONTROL deliveries (max 18.75 s) with a 3× margin, and 96.5% of
   IMPAIRED live deliveries; the residual 3.5% is the genuinely pathological tail
   (some >150 s).
3. It is *not* a data-derived optimum: there is no scalar optimum here because
   raising T trades duplicate-work/false-expiry against loss-detection latency linearly.
   60 s is the knee point where the two costs balance on the recorded data.

It is NOT ARBITRARY (it sits at the measured knee), and NOT an optimum (no unique
minimizer exists). It is a conservative engineering bound chosen at the knee.

## 4. Why ABRE v1's adaptive machinery is rejected (summary)

Economic replay (ABRE v1 §13): fast per-hash retry saves ≈ 0 useful frontier
(`saved_by_retry≈0`), 58% of expiries would arrive anyway, 52.5% of rerequests are
duplicates, and the only true frontier blockers are 14 batches. Occam comparison
(§14): adaptive ABRE-D, progress-hybrid, and delivery-frontier models are each no better
than a fixed wire-origin 60 s on false-expiry (3.7%/5.2% vs 3.5%), strictly worse on loss
detection (857/725 losses left pending vs 0), and require the full estimator/batch
machinery. Simplest data-supported semantics wins.

## 5. Clock origin (production semantics)

- **Origin:** wall-clock at the moment the getdata message is first written to the socket
  (`SendMessageMeta::firstSendUs`, net.h:1100-1111; stamped in `SocketSendData`,
  net.cpp:7104-7109). This field already exists and is observation-only; promoting it to
  drive expiry is the minimal change.
- **Why:** `MarkBlockInFlight` (net.h:2250) stamps `mapBlockInFlightSince` at
  enqueue/build time. Any local queue delay (SendMessages batching, 1000-cap flush,
  cs_vSend contention) would count against the peer; the traces show queue_delay is
  ~0 in practice, but wire-origin removes the class of error entirely and is the only
  semantically correct choice.
- **Pending-wire bound:** a hash that is marked in flight but whose getdata message has
  not yet been sent must still expire (safety net), keyed to a small fixed bound from
  enqueue (not the full 60 s). ABRE v1 called this `maxPendingWireUs` (2 ms observed
  ceiling). This is required so the CONTROL-style edge cannot hang.

## 6. Expiration unit

- **Clock per batch, expiry per hash.** The wire timestamp is per getdata message (one
  send stamps all hashes in that batch); the timeout decision stays per-hash exactly as
  `ExpireBlockInFlight` iterates today (net.h:2169-2242). No per-batch deadline / no
  position-correction is needed.
- Rejected alternative "expire whole batch": would evict delivered hashes still in the
  batch tail; the per-hash iterate already handles partial delivery correctly.

## 7. Ownership & late-delivery semantics (30be277 invariant preserved)

- `ReleaseBlockRequestOwnerOnReceive` (net.cpp:3267-3295) already refuses to release
  ownership when the delivering peer is not the current owner; a late block from the
  previous owner after reassignment is counted (`block_owner_receive_mismatch_preserved`)
  but does not disturb the new owner. Clock-origin change does not touch this path.
- `ExpireBlockInFlight` releases owner with reason "timeout" only when
  `setBlocksInFlight.erase()` succeeds (net.h:2185), i.e. only live requests; a stale
  snapshot can't double-release gauges. Preserved.
- `ClearBlockInFlight` (net.h:2274) on a late arrival after timeout consumes the stored
  late-delivery expectation and records the outcome to the *timeout* owner without a
  second lifecycle decrement (net.h:2296-2311). Preserved.
- The only change: the deadline value used to trigger these paths moves from
  `enqueue + 5 s` to `wire + 60 s`.

## 8. Peer-quality scoring interaction

- `timeout_score` increments only on `RecordIbdBlockTimeout` (net.h:2379-2392). Under 60 s
  the IMPAIRED peer produces 242 timeouts instead of 6 836, so a slow-but-live peer no
  longer accumulates a large timeout_score purely from being slow. This is a *benefit*:
  the peer's tier (TierFromScores, net.h:2401) reflects real behavior, not policy clock.
- `receive_score` / `latency_ewma` are updated on every delivery regardless of expiry
  (net.h:2334-2376), so a healthy peer retains high ranking even if one or two requests
  cross the 60 s line. `received_after_timeout`/`late_delivery_score` only count genuine
  late arrivals (>60 s), which is the correct signal.
- No scoring field is removed; the change only suppresses false timeouts. No audit
  finding: interaction is safe and directionally positive.

## 9. Scope of the production fix (minimal, when implemented)

1. net.h:2169 — `ExpireBlockInFlight`: clock origin = wire timestamp; deadline =
   `wireUs(batch(hash)) + 60 s`; keep per-hash iteration and all release/owner/late
   hooks byte-identical.
2. Stamp per-hash (or per-batch) wire time from `SendMessageMeta::firstSendUs` in
   `SocketSendData`; add a hash→batch association (small map or reuse the batch id) —
   the smallest plumbing to read the wire time for a hash.
3. Add the pending-wire bound (from enqueue, small fixed value) so an unsent getdata
   still expires.
4. Remove nothing else; do NOT add estimators, per-position logic, or fallback tables.
5. Only if evidence demands: re-check the 60 s constant against a fresh trace. Constant
   should be a named `static const`, documented as data-derived knee.

## 10. Regression tests required (before merging the eventual fix)

1. **Wire-origin clock unit test**: mark a hash, defer its getdata send by N s, assert it
   does not expire until wire+N+60 (not mark+60).
2. **Pending-wire safety net**: mark a hash, never send its getdata; assert it still
   expires via the enqueue-bound and releases owner/gauges exactly once.
3. **Owner-identity on late receive (30be277)**: expire → reassign to peer B → late block
   from peer A; assert owner of B preserved, `block_owner_receive_mismatch_preserved`
   incremented, gauges not double-decremented.
4. **Gauge balance**: timeouts + late receives vs issued must leave active/inflight
   counters at zero at shutdown (existing invariant harness).
5. **Tier / quality**: a peer delivering at ~14 s must not acquire a timeout-tier under
   60 s (regression on `timeout_score` inflation).
6. **Functional regtest replay** of the IMPAIRED trace: fixed 60 s must yield
   ≈ 242 false expiries / 0 pending losses (matches §2.1).
7. **Frontier stall**: never-head batches must be re-requested at wire+60 s and the IBD
   pipeline must not block longer than that bound.

## 11. Documentation disposition

- ABRE v1 (`ibd-adaptive-block-request-expiration-v1.md`) is the **historical record** of
  the measurement campaign and the adaptive design that was investigated and **rejected**
  for production. Its Status block is updated to `SUPERSEDED` and cross-links this
  document. Sections 13–14 (economic/Occam verdicts) remain valid as the evidence base.
- This document (`ibd-conservative-block-request-expiration.md`) is the **final design**
  for implementation and supersedes ABRE v1 for the expiry policy.
- No separate "implementation" doc is needed; this file is the single source of truth.

## 12. Open items / out of scope

- Multi-peer (non-loopback) real-network evidence for cross-peer re-request benefit
  (ABRE v1 §13.4) remains unmeasured; the conservative choice is robust to it either way
  (it never worsens duplicate work).
- A pure black-hole / silent-peer trace (peer that completes handshake, accepts getdata,
  then sends nothing) is not present in either recorded trace; the socket inactivity
  check (net.cpp:7579-7607, 20-min TIMEOUT_INTERVAL) is the only independent backstop and
  is far slower than 60 s. See §13 for the recommended controlled test if that behavior
  becomes operationally relevant.
- `-ibdstaleseconds` (main.cpp:3618) and stalled-sync recovery (net.cpp:923-979,
  `-syncstalltimeout`) are orthogonal peer-level recoveries and are unchanged.

## 13. Recommended controlled test (if silent-peer behavior is material)

If a production peer is suspected of black-holing (connected, handshake OK, never answers
getdata), run the existing impaired-proxy harness (used for B2/B3 in
`ibd-degradation-runtime-validation.md`: ~10% drop / 6950 ms one-way) with a mode where
the proxy drops 100% of block responses but forwards version/verack/inv. Expected under
the 60 s policy: the frontier stalls up to wire+60 s before a re-request is issued; the
socket inactivity disconnect still fires only after TIMEOUT_INTERVAL (20 min). If faster
peer-level abandonment is desired, that is a separate (non-expiry) mechanism and out of
scope here.
