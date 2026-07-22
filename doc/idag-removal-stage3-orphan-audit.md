# Innova DAG/IDAG Removal Stage 3: Merge-Parent Orphan Audit

Status: pre-removal audit; documentation only.

Audit date: 2026-07-22

Repository: `/home/user/innova`

Branch: `master`

Audited HEAD: `d508b53fa5e567457b9fb0fd4e5f851044885669`
(`docs(idag): finalize forensic findings and removal rationale`)

## 1. Executive conclusion

The remaining DAG merge-parent orphan path is not reachable by the audited
mainnet history or by an honest continuation at current mainnet heights. It is
also not reachable merely because a remote peer places an `IDAG` payload in a
block sent to a current mainnet node: both `ProcessBlock` and the helper use the
mainnet DAG height gate, which is currently `999999999`. The fixed block scan
independently found no valid or malformed IDAG commitment in active, indexed
side, or unindexed physical records.

The path is reachable on regtest/testnet, where the gate is height 11, and on a
hypothetical private chain that reaches an enabled gate. There it is live P2P
and orphan-recovery behavior. A syntactically valid commitment whose primary
parent is known and whose merge parent is absent causes the entire block to be
held in transient orphan memory, requests all missing merge parents, and defers
the remainder of `AcceptBlock` validation. When a requested parent arrives,
the orphan is either re-keyed under the next missing parent or submitted to
`AcceptBlock`.

Removing only those two `ProcessBlock` branches is behavior-preserving for the
audited mainnet database, current mainnet IBD, and current mainnet tip operation.
It is intentionally not behavior-preserving for hypothetical activated DAG
histories: out-of-order merge-parent recovery would be removed. Outside IBD an
activated node would reject a child with a missing merge parent until it is
resent; during IBD the existing `AcceptBlock` fallback can accept/index the
child before that merge parent and repair DAG child links later.

No mainnet blocker was found. The smallest clean history is:

1. Stage 3 removes the two reachable DAG orphan behaviors from `ProcessBlock`.
2. Stage 3b removes the then caller-free local adapters
   `GetDAGParentsFromBlock` and `GetMissingDAGMergeParents`.

This conclusion does not authorize removal of `ExtractDAGParents`, the parent
tag, DAG validation, mining, DAG-manager state, serializers, database records,
RPC, Qt, epoch/finality code, or ordinary orphan infrastructure.

## 2. Repository and semantic scope

The audit prerequisite and removal ancestry are consecutive on `master`:

| Role | Commit and exact subject |
|---|---|
| Final forensic report | `d508b53fa5e567457b9fb0fd4e5f851044885669` — `docs(idag): finalize forensic findings and removal rationale` |
| Removal Stage 2b | `6885e9ff55899cc787ee44546dee4c8da842b027` — `refactor(idag): remove dead getblocks DAG queue state` |
| Removal Stage 2 | `051912edc89d83ba12231ad9fa63d0474b57fbb8` — `refactor(idag): remove DAG-expanded getblocks traversal` |
| Removal Stage 1 | `c9ea3d94fd05138f7fb02333b8c220e208e03a90` — `refactor(idag): remove unused DAG database read wrappers` |
| Completed forensic baseline | `c59c8062783d10926b55dd50898199a8076a6783` — `tools(idag): complete chain trust forensic verification` |

The tracked tree was clean before this report was created. Pre-existing
untracked forensic, documentation, and build artifacts were neither modified
nor staged.

### 2.1 LSP-derived Semantic Scope

`innova-clangd` was used before source inspection. Its definition/reference
results established this initial scope:

| Symbol/state | Definition | Current semantic result |
|---|---|---|
| `GetDAGParentsFromBlock` | `src/main.cpp:2240-2256` | One reference: `GetMissingDAGMergeParents` at line 2270. |
| `GetMissingDAGMergeParents` | `src/main.cpp:2258-2281` | Two references, both in `ProcessBlock`, at lines 6395 and 6522. |
| `ExtractDAGParents` | `src/dag.cpp:21-65` | Four production references: the local adapter, `CBlock::AddToBlockIndex`, `CBlock::AcceptBlock`, and CPU-miner work identity. It is not Stage 3-dead. |
| `ProcessBlock` | `src/main.cpp:6298-6567` | Owns both candidate branches and ordinary orphan insertion/retry. Inbound `block`, import, and local block-production paths call it. |
| `PruneOrphanBlocks` | `src/main.cpp:2203-2238` | Called by both DAG-specific and ordinary orphan insertion. It must remain. |
| `mapOrphanBlocks*` and per-node counts | `src/main.cpp:116-120` | Shared by the candidate and ordinary orphan/checkpoint paths. They must remain. |
| `CNode::AskFor` / `mapAskFor` | `src/net.h:960-1015` / `CNode` state | Generic per-peer request scheduling, not DAG-only. |
| `mapAlreadyAskedFor` lifecycle | `src/net.cpp:1668-1759` | Generic global request timing/cap/ownership cleanup, not DAG-only. |

