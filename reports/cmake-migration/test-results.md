# Test results

## CTest

`enable_testing()` is called before `add_subdirectory(src)`, so the suites
registered in `src/test/CMakeLists.txt` are visible:

```
$ cd build/cmake-linux-release && ctest -N
Total Tests: 9
  blockindex_authoritative_regtest_bootstrap_tests
  blockindex_derived_state_tests
  blockindex_startup_seam_tests
  candidate_frontier_tests
  cpu_mining_controller_tests
  qt_tx_semantics_tests
  blockindex_hashindex_tests
  cpu_mining_nbits_tests
  invalidate_reconsider_tests
```

Each test runs `test_innova --run_test=<suite> --log_level=error` in its own
process and its **exit code is reported unchanged** — no `PASS_REGULAR_EXPRESSION`,
no output parsing.

**CTest was not executed in this pass**, because `test_innova` was not built from
CMake (the test target compiles 90 translation units; the session budget was spent
on the daemon and the infrastructure). Status: **REGISTERED / NOT EXECUTED**.

## Legacy (Makefile) test evidence carried over

From the previous pass, on the same sources:

| suite | cases | process exit |
|---|---|---|
| `blockindex_authoritative_regtest_bootstrap_tests` | all pass | 200 |
| `blockindex_derived_state_tests` | all pass | 200 |
| `blockindex_hashindex_tests` | all pass | 200 |
| `cpu_mining_nbits_tests` | all pass | 200 |
| `qt_tx_semantics_tests` (new) | all pass | 200 |

The nonzero exit is the Boost harness defect documented in `boost-exit200.md`;
it is not masked here and would be reported faithfully by CTest.
