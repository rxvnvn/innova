# Innova DAG/IDAG Removal Stage 4 — Runtime and Storage Audit

Status: pre-removal audit; documentation only.  No Stage 4 production change is
authorized by this document.

Audit date: 2026-07-22

Repository: `/home/user/innova`

Branch: `master`

Audit baseline: `0b03ed4de9a2ced0cf41414447fb23431c425689`
(`refactor(idag): remove dead DAG orphan helpers`)

## 1. Executive conclusion

The remaining subsystem is not one removable unit.  It has three materially
different compatibility boundaries:

1. DAG graph behavior is height-gated in block acceptance, indexing, mining,
   chain selection and startup ordering.  It is unreachable for the audited
   mainnet history and honest continuation below the disabled mainnet sentinel,
   but remains active on current testnet/regtest and on a hypothetical activated
   private chain.
2. DAG and epoch records are separate LevelDB key/value families.  Their bulk
   loaders run unconditionally during `CTxDB::LoadBlockIndex`, even when the
   chain is below the DAG gate.  Removing a loader can leave old keys physically
   harmless, but also deliberately stops reconstructing their runtime semantics.
3. `CEpochState` is not merely graph presentation.  Its curve-tree and
   nullifier roots are consumed by separately gated FCMP, wallet and private
   finality validation.  Epoch/finality removal therefore requires its own
   experimental-v5 audit and must not be bundled with DAG graph cleanup.

The smallest safe immediate behavior stage is removal of the isolated P2P
`getdagtips`/`dagtips` exchange.  It has no capability bit, no outbound
`getdagtips` sender in the current repository, and no block/database/wallet
serialization role.  The inbound `dagtips` handler is nevertheless remotely
reachable today and requests unknown hashes, so this is an intentional old-peer
behavior change and needs a controlled message test plus request-lifecycle
checks.  A separate dead-code follow-up should be used only if that removal
makes a symbol caller-free.

No mainnet database blocker exists for that isolated P2P stage.  Database,
testnet/regtest, chain-selection, and epoch/finality blockers remain for later
stages.

## 2. Repository and removal lineage

The audit began with a clean tracked tree and pre-existing untracked forensic,
documentation and build artifacts.  None of those artifacts was modified.

| Stage | Commit | Exact subject |
|---|---|---|
| Stage 1 | `c9ea3d94fd05138f7fb02333b8c220e208e03a90` | `refactor(idag): remove unused DAG database read wrappers` |
| Stage 2 | `051912edc89d83ba12231ad9fa63d0474b57fbb8` | `refactor(idag): remove DAG-expanded getblocks traversal` |
| Stage 2b | `6885e9ff55899cc787ee44546dee4c8da842b027` | `refactor(idag): remove dead getblocks DAG queue state` |
| Final report | `d508b53fa5e567457b9fb0fd4e5f851044885669` | `docs(idag): finalize forensic findings and removal rationale` |
| Stage 3 audit | `9a26f5ef0ddf859df07bcb73cea5822c84dda9c1` | `docs(idag): audit DAG orphan compatibility removal` |
| Stage 3 | `5ddec3bb65d5e4fcfbc28e736db5a309647c2065` | `refactor(idag): remove DAG merge-parent orphan handling` |
| Stage 3b | `0b03ed4de9a2ced0cf41414447fb23431c425689` | `refactor(idag): remove dead DAG orphan helpers` |

## 3. Semantic scope and method

The initial graph was built with `innova-clangd` definitions and references for
`g_dagManager`, `CDAGManager`, `CBlockDAGData`, `CEpochState`, both startup
loaders, all requested LevelDB accessors, `ExtractDAGParents`, DAG RPCs and the
Qt status model.  Narrow source ranges were then read around the LSP-resolved
locations.  Text search was used only for protocol command strings, registration
tables, preprocessor gates, scripts and tests, where symbol LSP is insufficient.

Directly modified production files: none.

Semantically affected files in the current graph:

- manager/types: `src/dag.h`, `src/dag.cpp`;
- acceptance/indexing: `src/main.cpp`;
- mining: `src/miner.cpp`;
- startup: `src/init.cpp`, `src/txdb-leveldb.cpp`;
- persistent declarations: `src/txdb-leveldb.h`;
- epoch/finality consumers: `src/finality.cpp`, `src/rpcshielded.cpp`,
  `src/wallet.cpp`;
