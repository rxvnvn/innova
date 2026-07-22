# Innova DAG/IDAG Removal Stage 4B — RPC and Qt Presentation Audit

Status: pre-removal audit; documentation only. No production removal is
authorized by this document.

Audit date: 2026-07-22

Repository: `/home/user/innova`

Branch: `master`

Baseline: `e9114748e50c9f42f22b141c5ec1807ff0a550b7`
(`refactor(idag): remove legacy DAG tip exchange`)

Parent Stage 4 audit: `2e23ab40ebf01b4cfdf571c3471a60a9e32ad51d`
(`docs(idag): audit runtime and storage removal boundaries`)

## 1. Executive conclusion

The remaining RPC and Qt surfaces are presentation/query layers. None of the
five audited RPC functions mutates DAG, epoch, wallet, mining, network or
consensus state, and none is called internally by production code. Their only
semantic references are command-table registrations. Removing a registration
therefore changes command availability, not backend behavior.

The functions must nevertheless be separated by meaning:

- `getdagtips`, `getdagorder` and `getdagconfidence` are graph-only queries.
- `getdaginfo` is DAG-branded but combines graph status with epoch, finality and
  adaptive-block-size reporting. It is still safe to unregister as an external
  presentation command, but its implementation should not be used as evidence
  that the queried backends are removable.
- `getepochinfo` reads `CEpochState`, exposes finality/privacy roots and can read
  epoch blocks from disk to calculate a deferred transaction count. It belongs
  to the separate epoch/finality boundary and must remain registered in the
  immediate DAG-only stage.

Qt is also broader than `IDAGPage`. `ClientModel::getDAGStatus()` has three live
callers: the page, an IDAG section in RPC Console, and the main-window block-sync
tooltip. Removing only the page leaves the model live. A coherent Qt removal
must remove all three presentation consumers before the status model becomes
dead, and must update navigation, action ownership, stacked-widget wiring and
qmake source lists.

The single safest immediate implementation stage is registration-only:
unregister `getdaginfo`, `getdagtips`, `getdagorder` and `getdagconfidence` in
`src/innovarpc.cpp`, while retaining `getepochinfo`, all implementations and
declarations, generic RPC fields, Qt, backend managers and storage. This produces
a clear API rollback point. A subsequent Stage 4B-b can remove the four newly
unregistered declarations/definitions after a fresh reference audit.

## 2. Repository state and semantic scope

The audit started on `master` at the exact Stage 4A commit above. The tracked
tree was clean. Pre-existing untracked forensic, documentation and build
artifacts were not modified.

The semantic graph was built with `innova-clangd` definitions and references.
Direct RPC scope:

- declarations: `src/innovarpc.h`;
- definitions: `src/rpcblockchain.cpp`;
- registrations: `src/innovarpc.cpp`.

Direct Qt scope:

- model types/queries: `src/qt/clientmodel.h`, `src/qt/clientmodel.cpp`;
- page: `src/qt/idagpage.h`, `src/qt/idagpage.cpp`;
- ownership/navigation/tooltip: `src/qt/bitcoingui.h`,
  `src/qt/bitcoingui.cpp`;
- console status panel: `src/qt/rpcconsole.h`, `src/qt/rpcconsole.cpp`;
- build metadata: `innova-qt.pro` and generated root `Makefile`.

Runtime backend participants, not candidates for presentation patches, are
`g_dagManager`, `g_finalityTracker`, `pindexBest`, `mapBlockIndex`, block disk
reads and activation/adaptive constants.

## 3. RPC inventory

All five commands are registered in `vRPCCommands` at
`src/innovarpc.cpp:484-488`. Their declarations are in `src/innovarpc.h`. LSP
found exactly one reference to each function: its registration entry. Qt and
other production code do not call these RPC functions.

### 3.1 `getdaginfo`

- Declaration: `src/innovarpc.h`.
- Definition: `src/rpcblockchain.cpp:1450-1547`.
- Registration: `src/innovarpc.cpp:484`.
- Arguments: exactly zero; help or any argument throws the help text.
- Locking/state: takes `cs_main`; read-only.
- Network scheduling: none.
- Mutation: none.

Backend reads:

- `pindexBest` and `FORK_HEIGHT_DAG`/`FORK_HEIGHT_DAGKNIGHT`;
- `g_dagManager.GetDAGTips`, entry/prune counts, best tip, score and DAG data;
- epoch interval/number and `g_dagManager.GetEpochState`;
- `g_finalityTracker` tier, hard-epoch count and finalized hash/height;
- adaptive block limits.

