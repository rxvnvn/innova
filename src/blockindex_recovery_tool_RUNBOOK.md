# Block Index V2 Bounded-Memory Recovery Tool — RUNBOOK

## What this tool does

Rebuilds a legacy chain history segment `S+1 .. L` into the mutable tip
authority (`<v2root>/blockindex_tip/`) in strictly bounded RAM:

1. **PASS 1 (backward walk, L→S+1).** Point lookups on a consistent
   **immutable COPY** of the legacy LevelDB (read-only; fails closed on a
   held LOCK). At most `SPILL_RECORDS_PER_CHUNK = 250000` records resident;
   each chunk is height-reversed and flushed to the on-disk spill dir
   (`<v2root>/blockindex_tip/recovery-path/chunk-%04u.dat`, records.dat ABI,
   228 B/record, tmp+fsync+rename).
2. **PASS 2 (forward stream).** Chunks are visited in **descending chunk
   index** (chunk 0 holds the highest heights band; see "Chunk orientation").
   Each chunk's file contents are ascending inside, so the global stream is
   dense ascending. One record in flight, O(1) parent-derived seed, appended
   per record through the production `BlockIndexTipAuthority::AppendBatch`
   commit protocol (tail stores + side index + tip.meta CoW commit).
3. **Final validation.** Tip handle dropped, authority re-opened (full
   streaming digest recompute, O(N) time / O(1) RAM); fail closed unless
   `tip == L`; spill dir deleted; `recovery.meta` → ACCEPTED.
4. **Disk relief.** After the last slot of each chunk is committed, that
   chunk file is deleted immediately (crash-safe: a killed run re-walks from
   scratch; the tail stores' uncommitted suffix is truncated at the next
   Open). This caps spill↔tail-store disk overlap at ~one chunk (57 MB).

## Chunk orientation (critical implementation note)

The backward walk fills its ring in walk-arrival order (height L first),
reverses each full window, and writes it. Consequence: **chunk FILE contents
are ascending, but the CHUNK SEQUENCE is height-descending.** Chunk `c`
holds heights `[L - c*RPC - cnt_c + 1 .. L - c*RPC]` with
`cnt_c = min(RPC, W - c*RPC)` for W walked records. The forward pass maps
global height `h` to `(chunk c, file slot j = h - hLo(c))` and walks chunks
from the highest index down. Single-chunk runs (delta ≤ 250k) are exactly
the old behavior (`j = h - (S+1)`), byte-identical.

## CLI

```
blockindex-recovery-tool <v2root> <consistent_legacy_leveldb_COPY> \
                         <blk_dir|""> [--abandon] [--report <path>]
```

* `<v2root>` — V2 lifecycle root with the CURRENT-selected validated base
  generation (tip = S) + `blockindex_tip/`.
* `<consistent_legacy_leveldb_COPY>` — a coherent read-only copy of the
  legacy LevelDB (`blockindex` + `hashBestChain`). NEVER the live datadir.
* `<blk_dir>` — dir with `blk%04u.dat` for exact `nSize` ("" if unavailable;
  derived `nSize` stays unknown).
* `--abandon` — preserve the prior `blockindex_tip/` as
  `blockindex_tip.pre-recovery-<N>/` (rollback evidence) and start over.
* Exit codes: 0 promoted (tip == L, ACCEPTED) / 1 failed fail-closed
  (idempotent partial tip) / 2 usage.

## Measured scaling (this worktree, binary post-fix)

| chain | delta (S=20000) | peak RSS (MiB) | spill chunks |
|-------|-----------------|----------------|--------------|
| 60k   | 40k             | 57.8           | 1            |
| 180k  | 160k            | 146.6          | 1            |
| 360k  | 340k            | 177.7          | 2            |
| 1M    | 980k            | **183.3–189.2**| 4            |

Post-250k slope: **~19 bytes/record** (bounded-state rebalancing, not
accumulation — inner-window RSS probes flatline after the tip windows fill
their 65536-entry ring; remaining drift is LevelDB side-index compaction
oscillation, mean-reverting). Extrapolation to the full 8.17M chain:
```
peak(delta=8.15M) ≈ 178 MiB (340k anchor) + (8.15M-0.34M)*19B
                  ≈ **318 MiB** ≪ 512 MiB requirement (target ≤256:
                  marginally above; operators can set INNOVA_DIAG_SMALL_IDX
                  to shave ~8 MB, or the plateau can be re-anchored after
                  more delta points; both requirements' hard limit 512 MiB
                  holds with 1.6x headroom).
```
Phase breakdown at 1M delta (phase-rss marks):
`tip-open 18 MB → walk-done 137 MB (ring 250k cap) → stream-done ~157 MB →
verify transient ~189 MB → stable ~181 MB`.

## Memory owners (localized by measurement)