- RPC reporting/mining: `src/rpcblockchain.cpp`, `src/rpcmining.cpp`,
  `src/innovarpc.cpp`, `src/innovarpc.h`;
- Qt presentation: `src/qt/clientmodel.{h,cpp}`, `src/qt/idagpage.{h,cpp}`,
  `src/qt/bitcoingui.{h,cpp}`, `src/qt/rpcconsole.cpp` and build inputs.

Runtime participation alone does not imply that each file belongs in the same
patch.

## 4. Remaining-surface inventory

### 4.1 Types and manager

| Symbol | Definition | Direct users / state | Gate and role | Persistence / classification |
|---|---|---|---|---|
| `g_dagManager` | `src/dag.cpp:14` | main, init, miner, finality, RPC, wallet/shielded and Qt | individual operations are gated differently; queries are often ungated | owns graph, epoch and curve-tree maps; live global |
| `CDAGManager` | `src/dag.h` | global instance | graph ordering, epoch construction, status | runtime backend; not caller-free |
| `CBlockDAGData` | `src/dag.h:123-148` | manager, LevelDB iterator/writer, acceptance, mining, RPC/Qt | graph operations normally at `FORK_HEIGHT_DAG` | separate `daglinks` value; not embedded in blocks/index |
| `CEpochState` | `src/dag.h:65-115` | manager, LevelDB, finality, FCMP, wallet, RPC | construction is DAG-era; consumers also use `FORK_HEIGHT_EPOCH_ROOT_FCMP` or private-finality gates | separate `epochstate` value; live cross-subsystem type |

Neither type is caller-free.  `CEpochState` contains epoch range and ordered
hashes, curve/nullifier/finality roots, trust/counts and finality status.

### 4.2 Startup and LevelDB

| Symbol/path | Definition | Current caller | Gate | Effect |
|---|---|---|---|---|
| `CDAGManager::LoadDAGLinks` | `src/dag.cpp:891-909` | `CTxDB::LoadBlockIndex` | none | clears graph maps, bulk-loads `daglinks`, rebuilds pending-child/tip indexes |
| `CDAGManager::LoadEpochStates` | `src/dag.cpp:911-937` | `CTxDB::LoadBlockIndex` | none | bulk-loads `epochstate` and `ce` curve snapshots, rebuilds epoch-boundary set |
| `CTxDB::LoadBlockIndex` calls | `src/txdb-leveldb.cpp:1053-1058` | node startup | none for the two calls | load occurs before best-chain pointer completion |
| startup DAG rebuild | `src/init.cpp:1734-1768` | node initialization | best height `>= FORK_HEIGHT_DAG` | reads clean height, restores prune boundary, rebuilds order |
| shutdown clean-height write | `src/init.cpp:142-147` | shutdown | best height `>= FORK_HEIGHT_DAG` | writes `dagcleanheight` |

The unconditional loaders tolerate zero records: both prefix iterators return
an empty map and success.  `LoadDAGLinks` currently ignores the iterator return
value and returns true; `LoadEpochStates` propagates failures from both epoch
and curve-tree iterators.

### 4.3 Acceptance, indexing and chain behavior

`CBlock::AcceptBlock` (`src/main.cpp:6007` onward) applies DAG-era rules only at
`nHeight >= FORK_HEIGHT_DAG`: post-gate PoS rejection, required coinbase parent
commitment, primary-parent identity, parent count/existence/depth/type checks.

`CBlock::AddToBlockIndex` (`src/main.cpp:5697` onward) begins DAG initialization
only for post-gate PoW blocks.  It extracts parents, initializes and colors graph
data, persists child and parent entries, replaces ordinary accumulated
`nChainTrust` with `ComputeDAGScore`, applies sibling mempool cleanup, computes
and writes completed epoch state, and prunes graph data.  Failure cleanup erases
the inserted graph record and repairs parent entries.

`ConnectBlock`, `Reorganize` and `SetBestChainInner` contain further gated DAG
sibling/reorg/mempool behavior.  These are consensus/runtime-capable paths and
must be removed only as a coordinated behavior stage.  Classic `pprev`, side
branches and reorganization are separate and must remain.