The scope expanded from `main.cpp` to `dag.cpp`/`dag.h` because LSP resolved
the parser definition and its other live callers, and to `net.h`/`net.cpp`
because both DAG branches call `CNode::AskFor`. It expanded to
`src/test/p2p_sync_tests.cpp` after LSP identified request-lifecycle symbols
whose existing unit coverage had to be classified. `checkpoints.cpp` is a
semantic participant through the ordinary orphan maps, but its code is not a
Stage 3 candidate. Runtime participation is not treated as a reason to modify
any of these files.

Text search was used only after LSP established that the candidate C++ symbols
had no test references. It was then appropriate for shell tests, test macros,
log strings, comments, and Git-history diffs that LSP does not model.

## 3. Exact current call graph

### 3.1 Inbound block and initial classification

```text
ProcessMessage("block")                         src/main.cpp:8494-8570
  deserialize CBlock
  ClearBlockInFlight(hash)
  lock cs_main
  ProcessBlock(peer, block)                     src/main.cpp:8536
    reject duplicate indexed/orphan hash        src/main.cpp:6313-6316
    preliminary CheckBlock                      src/main.cpp:6335-6338
    checkpoint/minimum-work checks              src/main.cpp:6341-6361

    [DAG-specific, candidate Stage 3 branch]
    if pindexBest exists
       and pindexBest.height + 1 >= FORK_HEIGHT_DAG
       and ordinary hashPrevBlock is indexed    src/main.cpp:6390-6395
      GetMissingDAGMergeParents(block)
        resolve ordinary previous index         src/main.cpp:2262-2264
        require previous.height + 1 >= gate     src/main.cpp:2266-2268
        GetDAGParentsFromBlock(block)            src/main.cpp:2270
          scan coinbase outputs
          ExtractDAGParents(script)              src/main.cpp:2244-2253
        for parent indices 1..N-1
          retain unique hashes absent from mapBlockIndex
      if missing is non-empty:
        prune shared orphan pool
        enforce per-peer orphan cap
        clone block into mapOrphanBlocks
        key mapOrphanBlocksByPrev by first missing merge parent
        record original peer ownership/count
        AskFor every missing merge parent
        return true without AcceptBlock          src/main.cpp:6396-6432

    [ordinary blockchain orphan path; not Stage 3]
    if ordinary hashPrevBlock is not indexed     src/main.cpp:6436-6501
      prune shared orphan pool
      enforce per-peer cap
      store by ordinary hashPrevBlock
      PushGetBlocks outside IBD
      AskFor WantedByOrphan
      return true

    AcceptBlock                                 src/main.cpp:6504-6507
      normal validation
      DAG validation only at actual height gate src/main.cpp:6164-6229
      disk write/index only after validation    src/main.cpp:6231-6242
```

The initial DAG branch is both height-gated and content-dependent. It is not an
unconditional parse of every incoming block.

### 3.2 Orphan retry

```text
accepted block hash enters vWorkQueue            src/main.cpp:6510-6515
  visit mapOrphanBlocksByPrev[accepted hash]
    child = stored orphan                         src/main.cpp:6516-6521

    [DAG-specific, candidate Stage 3 branch]
    GetMissingDAGMergeParents(child)              src/main.cpp:6522
    if another merge parent is absent:
      re-key child under first remaining hash     src/main.cpp:6529
      AskFor every remaining hash through the peer
        that delivered the just-accepted parent  src/main.cpp:6530-6535
      continue without AcceptBlock

    [shared ordinary completion; must remain]
    child.AcceptBlock()
    if accepted, enqueue child hash so its dependants are retried
    remove child from all orphan/owner/stake maps and delete it
                                                 src/main.cpp:6539-6552
```

The implementation is an iterative work queue over a multimap, despite the
legacy comment saying “Recursively process.” There is no C++ recursive call in
this retry traversal.

### 3.3 State ownership

- `mapOrphanBlocks` owns the hash-to-heap-block association;
  `mapOrphanBlocksByPrev` supplies dependency lookup. The same maps serve
  ordinary previous-block orphans.
- `mapOrphanBlocksByNode` records only the peer that supplied the original
  orphan. `mapOrphanCountByNode` enforces the current 750-orphans-per-peer cap
  (`src/main.cpp:116-120`, `6405-6416`).
- Re-keying does not change original ownership. If a different peer supplies a
  merge parent, the retry code schedules remaining `AskFor` requests on that
  current peer (`pfrom`), not necessarily the original owner
  (`src/main.cpp:6529-6535`).
- The shared pool is pruned before insertion when
  `mapOrphanBlocksByPrev.size()` is already greater than the configured limit;
  the default is 2,500 (`src/main.cpp:2203-2238`, `src/main.h:99-100`). This is
  a soft bound that can overshoot by one insertion because the predicate uses
  `<=` and pruning precedes insertion.
- `mapAskFor` is per-peer scheduling state. `mapAlreadyAskedFor` is global
  timing/cap state; it does not own an orphan and is not a set-valued duplicate
  suppression invariant.

## 4. Historical origin and later changes

Git `-S`, `-G`, blame, and the relevant diffs establish this lineage:

