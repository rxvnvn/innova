# Innova DAG/IDAG Final Forensic Report

Status: final forensic baseline before Removal Stage 3; documentation only.

Report date: 2026-07-22

Repository: `/home/user/innova`

Branch: `master`
Report baseline: `6885e9ff55899cc787ee44546dee4c8da842b027`

This report consolidates the historical, chain-data, database, runtime, and
ChainTrust evidence used to authorize incremental removal of Innova's DAG/IDAG
subsystem. It does not authorize a one-shot deletion of the remaining code or
formats.

## 1. Executive conclusion

The audited Innova mainnet is a conventional linear active chain. Its canonical
links are the ordinary `hashPrevBlock`/`pprev` links, and the audited active tip
was reconstructed by following those links. The database also contains classic
side-chain records; they are not DAG parents and do not make the active chain a
multi-parent graph.

No block at or above the current mainnet DAG activation sentinel was present in
the fixed snapshot (`post_dag_records = 0`). The earlier full byte-level scan of
mainnet blocks found no valid IDAG commitment, malformed IDAG candidate, or raw
`IDAG` tag in active, indexed-side, or unindexed physical block records. No
`daglinks`, `epochstate`, or `dagcleanheight` record was found in the audited
LevelDB snapshots. Consequently, no audited mainnet block required DAG-parent
semantics.

For the audited fixed snapshot, independent trust reconstruction and runtime
exports found no contribution from DAG/IDAG to per-block trust, accumulated
`nChainTrust`, active-chain membership, or the selected tip. A no-DAG
experimental binary opened a copied datadir without reindex, restarted, accepted
ordinary continuation blocks, and allowed rollback to the baseline binary.

These are scoped conclusions about the audited Innova mainnet data and tested
binaries. They do not prove that no experimental or externally constructed DAG
block/database can exist. Removal is therefore being performed incrementally.
Serialization readers, orphan compatibility, database layout, finality/epoch
dependencies, RPC/Qt surfaces, and other reachable paths remain until each is
separately proven removable.

## 2. Scope and chain snapshot

### 2.1 Fixed ChainTrust snapshot

The canonical snapshot for trust and baseline/no-DAG comparison is:

| Property | Verified value |
|---|---:|
| Active-chain tip height | `7,815,016` |
| Active-chain tip hash | `00000021233a369416466ba2a0a6d10e5bfeb19b306150404f2f27220cbf5a74` |
| Total block-index records | `7,816,068` |
| Active records | `7,815,017` |
| Side records | `1,051` |
| Records at/above current DAG sentinel | `0` |

The values are recorded in
[`forensics/idag-chaintrust-numeric-summary.json`](forensics/idag-chaintrust-numeric-summary.json),
whose SHA-256 at this report baseline is
`ec84ff2e9dbae04e0c0aa34c6a17d3ed65087dabdf9d40ff641f0715b9f80d98`.
The record identity `total = active + side` holds exactly.

### 2.2 Earlier full block-content snapshot

The byte-level block and database scan was performed at height `7,813,845`, tip
`3eeb41e301c3c904bc6e10409385ad766e871c6630b7cd4cbca8175450861bcd`.
It inspected `7,813,846` active blocks, `1,030` indexed side blocks, and `931`
unindexed physical records: `7,815,807` physical block records in total. It also
enumerated `18,292,130` LevelDB entries. All candidate and DAG-prefix counts were
zero.

The authoritative summary is
[`forensics/idag-stage2-summary.json`](forensics/idag-stage2-summary.json)
(SHA-256 `700a9f44e095f2be53814f7fdcfc11e6d3249d5ac427c045f91d9c97008eedf6`).
The before/after filesystem inventory is
[`forensics/idag-stage2-manifest.txt`](forensics/idag-stage2-manifest.txt)
(SHA-256 `375df156064d67c6ffde2e1967f5c537d187512245e0cba9fe1af096346c8917`).

The two snapshots serve different purposes and must not be conflated. The later
fixed snapshot is the ChainTrust comparison baseline. The earlier snapshot is
the exhaustive block-byte/coinbase and LevelDB-key inventory. Later startup and
IBD observations are supplementary; they do not silently redefine either fixed
snapshot.

## 3. Historical origin

