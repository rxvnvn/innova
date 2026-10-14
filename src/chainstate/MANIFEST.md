# MANIFEST specification v1 (isolated chainstate vertical slice) — Stage 2B

Status: prototype spec for the isolated slice. Versioned, fail-closed. All multi-byte
integers are little-endian. Stage 2B adds the durable ACK record and a fail-loud fallback
policy (F01) without changing the MANIFEST v1 layout.

## Purpose

The MANIFEST is the **single commit pointer** for one chainstate directory. It selects
exactly one immutable state file and one committed prefix of the append-only undo log.
Its atomic rename into place is the *visibility* boundary; the subsequent parent-directory
`fsync` is the *durability* boundary; a separate durable ACK record is the *acknowledgment*
boundary.

## Files in a chainstate directory

| File | Role |
|---|---|
| `MANIFEST` | current committed pointer (this spec) |
| `MANIFEST.bak` | previous committed pointer, durably established before each publish |
| `ACK` | durable acknowledgment record: highest generation whose publish completed |
| `state-<gen>.dat` | immutable CoW UTXO snapshot for generation `<gen>` |
| `undo.dat` | append-only ordered undo records |
| `LOCK` | advisory single-writer lock (`flock LOCK_EX|LOCK_NB`) |
| `MANIFEST.tmp`, `state-<gen>.dat.tmp`, `ACK.tmp` | staging (never authoritative) |

## MANIFEST v1 layout (total = 140 + L bytes)

| Off | Size | Field |
|---|---|---|
| 0 | 8 | magic `"INNCSMNT"` |
| 8 | 4 | `formatVersion` = 1 |
| 12 | 4 | `manifestSize` (= total bytes, incl. CRC) |
| 16 | 8 | `generation` (monotonic; 0 = not published) |
| 24 | 8 | `undoSize` (committed byte length of `undo.dat`) |
| 32 | 4 | `stateNameLen` = L |
| 36 | L | `stateName` (validated: `state-<digits>.dat`) |
| 36+L | 32 | `stateDigest` — canonical UTXO-set digest (semantic equality) |
| 68+L | 32 | `fileHash` — SHA-256 of the serialized state file (integrity) |
| 100+L | 32 | `blockHash` — tip block |
| 132+L | 4 | `tipHeight` (int32) |
| 136+L | 4 | `crc32` over bytes `[0, 136+L)` |

Two distinct hashes are deliberate: `fileHash` detects byte corruption of the state file;
`stateDigest` verifies the deserialized UTXO set is the canonical one.

## ACK record layout (`ACK`, fixed 12 bytes, F01)

| Off | Size | Field |
|---|---|---|
| 0 | 8 | `ackGeneration` (LE u64) — highest generation whose publish returned success |
| 8 | 4 | `crc32` over bytes `[0, 8)` |

`ACK` is written durably (`ACK.tmp` → fsync → rename → dir fsync) **after** the MANIFEST
durability fsync and before `publish` returns success. It is the *independent* evidence used
to classify a corrupt MANIFEST as acknowledged (must not be rolled back) or unacknowledged
(safe to discard). An absent ACK is only valid for a never-committed (generation 0) store.

## Undo record layout (append-only, `undo.dat`)

| Off | Size | Field |
|---|---|---|
| 0 | 8 | magic `"INNUNDO1"` |
| 8 | 4 | `recLen` (total record bytes) |
| 12 | 32 | `blockHash` |
| 44 | 4 | `height` (int32) |
| 48 | 4 | `opCount` |
| 52 | … | `opCount` ordered ops |

Each op: `key(36)` = `txid(32) || n(4 LE)`, then `beforePresent(u8)` [+ before Coin],
then `afterPresent(u8)` [+ after Coin]. Coin encoding:
`value(i64) | height(i32) | flags(u8) | scriptLen(u32) | script`.

`undo.dat` is never truncated in place; a disconnect publishes a new MANIFEST whose
`undoSize` excludes the last record (the committed undo prefix shrinks atomically with the
state pointer). See F03 below for the unbounded-undo limitation.

## State file layout (`state-<gen>.dat`)

`magic "INNCUTX1"(8) | formatVersion(u32)=1 | count(u64) | count × [ key(36) | Coin ]`.
Entries are written in canonical key order (ascending `keyBytes`).

## Versioning / fail-closed rules

- Unknown `formatVersion` (MANIFEST or state) → `UNKNOWN_VERSION`, open fails closed.
- `manifestSize` mismatch, CRC mismatch, magic mismatch, truncated field, invalid `stateName`
  → `CORRUPT`.
- `fileHash` mismatch → `CORRUPT`; `stateDigest` mismatch after decode → `CORRUPT`.
- `undo.dat` shorter than `undoSize` (or a record not ending exactly on `undoSize`) → `CORRUPT`.
- `ACK` wrong size or CRC mismatch → `CORRUPT` (evidence unusable → fail closed).

## Backup establishment (F02)

Before the MANIFEST rename, when a prior `MANIFEST` exists, publish durably writes it to
`MANIFEST.bak` (write → fsync → verified read-back → decode → confirm the referenced
`state-<gen>.dat` exists). **Any** failure to read the prior MANIFEST, write/fsync the backup,
decode the backup, or find its referenced state file aborts the publish before the rename.
The only known-good recovery pointer is never destructively replaced.

## Recovery selection (F01 — fail-loud)

Let `ackGen` = ACK generation (absent = no evidence). Order:

1. **Primary loads** (`MANIFEST` valid): require `primaryGen == ackGen` or `primaryGen == ackGen+1`
   (the latter is a rename-committed but not-yet-acked generation, which is permitted).
   - `primaryGen < ackGen` → **FAIL LOUD** (would roll back an acknowledged generation).
   - `primaryGen > ackGen+1` → fail closed. If no ACK evidence and `primaryGen != 0` → fail closed.
   - If `primaryGen == ackGen+1`, ACK is reconciled up to `primaryGen` durably.
2. **No primary**: if an ACK with `ackGen > 0` exists → fail closed (acknowledged pointer missing);
   else empty (NOT_PUBLISHED).
3. **Primary present but unloadable (corrupt)**: load `MANIFEST.bak`.
   - No valid backup, or no ACK evidence → fail closed.
   - `ackGen > bakGen` → **FAIL LOUD** (an acknowledged generation is corrupt/unloadable;
     silent rollback refused; operator intervention required).
   - `ackGen < bakGen` → fail closed (inconsistent evidence).
   - `ackGen == bakGen` → the corrupt primary was an unacknowledged attempt; the acknowledged
     state (== backup) is preserved. Recover to `bakGen`, set `hadRecoveryIssue`, and repair
     `MANIFEST` from the backup bytes durably.
4. Then: remove stray `*.tmp`; GC `state-*.dat` outside `{generation, generation-1}`.

A recoverable crash (interrupted, unacknowledged publication) is never rejected merely because
uncommitted staging files exist. Corruption of an acknowledged committed generation always
fails loudly; it is never silently downgraded to an older tip.

## F03 — undo retention (OPEN, mandatory Stage 3 gate)

The prototype keeps an in-RAM **metadata-only** index of undo records (`blockHash`, `height`,
`offset`, `len`); record bodies are read from `undo.dat` on demand. This bounds per-record RAM
to a small fixed struct instead of full op vectors, but the index and `undo.dat` still grow with
history (O(history)). Bounded retention/reclaim is **not** implemented and is a mandatory Stage 3
acceptance gate: cap retained undo to the supported reorg depth, reclaim via MANIFEST-relative
rewrite/compaction, and never truncate a committed prefix without a new MANIFEST pointer.
