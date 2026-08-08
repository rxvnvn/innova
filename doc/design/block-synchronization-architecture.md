# Innova Block Synchronization Architecture

**Status:** CURRENT architecture record; Stage-2 portions remain **EXPERIMENTAL**.

**Evidence base:** the current source tree and the design/forensic records linked
throughout this document. This document explains the synchronization system as a
whole. It is not a Stage-2 implementation note and not a replacement for the
individual forensic reports.

## 1. Purpose and Scope

Block synchronization is the system that moves a node through the whole range
from `genesis` to a useful live tip:

```text
genesis -> deep IBD -> catch-up -> near-tip -> live tip
                         ^                         |
                         |                         v
                    recovery after lag/re-entry <-+
```

This document records why Innova's historical block scheduler changed, how the
legacy and replacement models differ, which boundaries remain authoritative,
and which invariants future work must preserve.

**CURRENT:** the legacy scheduler remains available, and the ordered headers
graph and scheduler can be enabled explicitly during IBD. The consensus and
block-processing path is shared by both modes.

**EXPERIMENTAL:** the Stage-2 ordered selector and the separate regtest IBD
override are not default behavior and have not passed the final healthy A/B or
B3 gates.

**FUTURE/TARGET:** a single understandable synchronization state machine that
continues from IBD into near-tip and live synchronization, including automatic
re-entry after a node falls behind. Post-IBD behavior must be audited before
deciding how much of the ordered manager remains active there.

## 2. Why Innova Needed a New Synchronization Architecture

The legacy full-node path lets historical work emerge from peer announcements:

```text
getblocks -> INV announcements -> AskFor/admission -> ownership
           -> getdata -> block receive -> ProcessBlock
```

That model can be healthy. The recorded direct CONTROL runs reached roughly
120 connected blocks/s with approximately 99.6% received-to-connected
efficiency, no meaningful orphan pressure, and a full useful pipeline. The old
system was therefore not universally or intrinsically broken.

Its information model becomes insufficient when announcement order is a poor
proxy for chain order. The measured failure family included:

* announcement-driven historical scheduling and first-wins/exclusive ownership;
* stale or future supply occupying the request budget;
* received/connected divergence and orphan-pool saturation;
* getblocks suppression and locator deduplication hiding a needed request;
* a pipeline that was full but semantically unable to advance the connected tip;
* recovery gated on pipeline emptiness, even while the pipeline remained full;
* the B3 signature of a flat or slowly moving tip, widening gap, and pinned
  `ORPHAN_LIMIT` occupancy.

The [max-ahead window audit](../forensics/max-ahead-window-audit.md) and the
[degradation runtime validation](../forensics/ibd-degradation-runtime-validation.md)
separate healthy throughput from semantic progress. The B3 gate at the
[clean HEAD result](../forensics/ibd-gate-b3-head-result.md) reproduced the
failure after conservative expiry: the pipeline remained full, the tip stayed
flat for minutes, and the orphan cap pinned.

The conservative wire-origin timeout was still the correct first decision. A
5-second enqueue-origin timeout was a measured amplifier of slow-but-live
delivery, so commit `59d03e4` moved expiration to the first wire send with a
60-second bound and a small pending-wire safety limit. The [conservative expiry
design](ibd-conservative-block-request-expiration.md) records why the adaptive
proposal was rejected. Expiry improved one amplifier but did not make a full
pipeline useful.

The project had pre-registered a gate: retain the current scheduler if the
conservative patch repaired the measured failure, and escalate only if the
gate failed. It failed. The resulting decision in
[IBD scheduler architecture decision](ibd-scheduler-architecture-decision.md)
was therefore evidence-driven:

```text
PATCH CURRENT  --pre-registered B3 gate failed-->  REPLACE IBD SCHEDULER
```

Replacement is a change to the information and selection model, not a claim
that every existing P2P subsystem must be discarded.

## 3. Legacy Innova Synchronization Model

