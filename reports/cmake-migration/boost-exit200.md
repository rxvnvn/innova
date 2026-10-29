# Boost.Test exit 200 — open defect

## What is proven

1. All test cases execute and pass (`*** No errors detected`, every case listed as
   passed in `--report_level=detailed`).
2. The global fixture constructor completes: markers printed at `ctor-in` **and**
   `ctor-out`.
3. The global fixture destructor completes: markers at `dtor-in` **and**
   `dtor-out`.
4. Neither body throws: both are wrapped in `try/catch` that prints the exception
   text; nothing was printed.
5. `gdb -batch -ex 'catch throw'` shows a single C++ throw inside
   `libboost_unit_test_framework.so.1.83.0`
   (`unit_test_main` → framework internals), raised **after** the case list has
   finished and after a worker thread exits.
6. Removing the temp-datadir deletion from the destructor does **not** change the
   exit code.
7. `--log_level=all --report_level=detailed` shows no failed assertion anywhere.

## What is NOT proven

The earlier hypothesis ("threads started by the V2 bootstrap confuse the
framework") is **not** proven. The framework throws from its own code; which
internal invariant it is checking has not been identified. The remaining candidates:

- Boost.Test 1.83 global-fixture bookkeeping around a thread that is still joined
  during framework shutdown;
- interaction between `BOOST_GLOBAL_FIXTURE` and a fixture that opens a LevelDB
  (the case that made the difference versus the base commit, which exits 0).

## Status

**OPEN.** Exit code is not suppressed, teardown is not disabled and no suite is
disabled. The suites must be reported as "cases PASS / process exit 200".
