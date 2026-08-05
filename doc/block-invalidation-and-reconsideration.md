# Block Invalidation and Reconsideration

## Overview

Innova Core provides two operator RPC commands for controlled chain recovery:

```text
invalidateblock <hash>
reconsiderblock <hash>
```

These commands allow a node operator to exclude a known block and its descendants from active-chain selection, and later restore that branch to eligibility.

The implementation was introduced in commit:

```text
a3e3d97f51f8585d3691b380db42101b90d59fa8
feat(rpc): add block invalidation and reconsideration
```

This mechanism is intended for operational recovery from an unwanted chain branch, split-chain incident, or other situation where an otherwise consensus-valid branch must temporarily be excluded by the local node operator.

Operator invalidation is local node state. It does not change consensus rules and is not propagated to peers.

---

## RPC Commands

### `invalidateblock`

```text
invalidateblock <hash>
```

Marks the specified block hash as explicitly invalid for local chain selection.

If the block is part of the active chain, the node disconnects that block and all active descendants, then attempts to activate the highest-trust eligible alternative chain.

Example:

```bash
innovad \
  -datadir="$HOME/.innova" \
  invalidateblock <block-hash>
```

The block hash must be supplied as a string.

### `reconsiderblock`

```text
reconsiderblock <hash>
```

Removes the specified hash from the explicit invalidation set.

After the hash is removed and the updated set is persisted, the node evaluates all eligible chain tips and may reactivate the reconsidered branch if it has the highest chain trust.

Example:

```bash
innovad \
  -datadir="$HOME/.innova" \
  reconsiderblock <block-hash>
```

Calling `reconsiderblock` for a block that is not explicitly invalidated is an idempotent successful operation.

---

## Invalidity Model

The node stores explicitly invalidated block hashes in:

```cpp
std::set<uint256> setInvalidBlockHash;
```

Only hashes explicitly selected by the operator are stored in this set.

A block is considered operator-invalid when either:

1. its own hash is present in `setInvalidBlockHash`; or
2. any ancestor in its `pprev` chain is present in `setInvalidBlockHash`.

This is evaluated through the central predicate:

```cpp
IsBlockOperatorInvalid(...)
```

Therefore, descendants of an explicitly invalidated block are implicitly invalid without requiring every descendant hash to be stored.

Example:

```text
A1 → A2 → A3 → A4 → A5
          ^
          explicitly invalidated
```

In this case:

```text
A3 — explicitly invalid
A4 — implicitly invalid
A5 — implicitly invalid
```

Only `A3` is stored in `setInvalidBlockHash`.

---

## Multiple Explicit Invalidations

Each explicitly invalidated hash is independent.

Example:

```text
invalidate A3
invalidate A5
```

The stored set contains both hashes:

```text
{ A3, A5 }
```

If the operator then runs:

```text
reconsider A3
```

only `A3` is removed. `A5` remains explicitly invalid.

Similarly, reconsidering a descendant does not override an independently invalidated ancestor.

Example:

```text
invalidate A3
reconsider A5
```

The branch remains invalid because `A3` is still present in the explicit invalidation set.

---

## Difference from Consensus Invalidity

Operator invalidation is not a consensus failure.

Blocks excluded through `invalidateblock`:

* are not marked as consensus-invalid;
* do not trigger `InvalidChainFound`;
* do not increase peer misbehavior scores;
* do not cause peer bans;
* do not update consensus-invalid trust tracking;
* may become eligible again through `reconsiderblock`.

This distinction is important because the excluded block may be fully valid under consensus rules. It is rejected only because the local operator explicitly excluded it from chain selection.

Logs and return paths should therefore distinguish operator invalidity from ordinary validation failure.

---

## Chain Activation Enforcement

Operator-invalid blocks are excluded from every chain-activation path.

The central invalidation predicate is enforced in:

* `AcceptBlock`;
* `ProcessBlock`;
* `SetBestChain`;
* `SetBestChainInner`;
* `Reorganize`;
* best eligible chain selection.

This prevents an already indexed invalid branch from becoming active without passing through `AcceptBlock` again.

It also prevents new descendants from extending a branch whose ancestor is operator-invalid.

---

## Invalidation Flow

The high-level `invalidateblock` sequence is:

```text
validate request
→ persist updated invalid set
→ roll back active chain if necessary
→ locate best eligible alternative tip
→ activate best eligible chain
```

