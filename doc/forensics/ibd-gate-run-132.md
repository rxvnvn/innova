# Gate run §13.2 — reproducible re-measurement on clean 59d03e4

Status: **runbook / measurement plan**. No production code changes. Everything
here uses artifacts that already exist: the built `innovad`, the impairment
proxy, the campaign configs, and the in-repo analyzer. The purpose is to close
the single open measurement from `ibd-scheduler-architecture-decision.md` §13.2
— re-measure §12 clauses (b)-(c) on the current HEAD with the 60 s wire-origin
expiry in place — and to make the run reproducible.

## 0. Decision linkage

| Item | Source |
|---|---|
| Mandate | `doc/design/ibd-scheduler-architecture-decision.md` §13.2 (the gate), §13.3 (escalation), §12 (success invariant) |
| The open question being closed | `doc/forensics/max-ahead-window-audit.md` §12: "Does the 5 s-timeout removal (59d03e4) alone change the 73002a0 picture…?" |
| Invariant being tested | §12(b): no minutes-scale flat tip while the pipeline is full and peers are ahead; §12(c): orphan pressure bounded away from the cap except during genuine races |

## 1. Non-goals

- No source changes, no new instrumentation, no scheduler patch. The gate is
  run **on clean `59d03e4`** so it can attribute any change to the expiry fix
  alone.
- Not a PATCH-vs-REPLACE verdict. The verdict is already recorded (§13.4,
  PATCH CURRENT); this runbook supplies the proof point the verdict was made
  conditional on.
- Not a black-hole / silent-peer test (that is a separate companion,
  `ibd-conservative-block-request-expiration.md` §13).

## 2. Artifact inventory (all pre-existing)

| Artifact | Path | Role |
|---|---|---|
| Binary | `src/innovad` (built 2026-08-08, newer than every source file) | the node under test |
| Impairment proxy | `/tmp/opencode/impair_proxy.py` | loopback TCP proxy, per-message loss + one-way delay |
| Proxy launcher | `/tmp/opencode/launch_proxy.sh` | double-fork launcher (survives tool process-group kill) |
| Node A (miner) config | `/tmp/opencode/expA/innova.conf` | regtest miner, `listen=1 port=18444` |
| Node B healthy config | `/tmp/opencode/control/innova.conf` | direct connect to 18444 |
| Node B impaired config | `/tmp/opencode/mc_impaired/innova.conf` | connect via proxy; carries the forensic flags |
| Analyzer | `contrib/forensics/analyze_forensic_csv.py` | offline forensic CSV analysis (`--baseline`/`--compare` for A/B) |
| Instrumentation | `-ibdexptrace`, `-ibdactivepathtrace`, `-ibdefficiencytrace`, `-ibdforensic`, `-ibdblocklatency`, `-blockrequesttrace` | the metrics of §13.2 (map in §7) |

## 3. Clean-tree and binary verification

```bash
git -C /home/user/innova status --porcelain
# Expect: only untracked files (forensics docs, scripts, the AD). No modified
# tracked files. `git diff` must be empty.
git -C /home/user/innova rev-parse HEAD        # expect 59d03e4 (p2p: use wire-origin …)
stat -c '%Y %n' src/innovad $(find src -name '*.cpp' -o -name '*.h' | head -5)
# Binary mtime must be >= every source mtime; otherwise rebuild via the
# makefile.unix targets (run from src/: `make -f makefile.unix`).
# Optional instrumentation sanity check (run from src/):
cd src && make -f makefile.unix check-ibdforensic
```

The expiry under test is the 60 s wire-origin deadline
(`BLOCK_IN_FLIGHT_TIMEOUT_US`, `src/net.h:2228`) with the 1 s pending-wire
bound (`src/net.h:2235`). No `-ibdmaxactiveperpeer` / `-ibddivfuture` overrides:
the gate measures stock HEAD defaults (BULK window 128/peer, global 512).

## 4. Run topology (regtest)

* **Node A — miner.** Fresh datadir, `expA` config (regtest, `listen=1`,
  port 18444, rpcport 18445, `staking=0`). Driven with `setgenerate true`
  (CPU mining; regtest block spacing is 1 s, `src/main.cpp:7950`). This is the
  "constant miner, moving tip" — the B3 run kept A at ~5-9 blk/s
  (`ibd-degradation-runtime-validation.md` §0).
* **Proxy** (impaired runs only): `launch_proxy.sh` →
  `impair_proxy.py --listen 14800 --target 127.0.0.1:18444 --delay-ms 6950
  --drop-pct 10`. One-way 6950 ms ⇒ RTT ≈ 13.9 s; whole-message loss ~10%;
  version/verack/ping/pong/addr exempt so the link stays alive. Drop RNG is
  seeded (`random.seed(1337)`), so the *distribution* is reproducible; exact
  dropped-message positions can vary with thread scheduling, so the gate
  compares aggregates and distributions, not per-hash identity.