### 4.4 Mining

`CreateNewBlock` selects `SelectBestDAGTip`, builds a primary-plus-merge-parent
set and inserts `BuildDAGParentScript` at the DAG gate.  It also excludes sibling
conflicts.  CPU-mining identity records the selected parent and sorted DAG tips;
submitted work is rechecked with `ExtractDAGParents`.  These paths produce block
contents and cannot be treated as presentation cleanup.

### 4.5 P2P

Both command-string branches are in `ProcessMessage` at
`src/main.cpp:8945-8972`:

- inbound `getdagtips`: under `cs_main`, replies with `dagtips` only if the local
  best height is at or above `FORK_HEIGHT_DAG`;
- inbound `dagtips`: deserializes `vector<uint256>` unconditionally, assigns 20
  misbehavior points if its size exceeds `MAX_DAG_PARENTS * 3`, and otherwise
  calls `AskFor(CInv(MSG_BLOCK, hash), BLOCKREQ_SOURCE_OTHER)` for each unknown
  block hash.

Repository-wide command-string search found no current sender of `getdagtips`.
The only sender found is the response `PushMessage("dagtips", vTips)`.  No DAG
service/capability bit or protocol-version negotiation was found.  The payload
uses ordinary vector serialization and ordinary `MSG_BLOCK` requests.

`CNode::AskFor` owns per-peer scheduling in `mapAskFor` and the shared
already-asked lifecycle.  Current request-lifecycle fixes bound/suppress normal
duplicates, but the handler still gives a remote peer a bounded-per-message way
to nominate unknown block hashes.  Repeated-message behavior must be verified
with a controlled peer test; static analysis does not prove a global rate bound
for arbitrary fresh hashes.

### 4.6 RPC and Qt

The following RPCs are live and registered in `src/innovarpc.cpp:484-488`:

| RPC | Definition | Behavior |
|---|---|---|
| `getdaginfo` | `src/rpcblockchain.cpp:1450-1547` | mixes activation/status, graph manager, epoch roots, finality and adaptive-limit fields |
| `getepochinfo` | `1549-1631` | reads `CEpochState`; otherwise reports estimated range and zero roots |
| `getdagtips` | `1633-1669` | ungated local manager query; empty array when no tips |
| `getdagorder` | `1671-1720` | manager order query; errors when no DAG tip |
| `getdagconfidence` | `1722-1766` | explicitly gated by DAGKNIGHT height and graph data |

`blockToJSON` adds DAG fields only for post-gate indexed blocks.  Finality and
mining RPCs expose epoch roots/status independent of the five DAG-named RPC
registrations.

Qt `ClientModel::getDAGStatus` (`src/qt/clientmodel.cpp:230-337`) is live.  It is
called by `IDAGPage`, RPC console and GUI status code.  `IDAGPage` is constructed
and navigable from `BitcoinGUI`; this is an externally visible presentation
surface, not caller-free code.  Its inactive display is a height/empty-manager
report, not evidence about historical block contents.

## 5. Dependency graph

```text
A. Block acceptance and indexing
inbound/mined CBlock
  -> AcceptBlock
     -> height/PoW gate
     -> ExtractDAGParents
     -> parent/depth/type validation
  -> AddToBlockIndex
     -> InitBlockDAGData -> ColorBlock/ColorBlockDAGKnight
     -> WriteDAGLinks -> CTxDB::WriteDAGLinks -> "daglinks"
     -> ComputeDAGScore -> nChainTrust override -> best-chain comparison
     -> ComputeEpochState -> WriteEpochState
     -> PruneDAGData -> EraseDAGLinks + WriteDAGCleanHeight

B. Mining
CreateNewBlock / CPU work identity
  -> SelectBestDAGTip / GetDAGTips / ComputeDAGScore
  -> BuildDAGParentScript
  -> ExtractDAGParents on work validation

C. Startup/database
CTxDB::LoadBlockIndex
  -> LoadDAGLinks -> IterateDAGLinks
     -> deserialize CBlockDAGData -> populate g_dagManager graph maps
  -> LoadEpochStates -> IterateEpochStates + IterateCurveTreeEpochs
     -> deserialize CEpochState/CCurveTree -> populate epoch maps
init after index load [height gate]
  -> ReadDAGCleanHeight -> rebuild DAG order

D. Epoch/finality/shielded/wallet
ComputeEpochState
  -> DAG linear order + block reads + shielded outputs/nullifiers
  -> finality tracker -> CEpochState roots/certificate/tier
finality private proof/certificate validation
  -> GetEpochState/GetLastFinalizedEpochState
wallet + rpcshielded + main validation
  -> GetLastFinalizedEpochState -> ReadCurveTreeAtEpoch

E. P2P
getdagtips [local height gate] -> g_dagManager.GetDAGTips -> dagtips
dagtips [no height/version gate] -> unknown hash -> CNode::AskFor(MSG_BLOCK)

F. RPC
registered DAG/epoch commands + block/finality/mining reporting
  -> manager/status/CEpochState queries

G. Qt
BitcoinGUI/IDAGPage/RPCConsole
  -> ClientModel::getDAGStatus
  -> g_dagManager and CBlockDAGData queries
```