The legacy model is an announcement admission system. A peer's `getblocks`
response supplies INV hashes. `TryAdmitBlockInvOrDefer` and the `AskFor` queues
apply per-peer and global budgets, ownership, deduplication, and recovery
rules. `SendMessages` turns owned work into `getdata`; the peer serves blocks;
the receiver passes them to `ProcessBlock`.

The important distinction is:

```text
peer announced hash X       !=       node knows X is the next useful block
```

An announcement carries availability information but does not, by itself,
give the scheduler an ordered predecessor relationship. The receiver learns
that relationship only when blocks and their parents become known to the
authoritative block index or orphan machinery. A peer can therefore win work
ownership for a block that is valid but not useful for immediate frontier
progress.

Healthy peers and favorable announcement order can make this model fast. Under
impaired ordering, stale supply can consume the same active slots as useful
historical blocks, while later blocks become orphans. The pipeline counters
then describe transport occupancy rather than useful synchronization.

The old model's precise weakness is not that INV is always wrong. It is that
INV order and exclusive admission are insufficient as the primary historical
work-order authority when the node needs semantic continuity.

## 4. New Synchronization Model

The replacement model is:

> The node first learns the ordered road ahead from headers, decides for itself
> which exact blocks are useful next, and then uses peers as suppliers of those
> blocks.

The flow is:

```text
getheaders
  -> ordered provisional header knowledge
  -> authoritative anchor
  -> active provisional path
  -> ordered window
  -> exact block assignment
  -> ownership
  -> getdata
  -> ProcessBlock
  -> authoritative chain advance
```

The central transition is:

```text
OLD: peer announcement -> historical work
NEW: ordered chain knowledge -> node-selected work -> peer delivery
```

The current implementation keeps the graph private to the IBD scheduler. It
does not change consensus, block serialization, `ProcessBlock`, `AcceptBlock`,
chainstate, wallet, staking, or ordinary relay behavior.

## 5. Knowledge Plane and Data Plane

The architecture has two conceptual planes.

### Knowledge plane

The knowledge plane acquires headers and maintains `CIbdHeaderGraph`. It:

* learns ordered ancestry ahead of the connected tip;
* tracks an active provisional path and competing branches;
* retains source attribution and quarantine state;
* constructs continuation locators;
* exposes an ordered block window.

`CIbdHeaderGraph` is scheduler knowledge, not consensus. It does not make a
block authoritative and does not replace the existing block index.

### Data plane

The data plane delivers and validates full blocks. It owns:

* peer availability and exact-hash ownership;
* `AskFor` and `getdata` construction;
* wire-origin expiration and pending-wire safety;
* block receipt and queueing;
* `ProcessBlock`, orphan handling, and chainstate advancement.

A peer may supply headers without being the sole supplier of the corresponding
blocks. Conversely, a peer can supply blocks selected from the ordered graph
without having defined that order. This separation is the main architectural
benefit of headers-first discovery.

## 6. Consensus Boundary

The boundary is normative:

```text
provisional header graph = scheduler metadata
existing block index/ProcessBlock/chainstate = authority
```

Headers in the graph are accepted only far enough to establish safe provisional
ancestry and scheduling metadata. A full block still passes the existing
validation and chainstate path. A received block can invalidate or quarantine
the corresponding provisional branch; the graph must follow that result, not
override it.

The implementation deliberately avoids putting header-only entries into the
ordinary authoritative `CBlockIndex` state. In this codebase, that state has
meaning beyond ancestry, including full-block-derived PoS, stake-modifier,
disk, and DAG work. The [headers-first design](ibd-headers-first-scheduler-v1.md)
also records the historical PoS and post-DAG concern: serialized headers do not
contain all information required for full block validity, including transaction
and signature-dependent facts.

No provisional work, branch score, peer claim, or header count may bypass
`ProcessBlock` and existing consensus validation.

## 7. Compatibility With Legacy Innova Peers

This is a client-side migration. It uses existing messages:

* `getheaders`;
* `headers`;
* `getdata`;
* `block`.

