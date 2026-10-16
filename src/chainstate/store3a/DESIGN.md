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

## 12. Stage 3B-F0-R2 — descriptor pinning of the adopted empty residue (TOCTOU closure)

F0-2-R1 (residual MEDIUM): the accepted **empty** `undo.dat` was identified only by a pathname record.
A substitution performed between the fresh-open classification and the first undo write was therefore
invisible to the R1 `RequireExisting` writer, which re-resolved the pathname with a bare `O_WRONLY`:

* E24 — the empty file was replaced by a nonempty one; the first publish succeeded and overwrote the
  substitute's leading bytes;
* E25a — the empty file was replaced by a symlink; the first publish followed it and overwrote the
  link target's bytes.

Both are confirmed pre-fix failures (report `innova-v2-chainstate-stage3b-f0-r2-20261015.md`, Phase B),
including a live concurrent hammer that observed 76 substituted-file overwrites and a corrupted link
target while the store kept reporting successful publishes.

**Contract (implemented).** The accepted empty residue is pinned by an **open descriptor**, and the first
record is written through that descriptor. No pathname lookup ever chooses the write target again.

| step | mechanism | property |
|---|---|---|
| classification | `lstat` (unchanged R1 semantics: absent / empty / nonempty / non-regular / stat error) | policy decision |
| pin | `open(O_RDWR\|O_NOFOLLOW\|O_NONBLOCK)` + `fstat`: regular, `st_size==0`, `st_nlink>=1` | the classified object is captured on the descriptor itself; the `lstat` result is advisory |
| first write | `fstat` (regular, `st_nlink>=1`, still empty) | an unlinked original (`st_nlink==0`) is refused even if the filesystem reuses its inode number |
| before write | `lstat(path)` — regular **and** same `st_dev`/`st_ino` as the descriptor | the name must denote the object being written |
| write | `pwrite` on the pinned descriptor | the target is fixed by the descriptor: no window between validation and write |
| after `fsync` | `lstat(path)` — regular and same `st_dev`/`st_ino` | durability binding: the bytes that became durable are reachable under the committed name |

Absent residue (`RequireAbsent`) keeps the kernel-atomic `O_CREAT|O_EXCL` create and adds the same
post-`fsync` pathname binding. `UndoCreate::Any` (established store) is byte-for-byte the R1 path: no
extra syscalls, no new failure mode.

Why a fingerprint (dev/ino/ctime) is *not* sufficient: this platform reuses an inode **number** after
`unlink`+`create` at the same pathname (observed: `st_ino` identical for the original and the substitute),
and its `st_ctim` granularity can alias within the same millisecond (observed: identical `ctime` for the
original and the recreated file). An in-process fingerprint therefore both false-accepts a substitute and
cannot be repaired by widening the tuple; holding the descriptor keeps the original inode allocated, so a
substitute cannot reacquire its `dev`/`ino` at all, and `st_nlink==0` catches the unlinked case outright.

Ownership and cost: the descriptor is opened only for an accepted empty residue, owned by the store, and
closed by its first writer or by `close()` (including every `open()` error path). One descriptor and at
most three extra metadata syscalls per fresh-open store; `O(delta)` commit cost is unchanged.

Deliberately **not** done (unchanged scope): no on-disk format change, no MANIFEST/ACK protocol change, no
change to the backup-before-undo ordering, no new storage authority or file type, no retention/compaction/
GC, no generation pinning, no checkpoint seeds, no consensus/DAG change, no `tree.dat` classification
change (still recorded as a residual LOW), and no identity pin on the established-store (`Any`) path.

## 13. Stage 3B-F0-R3 — first-publication failure lifecycle (one-shot policy) and the post-`lstat` window

### 13.1 The publication path, its failure classes, and their durability consequences