### Request validation

The command rejects:

* an unknown block hash;
* the genesis block;
* operation during import;
* operation during reindex;
* invalidation that conflicts with finalized-chain constraints.

The finality check is performed before persistent state is modified.

### Persist-before-rollback

The updated invalidation set is written to the database before active-chain rollback begins.

This ordering guarantees that a crash cannot leave the node with an invalidated branch still considered eligible after restart.

The possible crash window is instead:

```text
invalid set persisted
→ process crashes before rollback finishes
```

This state is repaired automatically during startup.

### Active-chain rollback

If the invalidated block is an ancestor of or equal to the active tip, the node rolls back to the highest valid ancestor below the invalidated section.

The rollback reuses the existing chain transition machinery, including:

* `SetBestChain`;
* `Reorganize`;
* `DisconnectBlock`;
* wallet and mempool transition handling;
* finality constraints.

### Alternative-chain activation

After rollback, `ActivateBestEligibleChain` scans eligible chain tips and selects the highest-trust chain that:

* has no explicitly invalidated block in its ancestry;
* has complete indexed ancestry;
* satisfies finality constraints;
* can be connected through the normal chain activation path.

If no competing eligible chain is available, the node remains at the highest valid ancestor below the invalidated branch.

---

## Reconsideration Flow

The high-level `reconsiderblock` sequence is:

```text
remove exactly one explicit invalidation
→ persist updated set
→ scan eligible chain tips
→ activate highest-trust eligible chain
```

Reconsideration removes only the named hash.

It does not:

* clear invalidated ancestors;
* clear invalidated descendants;
* clear all invalidations for a branch;
* override another independently invalidated hash.

If the reconsidered branch has greater chain trust than the current active chain, it may become active again through the normal chain activation path.

---

## Persistence

The explicit invalidation set is stored under the database key:

```text
setInvalidBlockHash
```

Persistence is implemented in both supported block-index backends:

* Berkeley DB;
* LevelDB.

The database interface provides equivalent read and write operations in both backends:

```text
ReadInvalidBlockSet
WriteInvalidBlockSet
```

A missing database key is interpreted as an empty invalidation set for backward compatibility with existing datadirs.

No change is made to `CDiskBlockIndex` serialization. This avoids changing the per-block block-index record format.

---

## Startup Crash Recovery

A crash may occur after the invalidation set has been persisted but before the active chain has been rolled back.

Example crash state:

```text
persist invalid hash
→ crash
→ database best-chain pointer still descends from invalidated block
```

During startup, both database backends call:

```text
RecoverFromInvalidatedBestChain
```

through the normal `CTxDB::LoadBlockIndex` path.

The recovery procedure:

1. checks whether the persisted best chain descends from an operator-invalid block;
2. walks backward to the highest valid ancestor;
3. updates the persisted best-chain pointer;
4. leaves the node in a consistent state before normal chain activation continues.

This makes the persist-before-rollback ordering recoverable.

### Test-harness limitation

In the LevelDB test environment, a second call to `CTxDB::LoadBlockIndex` is a no-op when `mapBlockIndex` is already populated.

Therefore, the crash-window unit test exercises `RecoverFromInvalidatedBestChain` directly against a persisted crash-state fixture rather than pretending that a second in-process loader call is a real restart.

Production startup uses the normal empty-process `LoadBlockIndex` path and executes the recovery hook.

---

## Finality Constraints

Operator invalidation cannot be used to violate finalized-chain guarantees.

Before changing persistent invalidation state, the implementation verifies that invalidating the requested block would not require disconnecting protected finalized history.

The following cases are rejected:

* invalidating genesis;
* invalidating a protected finalized active block;
* invalidating a branch whose required rollback would cross finality;
* attempting the operation during import or reindex.

The RPC returns an error before mutation when the operation cannot be safely performed.

---

## RPC Registration

The commands are registered in the RPC table as:

```text
invalidateblock
reconsiderblock
```

Both commands:

* require an available RPC server;
* accept exactly one block hash string;
* return `RPC_INVALID_PARAMETER` for invalid requests;
* are not safe-mode-only commands;
* do not require the wallet to be unlocked.

No `RPCConvertValues` entry is needed because the block hash must remain a JSON string.

---

## Tests

The implementation adds:

```text
src/test/invalidate_reconsider_tests.cpp
```

The full test suite contains 272 passing tests, including four new block invalidation cases.

