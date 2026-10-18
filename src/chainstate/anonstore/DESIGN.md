# ANONSTATE — standalone ANON / key-image store under `chainstate/`

STATUS: DRAFT — architecture contract for M8.3 slice-a (build-the-replacement-index).
Approved datadir contract: `innova-v2-datadir-contract-20261018.md` (separate store
INSIDE `chainstate/`, sibling of `txloc`). This file is the load-bearing spec the
implementer codes against. It is NOT wired into production yet.

## Problem

Legacy leveldb (`txleveldb`, CTxDB) serves two historical ANON maps that are still in
the live consensus + wallet + RPC path at HEAD `22f2485b`:

- anon output  : key = 33-byte one-time pubkey (`CPubKey pkCoin`) -> `CAnonOutput`
                 `{COutPoint outpoint; int64 nValue; int nBlockHeight; uint8_t nCompromised;}`.
                 leveldb key = pair("ao", pkCoin); Written on anon output creation in
                 ProcessAnonTransaction; read in ring-sig verification (CheckAnonInputs)
                 by ring-member pubkey; never erased while the creating block is connected.
- key image    : key = 33-byte `ec_point keyImage` -> `CKeyImageSpent`
                 `{uint256 txnHash; uint32_t inputNo; int64 nValue;}`.
                 leveldb key = pair("ki", keyImage); Written on anon spend; read to reject
                 key-images already spent by another txn.
- Both ERASED on disconnect (UndoAnonTransaction) -> reorg must revert them atomically.

The store must preserve this historical consensus semantics while moving the reads away
from `txleveldb`. Caller map (current HEAD, 18 sites): main.cpp(4335, 5609, 5735),
wallet.cpp(5517, 5549, 5530, 5557, 5610, 5676, 5684, 5841, 5873, 5926, 5940, 6783, 7993),
rpcwallet.cpp(3693, 3868).

## Design

Reuse the audited store3a primitives; do NOT reimplement storage:

- `chainstate/store3a/btree.h` `class BTree` — self-contained CoW paged B+tree, Merkle
  node hashing, crash-tail drop (`truncatedTail`), free-list reclaim, partial-write
  injection hooks, `create/open/fsync/get/put/erase/setRoot/loadPage0AsRoot/collectAll/
  verifyAll/checkRootHash`. Key = 36-byte array; value = opaque bytes. THIS is the
  disk-resident random-access structure (bounded RAM, on-disk like the coin store).
- `chainstate/chainstate.h` `ChainError`/`ErrCode`, `Hash` (32-byte u256), the canonical
  little-endian `putU32/putI64/getU32/getU64` encoders.
- `chainstate/store3a/incstore.h` dump-suffix helpers `csWriteFile/csAppendFile/
  csWriteAtFile/csReadFile/csReadRange/csFsyncDir/csCrc32` (defined in incstore.cpp) for
  the commit-protocol files.

New value layer in a fresh TU `chainstate/anonstore/anonstore.{h,cpp}` (namespace
`cs::anon`), isolated from the coin store (no change to `Delta`/`Coin`/`Manifest`/`Op`,
no Cloud risk to existing Store3A):

### Key encoding (33-byte value -> 36-byte tree key)
```
[0]: namespace byte   ANON_OUTPUT_NS = 0xA1, KEY_IMAGE_NS = 0xB1
[1..33]: the 33 raw bytes (CPubKey::Raw() / ec_point bytes)
[34..35]: 0x00,0x00
```
The ns byte is collision-free against any future unified key layout; the tree treats
keys opaquely. (0xA1/0xB1 can never equal the high byte of a real txid-derived outpoint
prefix in this store because coin readers in the coin store are a DIFFERENT store; here
the tree only ever holds anon keys.)

### Value encoding (fixed-width, little-endian — matches legacy boost-serialized globals)
- CAnonOutput  : outpoint.txid[32] + outpoint.n(u32) + nValue(i64) + nBlockHeight(i32) + nCompromised(u8) = 49 bytes.
- CKeyImageSpent: txnHash[32] + inputNo(u32) + nValue(i64) = 44 bytes.