The term IDAG entered the history before the multi-parent implementation. The
relevant lineage, verified from Git objects and diffs, is:

| Commit | Exact subject | Role |
|---|---|---|
| `28bfb081f44b152f91f4a3c53bf284250b32e051` | `innova:core:[dev]: IDAG Phase 1 — POEM entropy weighting + PoS finality gadget` | Added POEM/finality terminology and machinery; it did not introduce the multi-parent graph. |
| `bbd9f42e811d8aa01c238f05a0d5d701181cf076` | `innova:core:[dev]: IDAG Phase 2 — Full DAG consensus with GHOSTDAG ordering` | Introduced `dag.{h,cpp}`, the coinbase `OP_RETURN` parent commitment, graph state, validation, DAG scoring/order, mining, `daglinks` persistence, P2P tip exchange, RPCs, and tests. |
| `0201620cd7bd9ad8a32d4af5235f8dbf10a8209b` | `innova:core:[dev]: IDAG Phase 3 + Phase 4 + Adaptive Block Sizing` | Added epoch persistence, pruning/restart support, DAGKNIGHT, mempool/finality coupling, adaptive sizing, and further tests. |
| `f31cb6e5a4ed8ecaca439855dc2bc726367636b2` | `core:[bug] - Defer DAG blocks missing merge parents` | Routed blocks with missing DAG merge parents through orphan deferral/retry. |
| `d299d24e4dfea1dc83c7cd098dbbf24092dc9ddf` | `net:[bug] - Advertise DAG merge parents during sync` | Added `GetDAGParentsFromBlock`, the first merge-parent `getblocks` expansion, and per-response DAG dedup state. |
| `d1c1405192c4596a006f7fdc78895f1c3ae29512` | `net:[bug] - Advertise recursive DAG side parents during sync` | Added recursive side-ancestor traversal and `QueueBlockInventory`. |
| `7d6289ba7d15505180cdc7797e6875caff886654` | `qt: expose IDAG consensus status` | Added the Qt IDAG status surface. |
| `2e96aa37e8dbb46a9a347388b5dd72c0c6ea2baa` | `fix(p2p): suppress repeated getblocks responses` | Integrated DAG-added inventories into the dedicated getblocks queue and response/rate accounting. |
| `8bc4e5d70f5f7537e1c8d7dbc57bd5b5851e36e7` | `consensus: postpone experimental v5 fork activation` | Moved default mainnet DAG and related experimental gates to `999,999,999`; it did not remove the implementation. |

`28bfb081 -> bbd9f42 -> 0201620` is a direct parent chain. Separately,
`f31cb6e -> d299d24 -> d1c1405` is the direct sequence that created the orphan
and `getblocks` compatibility machinery.

The Phase 2 design did not add a DAG field to the block header or to
`CDiskBlockIndex`. Additional parents were encoded in a coinbase output as
`OP_RETURN || "IDAG" || count || hashes`; those transaction bytes were covered
by the merkle root and therefore by the existing block hash. At an active gate,
the subsystem could affect block acceptance, DAG ordering, sibling transaction
execution, mining, and chain selection by replacing the ordinary accumulated
trust value with a DAG score. P2P used ordinary `MSG_BLOCK`, plus the
`getdagtips`/`dagtips` commands; no separate DAG inventory type was introduced.

## 4. Mainnet evidence

The full-chain scanner did not infer activity from RPC or UI labels. It decoded
the stopped mainnet database and all physical block envelopes, independently
parsed every coinbase output, searched raw block bytes, and reconciled index
membership with physical records.

The byte-level results were:

- `0` valid IDAG commitments;
- `0` malformed IDAG commitments;
- `0` raw ASCII `IDAG` hits;
- `0` reconstructed primary or merge-parent DAG edges;
- `0` `daglinks`, `epochstate`, or `dagcleanheight` LevelDB records;
- `0` unreadable physical block records and `0` non-zero corrupt gaps.

The scan included all active blocks, every indexed side record, and all
unindexed physical records in that snapshot. The latter are described as
“unindexed physical” rather than assumed to be DAG blocks or even a specific
kind of orphan: their exact historical origin is not recoverable from the
current index.

