# Test results

Binary: `src/test_innova`, rebuilt from this branch (`make -f makefile.unix -j4 test_innova`, RC=0).
Every suite ran in its own process (`--run_test=<suite>`), never in one shared run.

## Suites executed in this pass

| suite | result | detail |
|---|---|---|
| `blockindex_derived_state_tests` | **PASS** | `*** No errors detected` |
| `blockindex_startup_seam_tests` | **PASS** | `*** No errors detected` |
| `candidate_frontier_tests` | **PASS** | `*** No errors detected` |
| `cpu_mining_controller_tests` | **PASS** | `*** No errors detected` |
| `blockindex_recovery_tool_tests` | **PASS** | green once its sibling CLI binary is built (`make … blockindex-recovery-cli`); without it the suite fails with `./blockindex-recovery-tool: not found` (exit 127) — a harness prerequisite, not a code defect |
| `blockindex_authoritative_regtest_bootstrap_tests` | **FAIL (BASELINE)** | `TestingSetup must leave pindexBest non-NULL on a fresh regtest datadir` |

## BASELINE FAIL — pre-existing, not caused by this branch

`src/test/test_innova.cpp:50` calls `LoadBlockIndex(true)` on a fresh temp datadir **without** the
production startup sequence (ownership classification → `PrepareNativeBlockIndexGeneration` →
`LoadBlockIndex`). With no published V2 generation, `InitBlockIndexAuthoritative` fails and
`pindexBest`/`pindexGenesisBlock` stay NULL.

Evidence that this is pre-existing:

- the base commit is `014eb2e2 test: revert the V2 fixture bootstrap change — it is red on master`;
- the failing suite file itself documents the expected fixture (`blockindex_authoritative_regtest_bootstrap_tests.cpp:1-11`);
- neither `src/test/test_innova.cpp` nor the bootstrap path was modified by this branch.

The fix was implemented earlier in this session (fixture performs the production sequence) and reproduced
the assertion green, but it also made the global fixture teardown return Boost status 200, so it was
**reverted** rather than shipped half-verified. It is listed in `unresolved-items.md` §V2-fixture.

## Suites previously reported failing (status not changed by this branch)

`blockindex_hashindex_tests`, `blockindex_authoritative_live_tests`, `blockindex_window2_e2e`,
`cpu_mining_nbits_tests`, `checkpoint_v2_authoritative_tests`, `checkpoint_v2_startup_tests`,
`blockindex_block94_maturity_tests`, `p2p_sync_tests` — all fail on this tree with the same NULL-tip or
NULL-dereference signature that follows from the baseline fixture defect above. They were **not** disabled,
skipped or removed.

## Build gates

| gate | command | result |
|---|---|---|
| daemon | `make -f makefile.unix -j4 innovad` | **PASS** RC=0, 173,945,928 B |
| tests | `make -f makefile.unix -j4 test_innova` | **PASS** RC=0, 360,162,608 B |
| CLI tool | `make -f makefile.unix -j4 blockindex-recovery-cli` | **PASS** |
| clean-checkout graph | `git worktree add … HEAD` + `make -n innovad` / `make -n test_innova` | **PASS (static)** rc=0 |
| Qt GUI | `qmake6 innova-qt.pro && make -j4` | **FAIL** — see `platform-build-matrix.md` |

All builds ran with `-j4`, `nice -n 19`, `ionice -c3`; no full-core build was issued.