| Commit | Exact subject | Semantic effect and classification |
|---|---|---|
| `bbd9f42e811d8aa01c238f05a0d5d701181cf076` | `innova:core:[dev]: IDAG Phase 2 — Full DAG consensus with GHOSTDAG ordering` | Introduced the commitment parser and DAG validation/manager/mining/storage/P2P surfaces. `ProcessBlock` initially requested missing merge parents but did **not** orphan the child. Consensus plus P2P request behavior. |
| `f31cb6e5a4ed8ecaca439855dc2bc726367636b2` | `core:[bug] - Defer DAG blocks missing merge parents` | Added `GetMissingDAGMergeParents`, initial orphan insertion keyed by first missing merge parent, and retry/re-key logic. Its stated purpose was avoiding peer DoS scoring for ordinary out-of-order DAG delivery. This is the origin of the Stage 3 behavior. |
| `d299d24e4dfea1dc83c7cd098dbbf24092dc9ddf` | `net:[bug] - Advertise DAG merge parents during sync` | Extracted `GetDAGParentsFromBlock` so orphan handling and a new `getblocks` merge-parent expansion could share it. P2P compatibility, not a new orphan rule. |
| `d1c1405192c4596a006f7fdc78895f1c3ae29512` | `net:[bug] - Advertise recursive DAG side parents during sync` | Added recursive side-ancestor `getblocks` expansion and `QueueBlockInventory`; continued using the shared parser adapter. P2P compatibility. |
| `67b9e35f05cc57aaabbd9a731ffb9f572958cecb` | `core:[bug] - Fix DAG sync parent handling` | Made `AcceptBlock` tolerate an absent merge parent during IBD and added retroactive child-link repair when that parent arrives. DAG sync/manager behavior. |
| `d206986c5a249e8dcb00168b43e432a64e8a07a3` | `testnet:[bug] - Fix testnet sync propagation diagnostics` | Replaced the manager's retroactive full scan with `mapPendingChildrenByParent`, rebuilt that index on load, and added generic per-peer block in-flight scheduling. DAG manager plus P2P sync. It did not remove the orphan deferral. |
| `636e509ed9656400b7eee75bb9a4045af350650a` | `finality:[bug] Deterministic finalized-epoch anchor + cold-start vote bootstrap` | Changed the DAG orphan comment and supplied a textual misbehavior reason in that revision; it did not change the branch condition or dependency semantics. Diagnostics within a broader finality change. |
| `8399f1a69c3d2b07705521f3e325957068d53ddc` | `fix(p2p): prevent redundant sync traffic` | Suppressed ordinary-orphan `PushGetBlocks` during IBD while retaining direct `AskFor`; adjusted inv-driven ordinary orphan recovery. Generic P2P/orphan traffic, not the DAG-specific branch. |
| `f632c1278ca31a6097eface923d5f7f3f1615e5f` | `innova:net:[bug]: Fix deadlock in version handler and move getdata outside cs_main` | Moved generic `mapAskFor` draining outside `cs_main`; duplicate receipt was explicitly tolerated. P2P scheduling. |
| `b1680cd2741168fa5b1e66124065353c2822ade7` | `diagnostics: trace anomalous block request lifecycles` | Tagged both DAG merge-parent and ordinary orphan requests as `BLOCKREQ_SOURCE_ORPHAN`. Observability only. |
| `2faae9eb28fc66b911e4d8a83e8f6bc9167f80c4` | `fix(p2p): recover stalled block synchronization` | Added bounded stalled-sync recovery and rejected-block retry state; considered queued/in-flight requests when deciding whether the pipeline was active. Generic recovery. |
| `6509b10ab119c683527a7d8edbbd0e0ac40c61e5` | `fix(p2p): guarantee initial sync before stall recovery` | Required an initial sync request before recovery and tracked pending/sent lifecycle. Generic recovery. |
| `9aba964dca0ef21ab486081866ef361fcf3dd289` | `fix(p2p): recover sync when no block requests are active` | Stopped treating `fStartSync` alone as an active download pipeline. Generic recovery. |
| `5f1cb59db3fa1940a29fe3e986679fe3c1f97e00` | `Fix stalled sync recovery pipeline detection` | Refined pipeline detection/diagnostics and sync-peer ownership. Generic recovery, not merge-parent logic. |
| `08c4fd9d0c71fa44df299803c6f093ca16fcf3c3` | `fix(p2p): prune stale already-asked inventory` | Added the 50,000-entry cap, one-hour retention, and pruning to global `mapAlreadyAskedFor`. Request resource control. |
| `1bd9d02beb874d34dbb75dbd707dd9496a49aef3` | `fix(p2p): tie already-asked entries to request lifecycle` | Added cross-peer ownership checks and cleanup on receipt, already-have, same-peer in-flight suppression, and in-flight expiration. Request lifecycle. |
| `051912edc89d83ba12231ad9fa63d0474b57fbb8` | `refactor(idag): remove DAG-expanded getblocks traversal` | Removed only the two DAG-expanded `getblocks` helpers and call. It left `GetDAGParentsFromBlock` because LSP still showed the orphan helper caller. |
| `6885e9ff55899cc787ee44546dee4c8da842b027` | `refactor(idag): remove dead getblocks DAG queue state` | Removed only the helper/local state made dead by Stage 2. The two orphan callers remained. |