This graph proves that DAG graph storage and epoch storage can be staged
separately at their public APIs, but not that `LoadEpochStates` may simply be
deleted: it also restores curve-tree snapshots needed by FCMP/finality paths.

## 6. Reachability matrix

| Path | Audited historical mainnet | Honest current mainnet below sentinel | Arbitrary input | testnet/regtest | private activated DAG | DB with old records |
|---|---|---|---|---|---|---|
| DAG acceptance/indexing | not observed; zero commitments/post-gate records | height gate prevents it | block cannot enter DAG branch below gate | active at 11 | reachable | loaded records can affect manager, not acceptance gate |
| mining DAG parents | not applicable | height gate prevents commitment construction | not input-driven | reachable | reachable | manager contents can influence activated mining |
| DAG startup load | zero records, but loader ran | runs unconditionally | not network-triggered | runs | runs | deserializes and restores semantics |
| epoch startup load | zero records, but loader ran | runs unconditionally | not directly network-triggered | runs | runs | restores roots/snapshots |
| clean-height rebuild | not reached below sentinel | height-gated | no | reachable | reachable | consumes stored scalar when activated |
| `getdagtips` inbound | remotely callable; sends nothing below gate | same | yes | returns tips | returns tips | returned state depends on loaded graph |
| `dagtips` inbound | remotely callable | remotely callable | yes, bounded vector | yes | yes | independent of DB records |
| DAG RPCs | locally callable, empty/inactive/error forms | same | authenticated RPC only | live | live | expose loaded state |
| Qt status | live inactive/empty display | live | local UI | active display | active display | exposes loaded state |
| epoch/finality consumers | mainnet experimental gates not reached | not reached below sentinel | inputs may reach outer parsers but gated validation applies | reachable | reachable | loaded roots may be required |

The fixed mainnet scans prove zero IDAG commitments and zero DAG/epoch/clean
height records in the audited data.  They do not prove that a peer cannot send
`dagtips`, that a user cannot invoke an RPC, or that an externally supplied DB
cannot contain these key families.

## 7. Persistent key/value compatibility

### 7.1 `daglinks`

- Key: serialized `pair<string,uint256>("daglinks", block_hash)`.
- Value: `CBlockDAGData` fields in order: parents, children, blue flag, score,
  order, inferred-k.
- Writer/eraser: `CTxDB::WriteDAGLinks`/`EraseDAGLinks` at
  `src/txdb-leveldb.cpp:423-431`.
- Bulk reader: `IterateDAGLinks` at `702-761` seeks the serialized string prefix.
- Compatibility: the iterator explicitly accepts pre-Phase-4 values lacking
  `nInferredK` and defaults it to `-1`; malformed values are skipped.

The audited mainnet DB had zero records.  Writers and pruning are reachable
only through post-gate DAG indexing/reorg/epoch paths.  If the loader alone is
removed, LevelDB leaves unknown keys physically intact because ordinary reads
and prefix iteration address other serialized keys; the new binary would no
longer reconstruct graph semantics.  An old binary can therefore reopen the
physical DB provided a newer stage does not erase/rewrite these keys.  A new
binary that removes the type/reader cannot interpret an old DAG DB, even though
it can ignore the bytes.  These are different guarantees.