```
 FRESH_OPEN  (classification: absent | accepted empty residue | refuse)
     |        the fresh-open undo policy is ARMED here (RequireAbsent or RequireExisting + pinned fd)
     v
 FIRST_PUBLISH_STARTED        guard: opened_ && !needsReopen_   (every public mutation entry point)
     |
     | (A) tree pages staged; tree.fsync                        failure: nothing authoritative changed,
     |                                                            one-shot INTACT -> retry stays safe
     | (A2) backup barrier (MANIFEST.bak := current primary, atomic + dir fsync)
     v
 UNDO_WRITE  <-- THE ONE-SHOT IS SPENT HERE (policy != Any)
     |        (B) failure here, or anything below: the policy/pin is gone
     |
     | (C) MANIFEST.tmp write / fsync
     | (D) MANIFEST rename -> renamePassed_ = true
     | (E) directory fsync
     v
 UNDO_NAME_RECHECK   pathname->inode binding, log fd still held  (R3; F0-2-R2-2)
     |                                                          failure: not acknowledged, reopen required
     v
 ACK -> ESTABLISHED   (a successful first publication ends the one-shot; later failures behave as before)
```

| class | example | on-disk consequence of the failure | retry on the same instance |
|---|---|---|---|
| A | `tree.fsync` fails | staged unreferenced pages only | **allowed** (one-shot intact, pin still held) |
| A2 | backup replacement fails | backup either old or new, never partial | allowed |
| B | torn/partial undo write, undo fsync fails | partial or unfsynced log bytes | **refused** (one-shot spent) |
| C | `MANIFEST.tmp` write/fsync fails | log durable, no new MANIFEST | **refused** (one-shot spent) |
| D | rename fails | ditto | refused (pre-R3 rule: `renamePassed_`) |
| E | dir fsync fails | MANIFEST visible, not durable | refused (pre-R3 rule) |
| F | name binding broken | MANIFEST durable, log unreachable by name | refused (post-rename rule) |
| G | ACK write fails | generation published, not acknowledged | refused (pre-R3 rule) |

### 13.2 R3 fix — the fresh-open policy is one-shot, and failing it closes the instance (F0-2-R2-1)

R2 pinned the accepted empty residue but left the *consumption* of that policy invisible: a first
publication that failed with `renamePassed_ == false` stayed retryable, and the retry then reverted to
`UndoCreate::Any` — reopening `undo.dat` by pathname with no `O_NOFOLLOW`, no `O_EXCL` and no identity
check, i.e. exactly the substitution TOCTOU the pin had just closed (audited E-A: a 41-byte substitute
grew to 114 bytes; the same shape as E24/E25a, and measured again pre-fix in R3-01/R3-04).

