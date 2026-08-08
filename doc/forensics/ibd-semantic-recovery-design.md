# Semantic IBD Recovery: Continuity-Based Stalled-Sync Handling (rev. 2)

**Status:** design (draft, no production code)
**Scope:** Innova Core IBD recovery (`CStalledSyncRecoveryState`, `src/net.cpp`)
**Baseline evidence:** `doc/forensics/ibd-forensic-instrumentation.md` (canonical instrumentation),
`doc/forensics/ibd-root-cause-investigation.md`. expB run: 10 683 s, tip 48 406 → 53 670,
`global_active_zero_transitions=32` (105 ms empty total), `stalled_recovery_attempts=0`,
`terminal_pipeline_not_empty=479 229/479 530`, `active_decrement_inflight_timeout≈399 844`,
dispatch delay avg ≈ 6.9 s, late-IBD tip rate 0.2–0.4 blocks/s.

## Revision log (rev. 1 → rev. 2)

1. **A4 (front-age) removed as arm.** `BLOCK_IN_FLIGHT_TIMEOUT = 5` (net.h:1851) caps every
   live in-flight entry at 5 s, so any "oldest live in-flight > 20 s" threshold is unattainable.
   The front-stick signal is expressed through the `nBestHeight` slope (A1); in-flight head-age
   (`nHeadAgeUs`, net.h:1874-1886) is kept as a **diagnostic metric with a 5 s ceiling**, never an arm.
2. **Progress measured on `nBestHeight`**, the canonical tip (`src/main.h:375`,
   `src/main.cpp:89`), not on the `accepted_active` event counter. Batch-tip advances move
   `nBestHeight`; per-second `ΔnBestHeight` is the arm's rate input.
3. **Targeted AskFor needs an explicit reassignment step** to pass the single-owner gate
   (`ASKFOR_OWNED_BY_OTHER`, net.h:1496-1508).
4. **Continuity target anchored to the real frontier hole**, not any orphan root (orphan blocks
   carry no height — full CBlock only, no CBlockIndex).

### Revision log (rev. 2 → rev. 2.1)

5. **A3 numerator corrected.** `accepted_active / received` is not a complete "useful-connect"
   share: `received` (main.cpp:9455) includes duplicates and validation rejects that
   `accepted_active` (main.cpp:9471) never counts, so the ratio overstates productivity and
   cannot distinguish *missing-link* waste from *futility*. A3 now uses the direct orphan/reject
   outcome share `(orphan_new + orphan_limit_rejected) / received`, which requires adding two
   counters (`block_result_orphan_limit_rejected`, `block_result_rejected_total`). See §1.1.
6. **Reassignment lock graph verified (safe, no cycle).** All three steps re-enter
   `cs_mapAlreadyAskedFor`; `CCriticalSection` is `boost::recursive_mutex` (sync.h:82), so a
   single enclosing `LOCK` is safe on the message-handler thread, and the closure introduces no
   new lock-order cycle. See §4.2.

---

## 1. Arm signals

Current arm (`net.cpp:964-983`) fires only on an ordinary getblocks to an advance-capable peer
with an empty pipeline (gate `net.cpp:1557-1561`); expB shows emptiness is useless
(105 ms / 10 683 s). The arm must be rate/continuity-based and gated by the existing cooldown.

### 1.1 Recommended arm (v1): "busy but unproductive"

Evaluate every 1 s while IBD is active and ≥ 2 peers are eligible-ahead
(`TipAncestorOfPeerBestKnown > 0`, `main.cpp:156-167`). Arm when **all** hold:

| # | Signal | Definition | Source | Threshold (configurable) |
|---|--------|-----------|--------|--------------------------|
| A1 | tip-progress rate | median of per-second `ΔnBestHeight` over the last 300 × 1 s samples | `nBestHeight` slope, 1 Hz | `-ibdminprogressrate`, default 0.2 blocks/s |
| A2 | receive activity | received-block slope > 0 (not network silence) | `block_receive_total` | ≥ 1 block / 30 s |
| A3 | orphan dominance | share of received blocks that are orphan/limit-rejected (missing parent, not validation failure) | `(block_result_orphan_new + block_result_orphan_limit_rejected) / block_receive_total` | `> -ibdorphandom`, default 0.5 |
| A3b | futility guard | validation rejects must NOT dominate (else recovery is futile — blocks are bad, not missing) | `block_result_rejected_total / block_receive_total` | `< -ibdfutilereject`, default 0.2 |
| A4 | pipeline activity | dispatch/queue churn continues while A1 is flat | `active_*`/`pass_interval` metrics, dispatch events | > 0 events in window |
| A5 | peer gap | still far behind the frontier | `max(peer.nBestKnownHeight) − nBestHeight` (net.h:1840-1846) | `> -ibdgapmin`, default 5000 |