Stable output fields include `dag_active`, `fork_height`, `current_height`,
`dag_block_producer`, `pos_block_production`, `dag_tips`, `max_parents`,
`merge_depth`, DAGKNIGHT/algorithm/k fields, epoch and pruning fields, finality
fields, epoch roots and adaptive limits. Best-tip/score and inferred-k fields
are conditional on manager data.

With an empty manager on current mainnet it reports inactive status, zero
tips/entries, zero epoch roots plus `epoch_root_status=not_computed`, and no
best-tip fields. If old DAG/epoch records were loaded it exposes them. It can
compute a DAG score for the selected tip but does not persist or alter it.

Classification: external DAG status aggregation with mixed epoch/finality and
adaptive reporting. Safe to unregister as an API surface; unsafe to infer that
all its backends are dead.

### 3.2 `getdagtips`

- Declaration: `src/innovarpc.h`.
- Definition: `src/rpcblockchain.cpp:1633-1669`.
- Registration: `src/innovarpc.cpp:485`.
- Arguments: exactly zero.
- Locking/state: takes `cs_main`; manager/index reads only.
- Gate: none.
- Mutation/network scheduling: none.

It returns an array. Each manager tip always has `hash`; known block-index tips
also have `height` and `time`; available `CBlockDAGData` adds `blue`, `score`
and parent count. Empty manager returns an empty array. Loaded DAG records are
exposed regardless of current mainnet activation height.

Classification: DAG-only RPC.

### 3.3 `getdagorder`

- Declaration: `src/innovarpc.h`.
- Definition: `src/rpcblockchain.cpp:1671-1720`.
- Registration: `src/innovarpc.cpp:486`.
- Arguments: optional count, default 100, valid range 1–1000.
- Locking/state: takes `cs_main`; manager/index reads only.
- Gate: no explicit height gate.
- Mutation/network scheduling: none.

It selects the best DAG tip, errors `RPC_MISC_ERROR` when none exists, and calls
`GetDAGLinearOrder`. Entries contain order/hash and optionally height, PoW flag,
blue status and inferred-k. The query can traverse/order up to the requested
manager result and is potentially more CPU-intensive than simple status RPCs,
but is bounded by the 1000 argument limit.

Classification: DAG-only dynamic-order RPC.

### 3.4 `getepochinfo`

- Declaration: `src/innovarpc.h`.
- Definition: `src/rpcblockchain.cpp:1549-1631`.
- Registration: `src/innovarpc.cpp:487`.
- Arguments: optional epoch; valid range 0–100,000,000.
- Locking/state: takes `cs_main`; read-only.
- Network scheduling: none.

It reads `CEpochState` through `g_dagManager.GetEpochState`. A present state
returns range, boundary block, counts, trust, roots, certificate, finality tier,
hard-epoch count and ordered block hashes. If `nTxCount < 0`, it reads each
listed block from disk and counts transactions on demand. Missing state returns
an estimated range, zero roots/certificate, `finality_tier=none` and
`status=not_computed`.

Classification: epoch/finality/privacy-root observability, not DAG-only. It must
remain until the separate `CEpochState`/FCMP/finality decision.

### 3.5 `getdagconfidence`

- Declaration: `src/innovarpc.h`.
- Definition: `src/rpcblockchain.cpp:1722-1766`.
- Registration: `src/innovarpc.cpp:488`.
- Arguments: required block hash and optional comparison hash.
- Locking/state: takes `cs_main`; manager reads only.
- Gate: best height must reach `FORK_HEIGHT_DAGKNIGHT`.
- Mutation/network scheduling: none.

Below the gate it errors `DAGKNIGHT not yet active`. Above it, missing graph data
errors `Block not found in DAG`. Output includes blue/score/inferred-k/order
confidence and optional pairwise order/confidence.

Classification: DAGKNIGHT-only RPC.

## 4. RPC dependency graph

```text
CRPCTable::execute
  -> vRPCCommands registration
     -> getdaginfo
        -> chain height + activation constants
        -> g_dagManager graph queries
        -> CEpochState query
        -> g_finalityTracker queries
        -> adaptive block-limit query
     -> getdagtips
        -> g_dagManager.GetDAGTips/GetDAGData + mapBlockIndex
     -> getdagorder
        -> SelectBestDAGTip/GetDAGLinearOrder/GetDAGData
     -> getepochinfo
        -> GetEpochState
        -> optional ReadFromDisk for deferred transaction count
     -> getdagconfidence
        -> DAGKNIGHT gate
        -> GetDAGData/GetOrderConfidence/CompareBlockOrder
```

