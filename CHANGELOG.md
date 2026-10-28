# Changelog

## [Unreleased]

### Project direction — 2026-10-25

The owner’s priority is recovery and stabilization of the legacy network after the recent hard fork. ASERT is deferred indefinitely. Completed Stage C/D remain historical research only; their results are preserved, but no Stage D.1, parameter selection, ASERT activation, DGW change, new hard fork, or mandatory client upgrade is planned. Parameters and activation remain unset. Resuming ASERT requires a new explicit owner decision.

Active work continues on P2P-01 block propagation, Block Index V2/Store3A stability and compatibility, and outstanding recovery (REC) tasks under their existing approval and verification requirements. Completed research and the accepted REC-03 operational recovery do not establish overall release readiness; the release verdict remains HOLD.

This entry records a documentation-only policy decision, not a shipped consensus change.

### Documentation-only status annotations — 2026-10-25 (DOC-01 audit)

This section records documentation-only consistency edits (DOC-01, no code/node/consensus/storage changes):

- PROJECT_CANONICAL.md updated to v1.21: WALLET-02 Stage H recorded as AUTHORIZED + RUNNING (isolated copy `/home/user/innova-stagef`); outcome UNKNOWN and not to be cited as complete. Stage G verdict remains NOT READY. Overall release verdict remains HOLD.
- Stage-F→Stage-G contradiction recorded: the Stage-F report's hypothesis that the isolated test hang was a code regression was LATER REFUTED by Stage G (root cause: stale-object ABI mismatch from a copied `obj/` directory; fixed by `make clean` + full rebuild; rule: never move `obj/` between trees). Original Stage-F wording preserved with an explicit dated note.
- README.md: Maturity corrected to 65 blocks per source (`nCoinbaseMaturity = 65`, src/main.cpp:96; the historical "75" corresponds to the wallet spend-maturity heuristic, src/main.cpp:3884); source-verified notes added for block time, stake interest, P2P user-agent (`/innova:5.0.5/`); IIP-0001..0010 remain documented as historical proposals — note (already present) states none is active on current mainnet (gates inert, `MAINNET_EXPERIMENTAL_V5_DISABLED_HEIGHT = 999999999`); BIP39/BIP44 CoinType 116 claims marked as unverified against this source tree.

### Fixed — 2026-10-29

- **Block Index V2: tip hash index could be silently truncated** (node 31.77.57.75
  failed closed on every start with `post-S tip materialization failed: walk
  lookup failed`). A repeated record in the committed tip window made
  `BlockIndexHashIndex::Put` reject the second mapping; the rebuild passed that
  boolean to `StreamVisit`, which treats a false callback as a caller-requested
  early stop and therefore reported success, wrote the completion marker with the
  full count and left every later slot — including the newest tip — unindexed.
  `StreamVisit` now reports early stops, the rebuild resolves duplicates to their
  first slot, surfaces real errors and refuses to write the marker unless the pass
  completed and `tip.meta.tipHash` resolves; the reconcile verify path accepts a
  duplicate mapping instead of forcing a rebuild on every Open. Regressions:
  `r3g_duplicate_hash_sink_conflict`, `r3h_real_tip_store_walk_after_rebuild`.
- **getinfo wallet I/O amplification**: `getinfo` walked `mapWallet` six times
  (five aggregates plus the staking-balance pass) and each per-transaction
  `IsTrusted()`/`GetDepthInMainChain()` re-resolved the block hash through the
  block index — ~467 MB of store reads and ~1.43 s per call every 60 s on
  31.77.247.41, enough to evict page cache and stall `msghand`. `CWallet::GetAggregates()`
  now computes balance/unconfirmed/immature/stake/newmint in one pass with
  identical predicates; wallet passes per `getinfo` drop from 6 to 2. Regression:
  `getinfo_wallet_aggregates_match_individual_getters`.

### Fixed

- Fixed newly mined reward transactions appearing in Innova-Qt one block late. *(Historical entry from the inherited upstream changelog; retained for history. Not part of the 2026-10-25 experimental work and not verified against this dirty source tree — see PROJECT_CANONICAL.md C03.*