This history explains why the parser adapter survived Stage 2: it had become
shared in `d299d24`, and after getblocks removal it still had one live caller in
the orphan path. Stage 3 is the separate proof needed before removing that last
behavior.

## 5. Current runtime semantics

The following findings are proven directly by current source unless explicitly
labelled otherwise.

### 5.1 When extraction runs

It does not run for every incoming block. The initial DAG orphan check requires
all of:

1. a non-null best index;
2. `pindexBest->nHeight + 1 >= FORK_HEIGHT_DAG`;
3. an indexed ordinary `hashPrevBlock`;
4. the helper's independent `previous->nHeight + 1 >= FORK_HEIGHT_DAG`;
5. a parseable commitment with at least two parents; and
6. at least one merge parent (indices 1 onward) absent from `mapBlockIndex`.

The outer conditions are at `src/main.cpp:6393-6395`; the actual-height check is
at `src/main.cpp:2262-2270`. Retry extraction runs only for a block already in
the shared orphan map (`src/main.cpp:6516-6523`).

A pre-activation ordinary block cannot accidentally enter the DAG orphan
branch. Even in the edge case where the best chain is past the gate but a
side-branch child's own previous height is below it, the helper returns an
empty missing-parent vector. Pre-gate IDAG-looking bytes remain ordinary
coinbase output data as far as this path is concerned.

### 5.2 Exact recognized byte pattern

`ExtractDAGParents` (`src/dag.cpp:21-65`) recognizes a script only when:

1. the first parsed opcode is `OP_RETURN`;
2. a second opcode successfully pushes a byte vector;
3. the vector is at least five bytes;
4. bytes 0..3 are `49 44 41 47` (ASCII `IDAG`, the constant at
   `src/dag.h:26-29`);
5. byte 4 is a count from 1 through 32; and
6. at least `5 + 32 * count` bytes are present.

Each subsequent 32-byte span is copied into one `uint256`. The parser does not
require exact payload length, a minimal push opcode, the end of the script, or
a zero-valued output. Trailing bytes therefore do not make an otherwise valid
prefix malformed. `GetDAGParentsFromBlock` scans only the first transaction's
outputs and returns the first non-empty parse result.

Malformed/truncated candidates, a wrong tag, count zero, count above 32, or an
insufficient payload return an empty vector. With a known ordinary parent:

- below the gate, the DAG rule is not consulted;
- at/above the gate, `AcceptBlock` treats the absence of any parseable
  commitment as a DoS-100 missing-commitment error
  (`src/main.cpp:6164-6177`);
- a malformed candidate followed by a valid output can still be accepted,
  because output scanning continues until a valid commitment is found.

### 5.3 Deferral, validation, and arrival of a parent

Yes: on an activated network, a block with a known ordinary `hashPrevBlock` can
be stored solely because a merge parent is missing. It has already passed
`CheckBlock` and checkpoint/minimum-work screening, but has not passed all of
`AcceptBlock`. `ProcessBlock` returns `true` to mean handled/stored; the block is
not thereby indexed or canonical.

The source peer receives one `AskFor(CInv(MSG_BLOCK, hash),
BLOCKREQ_SOURCE_ORPHAN)` call for each unique missing merge hash
(`src/main.cpp:6423-6430`). If the first missing hash later arrives and is
accepted, the work queue finds the child. Remaining missing hashes cause it to
be re-keyed and requested again; none remaining causes `AcceptBlock` to run.
Success recursively (iteratively) unlocks dependants; either success or failure
removes that orphan from memory (`src/main.cpp:6510-6552`). A failed deferred
block must be sent again to receive another attempt.

The orphan branch itself performs no block-file, LevelDB, or block-index write.
Those writes occur only after `AcceptBlock` reaches `WriteToDisk` and
`AddToBlockIndex` (`src/main.cpp:6231-6242`). The branch does allocate a full
`CBlock` clone, mutates transient maps/counters, schedules network traffic, and
can increment peer misbehavior by one when the per-peer orphan cap is exceeded.

### 5.4 Interaction with later DAG validation

At an active gate, `AcceptBlock` separately requires:

- a commitment;
- parent 0 equal to `hashPrevBlock`;
- no self reference or duplicate;
- older merge-parent heights;
- no post-DAG proof-of-stake merge parent; and
- merge parents within `DAG_MERGE_DEPTH` (64).

These checks are at `src/main.cpp:6164-6228`. The orphan helper checks only
existence of indices 1 onward. Thus an invalid primary commitment combined with
a missing merge parent is currently deferred before the invalid-primary check.
When it is retried, `pblockOrphan->AcceptBlock()` can fail, but the retry loop
does not propagate that orphan's `nDoS` score to the original peer (or to the
peer that delivered the parent). This is a proven delayed-validation and
peer-accounting weakness, not an invalid-block acceptance bypass.

During IBD, `AcceptBlock` explicitly continues past an absent merge parent
(`src/main.cpp:6195-6207`). `CDAGManager` retains pending-child repair for this
case. Therefore, after Stage 3 removal an activated IBD node can index a child
before the merge parent; outside IBD it rejects the child with DoS 10 until
resent. That behavior difference is irrelevant to the current gated mainnet
but material to private/test DAG chains.

