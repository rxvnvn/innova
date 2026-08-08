# Max-ahead block-request window: audit and empirical replay

Status: audit complete. This report evaluates the proposed invariant

> Admit/request block H only if its inferred height is within W of the
> connected frontier, unless the predecessor path from the frontier to H is
> already represented by connected/owned/in-flight work.

against two instrumented IBD runs (one clean regtest flood boundary, one
mainnet flood), replays four policy variants decision-by-decision over the
forensic CSV, and recommends a concrete policy and the exact instrumentation
needed to validate it in a live run.

Relevant prior work:

* `frontier-admission-exemption.md` — the already-landed exemption that lets
  the frontier block through a zero budget.
* `ibd-semantic-recovery-design.md` — deferred-queue admission model and
  ASK_DEFER semantics.
* `getblocks-hashcontinue-batch-size-audit.md` — getblocks continuation
  batch-size facts.
* `ibd-degradation-runtime-validation.md` — runtime degradation reproduction.
* `ibd-forensic-instrumentation.md` — the exact-mode per-hash lifecycle CSV.

---

## 1. Terminology

| Term | Meaning |
|---|---|
| connected frontier | active-chain best block (pindexBest). The *connectable* frontier is the highest height whose predecessor path is all connected. |
| pipeline | set of block requests the node has admitted and asked for but not yet connected. Bounded by `MAX_DEFERRED_INV_ACTIVE_GLOBAL` (512) and `MAX_DEFERRED_INV_ACTIVE_PER_PEER` (128) during IBD (`GetMaxActiveBlockRequestsPerPeer`, src/net.h). |
| deferred queue | per-peer FIFO of admitted-then-deferred block invs, `MAX_DEFERRED_BLOCK_INV_PER_PEER` = 1000, refilled by `RefillDeferredBlockRequests` (src/main.cpp:516). |
| announcement | a block inv accepted at admission (`TryAdmitBlockInvOrDefer`, src/main.cpp:424), i.e. asked for, as opposed to a raw inv seen on the wire. The forensic CSV records announcements, not wire invs. |
| in-flight | `BLOCK_REQUEST_OWNER_IN_FLIGHT`: getdata sent, block not yet received. |
| ownership | one peer owns each requested block (`TryAssignBlockRequestOwner`); reassignment on timeout (commit 4244fb1, 30be277). |
| receipt | a `BLOCK_RECEIVE` event for a block we requested. |
| connect | `connected_active` outcome: block validated and connected to the active chain. |
| orphan | received block whose parent is not present; held in the orphan map capped at `MAX_ORPHAN_BLOCKS` (750). |
| `ORPHAN_LIMIT` | when the orphan map is full, newly received blocks whose parents are missing are rejected. |
| re-delivery | a block we already have or already requested that is announced/received again; recorded as `already_have_duplicate`. |
| ABRE / CS_OVERSHOOT / 5 s | request-expiry mechanisms: `CS_OVERSHOOT_DEFLECT_AGE_US` deflection; the wire-origin conservative expiry (commit 59d03e4) that replaced the 5 s absolute timeout. |
| announced-ahead | `height - connected_frontier(t)` for an announced block (height from block index, t = its batch's first hash mark). Negative = stale (frontier already past the block). |

---

## 2. What the invariant means, precisely

The proposed invariant has two clauses and one implicit premise.

* **Clause 1 (distance bound)**: `height(H) - connected_frontier_height <= W`.
  This bounds *future supply*: the node will not hold more than ~W
  not-yet-connectable blocks ahead of the frontier.
* **Clause 2 (predecessor-path exemption)**: a block whose predecessor path
  from the connected frontier to H is already represented by connected/owned/
  in-flight work may enter regardless of W. This is what keeps normal
  contiguous IBD pipelining intact: in healthy IBD the requested window
  [F+1, F+W] is exactly a contiguous run whose predecessors are owned.
* **Premise**: the node knows (or can cheaply estimate) `height(H)` at
  admission time. It is not directly present in a relay inv; it must come from
  the announcing context (getblocks response position + locator, or the
  block's already-known parent height).

Clause 2 is the operative discriminator in the measured floods (section 6).
Clause 1 is a defense-in-depth second bound that prevents a *far-ahead*
future-supply attack that the current run did not fully exercise.

---

## 3. Forensics instrumentation already present (needed for validation)

The exact-mode instrumentation (`ibd-forensic-instrumentation.md`, active via
`-ibdexptrace`) already records a per-hash lifecycle CSV with: batch id,
height, outcome, peer, first-mark time, and receive time, joined with a
forensic log that carries getdata-batch metadata (batch id, seq, n hashes,
first mark, receive time). This is sufficient to compute announced-ahead,
waste classification, and pipeline occupancy offline — which is what sections
5-6 use. It is *not* sufficient to distinguish admission-time from mark-time
ahead for deferred entries; that is the smallest instrumentation gap and is
listed in section 11.

The two runs:

| Run | Build | Chain | Announced | Connected | Wasted | Tip |
|---|---|---|---|---|---|---|
| 4244fb1 | forensic + debug log | regtest flood | 48694 | 24802 | 23892 | 4643 |
| 73002a0 | production (pre-59d03e4) | mainnet flood | 162090 | 57664 | 103780 | 250760 |

Waste = `rejected` + `timeout` + `incomplete_evicted` + `already_have_duplicate`
(+ a small `disconnect` tail). After commit 59d03e4 the 5 s-timeout class
(~33% of 73002a0 announcements, 104k timeouts) is eliminated; the remaining
waste classes are what the invariant can still address.

---

## 4. Empirical facts from the two runs

### 4.1 Healthy phase

500-800 connects/s, `received == connected`, no orphans, pipeline held at the
256/512 cap, received blocks at the frontier. No waste.

### 4.2 Degraded phase

* received 100-300/s, connected ~0-5/s (move tip slower than delivery);
* orphan map fills to 750 → `ORPHAN_LIMIT` rejects;
* deferred queue grows to 500-5000 per peer;
* getblocks dedup ~93% (same hashes re-announced across continuation
  responses);
* peer concentration: dominant peer serves 90-100% of announced blocks and
  owns the near-entire in-flight set.

### 4.3 Forward (ahead) demand is small and bounded

For **connected** (useful) announcements, announced-ahead:

| Run | p50 | p90 | p99 | max |
|---|---|---|---|---|
| 4244fb1 | 124 | 255 | 303 | 511 |
| 73002a0 | 86 | 221 | 296 | 488 |

Nothing useful is ever admitted more than ~511 ahead of the connected
frontier (the current global cap). Healthy pipeline depth is ~250-300.

### 4.4 Wasted supply is stale, not far-ahead

For **wasted** announcements, 80-96% have announced-ahead < 0 (the frontier
is already past them when they are admitted/requested). These are re-delivery
churn, deferred-then-admitted-late blocks that went stale, and fork/evicted
litter — not far-ahead future supply.

### 4.5 Batch composition

getdata batches are mostly single-hash (relay INV; p99 = 14, max 256).
256-hash batches are the getblocks prefetch responses. One 256-hash batch
spanned 4684 heights (heights -1..4683) — a non-contiguous, wide-span request
run whose blocks cannot connect (parents not owned). This is the archetypal
"future supply with no connectable path" and it is what Clause 2 refuses.

---

## 5. The offline replay

Method (deterministic, decision-level, no simulator): for every announcement
in the CSV, in mark order, compute announced-ahead and the no-outcome-oracle
predecessor predicate `parent owned/connected at admission OR parent in same
batch`. Then evaluate each policy's admit/defer decision and count what
happens to the *actual* useful (connected) and wasted outcomes. Deferred
blocks are kept eligible (queued), so deferral is delay, not loss — the only
cost is latency and deferred-queue depth.

### 5.1 4244fb1 (regtest flood; 24802 useful, 23892 waste)

| Policy | useful preserved | waste admitted | deferred |
|---|---|---|---|
| Baseline (admit all) | 100.0% | 100.0% | 0 |
| A/B/C W=256 | 94.5% | 100.0% | 1373 |
| A/B/C W=320 | 97.4% | 100.0% | ~500 |
| A/B/C W=384 | 99.5% | 100.0% | 136 |
| A/B/C W=512 | 100.0% | 100.0% | 0 |
| D pred+W=256 | 91.0% | 0.0% | 26121 |
| D pred+W=384 | 95.4% | 0.0% | 25039 |
| D pred+W=512 | 95.9% | 0.0% | 24906 |

### 5.2 73002a0 (mainnet flood; 57664 useful, 103780 waste)

| Policy | useful preserved | waste admitted | deferred |
|---|---|---|---|
| Baseline | 100.0% | 100.0% | 0 |
| A/B/C W=256 | 96.4% | 100.0% | 2061 |
| A/B/C W=384 | 99.6% | 100.0% | 239 |
| A/B/C W=512 | 100.0% | 100.0% | 0 |
| D pred+W=256 | 93.0% | 0.0% | 108469 |
| D pred+W=384 | 95.7% | 0.0% | 106898 |
| D pred+W=512 | 96.1% | 0.0% | 106660 |

### 5.3 Reading

* **A pure distance window does not stop the flood.** Policies A/B/C admit
  100% of the waste at every W, because the waste is stale (negative ahead) —
  it is already inside any window. A max-ahead cap only trims the useful tail
  (W=384 costs 0.4-0.5% of useful connects; W=256 costs ~4-5%).
* **The predecessor-path predicate is the discriminator.** Policy D defers
  the entire waste class (0% admitted) while preserving ~91-96% of useful
  connects. The deferred-but-useful remainder (1147 blocks in 4244fb1, ~2472
  in 73002a0, all eventually `connected_active`) is *delay*, not loss: their
  parents arrive later and they re-enter through the deferred queue.
* **W=384 with the predicate** preserves ~95-96% useful and defers ~100%
  waste; **W=512** is the "zero visible cost" setting for the predicate.
  Neither needs to cut the useful tail that a pure window would cut.

---

## 6. Finding: what actually goes wrong, and which clause fixes it

The measured flood does *not* deliver far-ahead supply; it delivers (a) stale
re-delivery churn and (b) non-contiguous, wide-span request runs whose blocks
cannot connect because their predecessors are never owned. The first is only
partly addressable by admission policy (the 5 s-timeout commit already
removed the largest class); the second is exactly the class that Clause 2
(predecessor path represented by connected/owned/in-flight work) refuses.

The far-ahead future-supply scenario the task is designed to bound — a peer
announcing a chain thousands of blocks past the frontier and driving the
deferred queue to backup, then having those entries admitted stale — was not
fully exercised by these runs (ahead is capped at ~511 by the existing global
window). Clause 1 exists to bound that *future* scenario; Clause 2 bounds the
*measured* one. A robust policy needs both, with W sized to the healthy
pipeline depth (>= ~384; 512 matches the current cap and is the conservative
recommendation).

---

## 7. Honest assessment of the first-order break

**Not first-order fixed.** The state that degraded is the orphan map +
re-delivery loop, and the dominant prior cause (the 5 s timeout) was already a
separate bug fixed in 59d03e4. The predecessor-path window removes the
unconnectable-supply driver (b), and the expiry fix removes the stale
re-delivery amplifier (a). But admission policy alone does not repair a node
whose connect rate is the bottleneck: if real contiguous blocks arrive faster
than validation can connect them, the orphan map will still fill and the node
will still stall — a window cannot manufacture connect bandwidth. The
invariant's contract is: *never hold more than ~W unconnectable future work
ahead of the frontier, and never request work whose predecessors are not
represented.* That is the right, bounded contract; it is not a claim that the
degradation is eliminated in all traffic shapes.

---

## 8. Decision (policy selection)

**Adopt policy D — predecessor-aware admission with a global distance bound:**

> At admission (`TryAdmitBlockInvOrDefer`), request block H only if
> **Clause 1**: `height(H) - nBestHeight <= W` (W = 512), **and**
> **Clause 2**: the predecessor path from the connected frontier to H is
> represented by connected/owned/in-flight work, or H is the frontier
> candidate covered by the existing frontier-admission exemption.
>
> Blocks failing either clause are *deferred* (kept eligible, queued), never
> dropped. W is the configured IBD active window
> (`GetMaxActiveBlockRequestsPerPeer`, default 512); reduce to 384 for a
> tighter bound at <0.5% useful-tail cost. Keep the per-peer cap (128) as the
> concentration bound.

Rejected alternatives:

* **A/B/C (pure distance window, global or per-peer)**: measured ineffective —
  admits 100% of the flood's waste (section 5.3). A pure window is only a
  far-ahead defense, which the data says is not the measured failure mode.
* **C with per-peer sub-cap**: the peer-concentration problem (90-100% from
  one peer) is real but is a *pipelining fairness* concern, not a supply
  bound; the per-peer cap already exists (128) and the dominant-peer effect is
  a symptom of the request-ownership/reassignment path, tracked separately.
* **Rejecting (not deferring) out-of-window supply**: wrong; it reintroduces
  the "announced frontier block dropped forever" cycle that
  `frontier-admission-exemption.md` fixed. Deferral preserves recovery.

---

## 9. Boundary and edge cases for the chosen invariant

* **Frontier candidate when budget is 0**: already exempt
  (`FrontierCandidateCanAdmit`, src/main.cpp:460); unchanged.
* **height unknown (relay inv)**: Clause 1 cannot be evaluated; fall back to
  Clause 2 alone — admit iff the predecessor path is represented. This is the
  safe direction: relay-announced blocks are normally the frontier+1 and pass
  Clause 2; unknown-height *non*-contiguous invs are deferred.
* **Orphan-parent repair AskFor**: a received block whose parent is missing
  asks for the parent at `height(H)-1`; with H admitted, the parent is within
  W by construction and its predecessor path is (H-1)'s, so it passes. Keep
  this path exempt from the *global* bound to avoid self-blocking on a deep
  orphan chain.
* **Fork/litter (negative ahead)**: deferred by Clause 2 (no represented
  predecessor path), never requested. This is exactly the measured waste.
* **Batch non-contiguity (getblocks continuation)**: the 256-hash/4684-height
  batch class fails Clause 2 (sparse predecessors) and is deferred, bounding
  orphan pressure and getblocks churn. Continuation responses whose hashes are
  contiguous still admit normally.
* **getblocks locator staleness**: unchanged; the frontier-candidate tagging
  already rejects responses requested against a stale locator
  (`locator-stale`).
* **W interplay with deferred queue**: deferral keeps the block in the
  per-peer queue (1000 cap); queue overflow drops oldest, unchanged. A
  far-ahead chain announces 10k blocks → most fail Clause 1 and sit at the
  queue tail; the queue's 1000 cap bounds memory exactly as today.

---

## 10. Interaction with the existing budget/frontier machinery

The invariant slots into the existing decision function
`TryAdmitBlockInvOrDefer` (src/main.cpp:424), which already:
* computes the per-peer + global budget from orphan pressure
  (`GetDeferredBlockRequestBudget`, src/main.cpp:285);
* applies the frontier-candidate exemption when the budget is 0
  (src/main.cpp:460);
* otherwise admits at `now` or defers via `RefillDeferredBlockRequests`
  (src/main.cpp:516).

The new predicate is an *additional* reason to defer within that function:
Clause 1 uses `nBestHeight` and the announcing context (locator height for
getblocks responses, parent height for relay/continuation); Clause 2 uses the
block-request-ownership map plus the connected chain. No new data structure is
required; `TryAssignBlockRequestOwner` already tracks ownership that Clause 2
queries. The front-`fFrontierResponsePending`/`nFrontierLocatorHeight`
mechanics (src/net.cpp, src/main.cpp:10728) already give the locator context
needed to estimate height for getblocks responses.

---

## 11. Smallest instrumentation to validate live

1. **Record admission-time ahead, not mark-time ahead.** Add the connected
   frontier height at the `TryAdmitBlockInvOrDefer` decision to the forensic
   CSV (one extra column), so deferred-late entries are measured at the time
   the decision is actually made. This is the single gap between the current
   CSV and a fully faithful replay.
2. **Emit the Clause-2 predicate result.** Log whether the predecessor path
   was represented at admission (yes/no + reason), so the live node can be
   verified against the offline replay (sections 5-6).
3. **Count defer-by-window vs defer-by-budget.** A tiny counter
   (`-ibdexptrace`) separating "deferred because beyond W" from "deferred
   because budget 0" makes the effect visible without a full CSV replay.

---

## 12. Open questions requiring a real run

* Does a contiguous but over-speed real chain still fill the orphan map at
  the connect bottleneck (section 7)? If yes, the invariant bounds it to W but
  the stall persists; the fix would then be connect-rate, not admission.
* Does the 5 s-timeout removal (59d03e4) alone change the 73002a0 picture
  enough that remaining waste is negligible? Re-run 73002a0's flood on the
  current HEAD before deciding W.
* Per-peer dominance: is it a supply effect (one peer floods) or a request
  ownership/priority effect (the node prefers one peer)? The latter needs the
  peer-ranking path, not the window.
* The far-ahead scenario (Clause 1's reason to exist): construct a synthetic
  flood that announces a chain 2000+ ahead and confirm the current code admits
  it into the deferred queue, then confirm policy D keeps it deferred.

---

## Reproducibility

* Replay script: `/tmp/opencode/policy2.py` (decision-level, no oracle).
* Runs: `/home/user/innova-logs/4244fb1/`, `/home/user/innova-logs/73002a0/`
  (`ibd-forensic-fix-0f707b1.log`, `ibd-blocklatency-expB.csv`).
* Result tables in sections 5.1-5.2.