**Why A3 uses orphan outcomes, not `accepted_active/received`:** `received` (main.cpp:9455)
counts every delivered block, including duplicates and validation rejects, so
`accepted_active/received` (a) under-counts the waste and (b) cannot separate *missing-link*
waste (recoverable via a targeted AskFor) from *futility* (bad blocks — recovery is pointless).
A3 counts only orphan-producing outcomes: `orphan_new` (main.cpp:9474, a lower bound — duplicate
orphans are not recounted) plus `orphan_limit_rejected` (added at the orphan-limit reject sites,
main.cpp:7190/7195). A3b vetoes the arm when validation rejects dominate, i.e. the pipeline is
futile rather than gapped. This requires two new counters (`block_result_orphan_limit_rejected`,
`block_result_rejected_total`), additive and off-default; `accepted_active` stays an independent
metric.

**Why A4 replaces "oldest in-flight age":** every live in-flight entry expires at 5 s
(net.h:1851), so its age cannot exceed 5 s; in the busy-but-stuck state entries simply churn at
the expiry rate (`active_decrement_inflight_timeout ≈ 37/s`) without any acceptance. The
"stuck front" is therefore identical to "no `nBestHeight` progress" (A1), and "busy" to
"receive + dispatch continue" (A2+A4). In-flight head-age stays a diagnostic
(`nHeadAgeUs`, net.h:1874-1886), capped at 5 s, and the connect ratio A3 is the discriminator
between slow-healthy (mostly connects) and stuck (mostly orphans).

### 1.2 Signals deliberately *not* used as the arm

- **Pipeline emptiness** (`net.cpp:1557-1561`): useless (evidence above). Diagnostic only.
- **`LastProgressTime` / `-syncstalltimeout`** (default 15 s): binary, fires on healthy bursts'
  troughs. Keep only as the cooldown/reenable gate.
- **Deferred emptiness**: non-empty deferred implies non-empty pipeline (redundant).
- **In-flight timeout count**: too noisy (37/s) for a primary; absolute cut-off diagnostic only.
- **In-flight head-age** as a *primary*: impossible above 5 s (point 1).

---

## 2. False positives

| FP scenario | Metrics appearance | Suppression |
|---|---|---|
| Slow-but-healthy peer (0.2–0.4 b/s) | A1 low, A2 positive, A3 **high connect**, A4 positive | Require ≥ 2 eligible-ahead peers; A3 connect ratio ≥ 0.5 ⇒ not stuck; action is harmless even on FP |
| Short burstless trough between healthy 1000-block bursts | A1 low < 5 min | 5 m median of per-second `ΔnBestHeight`; never arm from < 30 s of data |
| Disk pause (chainstate commit) | A2 high, A1 flat, A3 **low connect** | A3 discriminates: disk pause stalls acceptance, not orphan formation; if orphans are not dominant, do not arm (recovery futile) |
| Headers-only / announcement lag with orphan pile-up | A2 high, A3 high, A1 flat | This is exactly the continuity stall ⇒ **should arm**; action §4(2) targets it. Not an FP |
| One-off long response (dispatch 6.9 s avg) | A4 momentarily low | A1 uses 5 m median; one-off spikes filtered |
| Reorg / fork wobble | A1 dips, A3 may rise | A1 median + A3 suppress; action is one-shot and bounded (§7) |

---

## 3. Continuity target = real frontier hole

**Constraint:** orphan blocks in `mapOrphanBlocks` (`src/main.h:389`) are full `CBlock`s with no
`CBlockIndex`, hence no height. The frontier hole lives in `(nBestHeight, nFrontierHeight]`,
where `nFrontierHeight = max(peer.nBestKnownHeight)` (net.h:1840-1846).

