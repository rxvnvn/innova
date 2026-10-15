# Stage 3A design contract — incremental O(delta) chainstate storage

Status: design record for the isolated Stage-3A slice. Baseline: audited Stage-2B
`feat/chainstate-vertical-slice @ ae6b439a`. No production path is touched.

## 1. Problem with the Stage-2B mechanism (baseline)

Stage-2B persists the full UTXO set per block: `publish` calls `UtxoState::serialize()`
(O(state) bytes), `sha256(stateBytes)` (O(state)), and writes a new immutable
`state-<gen>.dat` (O(state) I/O). `UtxoState::existsUnspentTxid` scans the whole map
(O(state)) for the BIP30 check. So commit, digest, and BIP30 are all O(state). Stage-3A
must make these O(delta) with a bounded, explicit lookup cost.

## 2. Chosen design: copy-on-write paged B+tree with Merkle node hashing

State is stored in a single append-only page file `tree.dat`. Pages are fixed 4096 bytes.
The committed tree is identified by a root page id + root hash carried in the MANIFEST.
A commit mutates only the O(K changed keys x tree depth) nodes on the affected root-to-leaf
paths; every touched node is written as a **new** page (copy-on-write), so the previously
committed tree is never mutated and remains fully readable for fallback.

### Rejected alternatives
- **Append-only delta log + in-memory index + periodic full snapshot (LSM-lite).** Commit
  write volume is O(K), but restart needs either an O(history) log replay or an O(state)
  snapshot read, and the amortized snapshot cost reintroduces O(state) work. Rejected for
  the target multi-million-block node.
- **In-place page mutation + MANIFEST rename.** Rejected: exposes a partially applied block
  (violates the Stage-2B "no mixed generation" rule).
- **Full-state Merkle recompute.** Rejected: O(state) per commit.
- **Append-only log with no index.** Rejected: O(history) lookup/restart.

### Tradeoff accounting (as required)
- **Write amplification:** O(K x depth) pages per commit, depth ~ log_100(N), typically 3-5
  for mainnet scale -> a bounded constant factor, independent of N. No per-commit O(state)
  write. (Depth grows logarithmically, documented.)
- **Read amplification:** one page read per tree level (O(depth)); each page is
  authenticated against its expected Merkle hash before use (R1, §8.1), so verification adds
  one O(page-size) SHA-256 per level and no extra reads. No page cache is used in the slice,
  so RAM is bounded to O(depth) pages per operation.
- **Crash consistency:** new pages are written and fsynced *before* the MANIFEST that
  references them; the rename is the visibility boundary and the directory fsync the
  durability boundary, exactly as Stage-2B. A crash never exposes a partially applied block
  because the committed root only ever points at fully written pages.
- **Generation ownership:** pages written for generation g are referenced only by g's root
  and (until the next commit) by nothing else; the previous generation (g-1) keeps its own
  root in `MANIFEST.bak`. Page reclaim for generations older than {g, g-1} is **OPEN**
  (Stage 3A is append-only; see section 6).
- **Disk growth:** O(commits x K x depth) pages appended over a run (no reuse yet);
  compaction/free-list reclaim is OPEN.
- **Compaction:** not implemented in 3A (OPEN). The design keeps a per-generation root so a
  future free-list/compaction pass can reclaim pages unreachable from {current, previous}
  roots without changing the on-disk node format.
- **Reader isolation:** every generation has an immutable root; a reader pinned to root R
  always sees a consistent tree regardless of newer commits. Generation pinning for
  production readers is deferred to Stage 3B/3C.
- **Recovery cost:** open reads the MANIFEST (O(1)) and trusts the stored root hash as the
  commitment; per-key access is O(depth). No O(state) or O(history) scan on restart. A full
  tree verification (traversal) is available but is O(state) and used only in tests.
- **RAM bounds:** no page cache in 3A -> O(depth) pages held transiently per operation.
- **Migration / versioning:** MANIFEST v1 gains a `rootPageId` + `rootHash`; format version
  stays fail-closed (unknown version refuses to open). A v1 state-file store and a v3A tree
  store are distinguishable by MANIFEST formatVersion.

## 3. Data layout

### Page (`tree.dat`, 4096 bytes)
    [0]      u8   nodeType   1=leaf, 2=internal
    [1..2]   u16  count
    [3]      u8   reserved (0)
    [4..]    payload

Leaf payload: `count` entries sorted by 36-byte key ascending:
    key[36] | u16 coinLen | coin[coinLen]