The audited chain crossed the former planned heights `7,355,000`, `7,450,000`,
and `7,750,000` without an IDAG commitment. The `7,450,000` block and its
successors are especially strong evidence that the source gate then present did
not become the network-enforced rule for this chain: the corresponding source
would have required a non-empty parent commitment. The later planned heights
`7,950,000` and `8,150,000` were not reached by the byte-scan snapshot.

The current `999,999,999` default gate explains why current mainnet runtime paths
are not reached at the snapshot height, but it is not the sole proof. The proof
comes from the independent chain/block scan, zero DAG-derived database records,
trust reconstruction, and runtime experiments. An “IDAG not active” GUI label
would only report a height comparison and is not forensic evidence of block
contents.

## 5. ChainTrust reconstruction

### 5.1 Source and independent formula

`nChainTrust` is an in-memory `CBlockIndex` field; current
`CDiskBlockIndex::IMPLEMENT_SERIALIZE` does not serialize it. On index load the
ordinary value is reconstructed from the parent. For the audited mainnet below
the disabled experimental gates, the independent tool decoded compact `nBits`
to target `T` and used:

```text
block_trust = floor(2^256 / (T + 1))
chain_trust(block) = chain_trust(previous_hash) + block_trust
```

The independent implementation is
[`../contrib/analysis/reconstruct_chaintrust.py`](../contrib/analysis/reconstruct_chaintrust.py).
It reads the standalone `IDAGIDX1` export and does not open LevelDB or call
Innova's production trust helpers. Records are ordered by height and joined to
their previous hash, so classic side branches accumulate trust from their own
parent rather than from physical database order.

The production DAG override existed only in the gated post-DAG branch and
required extracted DAG parents. The audited snapshot contained no record at the
current gate and no DAG persistent input.

### 5.2 Export and comparison results

The fixed snapshot produced `7,816,068` independent rows and `7,816,068` rows in
each runtime export. The baseline and no-DAG runtime CSVs had the same SHA-256:

```text
d4b1971dfda8debf90f87193773a4c1715bd34ee29fce8afd091cda77ab8ec69
```

A hash-keyed full join matched all `7,816,068` block hashes. It found:

| Compared field | Mismatches |
|---|---:|
| Per-block trust | `0` |
| Accumulated chain trust | `0` |
| Height | `0` |
| Previous hash | `0` |
| Active-chain membership | `0` |
| Missing only from either side | `0` |

The durable machine-readable result is
[`forensics/idag-chaintrust-numeric-summary.json`](forensics/idag-chaintrust-numeric-summary.json).
The independent CSV was recorded during the experiment with SHA-256 prefix
`5c601368…`; the generated large CSV and its complete digest were not
retained in this repository. The generated comparison summary was recorded with
SHA-256 prefix `292f038b…`; its complete digest likewise is not present in a
retained artifact. This report intentionally does not invent the missing suffixes.
Future reproduction should record the full digests in a durable manifest.

For this fixed snapshot, DAG/IDAG input was not required to reconstruct either
the active chain or any indexed side record, and the baseline/no-DAG runtime
values were byte-for-byte identical.

## 6. Runtime compatibility experiments

The no-DAG experiment used the archived patch
[`forensics/idag-experimental-nodag.patch`](forensics/idag-experimental-nodag.patch).
It suppressed two audited paths only:

1. loading `daglinks`/`epochstate` into `g_dagManager` during block-index startup;
2. the gated DAG initialization/score branch in `CBlock::AddToBlockIndex()`.

It did not change ordinary block/index serialization or the ordinary trust
formula. The experimental binary SHA-256 was
`a89fd6f5816120456acfaff8df930c862a5136496a430779430b69d14c41bc90`.

On disposable copies of the existing datadir, the following completed:

- copied block index loaded without `-reindex`;
- offline RPC-ready startup and clean stop;
- clean no-DAG restart;
- rollback: the baseline binary reopened the same copy without schema errors;
- a live, not fully controlled continuation advanced from height `7,813,845` to
  `7,814,116`, accepting `271` ordinary active-chain blocks;
- the next observed block failed for the separately known collateralnode-payee
  validation condition, not for a DAG-specific error;
- the final no-DAG runtime export on the fixed snapshot matched the baseline
  export exactly at all `7,816,068` hashes.

No DAG-prefix decode, missing DAG parent, DAG ordering, or DAG score failure was
observed. Rollback was clean because the baseline binary could reopen the
experiment copy after the no-DAG runs.