**Selection rule (ranked):**
1. **Frontier-anchored orphan forest** — among all orphan forests (roots found via
   `GetOrphanRoot`, `main.cpp:2840-2846`), pick the one whose missing parent is closest to the
   frontier:
   - prefer forests whose root-parent is in `mapBlockIndex` at the *highest* height
     (a gap adjacent to the accepted chain), then
   - prefer the deepest forest announced by the peer with the largest `nBestKnownHeight`
     (the main continuation, not a stale side fork);
   - reject forests whose root-parent is already accepted (stale remnant — not the hole).
   Target = `WantedByOrphan(forest)` (`main.cpp:2849-2855`).
2. **Rejected-hash one-shot** — `TakeRejectedBlockForRetry` (delivered, unknown, nDoS==0);
   use when no qualifying forest exists.
3. **`g_frontierHash`** (frontier-admission exemption candidate) — use when (2) empty.
4. **Oldest in-flight hash** reassigned to another peer — last resort (§4.2).

The chosen hash must satisfy: `∉ mapBlockIndex`, not already owned (§4.2), and within the
frontier gap (announced by the frontier peer). Validation failures move down the list.

---

## 4. Recovery actions

### 4.1 Comparison

| Action | Pros | Cons | Verdict |
|---|---|---|---|
| (1) getblocks bare-tip (existing) | already built | no hash; peer must replay frontier; proven unhelpful in degraded profile | fallback only |
| (2) targeted `AskFor` of frontier-hole hash to a different eligible peer | minimal surface; hash arrival is a **provable causal test**; harmless on FP | requires reassignment if owned (§4.2); 1 round trip | **recommended v1 action** |
| (3) getblocks with continuity locator (= highest accepted ancestor) | is a **frontier-hole probe**: first hash of the reply after the tip locator is the hole itself | degenerates to (1) if locator is tip-adjacent; larger reply | secondary; used as the causal confirm of (2) |
| (4) reassign oldest in-flight hash | attacks front-stick directly | ownership churn; double-delivery risk (§4.2) | last resort |
| (5) pipeline reset / flush | "clean slate" | scheduler deterministically rebuilds the same pipeline; discards in-flight state; risky | **rejected** (no-reset constraint, §5) |

### 4.2 Single-owner gate and explicit reassignment

`AskFor` refuses a block owned by another peer: `ASKFOR_OWNED_BY_OTHER` (net.h:1496-1508).
Ownership states: `QUEUED`, `IN_FLIGHT` (net.h:580-581). `ReleaseBlockRequestOwner(hash, peer, reason)`
(net.cpp:2383-2409) removes the owner **but does not erase the old owner's dispatch queue**
(`mapAskFor`); it only clears `mapBlockRequestOwners` + generation record + `g_frontierHash` tie.
Reassignment therefore must be:

1. Locate the old owner's `mapAskFor` entry for the hash (multimap scan, keyed by time) — if
   absent the entry was already dispatched and the `IN_FLIGHT` policy (§2) applies instead.
2. In **one** enclosing `LOCK(cs_mapAlreadyAskedFor)` scope (not nested `LOCK`s):
   - `ReleaseBlockRequestOwner(hash, oldPeer, "semantic-reassign")`,
   - `oldPeer->EraseAskForEntry(it, ..., ACTIVE_DECREMENT_OTHER)` (net.h:1362),
   - `newPeer->AskFor(CInv(MSG_BLOCK, hash))` — the gate now sees no owner and admits.
3. Policy: reassign **only** if old owner is `QUEUED`, or `IN_FLIGHT` with in-flight age ≥ 4 s
   (within 1 s of the 5 s expiry, net.h:1851); otherwise skip this target/peer (avoids
   double delivery). If the hash is owned by the chosen peer already → no-op.

