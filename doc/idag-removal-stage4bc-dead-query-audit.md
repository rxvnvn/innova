# Innova DAG/IDAG Stage 4B-c Dead Query Audit

## 1. Executive conclusion

The residual `getdagorder` argument-conversion rule is stale client-side residue, not live RPC behavior. It sits in `RPCConvertValues()` inside `src/innovarpc.cpp` and is only reached by the CLI command path before `CallRPC()`. Since `getdagorder` is no longer registered in `CRPCTable`, removing this conversion rule does not change the final JSON-RPC result; the command still ends in method-not-found.

The three `CDAGManager` query helpers below are also caller-free in current source:

- `CDAGManager::GetPrunedBelowHeight()`
- `CDAGManager::CompareBlockOrder()`
- `CDAGManager::GetOrderConfidence()`

They are read-only reporting helpers with no production callers and no current tests or runtime paths that depend on them. They are therefore suitable for a small dead-code cleanup, but `CompareBlockOrder()` strands one additional private helper, `SupportingMass()`, which should be handled in a follow-up cleanup stage rather than mixed into the same boundary unless the history is intentionally widened.

Recommended immediate implementation boundary:

- remove the stale `getdagorder` conversion rule first, as a one-line isolated cleanup;
- remove the three manager query helpers in a separate backend cleanup stage;
- then remove `SupportingMass()` only after a fresh audit confirms it is caller-free.

This keeps the semantic layers separate: stale CLI residue versus backend dead helpers.

## 2. Repository state

- Branch: `master`
- HEAD: `d64228e` (`refactor(idag): remove dead DAG RPC handlers`)
- Stage 4B commit: `5a8bb69037ad39d84188f2c5397b78bfd4f47664` (`refactor(idag): unregister DAG RPC commands`)
- Stage 4B-b commit: `d64228ed8e0e21da8e116b02a5943ccf1d911142` (`refactor(idag): remove dead DAG RPC handlers`)

Working tree status at audit time:

- tracked tree clean except for pre-existing untracked forensic/docs/build artifacts
- no production file changes made during this audit

## 3. Stale RPC conversion analysis

### 3.1 Location and behavior

`src/innovarpc.cpp:1592-1748` defines `RPCConvertValues(const std::string& strMethod, const std::vector<std::string>& strParams)`.

Relevant current branch:

- `src/innovarpc.cpp:1711-1713`
- `if (strMethod == "getdagorder" && n > 0) ConvertTo<int64_t>(params[0]);`
- `if (strMethod == "getepochinfo" && n > 0) ConvertTo<int64_t>(params[0]);`

`RPCConvertValues()` is called by `CommandLineRPC()` before `CallRPC()`:

- `src/innovarpc.cpp:1748`
- `Array params = RPCConvertValues(strMethod, strParams);`

This means:

- the conversion rule is part of the CLI parameter-coercion layer;
- it is not part of the RPC command table;
- it does not make `getdagorder` callable again;
- removing it only changes pre-dispatch type coercion for an already-unregistered command.

### 3.2 Reachability

- JSON-RPC dispatch: unaffected by this rule, because registration lookup happens after the conversion step and `getdagorder` is absent from the command table.
- CLI path: still reaches the conversion code if the user types `getdagorder`, but the call still terminates in method-not-found once `CallRPC()` runs.
- Tests/scripts: no current production test depends on the conversion behavior itself; the remaining mentions are historical regression scripts and docs.

### 3.3 Other removed DAG RPCs

Search found no equivalent conversion residue for:

- `getdaginfo`
- `getdagtips`
- `getdagconfidence`

The only stale DAG conversion rule in the current source tree is `getdagorder`.

## 4. Manager method inventory

### 4.1 `CDAGManager::GetPrunedBelowHeight()`

- Declaration: `src/dag.h:227`
- Definition: `src/dag.cpp:1397-1401`
- LSP references: 0
- Textual production references: none

Semantics:

- returns the cached `nPrunedBelowHeight` value under `cs_dag`
- reads memory only
- does not read LevelDB
- does not mutate state
- does not affect pruning decisions; pruning is handled elsewhere, with startup restoration done through `SetPrunedBelowHeight()` in `src/init.cpp`

Assessment:

- pure query helper
- currently caller-free
- safe candidate for direct removal

### 4.2 `CDAGManager::CompareBlockOrder()`

- Declaration: `src/dag.h:188`
- Definition: `src/dag.cpp:1537-1577`
- LSP references: 0
- Textual production references: none

Semantics:

- compares two hashes as DAGKNIGHT ordering candidates
- first checks ancestry with `GetPastSet()`
- falls back to `SupportingMass()` when blocks are in each other's anticone
- returns `-1`, `0`, or `1`
- writes confidence through the out parameter
- reads `mapDAGData`, `GetPastSet()`, `SupportingMass()`, and `vDAGChildren`

Assessment:

- query helper only
- no consensus, mining, or validation caller found
- caller-free in current source
- removing it would make `SupportingMass()` caller-free as a new follow-up candidate

### 4.3 `CDAGManager::GetOrderConfidence()`

- Declaration: `src/dag.h:191`
- Definition: `src/dag.cpp:1711-1749`
- LSP references: 0
- Textual production references: none

Semantics:

- BFS over descendant graph from `hashBlock`
- counts blue descendants within `DAGKNIGHT_MAX_ANTICONE_WINDOW`
- returns a confidence score only
- reads `mapDAGData` and `vDAGChildren`
- does not mutate state

Assessment:

- query helper only
- no production caller found
- safe candidate for direct removal