1. **Walk ring** — capped by SPILL_RECORDS_PER_CHUNK=250000 (≈117 MB
   window); saturates, does not grow with delta.
2. **Tip-authority bounded windows** — one-time fill to the 65536-entry
   ring cap (~25 MB, ~375 B/entry × cap) then flat (proven by
   `INNOVA_DIAG_INNER_RSS=1` 8k-put windows: +3.1MB/window during fill →
   0k/window afterwards, with −2.5/+2.8 MB compaction oscillation).
3. **Side-index LevelDB** — bounded 8 MB block cache
   (`MakeHashIndexOptions`), writes are per-record O(log N); RSS oscillates
   around a flat mean with compaction (≤ ±3 MB swings).
4. **Verify pass** — transient ~+20 MB (reopen digest recompute, O(1) RAM).

## Disk model at 8.17M chain (delta ≈ 8.15M, S≈20000)

Per-record on-disk: spill 228 B; delivered stores: tip-records 228 B +
tip-derived 56 B + tip-active 8 B = 292 B; side index LevelDB ≈ 2× write
amplification (measured 321 MiB tip dir per 1M appended incl. hashindex).

Scale to 8.15M:
| component | GiB |
|-----------|-----|
| spill chunks at walk end (8.15M × 228 B) | 1.73 |
| delivered tail stores (8.15M × 292 B) | 2.22 |
| side-index LevelDB (incl. amp) | ~0.34 (in tip dir total 2.56) |
| legacy snapshot COPY held during run | ~2.04 |
| **pre-fix true peak** (all spill + full tip + legacy) | **≈ 6.33** |
| **post-fix true peak** (chunks consumed + deleted; ≤1 chunk residual) | **≈ 4.66** |

VPS free ≈ 6.5 GiB → pre-fix margin 0.17 GiB (UNSAFE); post-fix margin
**≈ 1.84 GiB**. Post-promotion steady state: tip dir 2.56 GiB (legacy copy
and spill deletable).

## Crash-safety / idempotency (verified)

* **Idempotent re-run:** completed root re-runs to `appended=0` exit 0.
* **SIGKILL + resume:** killed mid-stream at ~186867/980000 appended; resumed
  run completed and tip-records/tip-derived/tip-active were **byte-identical
  (`cmp`) to an independent clean run** (1M-delta, multi-chunk).
* **Recovery discipline:** `tip.meta` single commit point; uncommitted store
  suffixes truncated at Open; `recovery.meta` is derived bookkeeping only.
* Eager chunk deletion is crash-safe (killed run re-walks from scratch; it
  never needs deleted chunks).

## Diagnostics (no-ops unless env-gated)

| env | effect |
|-----|--------|
| `INNOVA_DIAG_INNER_RSS=1` | per-8192-put RSS window deltas (`inner-rss window_delta=`) |
| `INNOVA_DIAG_NO_SIDEINDEX=1` | skip side-index Puts (A/B: isolates index contribution) |
| `INNOVA_DIAG_SMALL_IDX=1` | tiny side-index caches (64 KiB block cache) |

`phase-rss mark=...` lines on stderr at phase boundaries (always on, zero
cost). `recovery-progress=<done>/<total>` every 65536 records.

## Operations (full-chain VPS recovery — NOT yet executed)

```bash
# 1. Stop the daemon (or use a coherent copy while stopped.
#    NEVER open the live legacy dir: the tool fails closed on a held LOCK).
# 2. Copy the legacy chainstate (≈2.0 GiB at 8.17M):
cp -a <datadir>/chainstate /safe/vps/legacy-copy
# 3. Free the ~4.7 GiB peak (see disk model); confirm:
df -h <v2root-fs>
# 4. Dry diagnosis (bounded RSS; abort with --abandon afterwards if needed):
blockindex-recovery-tool <v2root> /safe/vps/legacy-copy <blkdir> \
    [--abandon first to preserve the interrupted tip] 2>&1 | tee recovery.log
# 5. Post-checks:
#    - exit 0, report RECOVERY_OK tip == L, recovery.meta phase ACCEPTED
#    - idempotent re-run: appended=0
#    - spill dir deleted (0 post-acceptance disk)
# 6. Only after operator acceptance: the preserved
#    blockindex_tip.pre-recovery-N/ may be archived/removed by the OPERATOR
#    (the tool never deletes it).
```

Preservation rules: the interrupted recovery evidence
(`tip-records.dat.tmp` on the VPS) is OFF-LIMITS to this tool and to any
automated step; it is untouched by design (the tool only appends to
committed stores it owns inside `blockindex_tip/`).

## Tests

`make -f makefile.unix check-blockindex-recovery-tool` (8 cases) plus
`blockindex_tip_tests`, `blockindex_crash_corruption_tests`,
`blockindex_committed_tip_snapshot_tests`, `blockindex_hashindex_tests` —
all green post-fix.