No edge returns from any RPC to block acceptance, mining, wallet mutation,
network scheduling or database writes.

## 5. RPC reachability and compatibility

| Interface | Audited/current mainnet, empty manager | DB with DAG/epoch records | Activated testnet/regtest/private DAG |
|---|---|---|---|
| `getdaginfo` | callable; inactive/zero graph and roots | exposes loaded graph and epoch state | live full status and score computation |
| `getdagtips` | callable; empty array | exposes loaded tips | live tip listing |
| `getdagorder` | callable; errors no tips | computes order from loaded state | live ordering query |
| `getepochinfo` | callable; estimated range/zero roots | exposes epoch state and may read blocks | live epoch/finality query |
| `getdagconfidence` | callable; fails height gate | still fails below gate | live DAGKNIGHT query after height 13 |

These are authenticated/local RPC surfaces, not P2P handlers. Direct removal
causes old automation to receive JSON-RPC “method not found”. It does not change
chain or DB bytes. Bundled activated-network scripts depend heavily on them, so
their failure is an intentional experimental-network compatibility loss.

No formal external API specification beyond RPC help, bundled scripts and
forensic documentation was found. A long deprecation period is a product policy
choice, not a technical prerequisite. The repository's staged-removal policy
supports direct unregistering when the compatibility loss is explicit and
validated.

## 6. Qt dependency graph

```text
BitcoinGUI constructor
  -> new IDAGPage
  -> QStackedWidget::addWidget(idagPage)
  -> new QAction("IDAG")
     -> QActionGroup
     -> Window menu
     -> triggered -> gotoIDAGPage -> setCurrentWidget(idagPage)
  -> setClientModel -> idagPage->setModel

IDAGPage
  -> 2-second QTimer -> refresh
  -> numBlocksChanged -> refresh
  -> ClientModel::getDAGStatus(24)
     -> active chain + activation/adaptive constants
     -> g_dagManager graph queries
     -> g_finalityTracker queries
     -> up to 24 pprev rows

BitcoinGUI::setNumBlocks
  -> ClientModel::getDAGStatus()
  -> adds IDAG status to sync tooltip

RPCConsole::setNumBlocks
  -> updateDAGInfo
  -> ClientModel::getDAGStatus()
  -> updates embedded DAG status labels
```

### 6.1 `IDAGPage`

Declared in `src/qt/idagpage.h`, implemented in `idagpage.cpp`, constructed in
`BitcoinGUI`, inserted into the central stack, assigned a model, and navigated
through a live action. A 2-second timer and `numBlocksChanged` signal both invoke
`refresh`. The page reports DAG graph, adaptive sizing, finality/epoch and recent
ordinary block activity. It is presentation-only and does not mutate wallet or
core state.

### 6.2 Model and other consumers

`ClientModel::DAGStatus` and `DAGBlockActivity` are declared in
`clientmodel.h`; constructor and `getDAGStatus` are in `clientmodel.cpp`.
`getDAGStatus` uses non-blocking `TRY_LOCK(cs_main)`. Empty/current mainnet state
still returns a valid inactive snapshot with chain height, adaptive limit,
finality counters and recent ordinary block data. Loaded graph state becomes
visible through tips, scores and parent counts.

The model is not page-only: the main window and RPC Console are independent
live callers. Removing the page alone cannot remove the model.

### 6.3 Build, resources and settings

`innova-qt.pro` lists both `idagpage.h` and `idagpage.cpp`; the root Makefile is
qmake-generated and contains the resulting object/moc rules. The future source
change belongs in `innova-qt.pro`; the generated Makefile should be regenerated,
not hand-cleaned as unrelated source.

The action uses an existing generic toolbar glyph; no IDAG-specific icon or qrc
entry was found. No IDAG page setting or persisted navigation setting was found.
Strings use inline `tr()` and may appear in translation catalogs; translation
cleanup is not required for compilation and should be deferred.

## 7. Qt reachability and cost

| Surface | Empty/mainnet behavior | Loaded/activated behavior | Cost/mutation |
|---|---|---|---|
| IDAG page | live inactive page; timer every 2 s; recent linear blocks | graph/order/finality status | read-only; up to 24 `pprev` rows per refresh |
| sync tooltip | adds inactive fork-height text | tips/algorithm/k/adaptive fields | read-only per block-count update |
| RPC Console panel | inactive/zero status and finality counters | graph/finality status | read-only per block-count update |
| status model | queries empty manager and chain | exposes loaded graph, dynamically selects best DAG tip | read-only; bounded recent traversal |