coin encoding (canonical LE): `value i64 | height i32 | flags u8 | scriptLen u16 | script`.

Internal payload: `count+1` child page ids (u32 each), followed by `count` separator keys
(36 bytes each): `child[0]..child[count] | key[0]..key[count-1]`, where child[i] holds keys
< key[i] and child[count] holds keys >= key[count-1].

### Node hash (Merkle)
    leaf:     SHA256(0x01 || entries...)
    internal: SHA256(0x02 || (childId||childHash)...)
Root hash = hash of the root node. This is the incremental commitment: modifying a key
recomputes only the O(depth) node hashes on its path. Deterministic and restart-stable.

### Collision / security assumptions
The commitment is SHA-256 over a defined canonical encoding (domain-separated by the node
type byte and by the key/coin encodings). Security reduces to SHA-256 collision resistance
and the standard Merkle-tree assumption; no novel cryptographic claim is made. Collision of
two distinct trees at the same root requires a SHA-256 collision.

### MANIFEST additions (v1, extended)
Stage-2B MANIFEST fields are kept; `stateName`/`stateDigest`/`fileHash` are replaced in the
3A store by `rootPageId (u64)` and `rootHash (Hash)`. `undoSize` and the ACK record carry
over unchanged. Unknown versions fail closed.

## 4. Operations and complexity