No network-wide protocol upgrade or version bump is required. The current
source contains a full-peer `getheaders` handler that can walk the active chain
and return headers. That is a verified source fact, not a claim that every
historical peer implementation is equally reliable as a header source.

A new Innova node surrounded by legacy peers can, where peers answer standard
`getheaders` usefully, build its own ordered view and request exact blocks from
those peers. Peer capability and response quality remain runtime concerns.

The legacy `getblocks`/INV path remains available as a compatibility fallback
for peers or situations that do not provide useful headers-first discovery.
This distinguishes **protocol compatibility** from the **reliability of a
particular peer as a header source**.

## 8. Ordered Window and Frontier

The **authoritative frontier** is the connected active chain tip. The
**provisional graph tip** is the furthest usable node on the selected
provisional path. **Graph lookahead** is the height distance between them. The
**ordered window** is the bounded prefix of that path from the frontier toward
the graph tip.

The selector asks for exact hashes represented by a known predecessor path. It
does not treat arbitrary announced hashes as equivalent useful work. The
architectural invariant is:

> Historical work admitted by the ordered scheduler must correspond to a known
> ordered path ahead of the authoritative frontier.

The current experimental window is 512 entries. That is a parameter choice,
not the invariant. Window size, peer caps, and refill limits remain subject to
measurement and must not be confused with the requirement that selected work
has ordered predecessor coverage.

## 9. Ownership, Expiration, and Delivery

Scheduler replacement deliberately retains the existing request machinery:

* exact-hash ownership and ownership transitions;
* `mapAlreadyAskedFor` safety;
* late-delivery protection and owner identity checks;
* peer availability and source attribution;
* `AskFor`, `getdata`, block receive, and `ProcessBlock`.

One owner remains the default for each exact requested hash. A newer owner must
not be erased by a late response from an older owner. A request generation must
remain distinguishable from late delivery belonging to a previous generation.

Expiration begins at actual getdata wire transmission, with the conservative
60-second wire-origin bound and a small pending-wire safety net. The former
5-second enqueue-origin policy was a real amplifier, but changing expiration
alone did not solve the semantic scheduler failure. This is an example of a
subsystem retained and corrected rather than unnecessarily replaced.

## 10. Control Plane vs Data Plane Fairness

Stage 1 exposed a control-plane problem: strict FIFO could leave a solicited
headers response behind expensive block processing for very long periods. A
bounded priority extraction was added so expected headers receive service
without touching partial frames.

Stage 2 exposed the opposite problem: repeatedly prioritizing solicited
headers could leave ready block frames queued behind a continuing header
response. The validated bounded fairness rule prevents either side from
indefinitely starving the other.

The architectural invariant is:

> Control-plane messages require bounded service latency, while the data plane
> requires guaranteed productive forward progress. Neither plane may
> indefinitely starve the other.

The current one-priority/one-FIFO-turn mechanism is an implementation detail,
not a timeless ratio. Any future budgeting must be evaluated against both
header progress and connected block progress, and must not simply revert to
strict FIFO or make headers unconditionally dominant.

## 11. Incremental State Advancement

The first implementation of the replacement path exposed an important cost.
Every connected block followed:

```text
SetBestChain -> observer UpdateAnchor -> full graph Reanchor
```

Full reanchor work preserved or rebuilt the provisional suffix through global
operations during ordinary sequential progress. It produced approximately
1.55--1.70 seconds between block-processing events, while healthy CONTROL
processing was separated by milliseconds.

The corrected rule is local advancement for the common case. If the new
authoritative tip is exactly the next node on the current active provisional
branch:

```text
anchor H -> H+1
```

the graph advances the anchor and retains the existing provisional suffix.
Only the state that changed authority is reclassified. Heights and order after
the new anchor remain contiguous; source attribution and quarantine state are
retained; locator construction begins at the retained provisional tip and
falls back to the new anchor.

Full safe `Reanchor()` remains the fallback for a reorg, unknown anchor,
branch mismatch, or non-sequential jump. Correctness wins over the fast path
for structural changes.

