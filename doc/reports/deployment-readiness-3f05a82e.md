# DEPLOYMENT READINESS — IBD/miner candidate 3f05a82e (branch fix/ibd-stale-height-miner-status)

**Status: READY-FOR-REVIEW, NOT DEPLOYED.** This package only prepares; no daemon was touched.

## 1. What ships (3 local commits on top of clean master 39a1c4ef)

- `847f045f` Stage 1: activity-corroborated IBD peer-ahead-lag predicate; truthful CPU-mining hashing/pause status
- `56e1ef99` Stage 2: bounded grace window for fresh-only ahead claims; per-worker hashing aggregation
- `3f05a82e` Stage 3: grace-window reset cannot restart mining suppression via brief claim drops (new this review)

Full delta vs master: 7 files, +730/−45 (`git diff 39a1c4ef 3f05a82e --stat`).
No consensus/validation code touched; diff confined to `IsInitialBlockDownload()`,
`ResolveCorroboratedAheadPeerHeight()`, `CCPUMinerController` status plumbing,
ibdactivepath reason naming, and tests.

## 2. Reproducible build

```sh
cd /home/user/innova-ibd-stage1
git checkout 3f05a82e                      # == branch fix/ibd-stale-height-miner-status
cd src
make -f makefile.unix -j$(nproc)           # builds innovad
make -f makefile.unix -j$(nproc) test_innova
```

## 3. Resulting artifact identification

| Artifact | Path (build tree) | sha256 |
|---|---|---|
| daemon | `/home/user/innova-ibd-stage1/src/innovad` | `59bab7bcdf59cac37d2cda75fdd5cb347ee844ffa09c68d0f08d31c63e972c92` |
| test binary | `/home/user/innova-ibd-stage1/src/test_innova` | `6248496cb8c3bdebbb8c8f2215e38c6bc371aed3a20c7837f53bb7afef6fa01d` |

daemon: ELF x86-64 PIE, dynamically linked, with debug_info, not stripped,
BuildID `69a397f8ab10d2cd7703b571aeaecef529518151`.

## 4. Rollback target (recorded, NOT modified)

- Current installed binary: `/home/user/innova-qt/innovad-39a1c4efdaf7`
  sha256 `8db24ccdf4edb444088c38dd9aa1b0b3f760b3dd32541b9f38b4a3a81060ac5d`
  (mtime 2026-10-12 12:26:45 +0300, size 168899488)
- Also present but NOT the rollback target: `/home/user/innova-qt/innovad2`
  sha256 `68361b8168fa14d15e32b0d79f9b17a72f2cd7b1ca3f44cfef41cafe8908ba1e`

## 5. Rollback procedure (execute only after a failed deployment)

1. Stop the running innodad service exactly the way it is currently managed
   (systemd unit name / run script is deployment-site specific — record it
   BEFORE deploying).
2. Restore the binary: copy the backup of the previous binary back over the
   deployed path (or keep the deployed binary as e.g. `innovad-3f05a82e` and
   re-point the service to `innovad-39a1c4efdaf7`).
3. Verify: `sha256sum` of the restored file must equal
   `8db24ccdf4edb444088c38dd9aa1b0b3f760b3dd32541b9f38b4a3a81060ac5d`.
4. Start the service; confirm: daemon responds to RPC `getinfo`, chain tip
   height matches the last known value, and `getibdmetrics` reports the
   pre-deploy steady state.
5. Data dir is unchanged by this patch (no schema/levelDB changes), so no data
   rollback is required. If the new binary wrote new blocks, re-connecting the
   old binary to the same datadir re-uses the same block files (format
   unchanged); invalidate nothing.

## 6. Operator deployment checklist

Pre-deploy
- [ ] Confirm candidate hash: `git -C /home/user/innova-ibd-stage1 rev-parse HEAD` → `3f05a82e…`
- [ ] Record the way the current daemon is run (unit name, args, datadir, RPC port) BEFORE touching it.
- [ ] Take a full backup copy of the currently running binary (never copy over it in place).
- [ ] Confirm the sha256 of the NEW binary to be installed matches
      `59bab7bcdf59cac37d2cda75fdd5cb347ee844ffa09c68d0f08d31c63e972c92`.
- [ ] Confirm no other worktree/daemon/wallet is being modified.

Deploy (staged, observability first)
- [ ] Start the new binary with the same datadir and arguments.
- [ ] RPC sanity: `getinfo` returns coherent chain values within 2 min.
- [ ] `getibdmetrics` (or the equivalent reason string in logs) shows a sane
      IBD reason when peers are behind/engaged and `not-ibd` when synced.
- [ ] Miner: `getmininginfo` reports `hashing=true` and
      `hashingWorkers=<threads>` while the node is synchronized (not IBD).

Abort / rollback conditions (any one triggers rollback per §5)
- [ ] Daemon fails to start or crashes within the first 10 minutes (`dmesg`/journal shows a core).
- [ ] `getmininginfo` reports `hashing=false` while the node is clearly
      synchronized and has peers and collateral state (mining must not be
      suppressed in steady state; Stage 3 should keep it bounded: at most
      BURST+GAP ≤ 2×`-ibdstaleseconds` of suppression under an uncorroborated
      fake-ahead peer, not unbounded).
- [ ] IBD stuck reason `peer-ahead-lag-uncorroborated` persists continuously for
      >10 min while the node makes no progress AND peers are genuinely
      engaged (would indicate a stiff window/flag interaction).
- [ ] Sync to tip from a few blocks behind takes materially longer than the
      pre-deploy binary measured the same way (regression in genuine sync).
- [ ] Any unexpected chain-active behavior change (accepting different tip,
      reindex, or datadir-migration messages in the log).

Post-deploy observation window
- [ ] 30 min: no IBD/miner reason flapping that suppresses hashing more than ~2× window.
- [ ] 24 h: hash rate and tip height tracking match expectation.

## 7. Test evidence (raw)

| Suite | Case count | Result |
|---|---|---|
| ibd_miner_recovery_tests | 18 (Stage 1+2: 17, Stage 3 adds the reset-evasion case) | PASS, "No errors detected" |
| ibdsemantic_tests | 31 | PASS, "No errors detected" |
| cpu_mining_controller_tests | 10 | PASS, "No errors detected" |
| rpc_getinfo_tests | 6 | PASS, "No errors detected" |
| Checkpoints_tests | 6 | PASS, "No errors detected" |
| p2p_sync_tests | 323 | 322 PASS; 1 failure = known pre-existing `already_asked_for_negative_cooldown_lifecycle` (also failed on clean master base 39a1c4ef before this branch; timing-sensitive cooldown assertion) |

## 8. Evasion analysis summary (why Stage 3 is minimal)

- Stage 2's grace clock was per-continuous-claim: any evaluation with no
  uncorroborated ahead claim reset it, so a hostile peer could drop its claim
  for a single 1-second evaluation and re-arm a full window (documented gap).
- Stage 3 adds ONE companion clock (`nIbdFreshPeerGraceLastSeen`): the window
  fully resets only once no uncorroborated ahead claim has existed for a full
  `-ibdstaleseconds` window. Suppression stays bounded at ≈ BURST+GAP ≤ 2×
  window. ENGAGED corroboration (recent block delivery or in-flight/asked
  work) remains unbounded, so genuine synchronization is never delayed. No new
  CLI knob, no new map/state, no per-peer bookkeeping, no consensus surface.
- Validated: window still expires (first burst), stale/fresh flap still can't
  restart it, brief claim-drop can no longer restart it, and a genuinely much
  later new burst still gets a fresh window.
