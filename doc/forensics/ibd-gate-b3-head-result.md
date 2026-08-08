# Gate §13.2, Run 2 (B3 impaired) — result at clean `59d03e4`

Status: **measurement result**. Executes the runbook `ibd-gate-run-132.md`
§6 (Run 2) on clean HEAD `59d03e4` with the 60 s wire-origin expiry in place
(`BLOCK_IN_FLIGHT_TIMEOUT_US`, `src/net.h:2228`, 1 s pending-wire bound
`src/net.h:2235`). No source changes, no scheduler patch, no new
instrumentation. The single open measurement from
`ibd-scheduler-architecture-decision.md` §13.2 is now closed.

Run date 2026-08-08, ~11:06:35Z → ~11:17:30Z. B ran for 613 s of
`IBD_ACTIVE_1S` sampling (606 samples) before a clean shutdown so the
`forensic.csv` / `blocklat.csv` dumps were written.

## 0. Gate question (one line, from the runbook §6.1)

> Does the 60 s wire-origin expiry alone (a) let B's tip advance out of the
> flat-at-4 regime, and (b) stop the orphan cap from pinning at 750 with
> `limit_reject` on the pre-fix order of magnitude?

Answer on both: **NO.** Clauses (b) and (c) of §12 still fail; per §13.3 the
next decision point is REPLACE IBD SCHEDULER via the §11 A/B path.

## 1. §6.1 comparison — pre-fix B3 vs HEAD B3

| Metric | Pre-fix B3 | HEAD B3 (this run) |
|---|---|---|
| BULK recv / conn | 414 / 4 | 0 / 0 |
| RELAY recv / conn | 2700 / 30 | 2534 / 17 |
| OTHER recv / conn | 3460 / 7 | 15 / 0 (ORPHAN_PARENT 11, OTHER 4) |
| CONTINUATION req / recv / conn | 305 1-item | 238 / 181 / 0 |
| PROBE timeout_reask / late_received | 82 / 3455 | 0 / 0 (5 s path removed at HEAD; HEAD-equivalent is forensic `received_after_timeout=160`, 100% re-requested before receipt) |
| ORPHAN add | 2060 | 866 |
| ORPHAN limit_reject / parent_unknown | 4242 / 4242 | 1847 / 1847 |
| global_orphans | pinned at 750 | pinned at 750 (from ~t=191 s) |
| GBLOCKS total / dedup | 22422 / 20970 (93.5%) | 49373 / 41221 (83.5%) |
| 1item_relay / 1item_cont | 2331 / 305 | 3026 / 351 |
| INV blocks_relay | 38922 | 78712 |
| final local_h / tip gap | 1352 / 1063+ widening | 133 / ≥6315 widening (peer view frozen at 6448; A actually mined to 10950) |
| stalled-recovery arm | 0 fired | 8280 `IBD_RECOVERY_DECISION` evals, `should_recover=1` in **0**; `STALL_RECOVERY` events 0 |

`ORPHAN limit_reject` dropped 4242 → 1847, but remains hundreds-to-thousands —
the pre-fix order of magnitude the runbook §9 threshold names.

## 2. §12(b) — tip progress: **FAIL**

`IBD_ACTIVE_1S` local_height time series (613 s of sampling):

| segment | t (s) | span |
|---|---|---|
| h = 0 (no blocks yet) | 0 – 38 | 38 s |
| **h = 6 flat** | 39 – 280 | **241 s** |
| burst 6 → 58 → 64 → 74 → 80 | 281 – 292 | 12 s |
| **h = 80 flat** | 293 – 597 | **304 s** |
| burst 80 → 99 → 133 | 598 – 613 | 16 s |

During both plateaus the pipeline was **full**: `blocks_in_flight=128` (128/128
per-peer window), `inflight_peer=127`, the deferred queue grew monotonically
(`global_queued_current` 972 → 1691), and the peer was far ahead. `A` was ~4400
at B's start and mined to 10950 by run end; B's final height is 133.

Connect rate: `blocks_1s` mean **0.219 blk/s**, median **0**, nonzero in
**1.16%** of 1-second intervals, max 52 (the single recovery burst). Total
connects over the run: **133**. Healthy ceiling measured at
`73002a0`-era and validated since is 500–800/s
(`max-ahead-window-audit.md` §4.1) — this run is ~2500× below it.

Tip-gap: B = 133 vs A = 10950 → gap ≈ 10817 by run end (peer-visible
`peer_best_height` frozen at 6448 from ~t=205 s because outbound getheaders
dedup stops B learning A's rising height; the frozen figure alone gives gap
≥ 6315). Flat tip at minutes scale, full pipeline, widening gap — the pre-fix
B3 signature (flat at 4, gap 1063+) reproduced almost exactly, at higher
absolute heights.

## 3. §12(c) — orphan pressure: **FAIL**

* `global_orphans` pinned at the 750 cap from ~t=191 s through the end of the
  run (`EXPTRACE ACTIVE … global_orphans=750`; `EXPTRACE ORPHAN add=866`).