### 7.2 `epochstate`

- Key: serialized `pair<string,int>("epochstate", epoch)`.
- Value: all `CEpochState` fields in the exact `IMPLEMENT_SERIALIZE` order in
  `src/dag.h:65-115`.
- Writer: `CTxDB::WriteEpochState` at `434-437`.
- Bulk reader: `IterateEpochStates` at `439-478`; malformed entries are skipped.
- Related but distinct snapshot key: `pair<string,int>("ce", epoch)` containing
  `CCurveTree`, loaded by the same manager startup method.

The audited DB had zero `epochstate` records.  Removing the epoch loader while
leaving FCMP/private-finality consumers would convert stored roots into missing
runtime state and can reject or disable gated operations.  It is not a safe
mechanical DAG-storage cleanup.

### 7.3 `dagcleanheight`

- Key: serialized scalar string `"dagcleanheight"`.
- Value: serialized `int` height.
- Reader/writer: `src/txdb-leveldb.cpp:521-529`.
- It is read only in the post-gate startup rebuild and written during post-gate
  pruning/shutdown.

The audited DB had no such key.  Ignoring it preserves its bytes but loses the
incremental rebuild/prune-boundary hint; this matters only where DAG runtime is
still supported.  It has no standalone schema migration effect on linear
mainnet.

### 7.4 Compatibility dimensions

1. **Physical preservation:** stopping reads/writes does not itself delete old
   keys.  A DB-copy and old-binary rollback test must verify no incidental
   compaction/migration assumption.
2. **Deserialization:** deleting types/readers removes the new binary's ability
   to inspect those values, even if LevelDB tolerates unknown keys.
3. **Runtime semantics:** omitting loaders intentionally discards graph/epoch
   behavior reconstructed from old records.  This is incompatible with an
   activated DAG history and potentially with epoch-root consumers.

## 8. Serialization boundary

`CBlockDAGData` and `CEpochState` use the project's disk serialization macros,
but neither is embedded in `CBlock`, `CBlockHeader`, `CTransaction`,
`CDiskBlockIndex`, wallet records or a P2P message.  DAG parent commitments are
ordinary coinbase transaction output bytes parsed by `ExtractDAGParents`; no
header field was added.  `dagtips` serializes only `vector<uint256>`.

`CBlockDAGData` has an explicit legacy read accommodation: the bulk iterator
reads five core fields and conditionally reads `nInferredK`.  Its generic
`IMPLEMENT_SERIALIZE` writes all six fields.  `CEpochState` has no explicit
version tag or optional-tail reader; its current reader expects the complete
listed field sequence and skips exceptions.

Deleting either type would prevent compiling its reader and all live consumers.
It would not alter historical block/header/transaction/`CDiskBlockIndex` bytes,
but could remove the ability to read separate old LevelDB values.  Wallet.dat
does not embed either type; wallet runtime calls the manager for an epoch root.

## 9. Epoch/finality semantic separation

`CEpochState` is DAG-derived at creation but broader than DAG presentation:

- `CDAGManager::ComputeEpochState` (`src/dag.cpp:1138-1336`) obtains a DAG order,
  reads epoch blocks, accumulates trust, shielded outputs and nullifiers, stores
  curve/nullifier roots, and snapshots finality certificate/tier state.
- Private vote validation (`src/finality.cpp` around 2557) and private tally
  certificate validation (around 2697) require stored finalized epoch roots.
- Private vote production/relay (around 4365) obtains the last finalized epoch
  state and reads its curve-tree snapshot.
- Finality tip selection (around 4594) uses DAG tip/selected-parent traversal
  only after `FORK_HEIGHT_DAG`; this graph choice is separable from the root
  consumers but not yet replaced.
- `LoadFCMPValidationRoot` in `src/main.cpp`, the shielded RPC helper in
  `src/rpcshielded.cpp`, and the wallet helper in `src/wallet.cpp` require the
  last finalized epoch state at `FORK_HEIGHT_EPOCH_ROOT_FCMP`.
- `rpcblockchain.cpp` and `rpcmining.cpp` report epoch roots/status; missing
  state generally yields zeros/not-computed reporting, while validation/wallet
  paths fail with a missing-root error after their gate.