Limitations matter. These experiments used particular binaries and copied
datadirs. The continuation was connected to real network behavior rather than a
fully deterministic peer fixture, and a separate controlled peer attempt did not
deliver blocks. The experiment does not cover hypothetical databases containing
actual DAG records, external DAG chains, all testnet/regtest histories, or every
finality/FCMP state. The collateralnode-payee issue remains a separate blocker
outside this report.

## 7. P2P compatibility findings

Before Removal Stage 2, an inbound `getblocks` request produced:

```text
ordinary active-chain inventories followed through pnext
  plus DAG merge-parent inventories
  plus recursively discovered non-main-chain DAG side ancestors
```

The extra hashes were still advertised as ordinary `MSG_BLOCK` inventories and
were incorporated into the getblocks response/rate accounting. The expansion
was added by `d299d24`, made recursive by `d1c1405`, and integrated with response
accounting by `2e96aa3`.

Stage 2 removed only the DAG merge-parent/side-ancestor traversal and its call.
The current handler still resolves `CBlockLocator` using the normal active-chain
fallback, starts from `pindexLocator->pnext`, follows `pnext`, honors `hashStop`,
keeps the linear `1,000` item limit, sets `hashContinue` on that limit, queues
ordinary `MSG_BLOCK` through `PushGetBlocksInventory`, and records the actual
response. Stage 2b removed only the now-unreachable queue wrapper and local DAG
dedup set.

Thus a peer using a supported protocol receives a valid conventional linear
`getblocks` response. It no longer receives the former extra DAG inventories.
This is compatibility with Innova's audited linear mainnet behavior; it is not a
claim of universal compatibility with arbitrary non-mainnet DAG networks.

The separate `getdagtips`/`dagtips` command handling remains in current source.
Inbound `dagtips` is bounded but not negotiated by a dedicated feature bit and
can request unknown hashes as ordinary blocks. Its later removal or compatibility
policy requires its own audit and is not part of Stages 2/2b.

## 8. Removal stages completed

| Stage | Commit and exact subject | Files and semantic effect | Validation performed |
|---|---|---|---|
| Stage 1 — Dead DB wrappers | `c9ea3d94fd05138f7fb02333b8c220e208e03a90` — `refactor(idag): remove unused DAG database read wrappers` | `src/txdb-leveldb.h`, `src/txdb-leveldb.cpp`; 12 deletions. Removed only caller-free point reads `CTxDB::ReadDAGLinks` and `CTxDB::ReadEpochState`. Iterators, writers, keys, serializers, and startup loaders remained. | Repository-wide caller/reference audit; diff checks; clean daemon and Qt builds; `test_innova` baseline suite (`76` cases). |
| Stage 2 — DAG-expanded getblocks traversal | `051912edc89d83ba12231ad9fa63d0474b57fbb8` — `refactor(idag): remove DAG-expanded getblocks traversal` | `src/main.cpp`; 62 deletions. Removed `QueueDAGMergeParentInventories`, `QueueDAGSideBlockWithAncestors`, and their single `getblocks` call. Linear traversal and accounting remained unchanged. | Semantic call-site audit; clean `innovad`, `innova-qt`, and `test_innova` builds; `76` tests passed; existing-datadir startup smoke loaded the index without new DB/serialization errors. |
| Stage 2b — Dead getblocks DAG queue state | `6885e9ff55899cc787ee44546dee4c8da842b027` — `refactor(idag): remove dead getblocks DAG queue state` | `src/main.cpp`; 16 deletions. Removed the now-caller-free `QueueBlockInventory` and unused `setQueuedDAGParents`; did not change the live linear queue. | Post-removal LSP/reference audit; clean daemon and Qt builds; `test_innova` baseline suite (`76` cases). Runtime smoke was intentionally not repeated because only unreachable helper/local state was removed; daemon and Qt clean builds were repeated on the committed HEAD. |

The commits are consecutive on `master`:

```text
c59c8062783d10926b55dd50898199a8076a6783
  -> c9ea3d94fd05138f7fb02333b8c220e208e03a90
  -> 051912edc89d83ba12231ad9fa63d0474b57fbb8
  -> 6885e9ff55899cc787ee44546dee4c8da842b027
```