**VALIDATED:** the focused benchmark performed 10,000 sequential advances
with `fast=10000` and `full=0`. Tests also cover active-window behavior,
source/quarantine retention, and reorg fallback.

The narrow post-fix Stage-2 Experiment B recorded:

* `613 received / 613 connected`;
* `0` orphans and `0` timeouts;
* active/inflight occupancy of `128`;
* minimum graph lookahead of `2000`;
* `621` fast anchor updates and `0` full reanchors;
* approximately `2.5 ms` average fast update time;
* approximately `26--34 blocks/s` during the short productive capture.

This is **VALIDATED narrow Stage-2 evidence**, not final scheduler validation.
Healthy A/B, B3, mainnet, and long-duration behavior remain open.

## 12. Migration Lessons and Falsified Approaches

The migration history distinguishes architectural falsification from ordinary
implementation defects.

| Item | Classification | Lesson |
|---|---|---|
| Aggressive enqueue-origin expiration | Architectural policy defect/amplifier | Queue time was charged to peers; wire-origin expiry is safer. |
| Adaptive expiry proposal | **SUPERSEDED** design | Measurement and replay favored the simpler conservative 60-second wire-origin policy. |
| Max-ahead-only admission | Architectural falsification | Being ahead in height does not prove predecessor continuity or useful supply. |
| First-orphan causal hypothesis | Incomplete hypothesis | The first orphan is evidence of a continuity problem, not a complete explanation of all waste. |
| Pipeline-empty recovery | Architectural limitation | A full but unproductive pipeline never arms an empty-pipeline recovery path. |
| Continuation locator ordering | Implementation defect | Locator order must provide a usable continuation context; the correction was narrow and testable. |
| Receive-lock starvation | Implementation defect | Expensive dispatch must not hold the receive-queue lock across unrelated work. |
| Header priority starving blocks | Implementation defect | Priority must be bounded and fair, not repeatedly unbounded. |
| O(N²) header ingestion | Implementation defect | Batch graph insertion was required to avoid repeated global work. |
| Per-block full reanchor | Implementation defect | Sequential authority changes require incremental advancement, not reconstruction. |
| Lookahead-aware dispatch-budget experiment | **SUPERSEDED/REVERTED** | It improved a control symptom but still made Stage-2 data dispatch unacceptably slow; bounded fairness remains the validated mechanism. |

The [header dispatch starvation audit](../forensics/ibd-header-dispatch-starvation-audit.md)
and [Stage-1c graph performance report](../forensics/ibd-header-graph-performance-stage1c.md)
retain the measurements and source-level boundaries behind these lessons.

## 13. Recovery Philosophy

The old recovery model tends to wait for a timeout, pipeline drain, orphan
pressure, or another `getblocks` cycle. That is transport-centric: it can miss
the fact that the node is busy while the useful frontier is not moving.

The target model is semantic. It maintains knowledge of the useful frontier
and detects inability to advance it even when transport occupancy is healthy.
Important signals and actions are:

* frontier progress and age of the ordered window front;
* peer reassignment for exact owned work;
* branch and quarantine state;
* recovery while the pipeline is occupied;
* bounded request expiration and late-delivery accounting.

The [semantic recovery design](../forensics/ibd-semantic-recovery-design.md)
describes the intended continuity-based recovery state machine. Its proposed
busy-but-unproductive recovery arms are **FUTURE/TARGET** unless explicitly
present in the current source. This document does not claim that all of those
actions are implemented.

## 14. Beyond IBD: Continuous Synchronization

IBD replacement is the first deployment boundary, not the final
synchronization architecture. Historical operational evidence gives no basis
for assuming that post-IBD relay is automatically reliable in every condition;
older Innova nodes have previously been observed to lose synchronization.

**FUTURE/TARGET:** a synchronization manager with explicit states:

```text
deep historical sync
  -> near-tip catch-up
  -> live tip tracking
  -> temporary peer loss
  -> falling behind
  -> automatic catch-up re-entry
```