All three current network selectors map `FORK_HEIGHT_EPOCH_ROOT_FCMP` to the DAG
fork.  Mainnet uses the disabled sentinel; testnet/regtest activate DAG and
epoch-root FCMP at height 11.  Absence of records in the audited mainnet DB does
not establish safe removal for activated networks.

Conclusion: retain epoch serialization, loading, curve-tree snapshots and all
finality/shielded/wallet consumers until a separate experimental-v5 decision
either supplies a linear epoch-root adapter with tests or intentionally removes
those features.  This question does not block isolated P2P cleanup.

## 10. P2P security and compatibility

The `dagtips` vector is capped at 96 hashes because `MAX_DAG_PARENTS` is 32; an
oversized vector incurs 20 misbehavior points.  Unknown hashes enter ordinary
per-peer block request scheduling.  Known hashes cause no request.  No graph or
LevelDB write occurs merely from the message; a requested block must still pass
ordinary parsing/validation before persistent block/index effects.

The current `AskFor` lifecycle improvements reduce duplicate requests and stale
bookkeeping, but they do not make the unsolicited command negotiated.  A peer
can repeat messages or nominate new hashes.  Static analysis establishes the
bounded vector, not a complete bound across messages or peers.

Removing both handlers means old DAG-capable peers receive no `dagtips` response
and their unsolicited `dagtips` is treated as an unknown extensible command.
Old peers exchanging ordinary blocks continue to use standard block messages;
universal compatibility with activated DAG peers is neither claimed nor
required for audited linear mainnet.

## 11. RPC and Qt ordering

RPC and Qt do not affect consensus merely by querying the manager, but they are
public compatibility surfaces and some RPCs mix DAG, epoch, finality and mining
status.  Removing them before backend behavior would reduce observability during
later validation.  Recommended order:

1. retain them while removing reachable DAG backend behaviors;
2. split DAG-only RPCs from `getepochinfo` and finality/mining fields;
3. remove or deprecate DAG RPC registrations with explicit API tests/release
   notes;
4. remove Qt IDAG page/model only after the backend fields it exposes are gone.

`getepochinfo` and epoch fields in other RPCs belong with the epoch/finality
decision, not a DAG presentation-only patch.

## 12. Test inventory

| File/test | Coverage | Classification and next-stage impact |
|---|---|---|
| `src/test/cpu_mining_tests.cpp`: `work_identity_detects_same_height_reorg_and_dag_tip_change` | mining identity DAG tip changes | experimental behavior; preserve until mining stage |
| same: `block_identity_detects_stale_parent_and_dag_commitment` | `BuildDAGParentScript`/`ExtractDAGParents` work validation | experimental parser/miner characterization; preserve |
| `src/test/fcmp_root_tests.cpp` suite | FCMP root gate and mismatch/missing-root behavior | independent safety coverage; must remain through epoch audit |
| `src/test/DoS_tests.cpp`: `DoS_mapOrphans` | ordinary orphan bounds | non-DAG; preserve unchanged |
| `contrib/test/idag_phase2_test.sh` | height-11 activation, mining, tips, ordering, score, conflicts | activated regtest DAG characterization; affected by P2P/RPC/mining/backend stages |
| `contrib/test/idag_phase3_test.sh` | epoch interval/state RPC, persistence/restart, pruning, cross-node sync | activated storage/epoch characterization; retain through storage decision |
| `contrib/test/idag_phase4_test.sh` | DAGKNIGHT, inferred-k, confidence/order RPC, cross-node state | obsolete experimental behavior if DAGKNIGHT is retired |
| `contrib/test/idag_finality_relay_test.sh` | activated DAG plus private finality/epoch-root relay | critical boundary test; preserve for epoch/finality decision |
| `contrib/test/idag_hidden_finality_stress_test.sh` | post-DAG PoW, hidden finality and roots | critical experimental characterization |
| `contrib/test/idag_stress_test.sh`, `idag_tps_test.sh` | multi-node DAG/mining/finality/load | broad experimental characterization; not mainnet evidence |