Rule: **when a publication enters the undo write phase it spends the one-shot; if that publication does
not complete, the instance becomes `REOPEN_REQUIRED`** (`freshOneShotTaken_` -> `requireReopen(reason)`),
and every later `publish()`/`disconnect()` is refused with that reason *before* touching any persistent
state. Exit is only through `close()`/`open()`, and `open()` reclassifies the actual filesystem evidence
(it never trusts the closed instance's in-memory descriptor state): an unacknowledged or partial residue is
refused outright.

Why this activation point and not "any publish failure": a failure that happens *before* the undo write
leaves the policy and the pin intact, and a retry then re-enters the write with the same pin — safe, and
required for progress (R3-06a commits on retry; R3-06b proves the surviving pin still refuses a substitute
planted before that retry). "Any failure" would have been simpler, but it would also have made an ordinary
pre-undo I/O error fatal, and it would have hidden whether the pin really survives that class. The rule is
therefore keyed on the one-shot, which is exactly the resource whose loss makes a retry unsafe.

Established-store semantics are deliberately **unchanged** (C11): once a first publication succeeds, later
publish failures keep their pre-R3 behaviour (`Any` path, pre-rename failures retryable) — R3-21 pins that
down by retrying and committing gen2 after an injected pre-rename failure. No format, protocol, ordering or
`O(delta)` change; the new state costs one bool and one string.

### 13.3 F0-2-R2-2 — the post-`lstat` publication window: narrowed, NOT closed (OPEN)

Window (source): the last write-time binding check is inside `csWriteAtFile` (after the log `fsync`, before
the descriptor is released); between it and the ACK write sit the `MANIFEST.tmp` write, the rename and the
directory fsync, and the ACK is written in `publish()`/`disconnect()` after `commitBarriers` returns. A
substitution inside that interval leaves `MANIFEST(gen N)` and `ACK(gen N)` durable while the committed
name no longer denotes the log: the publish *reports success*, and the next `open()` refuses the generation
(`loadUndoFor` fails inside `tryLoadManifest` -> "primary corrupt and no valid backup"). Measured
pre-fix in this harness (R3-37a/R3-37b, 2 variants) and independently by the R2 auditor at 3 pause points x
6 substitution variants.

Threat model (explicit): the actor must write into the datadir while the store is publishing. The `LOCK`
file excludes a second *store instance* (R3-38 measures that a second instance is refused) but it is not a
file-integrity barrier: a non-cooperating process can replace `undo.dat` at any time (R3-38 measures that
the substitution succeeds while the store holds `LOCK`). The model assumed by this design is an
**exclusive datadir** owned by one store instance, with no non-cooperating writer -- the same assumption the
Stage-2B/3A protocol already makes for `MANIFEST`, `ACK` and `tree.dat`.

R3 adds one barrier: immediately before the ACK (after the rename and the dir fsync) the log descriptor is
still held open, so `fstat(fd)` (regular, `st_nlink>=1`) can be compared against `lstat(undo.dat)`
(regular, same `dev`/`ino`). Holding the descriptor is what makes the comparison meaningful on this
platform, which reuses inode numbers after `unlink`+`create`: while a descriptor refers to the inode, the
inode cannot be freed, so a `dev`/`ino` difference is real evidence of substitution rather than aliasing.
Effect: the interval that can produce a *silently acknowledged* unreachable generation shrinks from the
write-time check down to the single ACK write, and the false-success case is removed (measured: post-fix
R3-37a -> publish FAILS, no ACK written; pre-fix the same sequence reports SUCCESS with `ACK(gen1)`).

What this does **not** do, and why the finding stays OPEN: a substitution after that check (R3-37b: the
pause is moved after the comparison) still yields a reported success, an ACK, and an unrecoverable
generation. No name-based check can close an interval that ends with the name-based ACK write itself, so a
second `lstat` is explicitly not claimed to eliminate the race. Closing it would require an
at-open/descriptor-pinned publication path or directory-level locking, which is out of scope for R3
(no new publication architecture, no directory locking, no cross-platform filesystem redesign). Operational
assumption retained: exclusive datadir; if that is violated, the failure mode is fail-closed (refusal),
never silent corruption of unrelated bytes; the acknowledged generation may become unrecoverable and
requires operator intervention.

### 13.4 R3 test surface (focused, deterministic)

`F0-R3-retry` (R3-01..R3-18, R3-33, R3-34), `F0-R3-reopen` (R3-19..R3-32, R3-35, R3-36),
`F0-R3-postlstat` (R3-37a/R3-37b, R3-38, R3-39); each asserts sizes+hashes of every artifact before/after
(`r3Snap`) so "refused" always means "no byte was written". Failure classes are injected one point at a
time (`r3Inject`), the post-`lstat` window is exercised with a bounded pause hook (`setPause`, test-only,
default inert) so the substitution is deterministic rather than probabilistic, and the FIFO case is bounded
by `fork` + `alarm`. `STORE3A_BASE` (test-only env var, default unchanged) isolates the runtime dir per
stage. The control build for the pre-fix evidence is the R2 (`aec10e0c`) `incstore.{h,cpp}` compiled with
the same tests plus the pause hook, so pre/post differ only by the fix.

Deliberately **not** done in R3 (unchanged scope): no retention/compaction/GC or D1 work, no generation
pinning, no checkpoint seeds, no `tree.dat` classification change, no format/protocol change, no
directory-level locking, no change to the established-store (`Any`) write path, no new production
mutation entry point.

## 14. Stage 3B-D1-S1.1 — durable segmented UNDOINDEX (format, publication ordering, crash states)

Slice scope: introduce the derived, rebuildable on-disk undo index. This slice does NOT change undo
lookup, disconnect/reconnect semantics, the commit protocol, MANIFEST/ACK authority, or any consumer:
`std::vector<UndoMeta>` + `loadUndoFor()` remain the authoritative in-RAM view and the only thing
consumers read, and `MANIFEST.undoSize` remains the sole authority for the committed undo length. The
index is written, validated and classified — it is NOT yet consumed as a lookup accelerator (a later
slice must earn that).

### 14.1 Format (v1, little-endian)

  header (48 B)
    0   magic[8]        'I','N','N','U','I','D','X','1'
    8   formatVersion   u32 = 1
    12  segmentSize     u32 (records per segment; production default 4096)
    16  committedBytes  u64 (end offset of the last CLOSED segment described; 0 if none)
    24  baseOffset      u64 (0 in this slice; reserved for the bounded retention window)
    32  baseGeneration  u64 (0 in this slice; see OPEN-1)
    40  segmentCount    u32
    44  crc32           u32 over bytes [0,44)
  segment descriptor, 24 B each (repeated segmentCount times)
    +0  startOffset     u64
    +8  endOffset       u64
    +16 recordCount     u32 (== segmentSize for every closed segment)
    +20 crc32           u32 over the descriptor's preceding 20 bytes

Structural invariants enforced on decode: length == 48 + 24*segmentCount; segmentSize != 0;
segments[i].startOffset == (i==0 ? baseOffset : segments[i-1].endOffset); startOffset < endOffset;
1 <= recordCount <= segmentSize; committedBytes == (segments.empty() ? baseOffset : last.endOffset).
The file is published atomically (`UNDOINDEX.tmp` + fsync + rename); the `.tmp` residue name is already
covered by the existing `cleanupTmp()` sweep.

### 14.2 What a segment is, and when the index is published

A segment is a run of exactly `segmentSize` consecutive committed undo records (production 4096). Only
*closed* (complete) segments are described; the active, not-yet-full tail is deliberately absent and is
resolved from the committed stream (bounded by one segment). The index is therefore always a *prefix
description*, never a partial-record description: committedBytes <= MANIFEST.undoSize and the undescribed
remainder is less than one full segment.

Publication happens in exactly two places:
  (a) `commitBarriers()`, immediately AFTER the durable undo write and BEFORE `MANIFEST.tmp` is written,
      and only when this append closed a segment: one small file write + one fsync + one rename per
      `segmentSize` records. The directory fsync of the existing MANIFEST publication covers this rename
      (same directory), so the publish path adds NO extra directory barrier.
  (b) `open()`, as a normalisation (rebuild + publish + directory fsync) when the on-disk index is
      absent, falls short of, or reaches beyond the committed range.

### 14.3 Reconciliation of UNDOINDEX publication with MANIFEST publication

The index is published strictly INSIDE the existing commit window — after the undo record that closes the
segment is durable, before the MANIFEST rename. No new authority and no new barrier is introduced: the
index describes only records already covered by the durability of the undo write, and `MANIFEST.undoSize`
remains the only thing that decides what is committed. Because the index is derived and non-authoritative,
the accepted crash protocol supports this ordering without modification (no STOP condition was reached).

Five on-disk states are distinguished; the classification is logged at open and readable through
`undoIndexState()`:

  state              arises from                                  S1.1 response
  -----------------  --------------------------------------------  --------------------------------------
  absent             no UNDOINDEX (new store, operator deletion)  rebuild from the committed stream;
                                                                   publish ONLY if >= 1 closed segment
                                                                   exists (a store with no closed
                                                                   segment never gains an index file)
  current            index == the closed-segment prefix of the     accepted; no write at all
                     committed range
  ahead (legitimate) crash between the index rename and the       NOT corruption: classified Ahead, the
                     MANIFEST rename; or a disconnect that         uncommitted tail is dropped, the index
                     shrank undoSize below a boundary              is rewritten from the committed stream.
                                                                   Uncommitted records never become
                                                                   authoritative
  stale              the committed range crossed a segment         NOT silently accepted: classified
                     boundary whose publication was lost          Stale and rebuilt from the committed
                     (power-loss metadata reordering: the          stream (MANIFEST/undo.dat only, so no
                     MANIFEST rename became durable while the      history is replaced)
                     index rename did not)
  corrupt            magic/version/crc/structure/contiguity/       FAIL CLOSED at open with a diagnostic
                     length damage                                 naming UNDOINDEX; nothing written,
                                                                   nothing rebuilt, operator decides

  - "Ahead" must not be mistaken for corruption, and the crash tests pin it: a crash at BeforeRename in
    the commit that closes a segment leaves the index describing the 4th record while the MANIFEST still
    commits 3 — the reopened store commits 3 records and the index is normalised.
  - A changed `segmentSize` is a rebuild trigger (Stale), never corruption: the header records the
    segment size the index was built with, and the index is derived metadata whose layout may evolve.
  - Since the index carries no authority, a corrupt index can only refuse a startup — never change what
    is committed. That fail-closed choice is deliberate and is why the slice must be reviewed before the
    index is consumed.

### 14.4 Cost model (measured in the S1.1 focused suite)

  - bytes per publication = 48 + 24*segments; publications = floor(records / segmentSize)
  - measured (test segmentSize 4): 84-252 bytes per publication, exactly 1 fsync per publication, 0 extra
    directory fsyncs, measured publication latency 33-42 ms — the same as the freshly measured cost of a
    single 72-byte write+fsync on this filesystem (40.3 ms): the publication is exactly one barrier.
  - extrapolation (INFERENCE, production segmentSize 4096): 1953 publications over 8,000,000 records; the
    last index is ~45.8 KB; total written ~43.8 MB over the whole chain (~5.7 bytes/record). A
    full-index rewrite *per block* would be ~1000x larger and is NOT implemented — the rewrite happens
    once per 4096 records.
  - construction/serialisation memory: the encoded buffer (48 + 24*segments bytes) plus the in-RAM
    segment vector; measured peak-RSS delta in the focused suite < 1 MB.

### 14.5 Explicitly NOT in this slice

  - The index is NOT consumed: undo lookup, disconnect and restart behaviour are unchanged, asserted by
    byte-identical undo.dat/tree.dat/MANIFEST/ACK against an index-disabled control run.
  - No pruning, retention floor, compaction or PRUNED_BELOW_WINDOW behaviour.
  - No change to the MANIFEST format/version, the undo record format, the ACK protocol, the consensus
    surface or the archive preservation logic.

### 14.6 Open items

  - OPEN-1: per-segment *generation* bounds are not representable from today's on-disk sources (the undo
    record carries no generation; the MANIFEST carries only the tip generation). Validation therefore
    covers byte boundaries, counts and ordering against the authoritative committed stream; generation
    ordering is deferred to the slice that can recover it (versioned undo record or a persisted
    generation at append time).
  - OPEN-2: an UNDOINDEX present without MANIFEST/ACK on the fresh-open path is neither trusted nor
    deleted by this slice (the fresh path never touches the index; the next publication of a real store
    replaces it). Treating it as prior-store evidence is left to review.
  - OPEN-3: the full rewrite grows linearly in the number of segments (total ~44 MB at 8M records) versus
    a per-segment side file with O(1) publication cost. Documented for review; the single-artifact form
    was chosen for this slice.

