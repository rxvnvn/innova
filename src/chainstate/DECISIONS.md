# Chainstate slice - architecture decision log (Stage 2)

Scope: isolated prototype proving the core state-transition + durability mechanism.
No production path is touched. Base: master `05c1b525`. Branch `feat/chainstate-vertical-slice`.

## D-1 Coin representation
`Coin { value:i64; script:vector<u8>; height:i32; fCoinBase:bool; fCoinStake:bool }`.
Each field maps to a named legacy consumer (value/script -> spend validation; height ->
maturity; fCoinBase/fCoinStake -> maturity + PoS). stake modifier is a block/index attribute,
not a coin field. Chosen as the minimal field set that preserves legacy semantics.

## D-2 Ordered normalized delta
`Op { key; beforePresent; before Coin; afterPresent; after Coin }`, applied in block order.
Enables exact, order-sensitive reversal (create-then-spend within a block). `revertDelta`
walks ops in reverse restoring `before` exactly. Sets-of-created/spent (v0.2 concern) are
rejected because they lose order.

## D-3 Canonical state digest
SHA-256 over a domain tag plus entries in canonical key order. Deterministic and independent
of in-memory map layout. Used to prove connect->disconnect equality (U2) and restart equality.

## D-4 Single commit pointer + CoW state files
`MANIFEST` selects exactly one immutable `state-<gen>.dat`. Chosen as the **smallest correct**
mechanism that never exposes a partially applied change: rather than mutate a visible file in
place, each commit writes a new immutable file and publishes it by one atomic rename. Rejected:
in-place mutation (unsafe visibility), full-copy-per-generation with frequent publication
(quadratic IBD - not needed here, and not the production path).

## D-5 Commit protocol / boundaries
PREPARE (stage state + undo) -> PUBLISH (rename MANIFEST = **visibility**) -> DURABLE (parent-dir
fsync) -> POST (in-memory publish + GC). All mandatory checks precede the rename. A failed
staging (ENOSPC, fsync error) aborts before the rename, leaving the published state unchanged.

## D-6 Crash oracle
- before MANIFEST rename: deterministically OLD.
- after rename, before dir fsync: OLD **or** NEW depending on filesystem; asserted non-mixed
  (recovery resolves one committed generation; never a blend).
- after dir fsync: deterministically NEW.
SIGKILL-style process crash is used (`fork` + `_exit` at the barrier); this proves crash
consistency, NOT power-loss durability beyond the fsync contract.

## D-7 Single writer
Advisory `flock(LOCK_EX|LOCK_NB)` on `LOCK`. A second `open()` on the same directory fails with
`LOCKED`. No second authoritative storage path exists in the slice.

## D-8 Prototype (non-production) limitations
- `publish` serializes the **entire** state per commit (O(state) I/O). This is a prototype
  simplification to keep CoW trivially correct; the production O4 design uses a bounded delta
  path. T13 is therefore NOT an IBD test.
- `disconnect` supports only the most recent committed block (single-level). Arbitrary-depth
  reorg / rebuild / checkpoint seeds are out of scope for Stage 2.
- Maturity/PoS/BIP30 are modeled minimally (no script/signature verification, no coinstake
  structure); consensus rules are NOT re-implemented here.