### Inactive side-chain invalidation

Verifies that:

* invalidating a genuinely inactive side-chain block does not change the active chain;
* descendants are implicitly invalid;
* new work extending the invalid branch is rejected;
* reconsidering the explicit hash restores branch eligibility.

### Active-chain rollback and alternative activation

Verifies that:

* invalidating an active-chain ancestor disconnects it and its descendants;
* the best eligible alternative chain becomes active;
* reconsidering the block restores the longer or higher-trust branch.

### Persistence and crash-window healing

Verifies that:

* the explicit invalidation set survives database round-trip;
* a crafted persisted crash state with a stale best-chain pointer is detected;
* startup healing rolls back to the highest valid ancestor;
* the repaired best pointer is persisted.

### Multiple invalidations and errors

Verifies:

* unknown-hash errors;
* genesis protection;
* idempotent invalidation and reconsideration behavior;
* independent explicit invalidations;
* reconsidering one hash does not remove another;
* invalidating both competing branches leaves the active chain at their common valid fork.

---

## Operational Usage

### Inspect the target block

Before invalidating a block, record:

```bash
innovad getblock <hash>
innovad getblockcount
innovad getbestblockhash
```

Confirm that the target hash and expected rollback point are correct.

### Invalidate a branch

```bash
innovad \
  -datadir="$HOME/.innova" \
  invalidateblock <hash>
```

Then verify:

```bash
innovad \
  -datadir="$HOME/.innova" \
  getblockcount

innovad \
  -datadir="$HOME/.innova" \
  getbestblockhash
```

### Reconsider a branch

```bash
innovad \
  -datadir="$HOME/.innova" \
  reconsiderblock <hash>
```

Then verify whether the branch became active again.

### CLI argument ordering

This Innova command-line parser stops processing switches after the first non-switch command argument.

Therefore, all switches must appear before the RPC command.

Correct:

```bash
innovad \
  -datadir="$HOME/.innova" \
  -rpcuser=<user> \
  -rpcpassword=<password> \
  invalidateblock <hash>
```

Incorrect:

```bash
innovad invalidateblock <hash> -datadir="$HOME/.innova"
```

In the incorrect form, switches after the command may be ignored.

---

## Known Limitations

### No invalidation query RPC

There is currently no command such as:

```text
getinvalidatedblocks
```

Operators must track explicitly invalidated hashes externally or inspect the database state through diagnostic tooling.

### No bulk reconsideration

`reconsiderblock` removes exactly one explicit invalidation per call.

There is no RPC for:

* reconsidering an entire subtree;
* clearing all invalidations;
* listing all explicit invalidations.

This is intentional for deterministic per-hash semantics.

### Test-only stale connected-side-branch scenario

`ActivateBestEligibleChain` may activate an indexed side-chain tip that has not previously been active.

Repeated artificial rollback and activation sequences in the test harness can create a stale “connected but orphaned” side-branch state and cause a later reconnection attempt to double-connect a block.

The normal operator flow does not construct this state. The affected test setup was corrected to model the persisted crash window directly without creating an invalid connected-side-branch invariant.

### In-process LevelDB reload behavior

The LevelDB implementation returns early from `LoadBlockIndex` when the global block index is already populated.

As a result, an in-process second loader call is not equivalent to a real daemon restart.

Production restart behavior is covered by the startup healing path itself, while unit tests call the recovery helper directly against the persisted crash-state model.

---

## Security and Safety Considerations

`invalidateblock` can cause a deep local rollback and should be treated as an administrative recovery command.

Before using it on mainnet:

* back up the datadir and wallet;
* record the current best block hash and height;
* confirm the intended rollback point;
* verify finality constraints;
* ensure the target block hash is correct;
* monitor wallet and mempool behavior during reorganization.

The command affects only the local node. Other network nodes continue to follow their own chain-selection state.

An operator-invalid block may be fully consensus-valid. Invalidating it does not prove that the block or its creator is malicious.

---

## Future Improvements

Possible follow-up work includes:

* `getinvalidatedblocks`;
* explicit RPC result objects containing old and new tips;
* dry-run invalidation validation;
* detailed operator-invalid rejection telemetry;
* GUI or dashboard visibility;
* integration tests using a true process restart;
* administrative RPC audit logging;
* documentation for multi-node split-chain recovery procedures.

These additions are not required for the current persistent invalidation and reconsideration implementation.
