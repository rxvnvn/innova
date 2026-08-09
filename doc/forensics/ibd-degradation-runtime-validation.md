# IBD degradation model: runtime validation CONTROL vs B2 vs B3

Status: **falsification/refinement**. No production fixes were applied as a result of this
report. P1 is refuted as a universal claim and refined to a phase-dependent one; P2–P5 are
supported.

**Historical reproducibility note.** The original B3 contract recorded here is
**HISTORICAL / NOT BIT-REPRODUCIBLE**. Its evidentiary profile was a moving-tip
regtest source (approximately 9 blocks/s), 6950 ms one-way delay, approximately
13.9 s RTT, 10% whole-message drop with seed 1337, and
`version`/`verack`/`ping`/`pong`/`addr` exemptions, with 128 per-peer active,
512 global active, and a 750 per-peer IBD orphan cap. The original
`impair_proxy.py`, launcher, exact source/client configs, and proxy
statistics/logs are missing. The historical signature remains: flat frontier,
full pipeline, orphan cap pressure, `ORPHAN_LIMIT` rejects,
received/connected divergence, and a widening gap. The tracked B3-v2 harness
under `contrib/forensics/b3v2/` is a new test and must be calibrated separately.

## 0. Setup recap

* Regtest Innova, node A = constant miner (post checkpoint-fix, moving tip at ~9 blk/s).
* Node B syncs as a fresh client over an impaired proxy: 6950 ms one-way delay (RTT ≈ 13.9 s),
  ~10% message drop (version/verack/ping/pong/addr exempt). In-flight block timeout 5 s,
  getblocks timeout 15 s, BULK window 128, IBD orphan cap 750/peer.
* Telemetry: `-ibdexptrace=1`, SUMMARY emitted at shutdown.

Three runs:

| run | peer type | link | chain | result |
|-----|-----------|------|-------|--------|
| CONTROL | A→B direct | healthy | 0→1088 | clean baseline |
| B2 | A→B via proxy | impaired | 0→1999 (A static at 1999) | degraded, then caught up |
| B3 | A→B via proxy | impaired | 0→1352 (A moves past ~4750) | degraded, stuck at tip gap |

Note on `active=*` counters: values are negative in B2/B3 because a timed-out block is
decremented twice (release at timeout + release at late receive); the counters are
diagnostic-only and their sign is an artifact. `req/recv/conn` are trustworthy.

## 1. CONTROL (healthy baseline)

```
BULK req=1086 recv=1086 conn=1085 active=0
RELAY req=1 recv=1 conn=0
TIMEOUT_REISSUE req=0 recv=0
OTHER req=0 recv=0
GBLOCKS total=0 dedup=0
ORPHAN add=2 limit_reject=0 parent_unknown=0
PROBE timeout_reask=0 late_received=0
```

Every requested block is received and connected; zero timeouts, zero orphans, zero
getblocks, zero late deliveries. BULK is the entire request pipeline (1086 of 1088 req).

## 2. B2 — degraded, static tip (A capped at 1999 by the checkpoint bug)

```
BULK req=3005 recv=890 conn=36
RELAY req=0 recv=0 conn=0
CONTINUATION req=4 recv=1 conn=1
TIMEOUT_REISSUE req=128 recv=0 conn=0
OTHER req=0 recv=1737 conn=21
INV batches=23 blocks_resp=9636 blocks_cont=10 blocks_relay=0
  1item_resp=0 1item_cont=10 1item_relay=0
GBLOCKS total=5782 dedup=5225   (90.3% suppressed)
ORPHAN add=1941 limit_reject=303 parent_unknown=303
PROBE timeout_reask=128 late_received=1526
```

* Request pipeline dominated by BULK (3005 of 3153 req, 95.3%).
* Strong receive/connect divergence: BULK recv 890 / conn 36; OTHER recv 1737 / conn 21.
* No RELAY traffic at all: static tip emits no new-block INV relays; the only INVs are
  getblocks responses (9636 blocks) and 10 continuation batches.
* getblocks: 90.3% of push attempts are dedup-suppressed. Orphan cap hit 303 times.
* With A pinned at 1999, B2 eventually connected the whole range: final ACTIVE
  `local_h=1999 global_orphans=0`.

## 3. B3 — degraded, moving tip (A keeps mining past the checkpoint)

```
BULK req=1351 recv=414 conn=4
RELAY req=6567 recv=2700 conn=30
CONTINUATION req=284 recv=0 conn=0
TIMEOUT_REISSUE req=82 recv=8 conn=0
OTHER req=19 recv=3460 conn=7
INV batches=2704 blocks_resp=7989 blocks_cont=305 blocks_relay=38922
  1item_resp=0 1item_cont=305 1item_relay=2331
GBLOCKS total=22422 dedup=20970  (93.5% suppressed)
ORPHAN add=2060 limit_reject=4242 parent_unknown=4242
PROBE timeout_reask=82 late_received=3455
```

* Request pipeline dominated by RELAY (6567 of 8382 req, 78.3%); BULK only 16%.
* Massive receive/connect divergence in every origin:
  BULK recv 414 / conn 4, RELAY recv 2700 / conn 30, OTHER recv 3460 / conn 7.