No Qt surface schedules network requests, writes the DB, changes wallet state,
controls mining, or changes consensus. The UI remains reachable even though it
normally reports inactive on audited mainnet.

## 8. Generic RPC field inventory

Generic/non-candidate RPCs still expose experimental fields:

- `blockToJSON` in `rpcblockchain.cpp`: for blocks at/above `FORK_HEIGHT_DAG`,
  always adds `dag_block_producer=pow` and `pos_block_production=false`; when
  graph data exists it adds parents, children, blue, score, order and epoch.
- `getfinalityinfo`: always reports epoch/finality model and counters; epoch
  roots/certificate are real when `CEpochState` exists and zero/not-computed
  otherwise.
- mining/staking status RPCs in `rpcmining.cpp`: DAG-active/PoS-production,
  epoch progress, finality and private-promotion fields; these are externally
  visible and some describe mining policy.
- shielded/wallet RPC code consumes epoch roots for proof validation/creation;
  those are not mere presentation fields.

These outputs are not required by the four DAG-named registration entries.
They should be deferred. Removing them now would expand scope into generic API,
mining, finality, FCMP and wallet semantics.

## 9. Tests, scripts and documentation

No unit or GUI test directly exercises RPC registration removal, IDAG navigation
or `DAGStatus`. Relevant bundled scripts are activated-network characterization:

| File | Commands/surface | Purpose | Disposition |
|---|---|---|---|
| `contrib/test/idag_phase2_test.sh` | info, tips, order | activation/tips/GHOSTDAG/score | expected to fail after DAG RPC unregister; retain as historical characterization until policy cleanup |
| `contrib/test/idag_phase3_test.sh` | info, tips, epoch | persistence/restart/epoch/cross-node | keep; `getepochinfo` remains, DAG RPC assertions become obsolete |
| `contrib/test/idag_phase4_test.sh` | info, order, confidence | DAGKNIGHT/inferred-k | obsolete activated-DAG characterization after API removal |
| `contrib/test/idag_hidden_finality_stress_test.sh` | info, epoch | finality/root behavior | preserve epoch portions for Stage 4E; DAG status dependency needs later update |
| `contrib/test/idag_stress_test.sh`, `idag_tps_test.sh` | info and finality status | broad DAG load/finality | experimental characterization; do not rewrite in registration-only patch |
| `contrib/testnet_tools/innova_testnet_tool.py` | optional `getdaginfo` | testnet diagnostics/status | already optional in key paths; report method loss explicitly |
| forensic docs/artifacts | all names | durable historical evidence | retain unchanged |

RPC help itself makes the commands discoverable, so unregistering also removes
them from command enumeration/help dispatch. Old automation receives method not
found. Qt does not invoke these RPCs; it reads backend state directly.

## 10. Proposed staged removal

### Stage 4B — unregister DAG-named RPC behavior (recommended immediate stage)

- File: `src/innovarpc.cpp` only.
- Remove registration entries for `getdaginfo`, `getdagtips`, `getdagorder` and
  `getdagconfidence`.
- Keep registration for `getepochinfo`.
- Keep declarations and definitions intentionally.
- Behavior loss: external callers receive method not found; no backend change.
- Validation: LSP/reference and command-table audit, clean daemon/tests/Qt,
  runtime `help`/four method-not-found checks, successful `getepochinfo` and
  generic RPC checks on a safe datadir.
- Rollback: restore four table entries.

### Stage 4B-b — remove dead DAG RPC implementations

- Files: `src/rpcblockchain.cpp`, `src/innovarpc.h`.
- Remove exactly the four unregistered functions/declarations after confirming
  zero references.
- Keep `getepochinfo`, generic block fields, finality/mining RPC fields and all
  backend methods.
- Build/test requirements are the same; runtime repeat is optional because this
  is caller-free cleanup.

The registration-only split intentionally allows temporary compiler/link-visible
dead functions. Any resulting unused warnings should be recorded, not hidden.

### Stage 4C — remove complete Qt IDAG presentation

One coherent visible-behavior patch should remove:

- `IDAGPage` construction, stacked-widget insertion, model assignment, action,
  Window-menu entry, signal connection and `gotoIDAGPage` slot;
- IDAG page header/source and their `innova-qt.pro` entries;
- main-window IDAG tooltip block;
- RPC Console DAG group, labels, `updateDAGInfo` call/method/state;
- `ClientModel::DAGStatus`, `DAGBlockActivity` and `getDAGStatus`, once those
  three callers are gone.