## 15. Stage 3B-D1-S1.1-F1 — corrupt-index recovery (S1.1-1) and publication-barrier faults (S1.1-2)

Corrective slice over the accepted S1.1 commit `82d42ed9`. Nothing in §14 changes: the index is still
derived, still non-authoritative, still published only inside the commit window, and
`MANIFEST.undoSize` is still the sole authority. F1 only defines (a) what an OPERATOR may do when the
derived artifact is corrupt, and (b) how the new barrier is fault-injected for testing.

### 15.1 Policy: fail closed by default, recovery only on explicit authorization

`open()` is exactly `openWithOptions(dir, OpenOptions(), e)`, i.e. the accepted S1.1 behaviour, unchanged:
a structurally corrupt `UNDOINDEX` refuses the open and nothing is written, quarantined or rebuilt.

Recovery requires the caller to pass an explicit, one-shot authorization:

    cs::store3a::OpenOptions o; o.authorizeUndoIndexRecovery = true;
    CommitStore3A s; ChainError e;
    bool ok = s.openWithOptions(dir, o, &e);          // operator-authorized recovery
    const UndoIndexRecoveryReport& r = s.lastUndoIndexRecovery();

There is no datadir flag, no environment variable, no RPC, no CLI option and no production wiring; the
default value of the flag is `false` and only a caller that deliberately sets it can trigger recovery.