Normal relay should not be replaced immediately. Near-tip behavior must first
be audited and measured: header continuation, relay announcements, recovery,
peer loss, reorg handling, and re-entry after lag. Only that evidence can show
how much of the ordered synchronization manager should remain active near the
tip.

## 15. Architectural Invariants

The following are normative requirements for future changes:

1. Consensus remains authoritative outside the scheduler graph.
2. Provisional headers never become authoritative merely by observation.
3. Historical scheduler work follows a represented predecessor path.
4. Exact-hash ownership remains single-owner unless explicitly redesigned.
5. Ownership transitions cannot erase a newer owner.
6. Late delivery cannot corrupt a newer request generation.
7. Request expiry originates from actual wire send, with bounded pending-wire safety.
8. The control plane cannot starve the data plane.
9. The data plane cannot indefinitely starve synchronization control.
10. Sequential authoritative progress must not require global graph reconstruction.
11. Reorg or branch mismatch must prefer correctness over fast-path performance.
12. Legacy-peer compatibility must not require a simultaneous network upgrade.
13. Scheduler optimization is evaluated by connected/frontier progress, not merely
    requests, receives, or full pipeline occupancy.
14. Source attribution and quarantine state survive ordinary sequential anchor
    advancement.
15. Window lookahead and locator continuity remain valid after every frontier
    advance.

## 16. Current Implementation Status

### Implemented

* scheduler-private provisional header graph;
* continuation locator;
* header observation mode;
* batch provisional graph insertion;
* bounded inbound header priority and receive-lock refactor;
* Stage-2 ordered selection behind an experimental flag;
* separate default-OFF regtest IBD override;
* incremental sequential anchor fast path with full-reanchor fallback;
* retained exact-hash ownership, late-delivery protection, and wire-origin
  expiration machinery.

### Validated so far

* deterministic graph, branch, quarantine, locator, fairness, and focused
  regression tests;
* the 10,000-step fast-anchor benchmark;
* a narrow fresh-source Stage-2 Experiment B with 613/613 connected blocks,
  zero orphans/timeouts, 128 active/inflight, and lookahead minimum 2000.

### Not yet validated

* full healthy CONTROL versus EXPERIMENT A/B after the incremental anchor fix;
* the B3 adversarial/impaired gate;
* fresh mainnet IBD;
* VPS observation;
* mixed-peer performance at scale;
* final post-IBD/live synchronization behavior.

### Future

* continuous synchronization and near-tip integration after a separate audit;
* semantic busy-pipeline recovery where the measured implementation still lacks
  it;
* default enablement or removal of legacy scheduling only after the validation
  ladder is complete.

## 17. Validation Ladder

Future work must proceed in this order:

1. deterministic unit tests;
2. narrow Stage-2 runtime proof;
3. healthy CONTROL versus EXPERIMENT A/B;
4. B3 adversarial/impaired test;
5. fresh real-mainnet IBD;
6. VPS observation;
7. post-IBD long-duration synchronization audit;
8. only then consider default enablement or removal of the legacy scheduler.

Each stage has a stop condition. A failure at one stage is not evidence to skip
to the next; it is evidence to identify and isolate the first broken boundary.
In particular, a productive narrow Stage-2 run does not establish healthy A/B,
and healthy A/B does not establish B3 or live synchronization.

## 18. Architectural Summary

Legacy Innova synchronization largely allowed peer announcement order to shape
historical work. The replacement architecture first learns an ordered
provisional view of the chain, independently determines which blocks are useful
next, and then uses available peers to deliver those exact blocks. Consensus
validation remains authoritative. Synchronization therefore moves from
announcement-driven reaction toward frontier-driven execution.

A new Innova node does not require the entire network to upgrade before it can
benefit from this model. Existing `getheaders`, `headers`, `getdata`, and
`block` messages are sufficient where compatible peers provide useful header
responses, while the legacy discovery path remains available as a fallback.