### 5.5 Ordering and canonical acceptance

For the audited linear mainnet, orphan retry order cannot affect canonical
acceptance because this DAG branch is unreachable. Ordinary orphan retry still
submits blocks through `AcceptBlock` and normal chain selection and must remain.

For an activated DAG network, arrival order controls when a deferred child is
submitted and can affect transient DAG-manager/tip state. Whether all possible
arrival permutations converge to the same hypothetical DAG result was not
characterized in this audit. No universal compatibility or convergence claim
is made for such networks.

## 6. Mainnet reachability classification

The current mainnet gate comes from `GetForkHeightDAG()`:

- regtest: 11;
- testnet: 11;
- mainnet: `MAINNET_EXPERIMENTAL_V5_DISABLED_HEIGHT`, currently `999999999`.

See `src/main.h:114-118` and `src/main.h:285-294`.

| Case | Reachability of merge-parent orphan path | Evidence and scope |
|---|---|---|
| A. Canonical audited mainnet history | Not reached in either fixed audit snapshot. | Full byte scan: zero valid commitments, zero malformed candidates, zero raw IDAG tags. Fixed trust snapshot: zero records at/above the current gate. |
| A. Indexed mainnet side records | Not reached in the audited data. | The byte scan included 1,030 indexed side records; the later trust inventory contained 1,051 side records. No DAG commitment/record was found. |
| A. Unindexed physical records | No commitment found in the audited data. | The byte scan included 931 unindexed physical records. Their historical origin is not assumed, but their bytes were scanned. |
| B. Honest current mainnet continuation | Not reachable at present heights. | The best and previous heights remain far below the current `999999999` source gate. Runtime no-DAG continuation accepted 271 ordinary blocks without a DAG failure. |
| C. Arbitrary/malformed block sent to a current mainnet node | It enters ordinary deserialization/`ProcessBlock`, but cannot enter this DAG parser branch solely because of its contents while the node is below the gate. | The outer best-height gate is checked before `GetMissingDAGMergeParents`. It may still be rejected or orphaned by ordinary rules. |
| C. Arbitrary block on an activated node | Reachable if it passes `CheckBlock`/work screening, has a known ordinary parent, and contains a parseable commitment with a missing merge hash. | This is live testnet/regtest/private-chain behavior and creates bounded transient orphan/request state. |
| D. Hypothetical private/test DAG history | Reachable and semantically material. | Testnet/regtest activate at height 11. This audit does not prove compatibility after removal. |

The fixed byte-level evidence is
`doc/forensics/idag-stage2-summary.json`: at height 7,813,845 it covered
7,813,846 active blocks, 1,030 indexed side records, 931 unindexed physical
records, and 7,815,807 physical records total, with all IDAG candidate counts
zero. The later fixed ChainTrust snapshot in
`doc/forensics/idag-chaintrust-numeric-summary.json` contains 7,816,068 index
records (7,815,017 active and 1,051 side), tip height 7,815,016, and zero
post-gate records. Later observations supplement rather than replace these
snapshots.

## 7. Security and resource behavior

### 7.1 Current controls

- A candidate has already passed `CheckBlock` and checkpoint/minimum-work
  screening before orphan allocation.
- The commitment encodes at most 32 parents, so one candidate can schedule at
  most 31 unique merge-parent requests.
- The per-peer orphan cap is 750. The shared configured default is about 2,500
  blocks, with the one-entry soft-bound detail described above.
- The retry traversal is iterative. Per-candidate parser work is bounded by 32
  parents; the helper's vector deduplication is quadratic only within that
  small bound.
- No durable database/block write is made merely by entering the orphan path.
- Exceeding the per-peer cap causes a `PushGetBlocks`; outside IBD it also adds
  one misbehavior point. Invalid commitments that reach `AcceptBlock` use its
  normal DoS scores.
- The path does not bypass the ordinary previous-block requirement: it is
  entered only when that previous index is already known.
- A deferred block is not canonical until `AcceptBlock` and normal index/chain
  selection succeed.

### 7.2 Repeated-request lifecycle

`CNode::AskFor` first prunes global state and enforces a 50,000-unique-inventory
cap. For blocks it suppresses a request only when the same peer already has that
hash in flight; otherwise it advances the global scheduled time by one second
and inserts another item in the peer's `mapAskFor`
(`src/net.h:960-1015`). Multiple calls for the same `CInv` can therefore create
multiple per-peer queue entries. The global map is a timing map, not a strict
dedup set.

The send loop removes same-peer queued duplicates while the first request is in
flight and limits one peer to 128 in-flight blocks (`src/main.cpp:9503-9540`).
An in-flight request expires after five seconds (`src/net.h:1308-1327`). The
current ownership-aware lifecycle removes an unowned global entry after
receipt, already-have suppression, queued duplicate removal, or timeout
(`src/main.cpp:8494-8570`, `9507-9606`; `src/net.cpp:1672-1759`). Stale global
entries are capped and pruned.

These fixes bound global `mapAlreadyAskedFor` growth and stale ownership. They
do **not** fully bound all duplicate traffic from DAG orphan traversal:

- many DAG orphans can name the same absent merge parent and schedule repeated
  per-peer queue entries;
- different peers can own in-flight requests for the same hash concurrently;
- pruning an orphan does not cancel the `mapAskFor` work it scheduled;
- when one merge parent arrives, retry can schedule all remaining hashes again
  through the parent-supplying peer;
- after an in-flight timeout, a later queued duplicate can download the same
  hash again.

There is no explicit length limit on a peer's `mapAskFor`. Concurrent orphan
caps bound how many DAG children are retained at one time, but pruning an
orphan does not remove its queued requests. Because repeated scheduling of an
already-present global inventory does not increase the global map's unique-key
count, the 50,000-key cap is not by itself a proven lifetime bound on duplicate
per-peer queue entries under sustained activated-network churn.

The `inv` path adds a second, different mismatch. `AlreadyHave` classifies a
stored DAG orphan as already held (`src/main.cpp:7243-7259`). If the orphan's
hash is advertised again during IBD, the generic known-orphan branch calls
`WantedByOrphan` (`src/main.cpp:7822-7829`). That helper follows the ordinary
`hashPrevBlock`, not `mapOrphanBlocksByPrev`'s merge-parent key. For a DAG
orphan whose primary parent is already indexed, this can request the
already-known primary parent rather than the missing merge parent. Outside IBD
the same generic branch queues `PushGetBlocks` from `GetOrphanRoot`. This is
redundant generic recovery traffic caused by placing a merge-parent dependency
into maps whose helpers assume an ordinary previous-block dependency.

Thus the previously investigated pattern remains possible on an activated DAG
network:

```text
many DAG children/retries naming the same missing merge hash
  -> repeated AskFor(same merge hash)
  -> multiple mapAskFor entries and later retries
  -> possible repeated/cross-peer downloads

repeat inv for an already-stored DAG orphan during IBD
  -> WantedByOrphan follows ordinary hashPrevBlock
  -> redundant AskFor(known primary parent)
```

The current fixes prevent unbounded global-map retention and provide cleanup,
but they do not make `AskFor` idempotent. This is a proven resource/accounting
weakness. It does not block Stage 3 removal; removing the DAG-specific caller
eliminates one amplification source and therefore strengthens the case for the
patch. Generic `AskFor`, ordinary orphan recovery, and stalled-sync recovery
remain independently live and are outside Stage 3.

### 7.3 Cross-peer and validation observations

Original orphan ownership is retained for the memory count, while later
requests can move to the peer delivering a merge parent. A bad child can thus
consume one peer's orphan allowance and cause another peer to receive follow-up
requests. Moreover, failure in the retry-loop `AcceptBlock` does not explicitly
penalize either peer. These are observable implementation properties; this
audit did not build a network exploit fixture or quantify bandwidth gain.

No path was found by which merge-parent orphaning alone accepts an otherwise
invalid block, writes it durably, or bypasses the ordinary previous-block link.
The path delays validation; it does not replace validation.

## 8. Existing tests and coverage gaps

LSP found no direct C++ test reference to either candidate helper and no direct
test call to `ExtractDAGParents`. Macro/string and shell-test inspection found:

| File/test | What it asserts | Stage 3 classification |
|---|---|---|
| `src/test/cpu_mining_tests.cpp:226`, `block_identity_detects_stale_parent_and_dag_commitment` | Builds a valid parent script and checks CPU-miner work identity against primary parent and DAG tips. | Current mining semantics; preserve. It is not an orphan/parser-malformation test. |
| `contrib/test/idag_phase2_test.sh`, Tests 1-8 | Regtest pre/post height-11 activation, mined DAG metadata/order, and two-node synchronization. | Activated DAG integration coverage. No explicit missing-parent order or orphan-map assertion. May expose intentional compatibility loss; do not delete automatically. |
| `contrib/test/idag_1s_sync_regression_test.sh` | Mines through activation, stops one node, mines ahead, restarts it, and checks catch-up. | General activated-DAG sync regression; not a direct orphan test. Preserve for comparison and interpret any Stage 3 failure. |
| `contrib/test/idag_phase3_test.sh` Test 6; `idag_phase4_test.sh` Test 7 | Cross-node DAG/DAGKNIGHT synchronization. | Obsolete for a future fully linear-only implementation, but not specifically owned by Stage 3. Defer policy/deletion. |
| `contrib/test/idag_stress_test.sh`, `idag_tps_test.sh` | Multi-node activation, propagation, final alignment, and load metrics. | Broad activated-DAG behavior; no focused malformed/missing-parent assertion. Preserve until their subsystem stage. |
| `src/test/p2p_sync_tests.cpp:1348`, `already_asked_for_stale_entries_are_pruned_and_refill` | Stale global entries prune and capacity refills. | Generic current P2P lifecycle; preserve. |
| `src/test/p2p_sync_tests.cpp:1384`, `already_asked_for_negative_cooldown_lifecycle` | Negative cooldown is retained then expires. | Generic; preserve. |
| `src/test/p2p_sync_tests.cpp:1419`, `already_asked_for_future_scheduled_entry_is_not_pruned` | Future scheduled entries survive until due. | Generic; preserve. |
| `src/test/p2p_sync_tests.cpp:1441`, `already_asked_for_lifecycle_is_cross_peer_safe` | Global state remains while another peer owns queued work and clears when unowned. | Generic; preserve. It does not prohibit cross-peer parallel requests. |
| `src/test/p2p_sync_tests.cpp:1485`, `already_asked_for_recent_bound_remains_anti_spam` | A full recent global map blocks further `AskFor`. | Generic; preserve. |
| `contrib/test/blockchain_stress_test.sh:test_chain_fork_and_reorg` | Classic fork creation/reconnection and longer-chain convergence. | Ordinary side-chain/reorg coverage; preserve and do not classify as DAG. |