What an authorized open does when the artifact is structurally corrupt (read successfully, then failed
validation):

1. preserves the corrupt artifact under a collision-safe quarantine name (§15.3) — the evidence is kept,
   never deleted, never overwritten;
2. rebuilds the index EXCLUSIVELY from the committed state that `tryLoadManifest()` + `loadUndoFor()` have
   just validated in the same open (the existing Store3A recovery path, including its primary/backup
   MANIFEST logic) — the corrupt artifact is never read for content;
3. writes the rebuilt index through the normal atomic publication path;
4. never touches `MANIFEST`, `ACK`, `tree.dat` or committed `undo.dat`. Rebuilding an index therefore
   cannot change what is committed or which generation is authoritative.

Failures inside recovery stay fail-closed: if the quarantine or the rebuild publication fails, the open
fails, the committed state is untouched, and a later clean attempt completes the recovery. An I/O error
*reading* the artifact is classified differently from corruption (`indexReadError`) and is never
quarantined — a datadir-level read problem is not a corrupt artifact.

The authorization is one-shot per call and idempotent across interruptions and repetitions:

* repeat on a healthy index -> `resolution = "already-current"`, no quarantine, no write;
* the artifact was already quarantined and the name is free -> `resolution = "rebuilt"` (the ordinary
  absent path);
