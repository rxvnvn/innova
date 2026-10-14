# Chainstate slice - architecture decision log (Stage 2B)

Scope: isolated prototype proving the core state-transition + durability mechanism, with
Stage-2A audit remediation F01..F07. No production path is touched. Base: master `05c1b525`.
Branch `feat/chainstate-vertical-slice` (Stage-2 prototype at `734e8a82`; Stage 2B on top).

## D-1 Coin representation
`Coin { value:i64; script:vector<u8>; height:i32; fCoinBase:bool; fCoinStake:bool }`.
Each field maps to a named legacy consumer (value/script -> spend validation; height ->
maturity; fCoinBase/fCoinStake -> maturity + PoS). stake modifier is a block/index attribute,
not a coin field. Minimal field set that preserves legacy semantics.

## D-2 Ordered normalized delta
`Op { key; beforePresent; before Coin; afterPresent; after Coin }`, applied in block order.
Enables exact, order-sensitive reversal (create-then-spend within a block). `revertDelta`
walks ops in reverse restoring `before` exactly. Create/spent sets are rejected (they lose order).

## D-3 Canonical state digest
SHA-256 over a domain tag plus entries in canonical key order. Deterministic, independent of
in-memory map layout. Proves connect->disconnect equality and restart equality.

## D-4 Single commit pointer + CoW state files
`MANIFEST` selects exactly one immutable `state-<gen>.dat`. The **smallest correct** mechanism
that never exposes a partially applied change: each commit writes a new immutable file and
publishes by one atomic rename. Rejected: in-place mutation (unsafe visibility), full-copy per
generation with frequent publication (quadratic - not the production path).

## D-5 Commit protocol / boundaries (Stage 2B)
STAGE (state + undo) -> ESTABLISH BACKUP (F02) -> rename state -> [VISIBILITY] rename MANIFEST
-> [DURABILITY] parent-dir fsync -> [ACKNOWLEDGMENT] durable ACK -> POST (in-memory publish + GC).
All mandatory checks precede the rename. A failure before the rename (staging or backup
establishment) leaves the published state unchanged. A failure *after* the rename (dir fsync or
ACK) may have published NEW on disk; `publish` returns failure and marks the store
`needsReopen` so no caller treats it as acknowledged. "Failure after rename" is therefore not
equivalent to "failure before rename".

## D-6 Crash oracle (Stage 2B, F04)
- before MANIFEST rename: deterministically OLD (exact digest AND generation asserted).
- after rename, before dir fsync: OLD **or** NEW; asserted as EXACTLY (gen1,digestOLD) or
  EXACTLY (gen2,digestNEW) - mixed/other combinations are rejected. Expected OLD and NEW digests
  are precomputed, not merely `!=`.
- after dir fsync: deterministically NEW (exact digest AND generation).
Process crash (`fork` + `_exit` at the barrier) proves crash consistency, NOT power-loss
durability beyond the fsync contract.

## D-7 Single writer (Stage 2B, F07)
Advisory `flock(LOCK_EX|LOCK_NB)` on `LOCK`. Verified with a real two-process test: a forked child
holds the lock, the parent cannot acquire it (`LOCKED`), the parent acquires it after the child
releases it, and the OS releases the lock on abnormal child exit (no explicit unlock).

## D-8 Acknowledgment boundary + fail-loud fallback (Stage 2B, F01)
A durable `ACK` record (written after the durability fsync) is the independent evidence that
distinguishes an *acknowledged* committed generation from an *unacknowledged* crash attempt.
Corruption of an acknowledged generation (state, MANIFEST, referenced files, or undo tail) makes
`open` FAIL LOUD; the store never silently rolls back to an older tip and never returns success
with only a warning flag. When the evidence is insufficient to classify, `open` fails closed.
Backups are established with checked read/write/fsync/rename and verified before publish aborts.

## D-9 Undo memory/retention (Stage 2B, F03)
In-RAM undo history is reduced to a metadata-only index (bodies read from disk on demand), so RAM
is a small fixed struct per record rather than full op vectors. Disk `undo.dat` and the index still
grow with history; bounded retention/reclaim is **NOT** implemented and is recorded as an OPEN,
mandatory Stage 3 acceptance gate. F03 is not claimed closed while `undo.dat` can grow unbounded.

## D-10 Prototype (non-production) limitations
- `publish` serializes the **entire** state per commit (O(state) I/O) - a prototype simplification
  for trivially-correct CoW; production O4 uses a bounded delta path. T13 is NOT an IBD test.
- `disconnect` supports only the most recent committed block (single-level). Arbitrary-depth reorg
  / rebuild / checkpoint seeds are out of scope (Stage 3).
- Reader pinning of generations is not implemented; "no live reader pin invalidated" is NOT
  APPLICABLE to this prototype and OPEN for Stage 3.
- Maturity/PoS/BIP30 modeled minimally (no script/signature verification, no coinstake structure);
  consensus rules are NOT re-implemented here. F08 (coinstake time semantics) remains a documented
  production-integration requirement.