## 9. Compatibility intentionally retained

Current source was checked semantically at report baseline. The following are
still present by design:

| Retained boundary | Current evidence and reason |
|---|---|
| `GetDAGParentsFromBlock` | Defined in `src/main.cpp`; its only caller is `GetMissingDAGMergeParents`. It remains the shared coinbase parser adapter for orphan compatibility, not for `getblocks`. |
| `GetMissingDAGMergeParents` | Has two callers in `ProcessBlock`: initial missing-merge-parent orphan deferral and orphan retry. Removing it would change inbound block/orphan behavior. |
| `ExtractDAGParents`, `DAG_PARENT_TAG`, `CBlockDAGData` | The parser and serialized DAG metadata type remain. `CBlockDAGData` still serializes parents, children, color, score, order, and inferred K. |
| `CEpochState` | Still serializes epoch boundaries, ordered block hashes, curve/nullifier/finality roots, trust/counts, finality tier, and finalized state. Current consumers include `finality.cpp`, `rpcblockchain.cpp`, `rpcmining.cpp`, `rpcshielded.cpp`, and `wallet.cpp`. |
| LevelDB compatibility paths | `WriteDAGLinks`, `EraseDAGLinks`, `IterateDAGLinks`, `WriteEpochState`, `IterateEpochStates`, `WriteDAGCleanHeight`, and `ReadDAGCleanHeight` remain. The `daglinks`, `epochstate`, and `dagcleanheight` key layouts are unchanged. `IterateDAGLinks` retains pre-Phase-4 compatibility for missing `nInferredK`. |
| Startup manager loading | `CTxDB::LoadBlockIndex()` still calls `g_dagManager.LoadDAGLinks()` and `LoadEpochStates()`. This is an explicit compatibility boundary even though the audited snapshots contained zero such records. |
| P2P orphan/tip handling | The merge-parent orphan path and `getdagtips`/`dagtips` dispatch remain. Ordinary orphan maps and classic side-chain/reorg infrastructure remain and are not classified as DAG-only. |
| RPC | `getdaginfo`, `getdagtips`, `getdagorder`, `getepochinfo`, and `getdagconfidence` remain registered. Conditional DAG fields in generic block/epoch status responses also remain. |
| Qt | `IDAGPage`, `ClientModel::DAGStatus`, and `ClientModel::getDAGStatus()` remain, with live callers in the GUI and RPC console. |

Current `CBlock` and `CDiskBlockIndex` still have no separate serialized DAG
field. That fact does not authorize removing the coinbase parser or the separate
LevelDB record readers without their own proof. Retained code is a staged-removal
safety boundary, not positive evidence that audited mainnet blocks used it.

## 10. Remaining risks and non-claims

- The chain conclusions are scoped to the audited mainnet snapshots and tested
  builds. They do not prove compatibility with hypothetical external DAG chains,
  historical datadirs not examined here, or experimental testnet/regtest data.
- Git proves that a substantial consensus-capable DAG subsystem existed in the
  codebase. This report establishes only the absence of its semantics in the
  audited mainnet data and tested runtime paths.
- It does not authorize casual changes to historical block, transaction,
  `CDiskBlockIndex`, LevelDB, or wallet serialization. Byte compatibility remains
  mandatory wherever historical data is read.
- It makes no blanket compatibility claim for every old peer or private fork. It
  establishes that supported peers retain a conventional linear response for
  the audited linear mainnet.
- It does not resolve the separate collateralnode-payee validation issue that
  stopped the uncontrolled continuation experiment.
- It does not prove that every remaining IDAG-labelled RPC, Qt, epoch, finality,
  FCMP, miner, validation, storage, or P2P element is removable. Each needs a
  current dependency and reachability audit.
- The complete SHA-256 strings for the generated independent CSV and comparison
  summary were not retained in repository artifacts. Only their recorded
  prefixes are available; this is an evidence-retention limitation, not a reason
  to synthesize values.

## 11. Reproduction guide

All database commands below must use a stopped, disposable filesystem copy. The
standalone exporter can cause normal LevelDB service-file updates in the copy;
never point it at the live/original datadir. Keep outputs outside the copied
LevelDB directory.

### 11.1 Inspect history and removal lineage