* interrupted between quarantine and rebuild -> the next attempt rebuilds from the committed stream;
* nothing left to index (no closed segment) -> `rebuilt` with `nothingToIndex = true` (no index file is
  created; that is the correct rebuild for a store with no closed segment).

### 15.2 Quarantine semantics

Name: `UNDOINDEX.corrupt.g<manifestGeneration>` and, on collision, `.1`, `.2`, ... up to 999.

* Created with `link()` + `unlink()`: atomic, fails with `EEXIST` when the name is taken, so an existing
  quarantine artifact can never be overwritten or lost. There is deliberately **no rename fallback**: on Linux
  `rename()` atomically REPLACES an existing destination and does not report `EEXIST`, so a check-then-rename
  could destroy evidence. A hard link is the only move primitive here; if `link()` fails for any reason other
  than a taken name (for example `EPERM`/`EMLINK` where hard links are denied, or another filesystem error) the
  quarantine **fails closed**: the corrupt artifact stays exactly where it is, existing quarantine artifacts are
  untouched, nothing is rebuilt or published, and the diagnostic carries the errno and tells the operator to
  preserve/move the corrupt artifact manually before retrying.
* The move is `link()` then `unlink()` of the source, which is not one atomic step. If the `unlink()` fails, the
  open fails closed and the diagnostic states that the corrupt artifact is STILL IN PLACE and that a hard link to
  it may remain at the candidate name. An unlink failure must never be treated as success: the rebuild would then
  publish over the still-present corrupt artifact, destroying the very evidence the quarantine had just saved.
* The path, byte count and whether the fallback was used are reported in `UndoIndexRecoveryReport` and
  printed, together with the operator request and the rebuilding result, so the action is never silent.
* Quarantined artifacts are never reused, pruned or cleaned up by the store: they are diagnostic evidence
  for the operator. They are also outside the `*.tmp` residue sweep.

### 15.3 Updated state table for the corrupt row

| situation | default (no authorization) | with `authorizeUndoIndexRecovery` |
|---|---|---|
| artifact read fails (I/O) | fail closed, nothing quarantined | fail closed, nothing quarantined (`indexReadError`) |
| artifact read, validation fails | fail closed (`resolution="refused"`) | quarantine + rebuild from committed state |
| artifact healthy | normal S1.1 classification | normal classification + `resolution="already-current"` |
| absent / stale / ahead | normal S1.1 paths (unchanged) | identical (no change; they never needed authorization) |

### 15.4 Fault-injection contract (test-only, finding S1.1-2)