**Lock graph (verified against the code, no cycle):** the call site runs in the message-handler
thread inside `MaybeQueueStalledSyncRecovery` (net.cpp:7670) under `LOCK(cs_stalledSyncRecovery)`.
The three steps re-enter `cs_mapAlreadyAskedFor`:
`ReleaseBlockRequestOwner` (net.cpp:2386), `EraseAskForEntry` (net.h:1377 `ClearDiversifyDispatch`
→ net.cpp:1142, net.h:1400 `ReleaseBlockRequestOwner`), `AskFor` (net.h:1434 `PruneAlreadyAskedFor`
→ net.cpp:2827, net.h:1439 `ExpireBlockInFlight` → net.h:1890 `ReleaseBlockRequestOwner`, and
net.h:1478). `CCriticalSection` is `boost::recursive_mutex` (sync.h:82), so same-thread re-entry
is legal — a single enclosing `LOCK(cs_mapAlreadyAskedFor)` is safe (all steps execute on one
thread). Closure lock set = {`cs_stalledSyncRecovery`, `cs_mapAlreadyAskedFor`} only; no step
acquires `cs_main`, `cs_vNodes`, `cs_vSend` or `cs_inventory`. Reverse edge check: the only
`cs_stalledSyncRecovery` lock sites (net.cpp:980, 992, 1298, 1320, 1849, 4678, 7426, 7669) are
loop/diagnostic contexts, none inside a `cs_mapAlreadyAskedFor` closure, so no
`cs_mapAlreadyAskedFor → cs_stalledSyncRecovery` edge exists and no cycle is introduced.
Existing orders elsewhere (cs_main → cs_mapAlreadyAskedFor at net.cpp:2779; cs_main → cs_vNodes →
cs_mapAlreadyAskedFor in `MaybeProcessPipelineWake`) are unchanged. Implementation form: a single
`...Locked`-style helper taking the iterator, matching the existing
`TryAssignBlockRequestOwnerLocked` pattern (net.cpp:2288).

### 4.3 Observation window

`RECOVERY_RESPONSE_WINDOW_US = 2 s` (net.h:383) is far shorter than observed dispatch
(avg ≈ 6.9 s): a 2 s window makes the causal test always "fail". The design **must raise the
window** for the causal test (`-ibdrecovwindow`, default 60 s), reusing
`RecoveryResponseWindowState` (net.h:407-450).

---

## 5. No-full-reset constraint

A reset is both pointless and risky: the per-peer window scheduler deterministically rebuilds
the identical pipeline from code state; reset discards `mapBlockInFlightSince`, the ownership
ledger and the frontier-exemption cache; nothing in the degraded profile indicates stale state —
it indicates missing links (orphans + rejected blocks). All actions in §4 are therefore
**incremental**: one new request stream max, no `mapBlockIndex` mutation, no orphan-map clears,
no global cooldown changes. The only new global state is a bounded continuity quota.

---

## 6. State machine

States: `HEALTHY`, `SUSPECT`, `SEMANTIC_STALL`, `RECOVERY`, `RECOVERED`, `ESCALATE`.

```
HEALTHY --[A1..A5 all hold]--> SUSPECT
SUSPECT --[60 s window, still stalled]--> SEMANTIC_STALL
SEMANTIC_STALL --[hole target selected, cooldown OK]--> RECOVERY
RECOVERY --[causal test: target connects AND ΔnBestHeight ≥ 1 in window]--> RECOVERED
RECOVERY --[window expiry, no connect]--> ESCALATE
RECOVERY --[FP-suppression §2 / rollback]--> HEALTHY
RECOVERED --[rate restored for 5 m]--> HEALTHY
ESCALATE --[new target or second peer]--> RECOVERY
ESCALATE --[quota exhausted]--> SUSPECT (bounded backoff)
```

**Bounded state:** `nSemanticAttempts` (cap 3, then `SUSPECT` for `2×` backoff); per-target
`std::set<uint256>` one-shot dedup; ≤ 1 targeted AskFor + ≤ 1 getblocks per attempt. Reset on
`HEALTHY`.

---

## 7. Transitions (current vs recommended)

