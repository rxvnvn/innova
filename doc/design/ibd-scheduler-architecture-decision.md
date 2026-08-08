# IBD Scheduler Architecture Decision — Patch the Current Scheduler vs. Replace with a Headers-First Windowed Scheduler

## Status

- **Status:** ARCHITECTURE DECISION — final current verdict `REPLACE IBD
  SCHEDULER`. The earlier `PATCH CURRENT` verdict is retained below as the
  correct provisional decision before its pre-registered gate was run.
- **Date:** 2026-08-08
- **Audited trees:** Innova Core `master` @ `59d03e42` (HEAD, working tree);
  Blackcoin More `26.x` @ `1b54fa138` (v26.2.0-15-g1b54fa138)
- **Scope:** the block-download/IBD request scheduler only. No consensus, coin
  rule, message, or P2P wire change is proposed or implied by either option.
- **Evidence markers:** `Code fact` (verified in `src/` of the named tree),
  `Runtime observation` (forensic captures), `Working hypothesis`,
  `Offline replay` (decision-level deterministic replay), `Test fact`
  (repository's own test suite). All line numbers are against the audited
  trees named above.
- **Production behavior:** unchanged. This document is a decision record and a
  prescription; it implements nothing.

## Final current verdict (after the pre-registered gate)

**REPLACE IBD SCHEDULER.** The controlled B3 gate on clean `59d03e4` failed both clauses that the provisional decision made dispositive: the connected tip was flat for 241 s and later 304 s while the pipeline remained full and the gap widened (§12(b)), and the orphan pool pinned at 750 with 1847 `limit_reject=parent_unknown` outcomes (§12(c)). The conservative wire-origin expiry itself behaved correctly; it removed the aggressive 5 s mechanism but did not repair the full-but-useless pipeline. Therefore the escalation rule written in §13.3 before the run has fired. The decisive runtime record is `doc/forensics/ibd-gate-b3-head-result.md`.

## Provisional verdict before the gate (historical; superseded)

**PATCH CURRENT.** The evidence base identifies two independent drivers of the
observed IBD degradation: (a) a request-expiry policy that expired slow-but-live
deliveries (fixed by the wire-origin conservative 60 s expiry, already at HEAD),
and (b) a stalled-sync recovery arm keyed to *pipeline emptiness*, which is
provably blind in the measured degraded state. Both are addressable by
scheduler-only patches within the existing inv/getblocks admission model, using
only existing message types and no consensus change. A full headers-first
replacement (Blackcoin-style) is a real, architecturally superior destination
for the *root* removal of the frontier-hole failure class, but the evidence does
not justify taking its cost and risk now: the residual failure mode after the
expiry fix is unmeasured, and every measured failure driver has a smaller,
bounded patch. The verdict is therefore `PATCH CURRENT`, with a mandatory
post-fix re-measurement gate and a defined rule that escalates to `REPLACE IBD
SCHEDULER` only if the gate fails.

---

## 0. Evidence chronology and prior-vs-current findings

The scheduler at HEAD is 19 commits past the tree the two comparative audits
were written against. This matters: some claims in the evidence base describe an
older tree and must be read as prior findings, not current code.

| Commit | What changed | Prior finding it supersedes |
|---|---|---|
| `6cf6f5e` | audit base for `ibd-blackcoin-comparative-audit.md` and `ibd-pipeline-reset-feasibility.md` | — |
| `0f707b1` | release orphan stake markers on all removal paths | orphan-marker leak |
| `42154f0` | bound and expire getblocks request state | unbounded getblocks state |
| `73002a0` | reconcile in-flight state during peer cleanup | double-release on disconnect |
| `4244fb1` | rank IBD peers; reassign timed-out blocks | no cross-peer reassignment |
| `30be277` | preserve reassigned ownership on late receive | owner-identity on late receive |
| `cee0cba` | exclude regtest from hardened checkpoints | regtest checkpoint gate (test-only) |
| `59d03e4` | **wire-origin conservative block request expiry** (60 s) | the fixed 5 s timeout — the single largest measured waste class |

The conservative-expiry decision (`doc/design/ibd-conservative-block-request-expiration.md`)
is **implemented** at HEAD; the ABRE v1 adaptive design
(`doc/design/ibd-adaptive-block-request-expiration-v1.md`) is **superseded and
rejected** by the same evidence. The current HEAD therefore already differs from
both audits on the single most load-bearing parameter: in-flight expiry is now
wire-origin (getdata first socket send) with a 60 s ceiling and a 1 s
pending-wire bound (`net.h:2223-2352`, `BLOCK_IN_FLIGHT_TIMEOUT_US = 60 s`,
`MAX_PENDING_WIRE_US = 1 s`), per-batch stamping via `SendMessageMeta::firstSendUs`
(`net.cpp:7108-7133`). This AD evaluates the scheduler **as it is now at HEAD**,
and re-dates the "fixed 5 s timeout" statements in older docs to their proper
historical meaning.

---

## 1. Reconstructed state machine: Innova Core at HEAD 59d03e4

`Code fact` (current working tree).

Innova is an **inv/getblocks admission scheduler**, not a headers-first windowed
scheduler. Its full-node mode keeps no ordered header chain: `headers` handling
connects to the existing block index only (`main.cpp:9409-9598`), an unknown
parent triggers a fallback `getblocks` (`main.cpp:9517-9520`), and
`getheaders`/`fContinueHeaders` are SPV-mode-only (`main.cpp:8772, 9595-9596,
9691, 10771`). There is no `pindexBestKnownBlock`/`pindexLastCommonBlock`, no
`FindNextBlocksToDownload`, and no window.

### 1.1 Request queues and getblocks pipeline (per-peer)

- `getBlocksIndex` + parallel `getBlocksHash/RecoveryIds/Sources`
  (`net.h:1219-1222`); single-flight outstanding cycle
  `GetBlocksOutstandingState` (`net.h:1230-1253`).
- `PushGetBlocks` (`net.cpp:5764-5870`): 5 s identical-locator dedup
  (`net.cpp:5782-5796`), single-slot coalescing by
  `GetBlocksSourcePriority` (`net.cpp:5744-5762`; RECOVERY 100 … OTHER 10).
- Flush in `SendMessages` under the single-flight gate
  (`main.cpp:10690-10757`); frontier expectation armed when locator ==
  `pindexBest` and stop == 0 (`main.cpp:10726-10735`); recovery armed via
  `RecordOrdinaryGetBlocksCommitted` (`main.cpp:10747-10749`).
- Server side: window 1000, walk from `pindexLocator->pnext`
  (`main.cpp:9126-9300`), `hashContinue` continuation (`main.cpp:9262`), token
  bucket + repeat-cooldown anti-spam (`net.cpp:505-508`).
- Client-side outstanding-cycle timeout 15 s (`net.h:1058`,
  `ExpireGetBlocksOutstanding` `net.cpp:6506-6543`).

### 1.2 Admission and budgets

- `TryAdmitBlockInvOrDefer` (`main.cpp:424-514`): non-IBD admits directly;
  IBD budgets via `GetDeferredBlockRequestBudget` (`main.cpp:285-371`).
- Budget = `max(0, min(128 − peerActive, 512 − globalActive))`
  (`main.cpp:338-340`); per-peer 128 (`net.h:565`), global 512 (`net.h:567`);
  `peerActive = setAskForBlocks + setBlocksInFlight` (`main.cpp:314-323`).
- Zero budget → defer into `deferredBlockInv` (cap 1000, `net.h:569`), except
  the frontier candidate exemption
  (`FrontierCandidateCanAdmit`, `net.cpp:3359-3424`; global single slot,
  30 s expiry, stale-locator refusal).
- Refill from the deferred queue (`RefillDeferredBlockRequests`,
  `main.cpp:516-659`), bounded work 256 (`net.h:571`).

### 1.3 In-flight and ownership

- `setBlocksInFlight` / `mapBlockInFlightSince` / `mapBlockInFlightWireUs`
  (`net.h:1317-1330`); `MarkBlockInFlight` / `ClearBlockInFlight`
  (`net.h:2360-2440`).
- Ownership ledger `mapBlockRequestOwners` (`net.cpp:3138`, states
  QUEUED/IN_FLIGHT `net.h:585-589`); assign/transition/release
  (`net.cpp:3145-3331`), identity-checked release on receive
  (`net.cpp:3267-3295`).
- Expiry: wire-origin 60 s per-batch, 1 s pending-wire bound, no penalty on the
  pending-wire path (`net.h:2223-2352`).
- Peer ranking `SyncPeerScore` (`net.cpp:202-238`): height/lag/freshness/
  recency/in-flight/ping/connect, **no inbound/outbound term**.
- Per-hash redirection `ChooseIbdBlockRequestTarget` (`net.cpp:1961-2064`):
  exact-hash timeout, degraded-quality, and concentration rules
  (400‰, `net.h:726-727`).

### 1.4 Pipeline wake and refill

- `RequestBlockPipelineWake` (`net.cpp:3757-3791`) and
  `MaybeProcessPipelineWake` (`net.cpp:3840-4042`), invoked once per handler
  pass (`net.cpp:8667`).
- Terminal outcomes (`net.h:277-292`): pipeline not empty → queued/outstanding
  getblocks → deferred-refill work → getblocks queued.
- 1 s getblocks cooldown (`net.cpp:247`); candidate selection by
  `SyncPeerScore` with round-robin offset (`net.cpp:3982-3991`).
- Continuation on drain: peer ahead + `setBlocksInFlight <= 1` +
  `getBlocksIndex.empty()` + 10 s cooldown → `PushGetBlocks(pindexBest, 0,
  CONTINUATION)` (`main.cpp:10068-10109`).

### 1.5 Stalled-sync recovery

- `CStalledSyncRecoveryState` (`net.h:70-101`); armed only by an ordinary
  getblocks to an advance-capable peer (`net.cpp:996-1007`).
- `ShouldRecover` (`net.cpp:923-994`): requires **pipeline empty**
  (`fPipelineActive == false`, `net.cpp:950-966`), height-progress reset, capped
  exponential cooldown (`net.cpp:968-971`).
- Action (`net.cpp:2378-2567`): `PushGetBlocks(pindexTip, 0, RECOVERY)` +
  one-shot `AskFor` of a rejected hash (`net.cpp:2548-2561`);
  `-syncstalltimeout` 15 s / `-syncstallcooldown` 15 s (`net.cpp:8711-8714`).
- **Window-front / oldest-in-flight-age arm: NOT present.** The oldest in-flight
  head-age is computed only for forensics (`net.h:2317-2332`), never consulted
  by the scheduler.

### 1.6 Orphans and IBD definition

- Per-peer orphan occupancy cap 750 (`main.h:102`; `main.cpp:160-166`); global
  pool 2500 (`main.h:100`); orphan-limit reject → getblocks-from-tip
  (`main.cpp:7556-7560`) + 120 s peer-local cooldown (`net.h:628`).
- **No parent-known filter** in admission: `TryAdmitBlockInvOrDefer`,
  `AskFor`, and `ChooseIbdBlockRequestTarget` never read `hashPrevBlock`
  (`Verified`). Parents are resolved only on the receive/accept side
  (`ProcessBlock` orphan shunt, `main.cpp:7546`) and via the `WantedByOrphan`
  walk-back (`main.cpp:7623`).
- IBD definition: checkpoint estimate + peer-ahead lag + active-catchup +
  stale-recv latch (`main.cpp:3557-3626`); recomputed per call, no latched
  `fInitialBlockDownload` consumed by scheduling.

---

## 2. Reconstructed state machine: Blackcoin More at 1b54fa138

`Code fact` (`src/net_processing.cpp`, `src/net.h`, `src/validation.cpp`,
`src/node/blockstorage.cpp`, `src/kernel/chainparams.cpp`).

Blackcoin is a **headers-first, windowed scheduler**:

1. **Headers chain.** One peer syncs headers at a time (`nSyncStarted` latch,
   `net_processing.cpp:5805-5831`); `ProcessNewBlockHeaders` builds an ordered
   `CBlockIndex` tree independent of the connected tip
   (`validation.cpp:4128-4168`); `m_best_header` tracks the highest known header
   (`validation.h:1005`). Header gaps are impossible by construction: a header
   must connect to an existing index entry or genesis
   (`validation.cpp:4043-4048`, `blockstorage.cpp:224-228`).
2. **Per-peer knowledge.** `CNodeState` holds `pindexBestKnownBlock`,
   `pindexLastCommonBlock`, `hashLastUnknownBlock`
   (`net_processing.cpp:422-428`); `ProcessBlockAvailability` advances
   `pindexBestKnownBlock` (`net_processing.cpp:1446-1459`).
3. **Download window.** `FindNextBlocksToDownload`
   (`net_processing.cpp:1480-1516`; core `:1547-1600`) walks the ordered header
   chain from `pindexLastCommonBlock->nHeight + 1` to `+ BLOCK_DOWNLOAD_WINDOW`
   (1024, `:135`); the window-front sentinel (`nWindowEnd + 1`, `:1550`) exists
   purely to detect stalling (`:1582-1588`).
4. **In-flight ownership.** Global `mapBlocksInFlight` (hash → peer,
   `:1040-1041`), per-peer ordered `vBlocksInFlight`; a block is selected only if
   `!IsBlockRequested(hash)` globally (`:1275-1278`); up to
   `MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK = 3` parallel copies via compact blocks
   (`net_processing.h:32` — see correction §15).
5. **Stall detection.** Window-front ownership, **busy pipeline**:
   when this peer's queue is empty but the window front is in flight elsewhere,
   the owner is declared `m_stalling_since` (`:6193-6198`); disconnect after
   `m_block_stalling_timeout` (default 2 s, `:120`), doubling to 64 s
   (`:122`), reduced ×0.85 on progress (`:2152-2160`).
6. **Download timeouts.** Head-of-queue: `spacing × (1 + 0.5·(N−1))`
   (`:6119-6127`; mainnet spacing 64 s, `kernel/chainparams.cpp:98`); headers:
   15 min + 1 ms/header (`:61-62`, `:6129-6158`); stale tip:
   `TipMayBeStale` = no tip update for 3×spacing AND `mapBlocksInFlight.empty()`
   (`:1422-1430`).
7. **Peer preference.** `fPreferredDownload` (outbound or NoBan,
   `:3735-3736`) during IBD; inbound fallback when no preferred peer or the
   in-flight map is empty (`:5787-5803`).
8. **Recovery = emergent disconnect-and-reassign.** Stalled/disconnected peers'
   entries are removed from `mapBlocksInFlight` (`FinalizeNode` `:1787-1842`);
   the freed hash is immediately re-requestable by any peer that has it
   (`!IsBlockRequested` → `FindNextBlocks` re-admits). No separate recovery
   message exists.
9. **Near-tip direct fetch.** `CanDirectFetch` (tip within 20×spacing,
   `:1432-1435`) triggers a direct getdata of up to 16 consecutive blocks toward
   the announced header (`:2951-3011`).
10. **IBD exit.** `IsInitialBlockDownload` = `LoadingBlocks()` ∨ null tip ∨
    below `nMinimumChainWork` ∨ tip older than `max_tip_age` (24 h), latched in
    `m_cached_finished_ibd` (`validation.cpp:1729-1754`).

`Test fact`: `test/functional/p2p_ibd_stalling.py` pins the whole lifecycle —
staller declared when the 1024-window is exceeded, disconnected after ~2 s, the
block re-requested from another peer, timeout doubling 2→4→8 s, and reduction on
progress.

---

## 3. Innova mechanisms → Blackcoin equivalents (mapping)

`Code fact`. "Mechanism" here means *a distinct scheduler behavior*, not a
function signature; the mapping is behavioral.

| Concern | Innova @ 59d03e4 | Blackcoin @ 1b54fa138 | Assessment |
|---|---|---|---|
| Known ordered chain | none in full-node mode (`main.cpp:9409-9598`) | header tree + `m_best_header` | Blackcoin has the missing-hash oracle; Innova cannot name the exact next missing block |
| Window | none — frontier `getblocks` + inv admission | `BLOCK_DOWNLOAD_WINDOW` 1024 after `pindexLastCommonBlock` | Blackcoin bounds future supply; Innova bounds only the request pipeline (512/128) |
| Per-peer in-flight cap | 128 via deferred budget (`net.h:565`) | 16 (`:117`) | Innova is an order of magnitude more aggressive |
| Global in-flight ownership | single-owner ledger + set | `mapBlocksInFlight`, up to 3/block via compact | Innova: 1 owner, reassign on timeout; Blackcoin: parallel copies |
| Stalling detection | pipeline **empty** + 15 s no-height (`net.cpp:950-966`) | window **front** owned elsewhere, busy pipeline (`:6193-6198`) | Blackcoin's invariant is "window must move", Innova's is "pipeline must empty" |
| Stalling timeout | none (no disconnect on stall) | 2 s → 64 s, ×0.85 on progress | Blackcoin bounds escalation; Innova has no equivalent |
| Download timeout | wire-origin 60 s (`net.h:2228`), no disconnect | front `spacing × (1 + 0.5·N)` → disconnect | Both exist; Innova's is batch-clocked, Blackcoin's is queue-front |
| Headers timeout | n/a (no headers sync in full-node) | 15 min + 1 ms/header | Innova has no headers-sync state |
| Stale-tip action | recovery getblocks only | `TipMayBeStale` → extra outbound peer | Blackcoin grows the peer set |
| Recovery | `CStalledSyncRecoveryState` getblocks + one-shot AskFor | emergent reassign of freed hashes | Blackcoin recovery is property of the window, not a message |
| Near-tip direct fetch | n/a | `CanDirectFetch` ≤16 blocks | needs announced headers, which Innova full-node lacks |
| Peer preference | `SyncPeerScore`, no inbound/outbound term | outbound/NoBan first, inbound fallback | Blackcoin biases toward stable outbound peers |
| Tests pinning guarantees | `p2p_sync_tests.cpp` (unit) | `p2p_ibd_stalling.py` (functional) | Blackcoin's stall/reassign guarantee is pinned functionally; Innova has no functional IBD suite |

---

## 4. What is accidental complexity in the current scheduler

`Code fact` + `Working hypothesis`. "Accidental" = present because of how the
inv/getblocks admission model grew, not because a decision demanded it.

1. **Frontier re-announce loop as the only repair.** A timed-out or lost block
   is not re-requested by hash; the scheduler re-runs a frontier `getblocks`
   (`PushGetBlocks(pindexBest, 0)`) and hopes the peer re-announces the missing
   range. Evidence this is blind: `ibd-blackcoin-comparative-audit.md` §8.1
   (`Runtime observation`: 1.75 M `unknown` inv replies in expB); the
   getblocks/hashContinue audit's V7/V8 loop, where a frozen deep locator is
   answered with 1-item/empty responses and suppressed repeats
   (`getblocks-hashcontinue-batch-size-audit.md` §11).
2. **The 10 s continuation cooldown + pipeline-drained predicate.** The drain
   condition (`main.cpp:10068-10109`) conflates "pipeline drained" with "time to
   re-ask" and is rate-limited to 10 s; in the degraded state this produces
   exactly the observed `GETBLOCKS_CONTINUE ... 1/1` churn
   (`getblocks-hashcontinue-batch-size-audit.md` §8).
3. **Three overlapping request origins for the same need.** Inv-response,
   continuation, and prefetch all push getblocks with slightly different
   locators and priorities (`GetBlocksSourcePriority`,
   `net.cpp:5744-5762`). This is a workaround for the missing exact-missing-hash
   knowledge, not a design goal.
4. **`fPipelineActive` as a binary stall signal.** A single boolean over
   "any peer has in-flight or queued getblocks" (`net.cpp:2413-2417`) is the
   recovery gate. It is provably blind in the measured state
   (`ibd-degradation-runtime-validation.md`: `terminal_pipeline_not_empty` =
   99.94% of wake outcomes, pipeline empty 105 ms of 10 683 s in expB).
5. **No parent-known filter.** Admission never checks that a requested block's
   parent is downloaded/in-flight/owned (`Verified`, §1.6). This is the single
   largest difference from Blackcoin's "parents are always present in the
   pipeline" invariant and is what lets orphan pressure accumulate
   (`max-ahead-window-audit.md` §6; runtime B3 `limit_reject=4242`).
6. **Budgets as the de-facto window.** With no ordered chain, "the window" is
   implemented as the 512/128 request budgets plus orphan pressure; this is why
   the max-ahead invariant must be expressed as admission policy
   (`max-ahead-window-audit.md` §8, policy D).

---

## 5. Minimum replacement scope (if REPLACE were chosen)

`Working hypothesis` (scope reasoning from the two mapped trees). A faithful
Blackcoin-style replacement inside Innova would require, at minimum:

1. A **full-node header chain** that may diverge from the active tip
   (`AcceptBlockHeader`-style storage, `m_best_header` equivalent). Currently
   the headers message is connect-to-index-only in full-node mode and
   `getheaders` is SPV-only (`main.cpp:9409-9598, 8772, 10771`). This is the
   load-bearing prerequisite; nothing else is possible without it.
2. **Per-peer `pindexBestKnownBlock`/`pindexLastCommonBlock`** with ancestor
   walks, replacing height/hash-only tracking (`nBestKnownHeight`).
3. **A windowed selector** (`FindNextBlocksToDownload` equivalent) over the
   header chain, replacing the getblocks-frontier admission path.
4. **Window-front stall detection** replacing the `fPipelineActive` gate, with
   an escalation/reduction timeout (2→64 s, ×0.85).
5. **Precise reassignment** of a freed hash to any peer that has it, replacing
   the frontier re-announce loop.
6. **`CanDirectFetch`** near-tip, which presupposes (1).
7. **Preservation of all existing fixes**: single-owner invariant
   (`30be277`), peer ranking + timeout reassignment (`4244fb1`), in-flight
   reconciliation (`73002a0`), frontier-admission exemption
   (`frontier-admission-exemption.md`), orphan-limit cooldown and
   parent-connect retry (`orphan-cap-accounting-note.md`), and the 60 s
   wire-origin expiry (`59d03e4`).
8. **Wire compatibility with old/mixed Innova peers.** All scheduler input and
   output uses pre-existing messages only (`getheaders`, `headers`,
   `getblocks`, `inv`, `getdata`, `block`) — no new message types. The
   `getheaders` server handler is **not** SPV-gated: any peer may request
   headers and receives up to 2000 (`main.cpp:9376-9388`), so an old Innova
   full node already answers a headers-first scheduler. The constraints run
   the other way: old full-node peers never *send* `headers`
   (`fContinueHeaders` is SPV-only), so a replacement must not *require*
   `headers` from every peer — it must keep the `getblocks`/`inv` announce
   path as the fallback for legacy peers and SPV nodes. That is precisely the
   dual-path behavior Blackcoin already implements (header selection from some
   peers, inv/announce admission from all), so mixed-version operation is a
   deployment property of the replacement, not a new mechanism.

Items 1-3 are architectural (they change what information the scheduler holds,
not just what it does with it). Items 4-6 are scheduler-layer. Items 7-8 are
compatibility constraints: a replacement must not regress any landed fix
(item 7) and must interoperate with the installed Innova peer population
during a version-mixed transition (item 8).

---

## 6. Header-availability question: does Innova have the header info to drive a windowed scheduler?

`Code fact`. **Currently: no, in full-node mode.**

- Full-node mode never sends `getheaders` (`getheaders`/`fContinueHeaders`
  gated on `fSPVMode`, `main.cpp:8772, 9595-9596, 9691, 10771`).
- The `headers` message handler requires each header's parent to already exist
  in `mapBlockIndex` (`main.cpp:9512`); an unknown parent triggers a fallback
  `getblocks` (`main.cpp:9517-9520`), and known-parent headers only generate
  block `getdata` (`main.cpp:9589`). **No `CBlockIndex` header tree is built in
  full-node mode** — only SPV mode constructs header indices
  (`main.cpp:9546-9574`).
- Therefore the "window front is a concrete block" property that Blackcoin's
  scheduler depends on cannot be computed from the data Innova full-node
  currently maintains.

**Mitigation possibility.** The SPV header chain (`main.cpp:9546-9574`) proves
the codebase can already build an independent header tree; wiring the same
construction into full-node mode and maintaining `nBestKnownHeight` from
announced headers is the enabling change. This is not a rewrite of validation —
`AcceptBlockHeader`-style storage is additive — but it is a prerequisite change
to the scheduler's *information model*, and it is exactly the boundary the AD
treats as "REPLACE."

---

## 7. Throughput models compared

`Code fact` + `Offline replay` + `Runtime observation`.

### 7.1 Innova model (at HEAD, with the 60 s wire-origin expiry)

Request pipeline = frontier `getblocks` responses + relay INVs, admitted up to
the 512/128 budgets, expired at `wire + 60 s`, re-covered by frontier
re-announce. Steady throughput ceiling = **min(peer serve rate, connect rate)**,
with the pipeline never allowed to be a bottleneck at healthy speed (500-800
connects/s, `max-ahead-window-audit.md` §4.1).

Degraded profile (RTT 13.9 s, ~10% drop; pre-fix runs B2/B3):
`ibd-degradation-runtime-validation.md`:
- Static-tip phase: BULK-dominated (3005 req, 95.3%), 90.3% getblocks
  dedup-suppression.
- Moving-tip phase: RELAY-dominated (6567 req, 78.3%), 93.5% suppression,
  `limit_reject=4242`, orphan cap pinned at 750, `late_received=3455`.

After the expiry fix, the dominant amplifier (5 s expiry → 100% false expiry of
IMPAIRED live deliveries) is gone; the economic replay
(`ibd-conservative-block-request-expiration.md` §2.1) predicts 3.5% false expiry
at 60 s vs 100% at 5 s, with 0 pending losses.

### 7.2 Blackcoin model

Windowed selection from the header chain: per-peer ≤ 16 in-flight, window 1024,
every hash globally in-flight-checked. Throughput ceiling is the same
**min(serve, connect)**, but the *loss* path is bounded: a dropped block is
re-requested by exact hash within ~2 s + one RTT (or up to `spacing × (1+0.5N)`
for the front), and the window never admits unconnectable future supply because
selection walks an ordered chain whose parents are present.

### 7.3 Verdict on throughput

Both ceilings are connect-rate-limited; neither can manufacture connect
bandwidth (`max-ahead-window-audit.md` §7). The difference is **waste rate under
loss** and **loss-detection latency**, not the healthy ceiling. The expiry fix
already removed the largest Innova waste class; the remaining Innova loss path
(frontier re-announce, 1-item continuation churn) is slower to recover than
Blackcoin's exact-hash reassignment, but the magnitude is unmeasured post-fix.

---

## 8. Known failure modes compared

| Failure mode | Innova @ HEAD | Blackcoin | Evidence |
|---|---|---|---|
| Expire live slow delivery | fixed (60 s wire-origin) | avoided (2 s stall is *disconnect*, block reassigned not expired) | `conservative-expiration.md` §2 |
| Pipeline busy but unproductive (semantic stall) | **present**: recovery gate = emptiness, which never happens | absent: stall defined vs a target, works busy | expB: 99.94% terminal-not-empty, 0 recovery attempts |
| Frontier hole (missing link at the front) | re-announce loop, can repeat indefinitely | exact-hash reassign, bounded by escalation | comparative audit §8; getblocks audit §11 |
| Orphan pressure ahead of a missing parent | can accumulate to 750 cap (no parent-known filter) | impossible (parents present by construction) | B3 `limit_reject=4242`; max-ahead §6 |
| Peer withholds one block then serves the rest | 60 s expiry → re-announce → eventually served or re-listed | 2 s staller detect → disconnect → reassign (functional test) | `p2p_ibd_stalling.py` |
| Far-ahead future supply flooding deferred queue | unbounded ahead (bounded only by 1000 deferred/peer) | window 1024 caps it | max-ahead §4.4/§6 |
| Churn from slow-but-live peer | peer stays, timeout_score suppressed (60 s) | peer may be disconnected (2 s policy tuned to 64 s spacing) | `conservative-expiration.md` §8 |

The decisive asymmetry is the second row: Innova's *only* stall signal is
pipeline emptiness, and the measured state is never empty. That is the failure
mode the PATCH path fixes by re-arming recovery on progress/continuity, and the
REPLACE path fixes structurally.

---

## 9. Architectural and implementation complexity compared

`Code fact` + `Working hypothesis`.

| Dimension | PATCH CURRENT (proposed) | REPLACE IBD SCHEDULER |
|---|---|---|
| Volume | ~200-300 lines of C++ (arm eval, continuity locator, targeted reassign, metrics) per `ibd-semantic-recovery-design.md` §Final | header tree + per-peer state + window selector + stall machine: multi-thousand-line, multi-week change |
| New state | bounded continuity quota + 3-5 counters | `m_best_header`, per-peer `pindexBestKnownBlock`/`LastCommonBlock`, window cursor |
| New message types | none | none (headers/getheaders already in protocol) |
| Consensus surface | zero | zero (but header-tree validation must match consensus rules; `AcceptBlockHeader` must be correct for a chain diverging from the tip) |
| DoS surface | bounded by existing cooldowns and quotas | new: headers flooding, staller-disconnect semantics (Blackcoin mitigates via `HEADERS_DOWNLOAD_TIMEOUT`, escalation caps) |
| Regression risk | low; isolated to recovery arming + one action path | high; must not regress 7 landed fixes (§5.7) |
| Test effort | extend `p2p_sync_tests.cpp` + 1 functional-style replay | new functional IBD suite (`p2p_ibd_stalling.py` port) + port of every existing invariant test |
| Rollback | flag flip (`-ibdsemrecover`, default off) | dual scheduler selectable by flag, or a full revert |

---

## 10. Concrete Blackcoin code/concept reuse

`Code fact` + `Working hypothesis`. What is genuinely portable, in order of
cost:

1. **Window-front stall concept** — portable to the existing ownership ledger
   without a header chain: "oldest in-flight block older than M while tip has
   not advanced" is computable from `setBlocksInFlight` +
   `mapBlockInFlightSince` today. This is the core of the PATCH path
   (`ibd-blackcoin-comparative-audit.md` §9.1, §12; the head-age is already
   computed forensically at `net.h:2317-2332`).
2. **Continuity locator** — a recovery getblocks whose locator is the highest
   *connected* ancestor of the stuck frontier (or an exact `getdata` for the
   specific missing hash), instead of `getblocks(pindexTip, 0)`
   (`ibd-semantic-recovery-design.md` §4.1 items 2-3; §3 frontier-hole
   selection).
3. **Escalation/reduction timeout** — the 2→64 s doubling and ×0.85-on-progress
   policy ports directly to the 60 s expiry as a *disconnect-free* progress
   feedback (comparative audit §9.4; re-derived floor, not 2 s — Innova spacing
   is 15 s, `main.cpp:82`).
4. **Outstanding-getblocks as activity evidence** — feeding
   `getBlocksOutstandingSources` age into the recovery predicate
   (comparative audit §9.5).
5. **Outbound-first preference** — a small `fInbound` term in `SyncPeerScore`
   or the wake/recovery candidate loops (comparative audit §9.6).
6. **`CanDirectFetch`** — requires announced headers (needs §6's header-availability
   change); not portable to the current full-node information model.
7. **Full `FindNextBlocksToDownload` + 1024 window** — requires the header chain;
   part of REPLACE scope.

---

## 11. Migration / A-B strategy (contingent, if REPLACE wins the gate)

`Working hypothesis`. The decision today is PATCH, but the AD must define what
a responsible replacement looks like so the escalation rule (§13) has a concrete
target:

1. **Enable the header chain behind a flag** (e.g. `-ibdheadersfirst=1`),
   leaving the inv/getblocks path as the default until proven. Headers
   construction (§5.1) is additive; run both states concurrently in
   observation mode with no scheduling effect.
2. **A/B on the existing impaired-proxy harness** (RTT 13.9 s, ~10% drop, the
   B2/B3 proxy used in `ibd-degradation-runtime-validation.md`): same chain,
   same node A, A/B node B. Compare: connects/s, waste/announcements, orphan
   cap saturation, tip-gap over time, and the §12 invariant.
3. **Rollout as a flag flip**, not a fork: `REPLACE` here means "the scheduler
   reads the header chain instead of the inv stream," both compiled in, default
   off, rolled back by unsetting the flag. No wire, consensus, or storage
   migration is required.
4. **Exit criteria** for the A/B to be declared a win: the headers-first
   variant must hold the §12 invariant in the impaired profile *and* on a healthy
   mainnet flood without regressing the landed fixes (§5.7), and without a
   sustained increase in memory (header chain) or DoS exposure.
5. If the gate is NOT met, the PATCH path continues to own the scheduler.

---

## 12. Continuous-useful-IBD success invariant, evaluated for both

Define the invariant the scheduler must hold during IBD:

> **Continuous useful IBD:** while the peer set advertises a chain ahead of the
> connected tip, the connectable frontier advances at a rate consistent with
> real peer supply and link, with (a) received ≈ connected during healthy
> phases, (b) no sustained (minutes-scale) flat tip while the pipeline is full
> and peers are ahead, (c) orphan pressure bounded away from the cap except
> during genuine reorg/announcement races, and (d) every lost block recoverable
> within a bounded, data-derived time.

### Innova @ HEAD (pre-gate, pre-fix evidence)

- (a) **Healthy phase holds** (CONTROL: 1086/1086/1085 req/recv/conn, zero
  timeouts/orphans; `ibd-degradation-runtime-validation.md` §1).
- (b) **Violated in the degraded profile pre-fix** (B3: flat tip at 4, orphans
  pinned at 750, tip gap 1063+ and widening; recovery never armed because the
  pipeline was never empty).
- (c) **Violated pre-fix** (orphan cap hit 4242 times; `limit_reject =
  parent_unknown = 4242`).
- (d) The 5 s expiry violated it wholesale; the 60 s wire-origin expiry
  restores bounded recovery *at the policy level* (`conservative-expiration.md`
  §2.1: 0 pending losses, all 1 130 never-delivered detected by wire+60 s), but
  **the re-measurement of (b) and (c) on the current HEAD has not been run.**

### Blackcoin

- (a) Healthy: same ceiling (connect-rate-limited).
- (b) Holds structurally: stall is defined against the window front, so a busy
  but blocked pipeline is detected and repaired.
- (c) Holds structurally: parents are present in the header chain by
  construction; orphan accumulation ahead of a hole is impossible.
- (d) Holds by construction + `p2p_ibd_stalling.py`: a lost/reassigned block is
  re-requested by exact hash.

### Evaluation

Neither option manufactures connect bandwidth (invariant is about *useful*
IBD, and a window cannot fix a connect bottleneck — `max-ahead-window-audit.md`
§7). The two options differ in which clause they threaten to violate: PATCH
targets (b)-(c) via a rate/continuity recovery arm and (optionally) a
parent-known admission filter, but *relies on the post-fix re-measurement to
prove* (b)-(c) are repaired; REPLACE makes (b)-(d) hold by construction but
carries the cost/risk of §9 and presupposes §6. The invariant does **not**
force a particular architecture; it forces a measurement of (b)-(c) on the
current HEAD before a replacement is justified.

---

## 13. Decision matrix and final verdict

### 13.1 Matrix

Weights: **Evidence fit** (does the evidence support the option as the next
action), **Risk**, **Cost**, **Coverage of the measured failure modes**,
**Root removal vs. mitigation**.

| Criterion | PATCH CURRENT | REPLACE IBD SCHEDULER |
|---|---|---|
| Fixes the measured #1 waste (5 s expiry) | already done at HEAD | requires carrying the fix into a new scheduler |
| Fixes the measured #2 failure (empty-pipeline recovery arm) | small, bounded, behind a flag | structural, but requires §6 header-availability work first |
| Covers the residual B3 orphan/frontier-hole class | partial: needs parent-known filter (max-ahead policy D) | full, by construction |
| Evidence base supports as the *next* step | unanimous (semantic-recovery, pipeline-reset, comparative §12, max-ahead §8) | deferred by every audit ("architectural, not a quick fix") |
| Post-fix measurement exists | **no** — the gate | n/a (not started) |
| Risk | low, reversible by flag | high, multi-week, must not regress 7 landed fixes |
| Consensus/wire surface | zero | zero |
| Rollback | flag flip | dual-scheduler flag or revert |

### 13.2 The gate

The single reason this is not a "PATCH AND STOP" doc is that clause (b)-(c) of
§12 were last measured on the **pre-fix** tree. The mandatory next step,
regardless of any further patching, is:

> **Re-run the impaired-profile harness (B3 profile: moving tip, RTT 13.9 s,
> ~10% drop) and the mainnet-flood profile (73002a0) on the current HEAD
> (`59d03e4`) with `-ibdforensic=1` and the 60 s wire-origin expiry in place.**
> Record: connect rate, tip-gap over time, orphan-cap saturation, waste class
> breakdown, and `stalled_recovery_attempts`.

This is exactly the open question already recorded in
`max-ahead-window-audit.md` §12 ("Does the 5 s-timeout removal (59d03e4) alone
change the 73002a0 picture…?").

### 13.3 Escalation rule

If, **after the §13.2 gate**, the B3 profile still violates §12(b)-(c) on HEAD —
i.e. tip flat at minutes-scale with a full pipeline and a widening gap, or the
orphan cap still saturating with `limit_reject` counts comparable to the pre-fix
run — then the next decision point is **REPLACE IBD SCHEDULER**, to be pursued
via the §11 A/B path (header chain behind a flag, dual-scheduler A/B, then a
flag-flip rollout). In that case the replacement must preserve the seven landed
fixes (§5.7) and be validated against `p2p_ibd_stalling.py`-equivalent
functional tests.

### 13.4 Provisional verdict before the gate (historical)

> **PATCH CURRENT** — on the evidence: (1) the largest measured waste class was
> already eliminated by the wire-origin 60 s expiry at HEAD; (2) the second
> measured failure — a stalled-sync recovery arm gated on pipeline emptiness —
> is repaired by a small, bounded, flag-gated scheduler patch (rate/continuity
> arming, continuity locator, targeted hash reassignment) reusing existing
> ownership and `getBlocksOutstanding` state; (3) every comparative audit in the
> evidence base recommends exactly this incremental step and defers a
> headers-first rewrite; (4) a replacement's cost, risk, and header-availability
> prerequisite (§6) are not justified while the residual failure mode on the
> fixed HEAD is unmeasured. The verdict is conditional, not complacent: the
> §13.2 re-measurement is mandatory, and §13.3 names the evidence-based
> condition under which the verdict escalates to `REPLACE IBD SCHEDULER`.

The alternative verdict — `INCONCLUSIVE — ONE SPECIFIC BLOCKER` (the missing
post-fix measurement) — was considered. It is rejected on a narrow technicality
that matters for actionability: the evidence base is *unambiguous* about the
immediate next action being a bounded patch and a re-measurement, and only
ambiguous about the *long-run* destination. "PATCH CURRENT with a named gate and
escalation rule" preserves that actionability while refusing to overclaim that
patching fully solves the class.

### 13.5 Gate closure and final decision (2026-08-08)

The §13.2 B3 gate was run after §13.3 had defined its falsification rule, on clean HEAD `59d03e4`; it was not selected or reinterpreted after seeing the result. The complete measurements are in `doc/forensics/ibd-gate-b3-head-result.md`.

The timeout hypothesis and the scheduler-architecture hypothesis separated cleanly:

- `59d03e4` successfully replaced the enqueue-origin 5 s expiry with a 60 s wire-origin expiry and a distinct 1 s pending-wire safety bound. In the gate, 585 timeout generations closed at approximately 60.04 s and all 160 late receipts had already been re-requested. The timeout change is independently successful and remains part of the replacement design.
- §12(b) nevertheless failed: height 6 was flat for 241 s, height 80 was flat for 304 s, the pipeline was full at 128/128, mean connect rate was about 0.22 block/s, median was zero, only 1.16% of samples progressed, and the peer-visible gap was at least 6315 and widening. The peer-height series froze at 6448 because getheaders dedup stopped refreshing it; A actually reached 10950, so the peer-visible gap is a lower bound.
- §12(c) also failed: the orphan pool pinned at 750 from approximately t=191 s and recorded 1847 `limit_reject` outcomes, all with unknown parents.
- Recovery remained structurally blind: 8280 decisions produced zero `should_recover=1` outcomes and zero `STALL_RECOVERY` events; approximately 98% were skipped because the pipeline was active.
- Only 133 blocks were useful (`connected_active`) versus 1847 rejected, 585 timed out, and 2568 incomplete-evicted lifecycle entries; useful share among received blocks was approximately 4.9%.

Those are exactly the minutes-scale flat/full/widening-gap and orphan-cap conditions named in §13.3. The rule therefore fires as written. `PATCH CURRENT` was the appropriate bounded, reversible choice while the post-timeout residual was unmeasured; it is now falsified by its own gate rather than retrospectively declared unreasonable.

> **Final architectural decision: REPLACE IBD SCHEDULER.** Build the smallest flag-gated, ordered-header/frontier-driven IBD selector, retain the current scheduler as the A/B control, and preserve `ProcessBlock`, consensus, chainstate, wire formats, the ownership/late-delivery invariants, and the conservative expiry. The implementation design is `doc/design/ibd-headers-first-scheduler-v1.md`.

---

## 14. Evidence-base cross-reference

| Evidence | Held finding used here |
|---|---|
| `ibd-root-cause-investigation.md` | methodology note; small-fix-from-large-investigation |
| `ibd-forensic-instrumentation.md` | instrumentation exists; expB baseline; scheduler observation-only |
| `frontier-admission-exemption.md` | exemption invariant (preserved in any path) |
| `getblocks-hashcontinue-batch-size-audit.md` | INV=1 is remote-determined; V7/V8 loop; suppression ≠ INV=1 |
| `ibd-degradation-runtime-validation.md` | P1 refuted (phase-dependent); P2-P5 supported; B3 numbers |
| `ibd-semantic-recovery-design.md` | A1-A5 rate/continuity arm; frontier-hole selection; no-reset constraint; ~200-300 line patch |
| `max-ahead-window-audit.md` | policy D (predecessor-aware admission); waste is stale not far-ahead; connect-rate ceiling |
| `orphan-cap-accounting-note.md` | per-peer 750 current-occupancy cap; parent-connect retry semantics |
| `ibd-adaptive-block-request-expiration-v1.md` (SUPERSEDED) | measurement campaign; economic + Occam verdicts rejecting adaptive |
| `ibd-conservative-block-request-expiration.md` (FINAL, implemented at 59d03e4) | wire-origin 60 s; §2.1-2.2 replay; §7 ownership/late semantics preserved |
| `/home/user/ibd-blackcoin-comparative-audit.patch` (uncommitted audit) | Blackcoin scheduler reconstruction; portable mechanisms §9; minimal adaptation §12; risks §13 |
| `/home/user/ibd-pipeline-reset-feasibility.patch` (uncommitted audit) | reset is meaningless + risky; option A recommended; §9 "same pipeline" proof |

---

## 15. Corrections to prior evidence

1. **`MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK` is 3, not 2.**
   The comparative audit `ibd-blackcoin-comparative-audit.md` states "the same
   hash may be in flight from up to 2 peers (`MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK`)"
   (§2 item 4) and lists "up to 2 peers/block" in its §7 side-by-side table.
   Direct source verification of the audited Blackcoin tree
   (`1b54fa138`) shows `MAX_CMPCTBLOCKS_INFLIGHT_PER_BLOCK = 3`
   (`src/net_processing.h:32`; used at `net_processing.cpp:1300, 1335, 4702,
   4749`). The historical document is left unrewritten; this AD records the
   correct value. The corrected constant does not change any comparison in this
   document: both 2 and 3 mean "the same hash may be requested from more than
   one peer," which Innova's single-owner invariant forbids and which remains the
   meaningful structural difference.
2. **Prior-doc clock refs.** `ibd-semantic-recovery-design.md` and the two
   audits cite `BLOCK_IN_FLIGHT_TIMEOUT = 5` and pipeline-emptiness as current.
   Since `59d03e4` the expiry is wire-origin 60 s with a 1 s pending-wire bound
   (`net.h:2223-2352`), and those statements must be read against their
   pre-`59d03e4` trees.
3. **Head-age as a recovery arm.** `ibd-semantic-recovery-design.md` §rev-log
   item 1 removed "front-age" as an arm because 5 s expiry capped it. At HEAD the
   expiry is 60 s, so an oldest-in-flight-age arm is now *attainable* — this AD
   treats the rate/continuity arm (A1-A5) as the primary PATCH and the
   front-age/continuity arm as the Blackcoin-derived variant, both re-derived
   against the 60 s clock.

4. **Full-node `getheaders` service vs. header-chain construction.** Earlier passages in this AD say “getheaders is SPV-only.” Current source is more precise: the `getheaders` handler serves any requesting peer and returns up to 2000 active-chain headers (`main.cpp:9316-9406`), while automatic full-node header synchronization, 2000-header continuation, and creation of header index entries remain SPV-only (`main.cpp:8770-8773, 9542-9580`). A full node can receive headers today, but does not retain an ordered ahead-of-tip header chain. The replacement design uses the server capability and adds a scheduler-private graph; it does not rely on the stale shorthand.

---

## 16. Non-decisions and out of scope

- **No production code changes** were made for this AD.
- **Consensus, coin rules, message protocol, storage schema**: untouched by
  either option; any scheduler change must preserve them.
- **Connect-rate bottleneck**: neither option fixes a CPU/disk-bound connect
  path; that is a separate (non-scheduler) concern
  (`max-ahead-window-audit.md` §7).
- **Silent-peer / black-hole behavior**: the socket-inactivity backstop
  (`net.cpp:7579-7607`, 20 min) is the only independent check; a 100%-drop proxy
  test is recommended in `ibd-conservative-block-request-expiration.md` §13 and
  is a natural companion to the §13.2 gate, not part of the verdict.
- **This document is a decision record, not a specification.** The PATCH
  prescription's detailed design lives in
  `ibd-semantic-recovery-design.md` (arm, action, state machine, metrics) and
  `max-ahead-window-audit.md` §8/§10 (policy D + instrumentation); the
  replacement's shape is sketched in §5 and §11 here.