## 5. Dependency graph

### 5.1 CLI residue

`CommandLineRPC()`  
`-> RPCConvertValues()`  
`-> stale `getdagorder` `ConvertTo<int64_t>(params[0])` rule`  
`-> CallRPC()`  
`-> method-not-found once command lookup sees no registration`

### 5.2 Backend query helpers

`CDAGManager::GetPrunedBelowHeight()`  
`-> nPrunedBelowHeight`

`CDAGManager::CompareBlockOrder()`  
`-> GetPastSet()`  
`-> SupportingMass()`  
`-> mapDAGData / vDAGChildren`

`CDAGManager::GetOrderConfidence()`  
`-> mapDAGData / vDAGChildren`

`SupportingMass()` is currently only used by `CompareBlockOrder()`. If `CompareBlockOrder()` is deleted, `SupportingMass()` becomes caller-free and should be cleaned up separately.

`GetPastSet()` remains live through other DAG runtime code and is not part of this cleanup boundary.

## 6. Reachability and compatibility

| Case | Stale `getdagorder` conversion | `GetPrunedBelowHeight()` | `CompareBlockOrder()` | `GetOrderConfidence()` |
| --- | --- | --- | --- | --- |
| Audited historical mainnet | unreachable in meaning; command already unregistered | unreachable | unreachable | unreachable |
| Honest current mainnet continuation | same as above | unreachable | unreachable | unreachable |
| Activated testnet/regtest | CLI residue only; no dispatch change | unreachable unless compiled against headers | unreachable unless compiled against headers | unreachable unless compiled against headers |
| Hypothetical private DAG network | only CLI parameter coercion residue | query-only if called locally | query-only if called locally | query-only if called locally |
| Direct unit-test invocation | no current callers | no current callers | no current callers | no current callers |
| External RPC/CLI after Stage 4B | still reaches pre-dispatch conversion, then method-not-found | unchanged | unchanged | unchanged |

Compatibility conclusions:

- removing the stale conversion rule does not alter final RPC availability or error semantics
- removing the three manager methods affects only source-level API surface and any code compiled against those declarations
- no consensus, wallet, serialization, LevelDB, mining, staking, or P2P surface is involved
- no ABI policy was found in this repository that requires retaining caller-free public C++ methods

## 7. Tests and tooling inventory

Current textual references to `getdagorder` and the manager methods are historical/regression artifacts, not live production callers.

- `contrib/test/idag_phase2_test.sh`
  - `rpc1 getdagorder 10`
  - characterizes old DAG-order output
  - now obsolete because the RPC was unregistered in Stage 4B

- `contrib/test/idag_phase4_test.sh`
  - `rpc1 getdagorder 5` and `rpc1 getdagorder 50`
  - characterizes DAGKNIGHT-era ordering output
  - now obsolete for the same reason

- `doc/idag-stage1-historical-semantic-audit.md`
  - mentions `CompareBlockOrder` and `GetOrderConfidence`
  - historical evidence only

- `doc/idag-removal-stage4b-rpc-qt-audit.md`
  - records the earlier RPC removal boundary
  - historical evidence only

No unit-test or qa-test source was found that directly invokes `GetPrunedBelowHeight()`, `CompareBlockOrder()`, or `GetOrderConfidence()` as production code.

If the backend methods are removed, the two functional scripts above should be preserved only as historical evidence or updated to reflect the already-unregistered RPC behavior, depending on the project policy for obsolete characterization tests.

## 8. Recommended implementation boundary

### Option B recommended

Stage 4B-c:

- remove only the stale `getdagorder` argument-conversion rule from `src/innovarpc.cpp`

Stage 4B-d:

- remove `CDAGManager::GetPrunedBelowHeight()` from `src/dag.h` and `src/dag.cpp`
- remove `CDAGManager::CompareBlockOrder()` from `src/dag.h` and `src/dag.cpp`
- remove `CDAGManager::GetOrderConfidence()` from `src/dag.h` and `src/dag.cpp`

Stage 4B-e follow-up:

- remove `SupportingMass()` after a fresh caller audit

Why split:

- the conversion rule is a client-side pre-dispatch residue with zero backend dependency
- the backend methods are dead public query helpers with their own follow-up dead-helper cascade
- splitting keeps each commit semantically narrow and makes regression attribution clearer

Expected deletion counts:

- Stage 4B-c: 1 line or a few lines
- Stage 4B-d: small multi-file deletion set, likely header plus implementation blocks

## 9. Blockers and non-claims

Not blocked:

- no production caller was found for any of the three manager methods
- no runtime dependency was found for the stale `getdagorder` conversion rule

Open follow-up:

- `CompareBlockOrder()` removal strands `SupportingMass()`

Non-claims:

- this audit does not claim that other `CDAGManager` methods are dead
- this audit does not claim DAG storage, consensus, or epoch/finality code is removable
- this audit does not change or revisit `getepochinfo`

## 10. Validation plan

For Stage 4B-c:

- `git diff --check`
- clean `innovad` build
- `test_innova` build/run
- clean Qt build
- optional CLI smoke for `getdagorder` returning method-not-found, if a safe runtime is available

For Stage 4B-d:

- repository-wide symbol audit and LSP reference audit after editing
- `git diff --check`
- clean `innovad` build
- `test_innova` build/run
- clean Qt build
- optional runtime smoke is not required, because the change removes caller-free query methods only

For Stage 4B-e:

- fresh audit for `SupportingMass()` after `CompareBlockOrder()` is removed
- same build/test checks as above