* **Node B — sync client.** Fresh datadir each run, distinct
  port/rpcport from A, `listen=0`, `connect=127.0.0.1:18444` (CONTROL) or
  `connect=127.0.0.1:14800` (B3).

### 4.1 Node B flags (gate)

```
regtest=1
server=1
daemon=1
listen=0
staking=0
rpcuser=x
rpcpassword=y
rpcallowip=127.0.0.1
ibdexptrace=1
ibdactivepathtrace=1
ibdefficiencytrace=1
ibdforensic=1
ibdforensicpath=<run>/forensic.csv
ibdblocklatency=1
ibdblocklatencycsv=<run>/blocklat.csv
blockrequesttrace=1
```

`ibdexptrace` (a real flag at HEAD, `src/init.cpp:999`) emits the `EXPTRACE
ACTIVE` and `EXPTRACE SUMMARY/ORIGIN/INV/GBLOCKS/ORPHAN/PROBE` streams that
produced the B2/B3 tables. `ibdactivepath` emits one `IBD_ACTIVE_1S` line per
second (`src/ibdactivepath.cpp:590`) with connect rate and pipeline occupancy.
`ibdefficiency` carries `stalled_recovery_attempts` (`src/net.cpp:990`).
`blockrequesttrace` emits `STALL_RECOVERY` / `STALL_RECOVERY_OWNER`
(`src/net.cpp:2576, 5319`). `ibdforensic` + `ibdblocklatency` write the per-hash
CSVs used for the waste-class breakdown.

## 5. Run 1 — CONTROL (healthy baseline, §12(a))

Topology: A (miner) + B direct to `18444`, no proxy. Start A mining first,
start B once A has a short chain, run until B catches up and holds the healthy
ceiling for several minutes.

Pre-fix CONTROL signature to compare against (`ibd-degradation-runtime-validation.md`
§1): `BULK req=1086 recv=1086 conn=1085`, zero timeouts/orphans/getblocks,
healthy connect rate 500-800/s (`max-ahead-window-audit.md` §4.1).

Expected at HEAD: identical healthy ceiling; the only change (60 s expiry)
never fires on a healthy channel.

## 6. Run 2 — B3 impaired (the gate: moving tip, RTT 13.9 s, ~10% drop)

Order of operations:

1. Launch the proxy (`launch_proxy.sh`); verify it is listening on 14800.
2. Start node A with a fresh datadir; `setgenerate true` from t0 so the tip is
   moving for the whole run.
3. Start node B (fresh datadir) with the §4.1 flags and `connect=127.0.0.1:14800`.
4. Run for a fixed wall-clock duration. Pre-fix B3 reached orphan-cap saturation
   at 78.4 s and stayed pinned for the full ~394 s run (final `local_h=1352`,
   tip gap 1063+ and widening while A moved to ~4750+). A 600 s run gives the
   gap-evolution several tip generations to resolve as flat/stable/narrowing.
5. Stop B with a clean shutdown so the shutdown dumps write `forensic.csv` /
   `blocklat.csv` (`src/init.cpp:154-161`); record `proxy.stats`, A's final
   height, and the start/stop wall-clock.

### 6.1 Pre-registered comparison baseline (B3, pre-fix)

From `ibd-degradation-runtime-validation.md` §6:

| Metric | Pre-fix B3 | Record at HEAD |
|---|---|---|
| BULK recv / conn | 414 / 4 | |
| RELAY recv / conn | 2700 / 30 | |
| OTHER recv / conn | 3460 / 7 | |
| PROBE late_received | 3455 | |
| ORPHAN add | 2060 | |
| ORPHAN limit_reject / parent_unknown | 4242 / 4242 | |
| global_orphans | pinned at 750 | |
| GBLOCKS total / dedup | 22422 / 20970 (93.5%) | |
| 1item_relay / 1item_cont | 2331 / 305 | |
| final local_h / tip gap | 1352 / 1063+ widening | |
| stalled_recovery_attempts | 0 (pipeline never empty) | |

The gate question in one line: does the 60 s wire-origin expiry alone (a) let
B's tip advance out of the flat-at-4 regime, and (b) stop the orphan cap from
pinning at 750 with `limit_reject` on the pre-fix order of magnitude?

## 7. Run 3 — mainnet-flood profile (73002a0)

What it is: a production mainnet IBD characterized at `73002a0`
(`max-ahead-window-audit.md` §3/§5.2): 162090 announced, 57664 connected,
103780 wasted, tip 250760; healthy phase 500-800 connects/s (§4.1), degraded
phase received 100-300/s with connected ~0-5/s, orphan map to 750, getblocks
dedup ~93% (§4.2).

Reproduction caveat: the 73002a0 run is not bit-reproducible — it depends on
the live mainnet peer set and chain state. The profile is reproduced by
characterization, not by replay:

1. Fresh mainnet datadir at `59d03e4` with the §4.1 instrumented flags (no
   `connect`; normal outbound peer discovery).