`injectUndoIndexFault(UndoIndexFault)` arms exactly one deterministic, one-shot fault at the publication
barrier; `clearUndoIndexFaults()` resets it; `undoIndexFaultPending()` shows whether one is armed. The
default is `None`, so no injection exists in a production run, and the fault is consumed by the first
publication it affects (a second publication proceeds normally). No successful production ordering,
MANIFEST/ACK authority or publication condition changed.

Covered faults and where each is placed:

| fault | placement | shape it reproduces |
|---|---|---|
| `TmpCreate` | before writing the tmp | the tmp open/write cannot be created |
| `TmpShortWrite` | writes `n-1` bytes of the encoded index into the tmp, then fails | a short/partial write (a truncated tmp is left) |
| `TmpFsync` | after the tmp write, at the fsync | temporary-file fsync failure (the tmp survives) |
| `Rename` | at the rename | index rename failure (the tmp is removed, as the real path does) |
| `DirFsync` | after the normalisation publication's `rename`, at the directory fsync | directory fsync failure during an ahead/stale normalisation |

Two distinct evidence classes are reported separately in the F1 report:

* **synthetic one-shot faults** — the control-flow consequences above, deterministic and precisely placed;
* **real syscall failures** — no injection involved: `UNDOINDEX.tmp` symlinked to `/dev/full` (a genuine
  `ENOSPC` on write) and `UNDOINDEX.tmp` replaced by a directory (a genuine `EISDIR` at creation). These
  prove the same failure paths are reachable with the kernel actually refusing the call.

In every case the committed protocol is untouched: the failed commit does not become a generation, and
MANIFEST/ACK/logical digest/`undoCount`/the committed `undo.dat` and `tree.dat` PREFIXES are identical to
an index-disabled control (the physical tails may differ — an uncommitted undo record or unreferenced CoW
pages — which is the accepted semantics of every other barrier in this store).

### 15.5 Open items added by F1

* OPEN-F1-1 (**CLOSED** in S1.1-F2): the `lstat`-guarded rename fallback was deleted. Hard links are now the
  only move primitive and every failure other than a taken name fails closed (see §15.2), so no
  check-then-act path can overwrite quarantine evidence. `UndoIndexRecoveryReport.quarantineFallback` was
  removed with it (it could no longer be true).
* OPEN-F1-2: if the index path is a directory (not a file), the artifact is refused as unreadable rather
  than quarantined; such a datadir is broken independently of the store and no index recovery is attempted.

## 16. Stage 3B-D1-S1.2 — consuming the derived index at startup (fast startup)

S1.2 loads `undoIndex_` (the in-RAM `std::vector<UndoMeta>`) through the derived `UNDOINDEX` when that
artifact is CURRENT for the committed state, instead of always issuing one read per record. It is a pure
optimisation layer over the accepted S1.1/F2 machinery: nothing about authority, publication, recovery or
classification changes, and no failure mode is masked.

### 16.1 What the index can and cannot establish (the integrity question)

FACT, from the format in §14 and the record layout in `undo.dat`:

* a segment descriptor is `{startOffset, endOffset, recordCount, crc32}` and its `crc32` covers THOSE
  FIELDS ONLY — never the undo bytes it points at;
* `UndoMeta` needs per-record `offset`, `len`, `blockHash` and `height`, which live in the record headers
  inside `undo.dat` and nowhere else;
* record headers are NOT contiguous — records are variable length, so the header of record *k* is only
  reachable by walking from the start of its segment.

Therefore a valid, CURRENT index proves the BYTE BOUNDARIES of the closed segments and nothing about the
records inside them, and **no validation-free startup is possible with this format**: per-record metadata
cannot be produced without reading record bytes. What the index CAN safely buy is to replace one syscall
per record with one syscall per closed segment. That is what S1.2 does, and the report quantifies it
honestly (fewer read syscalls, MORE bytes, the per-record parse unchanged).

### 16.2 When the index is consumed