No current test directly asserts:

- exact `ExtractDAGParents` acceptance/rejection boundaries;
- malformed/truncated/count-zero/count-too-large/trailing-data behavior;
- pre-gate IDAG-looking data remaining ordinary;
- post-gate missing-commitment rejection;
- deferral solely for a missing merge parent;
- re-keying from one missing merge parent to another;
- duplicate missing-parent requests across multiple orphans/peers; or
- peer scoring after deferred `AcceptBlock` failure.

Because no test specifically owns the two Stage 3 branches, no test deletion is
required in the minimal behavior patch. A future fully linear testnet/regtest
policy may retire broad IDAG scripts, but doing that in Stage 3 would mix
subsystems. For Stage 3, add or adapt a deterministic block fixture only if the
existing harness can safely construct signed/PoW-valid pre/post-gate blocks;
do not add production instrumentation merely to obtain it.

## 9. Candidate removal boundary

### 9.1 Stage 3 — remove reachable orphan DAG behavior

Modify only `src/main.cpp`:

1. remove the initial block at lines 6390-6434 that calls
   `GetMissingDAGMergeParents`, stores the child by a merge-parent hash, and
   requests all missing merge parents;
2. remove the retry block at lines 6522-6538 that re-checks/re-keys the child
   and issues follow-up merge-parent requests.

Intentionally retain the two helpers as an intermediate dead-code checkpoint.
Do not restructure the linear `ProcessBlock` path or ordinary orphan work queue.

### 9.2 Stage 3b — remove newly dead local helpers

After a fresh LSP/repository-wide reference audit confirms zero callers,
modify only `src/main.cpp` to remove:

- `GetDAGParentsFromBlock` (`src/main.cpp:2240-2256`); and
- `GetMissingDAGMergeParents` (`src/main.cpp:2258-2281`).

No other symbol becomes caller-free solely because of Stage 3. In particular,
`ExtractDAGParents` remains used by acceptance, indexing, and mining.

### 9.3 Must remain for later stages

- `ExtractDAGParents`, `BuildDAGParentScript`, `DAG_PARENT_TAG`,
  `MAX_DAG_PARENTS`, and `DAG_MERGE_DEPTH`;
- post-gate `AcceptBlock` validation and `AddToBlockIndex` DAG initialization;
- `CBlockDAGData`, `CDAGManager`, pending-child repair, DAG/epoch persistence,
  and all serialization/database keys;
- DAG mining, RPC, Qt, epoch, finality, FCMP, and wallet consumers;
- `ProcessMessage("block")`, block in-flight lifecycle, and stalled recovery;
- block and transaction serialization and all wire protocol formats.

### 9.4 Ordinary orphan infrastructure explicitly excluded

The following are classic blockchain infrastructure and must remain:

- `mapOrphanBlocks`, `mapOrphanBlocksByPrev`, `mapOrphanBlocksByNode`, and
  `mapOrphanCountByNode`;
- `GetOrphanRoot`, `WantedByOrphan`, and `PruneOrphanBlocks`;
- the unknown-`hashPrevBlock` branch at `src/main.cpp:6436-6501`;
- the iterative accept/delete portion of the work queue;
- proof-of-stake orphan bookkeeping and checkpoint orphan callers;
- `CBlockIndex::pprev`, active/side-chain selection, and reorganization code;
- generic `AskFor`, `mapAskFor`, `mapAlreadyAskedFor`, block in-flight, and
  request-recovery state.

## 10. Compatibility matrix

| Surface | Expected Stage 3 result | Confidence / limitation |
|---|---|---|
| Existing audited mainnet database | Behavior-preserving; no audited record can invoke the removed branches. | Strong: fixed byte scan, index inventory, no-DAG runtime evidence, and current gate. |
| Current mainnet IBD | Behavior-preserving for the audited chain and ordinary continuation. | Strong for scanned history; not a claim about an imported external DAG database. |
| Current mainnet tip operation | Behavior-preserving below the disabled gate. | Proven by source gate; re-audit if the gate changes. |
| Old linear Innova peers | Ordinary block/orphan behavior unchanged. | Strong; no wire message or serialization changes. |
| Old DAG-capable peers sending ordinary blocks | Unchanged on current mainnet; ordinary previous-block recovery remains. | Strong below gate. |
| Peers sending actual IDAG commitments | Below mainnet gate the payload remains ordinary coinbase data. On an activated network, missing-parent deferral/recovery is intentionally removed. | Intentionally incompatible with hypothetical activated DAG orphan recovery. |
| Testnet/regtest/private DAG histories | Not guaranteed compatible. Out-of-order children may be IBD-accepted before a parent or non-IBD-rejected until resent. | Material known behavior change; broad convergence untested. |
| `wallet.dat` | No change. | Stage 3 does not touch wallet or transaction format. |
| Block/transaction serialization | Byte-for-byte format unchanged. | Stage 3 removes control flow only. |
| LevelDB schema/index loading | No key, value, reader, writer, or layout change. | Stage 3 does not touch storage. |
| RPC and Qt | No direct change. | Remaining DAG-labelled outputs still require separate audits. |