* Orphan cap: `global_orphans` saturates at 750 and stays there; `limit_reject=4242`,
  exactly matching `GBLOCKS src=ORPHAN_LIMIT=4242` (each cap rejection re-announces the
  whole range via getblocks-from-tip, `main.cpp:7558-7560`).
* getblocks: 93.5% suppression. INV relay flood: 38922 relayed blocks, 2331 one-item relay
  INVs.
* B3 never catches up: final ACTIVE `local_h=1352`, tip gap 1063+ and widening; A moved to
  ~4750+ during the run.

### 3.1 Quantitative reconstruction of the transition (B3)

Timeline relative to B3 start (UTC wall clock `1786125928`):

| t (s) | event |
|-------|-------|
| 0.0   | start; ACTIVE `local_h=0 global_orphans=0 active_total=0` |
| 7.0   | first GBLOCK `src=VERSION peer_h=2415 gap=2415` (initial getblocks) |
| 14.3  | first RELAY activity: `RELAY=1`, active_total=2 |
| 37.2  | orphans reach 100 (`local_h=4`); RELAY=128 active; OTHER=-104 (late arrivals begin) |
| 78.4  | orphans saturate at 750 (`local_h=4`); OTHER=-525 |
| …     | RELAY flood keeps arriving; every push goes through getblocks with dedup suppression |
| 393.7 | stop; `local_h=1352`, orphans pinned at 750, RELAY=-2328, OTHER=-3443 |

Sequence recovered: **BULK-dominated start → moving tip re-announces whole range →
RELAY flood (single-item INV relay) → orphan occupancy grows to the 750 cap → receive/connect
divergence (late_received=3455, OTHER recv≈3460) → getblocks suppression (~93.5% dedup) with
each ORPHAN_LIMIT rejection re-announcing the full range.**

The B2 static-tip run (transition in 164.5 s vs B3's 78.4 s to cap) shows the same final
machinery but no RELAY component and a BULK-dominated pipeline.

## 4. P1–P5 verdicts

| P | claim | CONTROL | B2 static-tip | B3 moving-tip | verdict |
|---|-------|---------|----------------|----------------|---------|
| P1 | degraded pipeline is BULK-dominated | ✓ BULK=1086 | ✓ BULK=3005 (95%) | ✗ RELAY=6567 (78%) | **refuted as universal; phase-dependent** |
| P2 | received >> connected in degraded state | ✗ (1:1) | ✓ 890/36, 1737/21 | ✓ 414/4, 2700/30, 3460/7 | ✓ supported |
| P3 | 1-item INV batches come from RELAY, not getblocks | n/a (1 relay) | ✗ (0 relay, 10 cont) | ✓ 2331 relay vs 305 cont | ✓ supported only in moving-tip phase |
| P4 | orphan occupancy grows toward ORPHAN_LIMIT, then rejections | ✗ (add=2) | ✓ add=1941, limit=303 | ✓ add=2060, limit=4242, pinned 750 | ✓ supported |
| P5 | getblocks returns large batches or nothing (suppression), not repeated 1-item | n/a (0) | ✓ 5225/5782 dedup | ✓ 20970/22422 (93.5%) | ✓ supported |

## 5. Key new finding: P1 is phase-dependent, not a property of the degraded state

The original model predicted a single degraded pipeline dominated by BULK. The runtime data
refutes that as a universal statement:

* **Static-tip degradation (B2)** → BULK dominates (3005 req, 95.3%), zero RELAY. The client
  re-runs getblocks from its own best tip, re-lists the whole range, and BULK-fetches it;
  because the tip never moves there are no relay INVs to interleave.
* **Moving-tip degradation (B3)** → RELAY dominates (6567 req, 78.3%). The miner keeps
  announcing new tip blocks; each arrives as a 1-item RELAY INV on the same slow link, wins
  the ask-for arbitration against the in-flight BULK window, and floods the request pipeline.
  BULK is starved: only 1351 req, 414 received.

This is a falsification of the "one degraded state" framing and a refinement: the dominant
request origin is governed by whether the peer tip is moving (relay source) or pinned
(getblocks source), given a degraded link. P2–P5 are consistent across both phases and are
kept as supported.

## 6. Key numbers to retain (B3, moving-tip phase)

* BULK recv=414 / conn=4
* RELAY recv=2700 / conn=30
* OTHER recv=3460 / conn=7
* late_received=3455
* orphan add=2060
* ORPHAN_LIMIT=4242 (= limit_reject = parent_unknown = GBLOCKS src=ORPHAN_LIMIT)
* global_orphans pinned at 750
* GBLOCKS=22422
* dedup/suppression=20970 (~93.5%)
* 1item_relay=2331 vs 1item_cont=305

## 7. No production changes

This report deliberately makes **no** production recommendation. The checkpoint fix landed in
`fix(checkpoints): exclude regtest from hardened checkpoints` (cee0cba) is a test/regtest-only
correctness fix, unrelated to the IBD degradation model. No production/IBD code was changed as
a result of these findings.
