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


## Update — V2 bootstrap fixture

`src/test/test_innova.cpp` now performs the production startup sequence
(ownership classification → `PrepareNativeBlockIndexGeneration` → `LoadBlockIndex(true)`).

Observed after the fix, per suite in its own process:

| suite | case results | harness exit |
|---|---|---|
| `blockindex_authoritative_regtest_bootstrap_tests` | **all cases pass** (pindexBest/genesis/BI-V2 assertions satisfied) | 200 |
| `blockindex_hashindex_tests` | **all cases pass** | 200 |
| `cpu_mining_nbits_tests` | **all cases pass** | 200 |
| `blockindex_derived_state_tests` | **all cases pass** | 200 |

The NULL-tip failures (the actual BASELINE FAIL) are gone. What remains is a
**harness teardown defect**: Boost prints `Test is aborted` twice before the case
list and returns exit 200 with an empty `Test setup error:`, while reporting
`*** No errors detected`. `gdb -ex 'catch throw'` shows the throw originates inside
`libboost_unit_test_framework` *after* every case has run and after a worker thread
exits; no exception escapes the fixture constructor or destructor (both are wrapped
and print nothing). Deleting the temp datadir was disabled and did not change it.

Therefore: **cases PASS / harness exit 200 — reported as UNRESOLVED, not as PASS.**


## Update — Boost exit 200: exact localization

Lifecycle instrumentation (markers printed by the fixture itself, then removed):

```
FIXA ctor-in
FIXA ctor-out
FIXA dtor-in
FIXA dtor-out
Test setup error:
rc=200
```

Both the constructor and the destructor run to completion and no exception escapes
either (both bodies are wrapped in try/catch that would print the message).
`gdb -ex 'catch throw'` shows the only throw inside
`libboost_unit_test_framework.so` (`unit_test_main` → framework internals), raised
**after** every case has run and after a production worker thread exits.

Conclusion: the nonzero exit is a Boost.Test 1.83 global-fixture lifecycle
interaction with threads started by the V2 bootstrap, **not** an assertion
failure and **not** our fixture throwing. All cases pass in every suite.
It is reported as an OPEN harness defect, not as PASS, and the exit code is not
suppressed.

## New regression suite: `qt_tx_semantics_tests`

`src/test/qt_tx_semantics_tests.cpp` (added to `TEST_OBJS`) pins the API contract
the Qt GUI now depends on:

| case | property |
|---|---|
| `gettransaction_unknown_hash_is_read_only` | unknown txid → `false`; mempool size and best height unchanged; no containing block reported |
| `check_only_accept_does_not_add_to_mempool` | `AcceptToMemoryPool(..., fOnlyCheckWithoutAdding=true)` never inserts; `mempool.exists()` false |

Result: built into `test_innova` (build rc=0); the suite executes with no case
failures; the process exit remains 200 for the reason above.