The correct scoped statement is: Stage 3 is behavior-preserving for Innova's
audited linear mainnet while intentionally discontinuing hypothetical
DAG-parent orphan recovery on activated networks.

## 11. Validation plan for implementation

No build, daemon, or network test is required to validate this documentation-
only audit. The future production patch should use this sequence:

1. Re-run `innova-clangd` definitions/references for both helpers,
   `ProcessBlock`, `ExtractDAGParents`, `PruneOrphanBlocks`, and the shared
   orphan maps before editing.
2. Apply only the two Stage 3 `ProcessBlock` deletions. Re-run LSP references;
   expect both helpers to remain defined but become caller-free. Run LSP
   diagnostics for `src/main.cpp`.
3. Verify repository-wide that ordinary orphan symbols, the `AcceptBlock` DAG
   parser, and miner/index parser callers remain. Inspect the full diff and run
   `git diff --check`.
4. From `src`, run sequentially:

   ```bash
   make -f makefile.unix clean
   make -f makefile.unix -j"$(nproc)" innovad
   make -f makefile.unix -j"$(nproc)" test_innova
   ./test_innova
   ```

5. From the repository root, run the established qmake-generated build:

   ```bash
   make clean
   make -j"$(nproc)" innova-qt
   ```

6. Run existing `p2p_sync_tests` through `test_innova`. If safe fixtures and
   environment already exist, run `idag_phase2_test.sh` and
   `idag_1s_sync_regression_test.sh` as characterization, explicitly treating
   activated-DAG behavior changes as non-mainnet evidence rather than silently
   redefining the goal.
7. If a deterministic valid-block fixture can be built without production
   instrumentation, characterize:
   - pre-gate valid IDAG-looking output follows ordinary processing;
   - post-gate known primary/missing merge no longer enters orphan storage;
   - post-gate malformed commitment reaches existing `AcceptBlock` rejection;
   - ordinary missing-previous blocks are still stored and retried.
8. Start the new daemon on a safe copy/existing stopped audited mainnet datadir,
   without reindex or rescan. Verify index load, tip height/hash/ChainTrust, and
   absence of new orphan, LevelDB, serialization, DAG/epoch-decode, or request-
   lifecycle errors. Perform a short IBD/live-tip smoke only under the existing
   process/datadir safety rules.
9. Implement Stage 3b only after Stage 3 is separately reviewed/committed and a
   fresh reference audit proves both helpers have zero callers.

## 12. Blockers, uncertainties, and non-claims

No blocker prevents the narrow Stage 3 patch for the audited mainnet goal.
The following limitations remain explicit:

- There is no deterministic unit/functional test directly covering the two
  candidate branches. Static semantics are clear, but the activated-network
  behavior difference should be characterized if such compatibility remains a
  project requirement.
- Whether testnet/regtest must continue supporting historical DAG recovery is a
  policy question. If that support is required, Stage 3 is blocked for those
  networks; it is not blocked for the stated linear-mainnet removal program.
- The audit does not prove compatibility with arbitrary external DAG chains,
  unexamined datadirs, or every arrival ordering.
- It does not authorize changing/removing the existing IBD missing-parent
  fallback in `AcceptBlock`, DAG-manager pending-child repair, consensus rules,
  ChainTrust, serialization, storage, RPC/Qt, epoch/finality, mining, staking,
  FCMP, or wallet code.
- It does not resolve the separate collateralnode-payee validation issue.
- It does not claim that generic orphan/request lifecycle is defect-free. The
  repeated-`AskFor` pattern also has ordinary callers that remain after Stage 3.
- The cross-peer follow-up ownership and delayed peer-penalty findings were
  proven statically but were not exercised by an adversarial network fixture;
  bandwidth/CPU amplification magnitude is therefore unquantified.
- A future change to `GetForkHeightDAG()` invalidates the current mainnet
  reachability conclusion and requires re-audit before deployment.

## 13. Decision

Removal Stage 3 is **safe to implement for the audited linear mainnet**, using
the two-branch `ProcessBlock` boundary in section 9. It intentionally removes
reachable orphan compatibility from activated DAG test/private networks and
must be described that way. Stage 3b should then remove only the two newly dead
local helpers after an independent post-Stage-3 caller audit.

Every ordinary orphan, side-chain, reorganization, request-lifecycle, storage,
serialization, consensus, mining, wallet, RPC, and Qt component listed as
retained above remains outside the patch.