Files: `bitcoingui.{h,cpp}`, `rpcconsole.{h,cpp}`, `clientmodel.{h,cpp}`, the two
`idagpage` files and `innova-qt.pro`. Generated moc/object files are build
artifacts, not source edits. Translation cleanup is deferred.

This is larger than RPC unregistering but still presentation-only. Splitting
page removal from tooltip/console is possible, but it leaves the model live and
creates more intermediate UI states without reducing backend risk. A single
coherent Qt patch followed by a build-artifact cleanup is clearer.

### Later epoch/finality and generic RPC stage

`getepochinfo`, `CEpochState`, epoch roots, generic finality/mining fields and
wallet/shielded consumers remain under the Stage 4 runtime/storage blocker.
They must not be inferred removable from Stage 4B/4C.

## 11. Compatibility assessment

| Area | Stage 4B registration-only effect |
|---|---|
| audited mainnet blocks/datadir | none |
| block/transaction/index serialization | none |
| LevelDB and startup loaders | none |
| consensus/chain trust/validation | none |
| mining/staking/wallet/FCMP/finality | none |
| P2P | none |
| four DAG RPC clients | method not found; intentional API break |
| `getepochinfo`/generic RPC clients | unchanged |
| Qt | unchanged |
| activated DAG networks | lose four diagnostic RPC interfaces, not backend behavior |

Stage 4C removes local GUI observability only. It does not remove backend state
or alter daemon behavior, but it affects users who rely on the IDAG page,
console panel or sync tooltip.

## 12. Blockers and non-claims

No audited-mainnet consensus, storage or wallet blocker exists for the narrow
registration-only Stage 4B. Required product acknowledgment: four documented
commands disappear and activated-network scripts/automation will fail.

Qt blockers are integration rather than chain safety: all action, stack,
console, tooltip, model and qmake edges must be removed together, and Qt must be
clean-built to detect dangling moc/slot references.

Epoch/finality is a hard scope boundary. `getepochinfo` and generic root fields
remain because `CEpochState` has live FCMP/finality/wallet consumers. Presentation
findings do not prove manager, storage, parser, consensus or serialization code
removable.

Non-claims:

- no universal external-client compatibility is claimed;
- no activated DAG testnet/regtest/private-chain compatibility is claimed after
  DAG RPC removal;
- no historical serialization or LevelDB format change is authorized;
- no claim is made that an inactive Qt page is dead code;
- no claim is made that all fields named epoch/finality belong to DAG cleanup.

## 13. Validation plan

### Stage 4B

1. LSP reference audit for all five functions before/after; verify only four
   registration references disappear and `getepochinfo` remains registered.
2. Repository command-string audit; classify scripts/docs without modifying them.
3. `git diff --check` and exact one-file registration diff.
4. Clean `innovad` build, `test_innova` build/run, and clean Qt build using
   `-j"$(nproc)"`.
5. Safe runtime RPC checks: command help/list excludes four commands; each
   returns method not found; `getepochinfo`, `getblock`, finality/mining status
   and ordinary wallet RPC remain available.
6. Existing-datadir startup only when no user process holds it.

### Stage 4B-b

Repeat references, diagnostics and builds; verify zero matches in tracked source
except scripts/docs. Runtime is optional because only unreachable definitions
are removed.

### Stage 4C

1. LSP audit of `IDAGPage`, action/slot, `DAGStatus` and all three status callers.
2. Remove source/build entries coherently and regenerate the qmake Makefile.
3. Clean Qt build; verify no moc/link dangling references.
4. GUI startup: main window, Window menu, stacked navigation, RPC Console and
   block-sync tooltip.
5. Confirm wallet navigation/settings and daemon build/tests remain unchanged.
6. Preserve translation catalogs unless separate mechanical cleanup is approved.

Activated DAG scripts are classified compatibility-loss tests rather than
release gates for these presentation removals. Epoch/finality tests remain
release gates for any later `getepochinfo` or generic-field change.

## 14. Decision

After separate authorization, implement Stage 4B as a four-line command-table
behavior removal in `src/innovarpc.cpp`: unregister the four DAG-named RPCs and
leave `getepochinfo` registered. Do not combine implementation deletion, generic
RPC field cleanup or Qt changes.

Then use Stage 4B-b for proven-dead declarations/definitions and Stage 4C for the
coherent Qt presentation graph. Storage, consensus, mining and epoch/finality
remain outside these stages.