`loadUndoFor()` first asks `tryLoadCurrentUndoIndex()`: the artifact must exist, decode
(magic/version/CRC/length/bounds as in §14), carry the configured `segmentSize`, have `baseOffset == 0`,
have descriptors contiguous from offset 0 with `recordCount == segmentSize` and
`committedBytes == last.endOffset <= MANIFEST.undoSize`. Only then is it consumed, and only after
`undo.dat` passes the existing regular-file guards. Anything else — absent, invalid, ahead, different
segment size, disabled for a control run — leaves `fastPath = 0` and the authoritative record scan runs,
with the S1.1 classification/normalisation pass afterwards repairing or refusing the artifact exactly as
before. `MANIFEST.undoSize` remains the sole authority: the fast path never reads the index for it.

### 16.3 The fast path, and why a decline is always safe

For each closed descriptor the loader issues ONE bounded read of the segment's byte span (a span above
`UNDOINDEX_MAX_BULK_SPAN`, 4 MiB, falls back to per-record header reads so the transient buffer stays
bounded), then re-parses every record in that buffer with the SAME validations as the scan (magic, length
range, bounds) plus two cross-checks the scan cannot make:

* the parse must land EXACTLY on the descriptor's `endOffset` (no slack, no overrun);
* it must yield EXACTLY `recordCount` records.

The uncovered tail after the final descriptor is then parsed with per-record header reads, and it must be
strictly shorter than one closed segment: a tail that reaches a full segment means the index missed a
closure (STALE) and the fast path declines instead of consuming it.

On ANY anomaly — mismatch, damaged data, I/O error, stale shape — the loader discards its partial result,
records the reason in `UndoLoadStats::fallback`, clears the error and runs the authoritative scan, whose
outcome is then exactly the pre-S1.2 outcome (same error codes, same classification, same rebuild or
fail-closed refusal). Because a declined index is still inspected by the S1.1 classifier, a *wrong but
well-formed* index is repaired through the normal Stale/Ahead normalisation rather than silently accepted.
Nothing is ever accepted on trust.

Kinds of damage, and where each is detected (unchanged from S1.1, stated because the mission asks):

| damage | detected by | when |
|---|---|---|
| index artifact structurally invalid | `UndoIndex::decode` | open (fail closed; authorised recovery available) |
| index boundaries disagree with the stream | the fast path's exact-landing/recordCount cross-checks, then classification | open (decline + rebuild) |
| `undo.dat` record header damaged (magic/length) | both the fast path and the scan | open (fail closed, same error in both modes) |
| `undo.dat` record PAYLOAD damaged | `UndoRecord::decode`'s trailing crc32 | when the record is actually decoded (disconnect/rollback) — identical in both modes |

### 16.4 Counters and cost model

`undoLoadStats()` reports the startup profile of the last open: `fastPath`, `fallback`, `descriptors`,
`segmentReads`, `headerReads`, `records`, `bytesRead`. Diagnostics only — they never influence behaviour.
Measured numbers (records scanned, reads, bytes, time, RSS, and the comparison against the scan path at
the same state) are in the S1.2 report; the model is:

* read syscalls: `R` (scan) -> `#closedSegments + tailRecords` (fast);
* bytes: `R x 52` (scan, headers only) -> `sum(segment spans) + tail x 52` (fast, headers + payloads);
* parse work: `R` records in both cases — this is the part that the format cannot remove while the full
  in-RAM `UndoMeta` vector is kept (see §16.5).

### 16.5 What a genuinely O(#segments) startup still needs (S1.3+)

Not available in this slice, by construction:

1. the in-RAM representation is a full `std::vector<UndoMeta>` (its replacement by a segment map is
   explicitly S1.3), so every record must still be parsed to fill it; and
2. the index carries no per-record metadata, so the parse cannot be avoided by reading the index instead.

A true fast startup therefore needs either the S1.3 representation change (start from the index alone and
read individual records on demand, with the existing fail-closed per-record decode) or a format addition
that lets the committed stream be validated without reading it (for example a per-segment CRC over the
segment's BYTES, so "the indexed range is intact" becomes checkable from metadata). Until then the honest
statement is: S1.2 reduces startup READ SYSCALLS by replacing per-record reads with per-segment reads, at
the cost of reading payload bytes it previously skipped — it does not make startup O(#segments).