No dedicated unit test was found for LevelDB `daglinks` backward reading,
`epochstate` malformed values, `dagcleanheight`, the P2P command-size boundary,
or Qt `DAGStatus`.  No sender test for `getdagtips` exists because no production
sender was found.  The ordinary 76-case unit baseline covers broad core behavior
but is not direct proof of activated DAG storage compatibility.

For the recommended P2P stage, preserve all existing tests initially and add a
controlled protocol characterization covering: valid empty/bounded/oversized
vectors, unknown versus known hashes, repeat delivery, request queue state,
misbehavior score and unknown-command behavior after removal.

## 13. Proposed staged-removal roadmap

### Stage 4A — remove isolated DAG-tip P2P exchange (recommended next)

- Files/symbols: only the `getdagtips` and `dagtips` branches in
  `src/main.cpp::ProcessMessage`.
- Removes: remote DAG-tip query/response and unsolicited unknown-hash request
  scheduling through this command.
- Keeps: ordinary `MSG_BLOCK`, inv/getdata/getblocks/getheaders, `AskFor`, all
  request lifecycle state, DAG manager, storage, acceptance, RPC and Qt.
- Compatibility loss: activated/old DAG peers no longer exchange tip lists;
  ordinary linear peer behavior is unchanged.
- Validation: LSP/reference audit, controlled P2P messages, request queue and
  banscore checks, clean daemon/tests/Qt, linear IBD or live-tip smoke.
- Expected follow-up: 4Ab only if command-specific constants/helpers become
  caller-free; do not clean generic `AskFor`.

### Stage 4B — remove DAG acceptance/indexing/mining behavior

- Files: `main.cpp`, `miner.cpp` and manager callers proven by LSP.
- Removes: commitment requirement/construction, graph scoring/order and sibling
  execution behavior; restores exclusively linear parent/tip/trust semantics.
- Keeps initially: persistent readers/types for old-key tolerance and epoch
  boundary analysis.
- Compatibility loss: activated testnet/regtest/private DAG histories and miners.
- Blockers: explicit network policy, deterministic linear mining/IBD/reorg tests,
  trust/tip comparison, and separation of epoch construction.
- Follow-up 4Bb: remove newly dead graph algorithms/helpers only after caller
  audit.

### Stage 4C — stop DAG graph runtime loading, then remove graph accessors

- First patch: remove `LoadDAGLinks` call/runtime population only after 4B.
- Follow-up: caller-free `LoadDAGLinks`, `IterateDAGLinks`, writer/eraser,
  clean-height reader/writer and graph data type where proven dead.
- Compatibility loss: new binary ignores old DAG graph keys and cannot restore
  activated graph runtime; old bytes should remain physically present.
- Validation: copied audited DB, copied archived/synthetic DAG-key DB, no-reindex
  startup, old-binary rollback, manifests and error-log audit.

### Stage 4D — split/remove DAG RPC and Qt presentation

- DAG-only RPCs: `getdaginfo`, `getdagtips`, `getdagorder`,
  `getdagconfidence`, plus conditional getblock DAG fields.
- Qt: `ClientModel::DAGStatus/getDAGStatus`, `IDAGPage`, GUI wiring and build
  entries.
- Defer `getepochinfo` and finality/mining root reporting to Stage 4E.
- Compatibility loss: local API/UI; validation requires RPC contract and clean
  Qt build.

### Stage 4E — separately decide experimental-v5 epoch/finality state

- Scope: `CEpochState`, `epochstate`, `ce` snapshots, finality/private vote,
  FCMP, shielded RPC, wallet root lookup and epoch reporting.
- This is not authorized as DAG dead-code cleanup.
- Choices: a tested linear epoch-state backend, or an explicit coordinated
  retirement of experimental finality/FCMP behavior.
- Validation: activated regtest finality/shielded/wallet suites, serialization
  fixtures, archived DB restart/rollback and wallet compatibility tests.

The numbering expresses dependency order, not permission to implement any
stage.

## 14. Compatibility matrix