| Transition | Current | Recommended |
|---|---|---|
| Arm | binary emptiness + `RecordOrdinaryGetBlocksCommitted` (net.cpp:964-983) | A1–A5 conjunction (§1.1), any pipeline state |
| Trigger | ordinary getblocks to advance peer, empty pipeline | stall state, no emptiness requirement |
| Progress basis | `nBestHeight` change (existing recovery) | same — `nBestHeight` slope, but now sampled/medianized at 1 Hz for the arm |
| Action | getblocks bare-tip + one-shot rejected-hash AskFor | targeted AskFor of frontier-hole hash to a *different* peer (§3, §4.2), reassign if owned |
| Observation window | `RECOVERY_RESPONSE_WINDOW_US` = 2 s | `-ibdrecovwindow`, default 60 s |
| Cooldown/exit | `nCooldown*(1<<min(attempts,5))`, `-syncstallcooldown` 15 s | unchanged; cap `-ibdrecovmaxbackoff`, default 300 s |
| Metrics | `recovery_outcome_*`, `FormatRecoveryResponseSummary` | add `semantic_target_asked`, `semantic_target_connected`, `semantic_causal_connect` |
| Rollback | n/a | one-shot abort, release target, return to `SUSPECT`; flag flip; nothing global mutated |

Required metrics (existing, reused for the causal test): connect rate, oldest in-flight head-age
(diagnostic), peer-gap, dispatch/pass interval, `recovery_outcome_*`. New: the three
`semantic_*` counters plus the two A3 counters `block_result_orphan_limit_rejected` and
`block_result_rejected_total` (§1.1).

---

## 8. First controlled experiment

Gate behind `-ibdsemrecover=<n>` (default 0 = inert):
- `=0` disabled; `=1` arm-only (emit SUSPECT metrics, no action); `=2` arm + action (2).
- No clearing of `mapBlockIndex` / `mapOrphanBlocks`; no global cooldown mutation; exactly one
  continuity action per attempt.
- **Causality proof:** the specific frontier-hole hash must connect within `-ibdrecovwindow`
  after the AskFor **and** `ΔnBestHeight ≥ 1` in the same window; otherwise recorded as a miss.
  Compare tip progress, connect rate and unknown-vs-known share before/after in the same node.
  The continuity getblocks (4.1 item 3) is the confirm probe: its first hash after the tip
  locator must equal the asked target.
- Success: `semantic_causal_connect = 1` on ≥ 30% of attempts in the degraded profile; else
  disable and keep arm-only.

---

## 9. Side-by-side phases

| Phase | Scope | Deliverable |
|---|---|---|
| 1 | instrumentation (exists) | metrics + expB baseline |
| 2 | arm-only experiment (`-ibdsemrecover=1`) | confirm A1–A5 (on `nBestHeight`) arm cleanly with no FPs on degraded + healthy profiles; tune thresholds |
| 3 | continuity action (`=2`) | validate targeted AskFor + reassignment causal test; measure tip-rate delta |
| 4 | (later RFC) headers-first or continuity getblocks | only if phases 2–3 justify a larger action |

---

## Final recommendation

- **Arm:** A1–A5 conjunction on `nBestHeight`: 5 m median tip rate < 0.2 blocks/s, receiving
  data, orphan dominance ≥ 50% (with a futility veto when validation rejects dominate),
  pipeline activity continuing, peer-gap > 5000,
  ≥ 2 eligible-ahead peers.
- **Minimal safe action:** one targeted `AskFor` of the frontier-hole hash (frontier-anchored
  orphan forest → rejected-hash → frontier-hash → oldest-in-flight) to a different
  eligible-ahead peer, with an explicit single-owner **reassignment** (§4.2) that only releases
  `QUEUED` owners or `IN_FLIGHT` owners within 1 s of expiry, all under `cs_mapAlreadyAskedFor`.
  Observation window raised 2 s → 60 s.
- **Mandatory metrics:** `semantic_target_asked`, `semantic_target_connected`,
  `semantic_causal_connect`, plus existing `recovery_outcome_*`, connect rate, peer-gap,
  in-flight head-age (diagnostic), and the A3 counters `block_result_orphan_limit_rejected` /
  `block_result_rejected_total`.
- **Stop/rollback:** quota (3 attempts) exceeded, rising unknown-share with no connect,
  non-advance peers, or ≥ 70% causally-negative attempts → revert to arm-only/off; nothing
  global is mutated, so rollback is a flag flip.
- **Expected volume:** ~200–300 lines of C++ (arm eval ~60, frontier-hole selection ~70,
  reassignment + one-shot askfor ~60, metrics ~40) + 1 functional test (degraded-profile replay
  asserting a single targeted AskFor and reassignment order) + 1 unit test (frontier-hole
  selection order). All behind `-ibdsemrecover`, default off.