| Operation | Cost | Notes |
|---|---|---|
| `get(outpoint)` | O(depth) page reads | tree descent |
| `put/erase(outpoint)` | O(depth) node writes (CoW) | path copy |
| BIP30 `existsUnspentTxid(txid)` | O(depth) + O(#outputs of that txid) | prefix lower-bound, NOT O(state) |
| commit (apply K-key delta) | O(K x depth) writes + hashes | no full-set scan |
| digest (root hash) | O(1) after commit | incremental |
| open/recovery | O(1) MANIFEST read + trust root hash | no scan |
| full verification (tests) | O(state) traversal | test-only |

Delete does not rebalance/merge (underflow allowed); correctness is preserved and the tree
stays valid. Merge/rebalance is an OPEN optimization (never O(state) work; a future
rebalance is O(depth)).

## 5. Commit / recovery / ACK state machine (preserved from Stage-2B)

    STAGE pages (write new pages to tree.dat + fsync file)
      -> STAGE undo append (+ per-record CRC, S2B-1)
      -> ESTABLISH MANIFEST.bak (checked; abort if it cannot be established)
      -> [VISIBILITY] rename MANIFEST (root id + root hash + generation)
      -> [DURABILITY] parent-dir fsync
      -> [ACKNOWLEDGMENT] durable ACK
      -> POST (publish root; GC of orphan tmp)

The F01 fail-loud / fail-closed recovery policy is carried verbatim: an acknowledged
generation is never silently rolled back; unclassifiable evidence fails closed; legitimate
unacknowledged recovery still succeeds.

## 6. Explicitly OPEN after Stage 3A

- Page reclaim / compaction / free-list (append-only 3A). Mandatory Stage 3B gate.
- Undo retention bound + reclaim (Stage-2B F03, still OPEN).
- Undo-body integrity is verified per record at access (S2B-1 addressed); a bounded rolling
  verification scan is OPEN.
- Arbitrary-depth reorg / checkpoint/seed / historical rebuild / production reader pinning:
  Stage 3B/3C. Extension points kept: per-generation root, immutable pages, undo prefix in
  MANIFEST.
- Mainnet migration and consensus integration; F08 coinstake time semantics.
- Power-loss durability beyond the fsync contract (process-crash proven only).

## 7. Extension points for 3B/3C (design must not preclude them)

- Multiple retained roots (a small root ring in MANIFEST) enables cheap arbitrary-depth
  reorg within the retained window.
- Immutable pages + a free-list enable online compaction without changing node format.
- Checkpoint/seed = publish a MANIFEST whose root references a fully-materialized tree; the
  same commit path applies.

## 8. Stage 3A-R1 — targeted integrity & recovery remediation

### 8.1 S3A-1 — authenticated traversal (path verification)

Correction of the Stage-3A record: non-root page corruption was **not** detected during
normal access in Stage 3A. `setRoot` verified only the root page (O(1)) and `get`/`modRec`
consumed child pages without checking them against the parent's stored child hash; a bit
flip in a non-root leaf or internal page was therefore returned/spent/rewritten silently.

R1 makes every normal traversal authenticated:

- `readNodeVerified(id, expect, ...)` reads + decodes a page and recomputes its node hash,
  rejecting it unless it equals `expect` (the parent's stored `chashes[i]`; for the root, the
  committed root hash from the MANIFEST).
- `get`, `existsTxidPrefix` (BIP30), `put`, `erase`, `keysWithTxidPrefix` (prefix scan),
  `collectAll` and `verifyAll` all descend through `readNodeVerified`; each page is verified
  **before** its contents are consumed, so a corrupt page fails closed and is never used to
  produce a value or to derive a new authenticated root.
- Cost is still O(depth): one page read + one O(page-size) SHA-256 per level on the traversed
  path. No O(state) work is added per operation.

### 8.2 S3A-3 — interrupted-append recovery (unreferenced trailing page)

Stage 3A refused to open a `tree.dat` whose size was not a multiple of PAGE_SIZE, so an
append interrupted mid-page (short write / ENOSPC / power loss) made an otherwise intact
acknowledged generation unopenable.

Pages are only ever written at whole-page offsets, and a page is fully written + fsynced
**before** any MANIFEST that references it becomes durable (the tree is fsynced before the
rename). Therefore bytes past the last whole page belong to a page that was never completed
and cannot be referenced by the committed root **or** by the retained fallback root.

R1 `BTree::open` drops exactly that footer: if `size % PAGE_SIZE != 0`, it `ftruncate`s to
`size - size % PAGE_SIZE` and fsyncs. Only the partial trailing page is removed — never a
whole page — so committed pages, the fallback (MANIFEST.bak) root and any page a future
generation could reference are preserved. The drop is reported via `truncatedTail()` and is
idempotent across repeated restarts. This is the minimal format-compatible change; no
on-disk format change is required.

### 8.3 Commitment contract (unchanged, restated)

The Merkle `rootHash` is a **physical, history-dependent** commitment over the page tree: it
depends on node layout/allocation and changes on every commit even when the logical UTXO set
does not. It is **not** the canonical logical UTXO digest (`UtxoState::digest()`, an
order-independent digest of the key→coin set). R1 does not change this contract: verification
authenticates pages against the committed physical root; correctness against the logical
model is checked separately by the differential tests.

## 9. Stage 3A-R2 — fresh-open contract (fail-closed)

Closes R1-1 (destructive fresh-create: an existing `tree.dat` was `O_TRUNC`-recreated when
`tree_.open`/`loadPage0AsRoot` failed with MANIFEST+ACK absent) and R1-2 (silent-empty
presentation: an existing committed `tree.dat` was presented as an empty generation-0 store).
Both were introduced/exposed by the R1 fall-open on the MANIFEST-absent, ACK-absent branch.

Rule: **existing on-disk state is authoritative evidence of a prior store.** A new empty store
is created only when the whole relevant on-disk state proves none exists — i.e. `stat(tree.dat)`
returns `ENOENT` and no MANIFEST.bak is present. Never truncate, recreate, delete or quarantine
an existing `tree.dat`; never treat a `stat` error (EACCES/EIO/…) as file absence.

State matrix (MANIFEST absent):

| # | On-disk state | Permitted result |
|---|---|---|
| 1 | no tree.dat, no MANIFEST/ACK/bak | CREATE_NEW |
| 2 | existing valid empty `tree.dat` (single empty-sentinel page, `nextPageId==1`) | OPEN_EXISTING (empty gen-0) |
| 3 | existing non-empty `tree.dat` (`nextPageId>1`) | FAIL_CLOSED |
| 4a | existing corrupt `tree.dat`, whole-page aligned | FAIL_CLOSED |
| 4b | existing `tree.dat` not page-aligned (partial page) | FAIL_CLOSED (no tail truncation on this path) |
| 5 | `tree.dat` unreadable (EACCES/EIO) or not a regular file | FAIL_CLOSED |
| 6 | `tree.dat` genuinely absent | CREATE_NEW |
| 7 | MANIFEST missing, ACK present (any generation) | FAIL_CLOSED (inconsistent pointer evidence) |
| 7b | MANIFEST missing, MANIFEST.bak present | FAIL_CLOSED (prior-store evidence) |
| 8 | MANIFEST present, ACK missing | unchanged: fail closed unless gen 0 |
| 9 | MANIFEST.bak present, primary missing, ACK present | unchanged: RECOVER_ACKNOWLEDGED (checked) |
| 10 | interrupted fresh init (partial/zero-size file) | FAIL_CLOSED (no destruction; operator decides) |
| 11 | interrupted page append with acknowledged root | RECOVER_ACKNOWLEDGED (S3A-3 tail drop, §8.2) |
| 12 | repeated restart after failure | idempotent; files byte-for-byte unchanged |

`BTree::open` may still drop an unreferenced trailing partial page on the acknowledged-recovery
path (§8.2); the no-pointer fresh path rejects non-aligned sizes before opening, so it never
modifies the file.

Crash contract: the only file created on the fresh path is `tree.dat` (one empty sentinel page)
via `O_CREAT|O_TRUNC` at a path proven absent; no MANIFEST/ACK is written before the first
successful publish. A crash between the `stat` and `create` can therefore only leave a fresh
empty store, never destroy prior data (the path was proven absent moments earlier, and any
concurrent writer is excluded by the `LOCK` flock held across the check).

## 10. Stage 3B-F0 — undo offset coherence (physical layout vs committed length)

Defect (pre-F0): undo records were appended with `O_APPEND` (physical EOF) while the store's
offsets (`UndoMeta.offset`, `Manifest.undoSize`) were logical: `disconnect` shrinks
`undoCommittedBytes_` without touching the file. After `disconnect` + `publish`, the new record
landed *past* the stale record, so the durable `undoSize` described a region whose last record was
the stale one. On restart the rebuilt index pointed at the discarded block: the tip could no
longer be disconnected, and a later disconnect could revert a different block's ops.

Fix: the undo record is written at the **committed logical offset** (`csWriteAtFile`), never with
`O_APPEND`, and never by truncating:

- bytes `[0, off)` are left byte-identical ⇒ no manifest (current primary **or** the
  `MANIFEST.bak` fallback) can lose a record region it references;
- only bytes at or beyond the committed logical end are ever rewritten — exactly the records
  logically discarded by an earlier `disconnect`, which no retained manifest references;
- the write is refused if the file is shorter than `off` (no zero-extended hole), and a torn/short
  write leaves only inert bytes beyond the committed end.

Resulting invariants (tested by `F0-undo`): `physical >= logical` always; `[0, undoSize)` is exactly
the committed record sequence; bytes beyond `undoSize` are inert garbage that is never read and may
be overwritten by the next record; a restart reconstructs the same index the live process had.

Deliberately **not** done (belongs to the D1 retention step): reclaiming the inert stale tail by
truncation. Truncation/compaction of `undo.dat` needs the retention water marks, the seed floor and
the whole-image maintenance protocol of DESIGN §9/§10 of the D1 report; the F0 fix is
non-destructive and leaves those bytes in place.

## 11. Stage 3B-F0-R1 — backup barrier ordering and fresh-open evidence

Two MEDIUM findings from the independent F0 audit (`innova-v2-chainstate-stage3b-f0-audit-20261015.md`),
neither of which lost an acknowledged generation, are closed here. No on-disk format changed, no second
state authority was introduced, and no previously recoverable state was made unrecoverable.

### 11.1 F0-1 — a retained backup must never reference an overwritten byte

Vulnerable state (FACT, reproduced): after `disconnect`, the primary describes generation `N` with the
*smaller* committed `undoSize`, while `MANIFEST.bak` is still the pre-disconnect generation `N-1` with the
*larger* `undoSize` (it references the logically-discarded tail). The F0 write at the committed offset then
rewrites bytes inside that referenced range ("vulnerable window"), and only afterwards was the backup
replaced (old order: write → `MANIFEST.tmp` → backup). In a double fault (crash inside the window **and** a
later primary corruption) the fallback could no longer be decoded: recovery failed closed, but with a
*degraded* fallback ("no valid backup") instead of the intended, informative ACK-gate refusal.

Evaluated candidates:

- **A. Reorder within the existing protocol** (chosen). Establish the backup from the *current* primary
  **before** the undo write. The backup then always carries `undoSize == off` — the exact write offset —
  so `[0, undoSize)` is by construction below and disjoint from the written range.
- B. Guard the pwrite against a backup that still references the region. Equivalent in effect to A (the
  guard can only succeed by replacing the backup first), but leaves a check that can never fire on a
  correct store, and a refusal path that would block a legitimate reconnect. Subsumed by A.
- C. Crash-safe preservation without a new format (chosen as a *component* of A): write the replacement to
  `MANIFEST.bak.tmp`, fsync, verify (decode + root reachability), then `rename()` over the live backup —
  never an in-place truncate of the live backup, which would leave a torn backup if the write died.

Rejected outright: deleting or invalidating `MANIFEST.bak`, and relaxing fail-closed behaviour.

Durability barriers (exact order, one publish):

1. page appends; 2. `tree_.fsync()`; 3. read current `MANIFEST` (decode + root reachable, else fail closed
with **zero** mutation) — plus the invariant check `backup.undoSize == off` whenever a write follows, which
fails closed if a foreign/legacy store contradicts it; 4. write `MANIFEST.bak.tmp` (+fsync) and verify it;
5. `rename(MANIFEST.bak.tmp → MANIFEST.bak)`; 6. **directory fsync** — the replacement must be durable
before the undo bytes change, otherwise a power loss could roll the rename back and re-expose the larger
backup after the write; 7. undo write at the committed offset (+fsync); 8. `MANIFEST.tmp` (+fsync);
9. `rename(MANIFEST.tmp → MANIFEST)` (visibility); 10. directory fsync; 11. `ACK` publish.

Consequences per crash state (all tested by `R1-crash`; "OLD" = acknowledged generation `N` survives):

| crash point | primary | backup | reopen | double fault (primary then corrupt) |
|---|---|---|---|---|
| before backup replacement | `N` | `N-1` (stale, larger) | OLD `N`, coherent | FAIL-CLOSED via the ACK gate (the stale backup cannot roll back an acknowledged generation) |
| backup tmps written, not renamed | `N` | `N-1`; stray `.tmp` removed on open | OLD `N` | FAIL-CLOSED (same) |
| after backup replace + dir fsync | `N` | `N` (usable copy) | OLD `N` | **RECOVERS `N`** from the backup |
| after undo write/fsync | `N` | `N` | OLD `N` | **RECOVERS `N`** |
| before / after rename, after dir fsync | `N` / `N+1` | `N` | OLD `N` / NEW `N+1`, ACK reconciled | NEW path: ACK gate protects `N+1` |
| after ACK | `N+1` | `N` | NEW `N+1` | unchanged |

The double-fault column is the measurable improvement: the F0-1 window no longer converts a recoverable
state into a fail-closed one, and the informative ACK-gate refusal survives where rollback is genuinely
forbidden. Cost: one extra directory fsync per publish (O(1), no page/byte change; measured in the R1
report).

### 11.2 F0-2 — an orphan `undo.dat` is prior-store evidence

`undo.dat` was never classified at open, so a fresh store adopted a directory containing an orphan log and
the first publish overwrote its leading bytes at offset 0. The fresh branch (no `MANIFEST`, no `ACK`, no
`MANIFEST.bak`) now classifies `undo.dat` with `lstat` **before** any creation, and only then creates:

| observed | decision |
|---|---|
| absent | fresh init proceeds; the first write uses `O_CREAT\|O_EXCL` |
| regular, size 0 | adopted as an accepted residue (0 bytes can be destroyed); the first write must find it (`O_WRONLY`, no `O_CREAT`) |
| regular, size > 0 | **refuse** — bytes without pointers are prior-store evidence |
| not a regular file (directory, symlink, fifo, device) | **refuse** (never followed; `lstat`, not `stat`) |
| stat error other than `ENOENT` | **refuse** (a stat failure is never read as absence) |
| regular but not readable+writable | **refuse** |

Refusal is pure: nothing is created, moved, deleted or quarantined, and the diagnostic is deterministic
(the same message and the same bytes on every restart). The `O_EXCL`/no-`O_CREAT` policies close the
classify→create TOCTOU window at the kernel level, so a file appearing (or vanishing) between
classification and the first write fails closed instead of being overwritten (or invented).

Empty-only is accepted because it is exactly the constructible-empty form, matching the R2 contract's
acceptance of an empty sentinel page (`DESIGN §9`); every non-empty residue is refused, which is stricter
than R2's tree rule and consistent with pre-existing refusals that inspect `MANIFEST.bak`, `ACK`, page
alignment and root emptiness first. The same `lstat`+regular-file check is applied when loading the log of
an acknowledged store, closing the LOW finding "non-regular `undo.dat` rejected only at first publish".
`tree.dat` classification is deliberately unchanged (it would alter an already-audited R2 contract) and is
recorded as a residual LOW in the R1 report.

Deliberately **not** done (unchanged scope): no `UNDOINDEX`, no MANIFEST v2, no generation pinning, no
checkpoint seeds, no retention pruning, no compaction/GC, no page reclamation, no consensus/DAG change.