### Semantics preserved
- putAo at an existing DIFFERENT de-serialized value => fail closed (legacy "duplicate
  anon output"); an identical-value re-put (idempotent re-connect / Update path) is
  allowed.
- putKi overwrites (last spend wins at connect; double-spend-of-different-txn rejection
  is the caller's job — the store reflects last-write). Legally, an overwrite that erases
  a DIFFERENT mid-block spend must still leave the committed tree consistent; the caller
  serializes against it.
- erase reverts to before-state (a missing-before erase is a no-op success, mirroring
  legacy Erase returning true when absent).

### API
```cpp
namespace cs { namespace anon {
 struct Mutation { uint8_t ns; std::array<uint8_t,33> key;
                   bool beforePresent; std::vector<uint8_t> before;
                   bool afterPresent;  std::vector<uint8_t> after; };
 class AnonStore {
  public:
   bool open(const std::string& dir, ChainError* e);     // fail-closed; authority = MANIFEST
   bool opened() const; void close();
   bool getAo(const uint8_t key33[33], CAnonOutput*, bool* found, ChainError* e) const;
   bool getKi(const uint8_t key33[33], CKeyImageSpent*, bool* found, ChainError* e) const;
   bool connect(const Hash& blockHash, int32_t height,
                const std::vector<Mutation>& muts, ChainError* e);        // atomic
   bool disconnect(const Hash& blockHash, const Hash& parentTipHash, ChainError* e); // atomic revert
   // diagnostics
   uint64_t generation() const; const Hash& tipHash() const; int32_t tipHeight() const;
   size_t valueCount() const;  bool needsReopen() const; const std::string& dir() const;
   const BTree& tree() const; BTree& tree();
   // test hooks
   void setCrashPoint(CrashPoint p);  void setInjectStage(InjectStage s);
   void injectNoSpace(bool); void injectFsyncFail(bool); void clearInjection();
 };
}}
```
`CAnonOutput`/`CKeyImageSpent` are re-declared as thin plain structs inside namespace
`cs::anon` with the exact field set above (do NOT depend on core.h in this slice — the
harness is isolated). `CAnonOutput`: `std::array<uint8_t,32> txid; uint32_t n;
int64_t nValue; int32_t nBlockHeight; uint8_t nCompromised;`. `CKeyImageSpent`:
`std::array<uint8_t,32> txnHash; uint32_t inputNo; int64_t nValue;`.

### Durability / atomicity / disconnect protocol (mirror store3a commitBarriers)
Authority: `MANIFEST` (holds `formatVersion`, `generation`, `undoSize`, `rootPageId`,
`rootHash`, `blockHash`, `tipHeight`). On open, load MANIFEST -> `tree_.open(pagetree)`
-> `setRoot(rootPageId, rootHash)` (hash-authenticated root; corrupt page fails closed).
- connect: CoW-mutate btree (`put`/`erase`), building a `tree.dat` tail; append an
  UNDO record `{blockHash,height, [ (ns,key,beforePresent,before,afterPresent,after) ]}`
  + trailing crc32 to `undo.dat`; fsync; write `MANIFEST.tmp` (new rootId/rootHash/
  undoSize/blockHash/height) + fsync; atomic rename -> `MANIFEST`; dir fsync; write
  `ACK`(generation) + fsync. Crash anywhere leaves a committed-previous-Manifest tree
  valid (CoW) — the undo log may be AHEAD (dropped, never authoritative) or equal
  (recovered); `undo.dat` bytes beyond committed `undoSize` are ignored/trimmed on open.
- disconnect: READ tip UNDO record (at committed `undoSize`); apply REVERSE mutations
  to the btree (CoW); set new root; write MANIFEST for parent (parentTipHash, height-1,
  undoSize minus the popped record); same commit barriers. Deep disconnect beyond the
  retained undo window => FAIL CLOSED (return error, never silently truncate authority)
  — documented as an OPEN gate for a later bounded/segmented-undo slice.
- fresh vs existing store both covered (create path: `tree_.create` + page-0 root; open
  of absent MANIFEST -> empty -> first connect authorizes).

### Test harness (isolated slice, NOT in the daemon build)
Single-TU harness with its own `main()`, mirroring the store3a_tests build recipe
(STORE3A-style env override, per-case isolated `baseDir()`):
```
g++ -std=c++11 -O2 -I. -Ichainstate -Ichainstate/store3a -Ichainstate/anonstore \
  -o <rt>/anonstore_tests \
  chainstate/anonstore/tests/anon_tests.cpp chainstate/anonstore/anonstore.cpp \
  chainstate/store3a/incstore.cpp chainstate/store3a/btree.cpp chainstate/chainstate.cpp
```
Cases (RED-GREEN proven, crash-injected, restart-equivalence):
- put/get/erase round-trip both namespaces incl. value equality + key-distinct.
- connect(commit) persists across open()/reopen (fresh store AND pre-existing store).
- disconnect reverts exactly (tree rootHash returns to parent; values gone).
- two-block connect then two-block disconnect (reorg) leaves state at the original root.
- crash-injection matrix (partial page append, short undo write, fsync fail, ENOSPC,
  rename pre/post, interrupted MANIFEST write) => after reopen, store is either the
  committed-previous state or the fully-committed state — NEVER a torn hybrid; assert
  the invariant (committed root hash always reachable + undoSize == authority).
- duplicate-anon-output fail-closed test; distinct-value key-image overwrite test.
- PKRSS limiting / valueCount accounting.

## Out of scope for slice-a (OPEN gates, must be stated in the report)
- Wiring into production ConnectBlock/DisconnectBlock/wallet/RPC (slice-b: the 18 call
  sites, migrate-then-delete).
- Genesis-to-tip migration feed for the `ao`/`ki` legacy keys (slice-c).
- Bounded / segmented undo (deep-reorg beyond retained window).
- Format version bump discipline for the anon MANIFEST on later field adds.

## slice-c-prep (fc971c61) — bulk-seed migration base
`seedAo`/`seedKi` (BTree `seedPut`: CoW append + free-page reclaim, root-hash
authenticated per write) + `finalizeSeed` (ONE atomic publish, `undoSize=0` base;
crash-before-finalize -> gen0 empty & re-seed re-runs; crash into/after the MANIFEST
rename -> committed-previous or fully-seeded, never torn; disconnect below the seeded
base -> fail-closed NOT_FOUND). Duplicate anon-output during seed -> DUP_OUTPOINT.
Post-seed connect()/disconnect() add/revert their own undo records.

## slice-b-feed — DETERMINISTIC, wallet-INDEPENDENT mutation derivation (anonfeed.h/.cpp)
Mandatory cutover condition #2: ANON consensus-state must be formed deterministically
from blocks and their transactions, INDEPENDENT of wallet ownership, wallet scan, or
pwalletMain — wallet-specific handling must NOT be the sole mutation source. `anonfeed`
owns this: a daemon adapter (main.cpp, later slice) extracts `FeedOutput`/`FeedInput`
records from a CBlock (core.h IsAnonInput/ExtractKeyImage/ExtractRingSize, IsAnonOutput),
and `deriveAnonMutations(BlockFeed, CommittedAoView, out, e)` emits the ordered
`Mutation` vector a block connect must apply (putAo for created anon outputs, putKi for
spent key images) and a disconnect must revert. Semantics:
- PER-TX sequence (BlockFeed::FeedTx) preserved: intra-block visibility matches legacy
  sequential processing — a later tx's inputs resolve against committed store + anon
  outputs of STRICTLY EARLIER txs, never its own/later txs' outputs.
- Key-image nValue = ring's common value (resolved from ring[0] + all members re-checked
  equal). Missing ring member or amount mismatch -> FAIL CLOSED (MISSING_INPUT / CORRUPT).
- All putAo/putKi are fresh: before-absent, after-present (anon output created once;
  key image written once). Disconnect erases. In-block duplicate anon-output key ->
  DUP_OUTPOINT fail-closed.
- Isolated-linkable (no core.h, no wallet); verified in the anonstore harness (T13-T16).
OPEN (still before authoritative read-switch, mandatory conditions #1 and #3): feed
wiring into ConnectBlock/DisconnectBlock + Store3A-consistent atomicity + rebuild/
fail-closed recovery + historical migration of non-empty state + undo; do NOT degrade
these because a probed mainnet datadir (.innova txleveldb, 8.75M keys) had 0 ao / 0 ki.