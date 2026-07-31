# Frontier admission exemption

## Invariant

During IBD the block directly after the local active tip — the *frontier
block* — must always be allowed to enter the request pipeline, even when the
per-peer deferred block-request budget is zero because of unresolved orphan
pressure. Without this, an announced frontier block is deferred, then dropped
once the deferred queue fills, the missing parent is never requested, every
subsequent announcement of that block is deferred/dropped again, and the
orphan forest can never reconnect to the chain.

The fix guarantees:

* At most one frontier candidate is outstanding at any time (one global IBD
  slot). Repeated or malicious announcements cannot bypass this bound.
* Only the first *unknown* block inv of a getblocks response that was
  requested with the **current active-tip locator** is eligible.
* The exemption never bypasses: `AlreadyHave` (checked by the caller), block
  request ownership, duplicate askfor state, orphan-limit reject cooldown,
  `MAX_BLOCKS_IN_FLIGHT_PER_PEER`, or normal validation — all of these are
  still enforced by the ordinary `CNode::AskFor` path.
* The slot is cleared (and no longer blocks anything) when the frontier block
  is received, when the owning peer releases the request (queue removal,
  timeout, `ClearAskFor`), when the peer disconnects, or when a new active tip
  invalidates the old locator context.

## Problem it fixes

Forensic proof (see the forensic trace in `/tmp/opencode/blockreqtrace.txt`,
event set for `bdca1873cbfa33bd97231c368864a3ab679c12618b03cec2e02bce27f813c1dc`)
showed a closed cycle during a stalled IBD:

1. The getblocks response for the active-tip locator announced
   `bdca1873...` — height 117339, parent is the local tip `015ec957...`.
   It has no missing parent of its own; it is the missing link.
2. Because the announcing peers' unresolved orphan counts kept
   `peer_pressure >= MAX_DEFERRED_INV_ACTIVE_PER_PEER`, the deferred budget
   `GetDeferredBlockRequestBudget` was `0` and every block inv was deferred
   (`ASK_SKIP reason=orphan-pressure`, `INV_DEFER`, then
   `INV_DEFER_OVERFLOW`).
3. The frontier block was never admitted: `0` `ASK_SCHEDULE`, `0`
   `GETDATA_SEND`, `0` `BLOCK_RECEIVE`, `0` `BLOCK_RESULT`.
4. Each arrival of the same block was again an orphan (or an unknown), so
   orphan pressure never drained, the cycle never broke, and IBD stalled.

The dead-code helper `ShouldSkipBlockInvForOrphanPressure` was ruled out:
it is never invoked by the runtime build.

## Mechanism

Per connection:

* `CNode::fFrontierResponsePending` / `CNode::nFrontierLocatorHeight`
  (src/net.h).  The flag is armed in `SendMessages` when a getblocks whose
  locator index is `pindexBest` and stop hash is zero is actually flushed.
  `nFrontierLocatorHeight` records the tip height at flush time.

Per inv message (`ProcessMessage`, "inv" handler, src/main.cpp):

* On entry the pending expectation is consumed once.  The first *unknown*
  `MSG_BLOCK` inv of that response is tagged as the frontier candidate; every
  later inv in the same response is a normal candidate.

Per admission (`TryAdmitBlockInvOrDefer`, src/main.cpp):

* When the deferred budget is `<= 0`, a frontier candidate may still be
  admitted through `FrontierCandidateCanAdmit` if:
  * the announcing peer's orphan count is `> 0` (the budget is zero *because
    of* orphan pressure), and
  * global active pressure is below `MAX_DEFERRED_INV_ACTIVE_GLOBAL`, and
  * the active tip still equals `nFrontierLocatorHeight` (locator not stale),
    and
  * the global single slot is free.
* On grant the block is queued via ordinary `CNode::AskFor`, which continues
  to enforce every other request invariant.

Slot state (src/net.cpp, guarded by `cs_mapAlreadyAskedFor`):

* `FrontierCandidateCanAdmit` claims the slot; a claim means admission.  The
  same candidate re-announced by the same peer after admission is refused
  (`already-admitted`); any other candidate while the slot is busy is refused
  (`slot-busy`); a stale locator is refused (`locator-stale`); the claim
  expires after `FRONTIER_ADMISSION_EXPIRE_US` (30 s).
* The slot is cleared automatically by the block-request ownership lifecycle:
  on receive (`ReleaseBlockRequestOwnerOnReceive`,
  `ReleaseBlockRequestOwner(... "receive")`), on release
  (`queue-removal`, `timeout`, `clear`), on disconnect
  (`ReleaseBlockRequestOwnersForPeer`), and on tip change
  (`InvalidateFrontierOnTipChange` from `SetBestChain`).

## Regression tests

`src/test/p2p_sync_tests.cpp`:

* `frontier_admission_breaks_orphan_pressure_fixed_point` — reproduces the
  runtime fixed point: tip H present, H+1 first unknown inv of the frontier
  response, orphan count zeroes the budget, ordinary admission denied, H+1
  granted via the exemption and queued for getdata, full receive path clears
  the slot, drained pressure resumes ordinary deferred admission.
* `frontier_exemption_holds_single_slot_across_peers` — only one exemption
  outstanding; a different peer's candidate is refused while the slot is busy;
  a different peer cannot duplicate the owned request; disconnect releases the
  slot and a fresh candidate is then admitted.
* `frontier_admission_refused_when_locator_is_stale` — a response requested
  against an older locator is refused, and a fresh tip-context response is
  admitted.
* `frontier_admission_cannot_be_replayed_to_bypass_bound` — a re-sent
  response cannot grant the same candidate twice; only the first unknown block
  inv of a response is offered the slot; unrelated invs stay blocked.

## Files

* src/net.h — constants, API, `CNode` fields.
* src/net.cpp — slot state machine, release/disconnect/tip-change hooks.
* src/main.cpp — `TryAdmitBlockInvOrDefer` exemption, inv-handler candidate
  detection, `SendMessages` arming, `SetBestChain` invalidation.
* src/test/p2p_sync_tests.cpp — regression tests.