* `ORPHAN limit_reject=1847 parent_unknown=1847` — same order of magnitude as
  the pre-fix 4242 (hundreds-to-thousands), and B was under the same RTT 13.9 s
  / ~10%-drop impairment.

## 4. Stalled-sync recovery arm: unchanged, still never fires

* `IBD_RECOVERY_DECISION`: **8280 evaluations, `should_recover=1` in 0**.
  Skip-reason distribution: `pipeline_active` 8120 (98.0%),
  `stall_timeout_not_reached` 73, `local_height_changed` 17,
  `no_eligible_peers` 70.
* `STALL_RECOVERY` / `STALL_RECOVERY_OWNER` events: 0.
* `stalled_recovery_attempts` (`src/net.cpp:990`): never incremented.
* `FIRST_CONTINUITY_BREAK` fired once, at h = 6 (t≈94 s,
  `last_accepted_age_s=60`, `orphan_count_global=420`, `recovery_attempts=0`);
  no recovery followed it.

The gate's empty-pipeline condition never opens because `blocks_in_flight=128`
is always nonzero — exactly the failure the PATCH path of the AD (§4/§5)
re-arms.

## 5. Waste-class breakdown (`blocklat.csv` terminal outcomes)

`src/ibdblocklatency.cpp:159` partition, this run:

| outcome | count |
|---|---|
| connected_active (useful) | **133** |
| rejected (orphan-limit cap) | 1847 |
| timeout (60 s expiry, never connected) | 585 |
| incomplete_evicted (lifecycle ring) | 2568 |
| accepted_side / already_have / disconnect | 0 / 0 / 0 |

Useful share of the 2730 received: **4.9%**. Forensic summary
(`IBDFORENSIC SUMMARY` + offline analyzer): 3050 batches, 3085 canonical
entries, 3443 generations (3315 closed, 128 active at shutdown); generation
close reasons `receive` 2725, **`timeout` 585** (lifetime p50 = 60.04 s,
max = 60.10 s — the 60 s wire-origin deadline firing exactly as configured),
`local-fail` 5; `received_after_timeout=160`, all 160 re-requested **before**
receipt; `never_received_total=367`.

So the 60 s expiry works as designed — it fires on schedule and its
re-requests succeed (160/160) — but it neither advances the tip out of the
flat regime nor relieves orphan-cap saturation. The fix alone does not clear
the gate.

## 6. Verdicts per the pre-registered thresholds (`ibd-gate-run-132.md` §9)

| Clause | Pre-registered threshold | Measured | Verdict |
|---|---|---|---|
| §12(b) tip progress | no minutes-scale flat tip with full pipeline + widening gap | flat at 6 for 241 s, flat at 80 for 304 s, pipeline full, gap ≥6315 widening | **FAIL** |
| §12(c) orphan pressure | bounded away from cap; `limit_reject` one/two orders below 4242 | cap pinned at 750; `limit_reject=1847` | **FAIL** |
| stalled-recovery arm | measured 0 = no change | 0 fired (8280 decisions, 0 `should_recover`) | recorded: no change |

Consequence per §13.3: the B3 profile still violates §12(b)-(c) on HEAD, so the
next decision point is **REPLACE IBD SCHEDULER**, to be pursued via the §11 A/B
path (header chain behind a flag, dual-scheduler A/B, flag-flip rollout),
preserving the seven landed fixes (§5.7) and validated against
`p2p_ibd_stalling.py`-equivalent functional tests. The §13.4 verdict
(PATCH CURRENT) was explicitly conditional on this gate; the gate result is
the condition named in §13.3, and it does not hold.

## 7. Caveats and non-goals

* Run 3 (mainnet-flood 73002a0 profile) is **not** run: mainnet
  characterization was deferred until the B3 gate artifacts are captured and
  analyzed (done) and the user re-authorizes.
* `BULK req=0` in this run vs 1351 pre-fix reflects this run's timing (the
  initial getblocks response was not admitted before the relay flood starved
  the BULK window), not a code-path removal; the profile remains
  RELAY-dominated exactly as pre-fix (78% → effectively 100% RELAY).
* Peer-height staleness (frozen 6448) means the reported gap is a lower bound.
* Single-peer regtest harness; the impairment RNG is seeded (proxy seed 1337)
  so the drop distribution is reproducible; exact dropped-message positions
  vary by thread scheduling, so comparisons use aggregates/distributions
  (runbook §4).
* This run is 613 s vs pre-fix 394 s; the longer window only confirmed the
  flat regimes persist rather than resolving.

## 8. Artifacts

| Artifact | Path |
|---|---|
| Run stdout log (all instrumented traces) | `/tmp/opencode/gate-b3/b/b3run.stdout.log` |
| Forensic CSV | `/tmp/opencode/gate-b3/b/forensic.csv` |
| Blocklatency CSV | `/tmp/opencode/gate-b3/b/blocklat.csv` |
| Node A final height | 10950 (stopped cleanly after run end) |
| Node A / proxy | stopped; mainnet datadir untouched |
