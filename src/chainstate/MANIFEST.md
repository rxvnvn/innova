# MANIFEST specification v1 (isolated chainstate vertical slice)

Status: prototype spec for the Stage-2 isolated slice. Versioned, fail-closed.
All multi-byte integers are little-endian.

## Purpose

The MANIFEST is the **single commit pointer** for one chainstate directory. It selects
exactly one immutable state file and one committed prefix of the append-only undo log.
Its atomic rename into place is the *visibility* boundary; the subsequent parent-directory
`fsync` is the *durability* boundary.

## Files in a chainstate directory

| File | Role |
|---|---|
| `MANIFEST` | current committed pointer (this spec) |
| `MANIFEST.bak` | previous committed pointer, for recovery fallback |
| `state-<gen>.dat` | immutable CoW UTXO snapshot for generation `<gen>` |
| `undo.dat` | append-only ordered undo records |
| `LOCK` | advisory single-writer lock (`flock LOCK_EX|LOCK_NB`) |
| `MANIFEST.tmp`, `state-<gen>.dat.tmp` | staging (never authoritative) |

## MANIFEST v1 layout (total = 140 + L bytes)

| Off | Size | Field |
|---|---|---|
| 0 | 8 | magic `"INNCSMNT"` |
| 8 | 4 | `formatVersion` = 1 |
| 12 | 4 | `manifestSize` (= total bytes, incl. CRC) |
| 16 | 8 | `generation` (monotonic; 0 = not published) |
| 24 | 8 | `undoSize` (committed byte length of `undo.dat`) |
| 32 | 4 | `stateNameLen` = L |
| 36 | L | `stateName` (e.g. `state-7.dat`) |
| 36+L | 32 | `stateDigest` — canonical UTXO-set digest (semantic equality) |
| 68+L | 32 | `fileHash` — SHA-256 of the serialized state file (integrity) |
| 100+L | 32 | `blockHash` — tip block |
| 132+L | 4 | `tipHeight` (int32) |
| 136+L | 4 | `crc32` over bytes `[0, 136+L)` |

Two distinct hashes are deliberate: `fileHash` detects byte corruption of the state file;
`stateDigest` verifies the deserialized UTXO set is the canonical one (catches semantic drift
that a raw byte hash of a re-encoded file could mask).

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
state pointer).

## State file layout (`state-<gen>.dat`)

`magic "INNCUTX1"(8) | formatVersion(u32)=1 | count(u64) | count × [ key(36) | Coin ]`.
Entries are written in canonical key order (ascending `keyBytes`).

## Versioning / fail-closed rules

- Unknown `formatVersion` (MANIFEST or state) → `UNKNOWN_VERSION`, open fails closed.
- `manifestSize` mismatch, CRC mismatch, magic mismatch, truncated field → `CORRUPT`.
- `fileHash` mismatch → `CORRUPT`; `stateDigest` mismatch after decode → `CORRUPT`.
- `undo.dat` shorter than `undoSize` (or a record not ending exactly on `undoSize`) → `CORRUPT`.
- If `MANIFEST` is corrupt and `MANIFEST.bak` is valid, recovery selects the previous committed
  state and flags the recovery issue; if neither is valid the store refuses to open.

## Recovery selection

1. Try `MANIFEST`; on success, that generation is committed.
2. Else try `MANIFEST.bak`; on success, recover that (older) generation, set `hadRecoveryIssue`.
3. Else, if `MANIFEST` was absent → empty (NOT_PUBLISHED).
4. Else (present but corrupt) → fail closed.
Then: remove stray `*.tmp`; GC `state-*.dat` outside `{generation, generation-1}`.