| Consumer | Stage 4A | Later graph runtime/storage removal | Epoch removal |
|---|---|---|---|
| audited mainnet DB | no DB effect | safe only with copied-DB/unknown-key proof | blocked pending root audit |
| current mainnet IBD/tip | ordinary protocol unchanged | behavior-preserving if linear paths retained | gates currently disabled but broader proof required |
| old linear peers | unchanged | unchanged if ordinary messages/serialization retained | normally unrelated |
| old DAG peers sending ordinary blocks | lose tip extension only | ordinary blocks still compatible | normally unrelated |
| peers/DB with real DAG state | intentionally incompatible exchange | intentionally incompatible runtime | not claimed |
| testnet/regtest/private DAG | P2P compatibility reduced | DAG behavior removed | finality/FCMP may fail/remove |
| block/transaction/index serialization | unchanged | must remain unchanged | unchanged unless separately authorized |
| LevelDB physical keys | unchanged | preserve until explicit accessor stage | preserve until explicit epoch stage |
| wallet.dat | unchanged | unchanged | runtime wallet roots are a blocker; format itself does not embed state |
| RPC/Qt | unchanged | temporarily reports empty/inactive backend | explicit API/UI stage required |

## 15. Risks, blockers and non-claims

Mainnet blocker for Stage 4A: none identified.  It does not read/write the DB or
alter block acceptance, trust, wire serialization of existing ordinary messages,
wallet, RPC or Qt.

Remaining blockers:

- database: archived/synthetic old-key startup and old-binary rollback have not
  been tested for a loader/accessor removal;
- policy: testnet/regtest activate DAG and epoch roots at 11 and DAGKNIGHT at 13;
- peer compatibility: removing tip exchange intentionally drops activated DAG
  peer behavior; exact unknown-command interoperability needs a controlled test;
- consensus/mining: DAG scoring, sibling conflict handling, validation and block
  construction must be removed as one reviewed behavior boundary;
- epoch/finality: `CEpochState` roots are live FCMP/private-finality/wallet inputs;
- tests: no focused DB format fixtures or P2P command unit tests were found.

This audit does not claim that DAG data never existed outside the audited
mainnet snapshots, that arbitrary activated DAG networks remain compatible, or
that old separate LevelDB formats may be deleted.  It does not authorize changes
to block, transaction, `CDiskBlockIndex`, network-message or wallet serialization.

## 16. Validation plans

Every implementation stage starts with a fresh LSP definition/reference map and
ends with diagnostics for changed C/C++ files, `git diff --check`, complete diff
inspection, clean `innovad`, `test_innova` and Qt builds using
`-j"$(nproc)"`, and the actual unit-suite count/result.

Stage-specific checks:

- **4A P2P:** controlled peers send both commands below/above simulated gate;
  inspect response, banscore, `mapAskFor`, already-asked and in-flight state;
  verify ordinary inv/getdata/getblocks/getheaders and stalled recovery; run a
  short linear IBD/live-tip smoke.
- **4B behavior:** deterministic ordinary block acceptance, classic side branch
  and reorg, mining coinbase/merkle/hash, mempool conflicts, trust/tip equality,
  existing-mainnet startup and live continuation.
- **4C storage:** byte-preserving copies of the audited DB and an archived or
  explicitly synthetic DAG-key fixture; startup without reindex; tip/hash/trust
  equality; before/after manifests; old-binary reopen; malformed/legacy
  `nInferredK` fixture.
- **4D RPC/Qt:** RPC registration/help/error contract, conditional getblock JSON,
  clean Qt build and navigation/status smoke.
- **4E epoch:** epoch serialization fixtures, curve snapshot loading, private
  vote/certificate validation, shielded spend/root checks, wallet proof creation,
  reorg/restart determinism and activated regtest functional suites.

No synthetic DB, runtime instrumentation, build or runtime experiment was
created during this audit because static analysis answered the removal-boundary
question.  Such fixtures are required before the corresponding storage stage,
not before isolated Stage 4A.

## 17. Decision

Proceed, only after separate authorization, with Stage 4A: remove the two
isolated DAG-tip P2P command branches while preserving every ordinary P2P and
request-lifecycle path.  Do not remove DAG startup loading, LevelDB accessors,
graph acceptance/mining behavior, RPC/Qt surfaces or any epoch/finality state in
that patch.

The later graph-runtime, graph-storage, presentation and epoch/finality stages
must remain independently reviewable and reversible.  Historical serialization
and physical old-key preservation take priority over source cleanup.
