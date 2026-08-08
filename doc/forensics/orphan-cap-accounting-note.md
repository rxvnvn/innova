# Orphan-cap accounting: exact production semantics (verification note)

Status: **verification only**. No code was changed.

Question audited: does `MAX_ORPHAN_BLOCKS_PER_PEER` bound *current* orphan occupancy or
*cumulative* events, and what exactly happens to a block rejected at that cap?

## 1. The limit is on CURRENT per-peer orphan occupancy, not cumulative events

* Counter: `mapOrphanCountByNode[peer]` (`main.cpp:132`), incremented exactly once per
  stored orphan (`main.cpp:7594`, in the insert branch) and decremented exactly once per
  stored orphan that connects or is otherwise removed (`main.cpp:7668`).
* Predicate: `PeerOrphanStorageLimitExceeded()` (`main.cpp:160-166`) returns
  `GetPeerOrphanCount(peer) >= 750`. The comment confirms this is now the sole enforcement
  point and is kept behavior-identical to the previous inline checks.
* Checked at two points, both against *current* occupancy at that instant:
  1. before validation, `ProcessMessage(block)`: `fWouldHitIbdOrphanLimit =
     PeerOrphanStorageLimitExceeded(...)` (`main.cpp:9795-9801`);
  2. inside `ProcessBlock`, orphan branch: the same predicate (`main.cpp:7554-7571`).
* The count is **not** cumulative: it rises as orphans are stored and falls as they connect
  (the flush loop at `main.cpp:7642-7677` decrements per connected orphan). A peer whose
  orphans flush below 750 immediately becomes eligible again.
* Separately, `PruneOrphanBlocks()` (`main.cpp:2948`, default cap 2500, `-maxorphanblocks`)
  bounds the **global** orphan map; it runs before the per-peer check (`main.cpp:7552`) and
  is a different mechanism.

## 2. What happens to a block rejected at ORPHAN_LIMIT_IBD

Rejection path (IBD branch, `main.cpp:7554-7571`): no penalty (`Misbehaving` only on the
non-IBD branch, line 7567), returns error, block is dropped.

* **Already-have state**: the block is recorded *nowhere* as received/known.
  - Not inserted into `mapBlockIndex` (validation never runs).
  - Not inserted into `mapOrphanBlocks` / `mapOrphanBlocksByPrev` / `mapOrphanBlocksByNode`.
  - Not given a stake-seen marker (`setStakeSeenOrphan` insert at 7585 is *after* the cap
    check, so it is skipped).
  - It IS recorded as rejected for sync recovery:
    `RecordRejectedBlockForSync(hash, fRetryEligible=false)` (`main.cpp:9880-9882` →
    `net.cpp:2152`), i.e. `REJECT_RETRY_SUPPRESSED`.
  - Negative-cache entries: global 5 s `ALREADY_ASKED_FOR_NEGATIVE_COOLDOWN_US`
    (`net.cpp:3507`, `net.h:626`) and peer-local 120 s
    `ORPHAN_LIMIT_REJECT_RETRY_COOLDOWN_US` (`main.cpp:9883-9889`, `net.h:627`).
* **Request eligibility**: the request ownership for this hash was already released on
  delivery (`ReleaseBlockRequestOwnerOnReceive`, `main.cpp:9777`) and the in-flight marker
  cleared (`BlockRequestTraceInFlightClear`, 9810). Re-request is then *blocked for 120 s
  from the same peer* by the peer-local cooldown (`AskFor`, `net.h:1731-1740`). Other peers
  are **not** blocked: the peer-agnostic negative cache is only 5 s, and a cross-peer ask is
  admitted and counted (`net.h:1742-1750`).
* **Orphan maps**: none of the three orphan maps receives the hash; `mapOrphanCountByNode`
  is not incremented. Instead the hash enters the peer-local rejected map
  `mapOrphanLimitRejectedBlocks` (`net.cpp:3459-3496`), bounded per-peer 750 / global 50000
  (`net.h:634-635`), keyed by `(CInv, peer)` with `nUntilMicros` (120 s) and the rejected
  block's `hashParent`.
* **Retry path**: two mechanisms, both bounded:
  1. At rejection time the client pushes a getblocks **from its best tip with
     `hashContinue = 0`**: `pfrom->PushGetBlocks(pindexBest, uint256(0),
     GETBLOCKS_SOURCE_ORPHAN_LIMIT)` (`main.cpp:7558-7560`). This re-announces the whole
     range from the client's best connected tip, which re-lists the missing parent as well
     as the rejected children.
  2. On any later accepted block, `RetryOrphanLimitRejectedOnParentConnect(hashParent,
     pfrom)` (`main.cpp:9849`, `net.cpp:3598-3659`) scans the peer-local rejected map and
     re-AskFor's every live entry whose `hashParent` matches, with
     `BLOCKREQ_SOURCE_ORPHAN_LIMIT_RETRY`, which bypasses the 120 s cooldown check
     (`net.h:1731-1732`). Entries are erased only after `AskFor` proves the request was
     retained; otherwise they stay in the bounded map.
* **Parent recovery**: NOT via the orphan walk-back `AskFor(BLOCKREQ_SOURCE_ORPHAN)` — that
  call (`main.cpp:7612-7614`) sits after the early `return error` on the cap branch and is
  never reached for a rejected block. The parent is recovered instead through the
  getblocks-from-tip re-announcement (mechanism 1) and, once the parent is accepted, the
  parent-connect retry (mechanism 2).

## 3. Bottom line for the explanatory model

The production cap matches the model's corrected semantics: it is a *current-occupancy* per
peer cap (not cumulative), and a rejected block is fully forgotten (no have state, no orphan
insert) while remaining recoverable through (a) a getblocks-from-tip re-announcement at
rejection time and (b) parent-connect-driven retry with cooldown bypass. The model's
`rejected` set + re-listing + parent-connect retry is a faithful analogue.