2. Run to IBD completion (tip catch-up).
3. Classify waste from `blocklat.csv` terminal outcomes
   (`src/ibdblocklatency.cpp:159`): `connected_active`, `accepted_side`,
   `already_have`, `rejected`, `timeout`, `disconnect`, `incomplete_evicted`
   — the same partition the audit used (waste = rejected + timeout +
   incomplete_evicted + already_have_duplicate + small disconnect tail,
   §3). The ~33% of 73002a0 announcements that were 5 s-timeout (`~104k
   timeouts`) cannot recur at HEAD; the remaining waste and the connect-rate /
   tip-gap time series are the record.

The 73002a0 comparator is only the healthy/degraded phase numbers above; the
`59d03e4` run supersedes it as the current-flood characterization.

## 8. Metric → source map (mandatory §13.2 fields)

| §13.2 field | Source |
|---|---|
| Connect rate | `IBD_ACTIVE_1S … blocks_1s=` (per-second connect count); `EXPTRACE ORIGIN … conn=` (run total) |
| Tip-gap over time | `EXPTRACE ACTIVE time_us=… local_h=…` vs peer best from `EXPTRACE GBLOCK … peer_h=`; `getinfo` snapshots; time series from `IBD_ACTIVE_1S local_height=` |
| Orphan-cap saturation | `EXPTRACE ORPHAN add=… limit_reject=… parent_unknown=…`; `EXPTRACE ACTIVE … global_orphans=` |
| Waste class breakdown | `analyze_forensic_csv.py` buckets (no_rerequest / same_peer / cross_peer, late / never, fully_late); `EXPTRACE ORIGIN` recv vs conn divergence; `PROBE late_received=`; `blocklat.csv` terminal outcomes |
| `stalled_recovery_attempts` | `IBDEFFICIENCY` shutdown counter (`src/net.cpp:990`); `STALL_RECOVERY` / `STALL_RECOVERY_OWNER` events under `-blockrequesttrace` |

## 9. Pre-registered pass/fail thresholds (from §13.3)

Invariant §12(b) — tip progress:

* PASS: B's tip advances at the connect-rate ceiling; no minutes-scale flat tip
  while the pipeline is full and the peer is ahead (tip gap narrowing or
  closing).
* FAIL: flat tip at minutes scale with a full pipeline and a widening gap (the
  pre-fix B3 signature: flat at 4, gap 1063+ widening).

Invariant §12(c) — orphan pressure:

* PASS: orphan occupancy bounded away from the cap except during
  reorg/announcement races; `limit_reject` well below the pre-fix 4242
  (one or two orders of magnitude smaller, or zero).
* FAIL: cap pinned at 750 with `limit_reject` on the pre-fix order of
  magnitude (hundreds-to-thousands).

`stalled_recovery_attempts`: the empty-pipeline recovery arm is unchanged at
HEAD, so a measured 0 is "no change"; any nonzero value is a material signal
to record. This is exactly the counter the PATCH path (§5 of the AD) re-arms.

Decision procedure: if Run 2 violates (b) or (c) as defined above, then per
§13.3 the next decision point is REPLACE IBD SCHEDULER (via the §11 A/B path),
with the PATCH option (§4/§5 of the AD) remaining the lower-cost first attempt.
If Run 2 holds both clauses, the verdict PATCH CURRENT with the gate closed is
confirmed.

## 10. Artifact capture and analysis commands

Preserve per run: `<run>/debug.log`, `forensic.csv`, `blocklat.csv`, the
`innova.conf` used, `proxy.stats`, A's final height, start/stop UTC.

```bash
# Waste-class breakdown (this run), and A/B against the pre-fix IMPAIRED trace:
python3 contrib/forensics/analyze_forensic_csv.py <run>/forensic.csv
python3 contrib/forensics/analyze_forensic_csv.py \
    --baseline /tmp/opencode/mc_impaired/forensic.csv --compare <run>/forensic.csv

# EXPTRACE summary + orphan counters:
grep -E "EXPTRACE (SUMMARY|ORIGIN|ORPHAN|PROBE|INV|GBLOCKS)" <run>/debug.log

# Connect-rate + tip time series:
grep "IBD_ACTIVE_1S" <run>/debug.log

# Stalled-sync recovery:
grep -E "STALL_RECOVERY" <run>/debug.log
grep "IBDEFFICIENCY" <run>/debug.log
```

The offline replay of the 60 s policy on the recorded traces
(`ibd-conservative-block-request-expiration.md` §2.1: 3.5% false expiry at 60 s
vs 100% at 5 s, 0 pending losses) already bounds what the live gate should show;
the live gate's added value is the end-to-end (b)-(c) outcome that only a run on
the fixed tree can measure.

## 11. What this gate does not decide

* The PATCH-vs-REPLACE verdict (decided in the AD, §13.4).
* Any black-hole / silent-peer bound (separate controlled test, §1).
* Whether the healthy ceiling itself can be raised (connect-rate bound is
  non-scheduler, `max-ahead-window-audit.md` §7).