```bash
git show -s --format=fuller 28bfb081 bbd9f42 0201620 \
  f31cb6e d299d24 d1c1405 2e96aa3 8bc4e5d

git show --stat --oneline \
  c9ea3d94fd05138f7fb02333b8c220e208e03a90 \
  051912edc89d83ba12231ad9fa63d0474b57fbb8 \
  6885e9ff55899cc787ee44546dee4c8da842b027

git show --no-ext-diff d299d24 -- src/main.cpp
git show --no-ext-diff d1c1405 -- src/main.cpp
```

### 11.2 Export a copied block index

Build the repository's standalone LevelDB exporter:

```bash
g++ -std=c++17 -O2 -Wall -Wextra -pedantic \
  -Isrc/leveldb/include \
  contrib/forensics/idag_leveldb_export.cpp \
  src/leveldb/libleveldb.a -pthread \
  -o /tmp/idag_leveldb_export
```

Its actual interface is
`DB_COPY INDEX_BY_HASH FILEPOS_INDEX RAW_DAG_CSV SUMMARY_JSON`:

```bash
/tmp/idag_leveldb_export \
  /path/to/stopped-copy/txleveldb \
  /tmp/idag-index.bin \
  /tmp/idag-filepos.bin \
  /tmp/idag-dag-prefixes.csv \
  /tmp/idag-export-summary.json
```

### 11.3 Reconstruct and compare ChainTrust

Run the independent formula implementation:

```bash
python3 contrib/analysis/reconstruct_chaintrust.py \
  /tmp/idag-index.bin \
  --summary /tmp/idag-independent-summary.json \
  --csv /tmp/idag-independent-chaintrust.csv
```

Compare a separately produced runtime CSV by hash:

```bash
python3 contrib/analysis/compare_runtime_chaintrust.py \
  --runtime /path/to/runtime-chaintrust.csv \
  --independent /tmp/idag-independent-chaintrust.csv \
  --summary /tmp/idag-chaintrust-comparison.json \
  --diff /tmp/idag-chaintrust-differences.csv
```

The runtime CSV exporter was temporary forensic instrumentation and was removed
after the experiment; current `master` does not expose that production runtime
option. Do not invent or re-add an exporter merely to follow this guide. The
durable accepted results are located with:

```bash
git show c59c806:doc/idag-chaintrust-numeric-verification.md
git show c59c806:doc/forensics/idag-chaintrust-numeric-summary.json
```

If the original large CSVs are available from external forensic storage, verify
them directly:

```bash
sha256sum /path/to/baseline-runtime.csv \
  /path/to/no-dag-runtime.csv \
  /path/to/idag-independent-chaintrust.csv \
  /path/to/idag-chaintrust-comparison.json
```

### 11.4 Locate snapshot evidence

```bash
python3 -m json.tool doc/forensics/idag-chaintrust-numeric-summary.json
python3 -m json.tool doc/forensics/idag-stage2-summary.json
head -13 doc/forensics/idag-stage2-manifest.txt

sha256sum \
  doc/forensics/idag-chaintrust-numeric-summary.json \
  doc/forensics/idag-stage2-summary.json \
  doc/forensics/idag-stage2-manifest.txt
```

The full block scanner and its tests are
`contrib/forensics/scan_idag_chain.py` and
`contrib/forensics/test_scan_idag_chain.py`. Their exact historical invocation,
including the Tribus helper and required exporter outputs, is recorded in
[`idag-stage2-mainnet-chain-forensics.md`](idag-stage2-mainnet-chain-forensics.md).

## 12. Decision

Incremental DAG/IDAG removal is approved for the audited Innova mainnet.
Canonical mainnet behavior must remain linear. Existing historical
serialization must remain byte-compatible wherever it is still read, and
classic forks, side chains, orphans, reorgs, locator resolution, and ordinary
`MSG_BLOCK` behavior must not be removed merely because they participated in a
DAG-era call path.

Each subsequent removal stage requires its own current caller/reference audit,
the smallest semantically homogeneous patch, daemon and relevant client/test
builds, and runtime verification whenever the changed behavior is reachable.
Storage, serialization, consensus, finality/FCMP, or wire changes require a
separate compatibility proof. This report closes the forensic rationale; it
does not begin or pre-approve the implementation details of Removal Stage 3.
