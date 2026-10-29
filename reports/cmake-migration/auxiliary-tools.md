# Auxiliary sources outside the build manifests (24 files)

Classification carried over and re-checked against the CMake graph. None of them is
part of `innova_core`; each is a standalone entry point or a library used only by
another standalone harness.

| file | entry point | purpose | CMake target |
|---|---|---|---|
| `src/bootstrap_export.cpp` | `main()` | bootstrap export utility | none (not integrated) |
| `src/chainstate/commitstore.cpp` | library | Stage-2B durable commit store | not built (used by `chainstate_slice_tests.cpp`) |
| `src/chainstate/store3a/tests/inc_tests.cpp` | `main()` | Store3A focused suite | none |
| `src/chainstate/store3a/tests/repro_f0_undo.cpp` | `main()` | undo-offset repro | none |
| `src/chainstate/store3a/tests/repro_r2_freshopen.cpp` | `main()` | fresh-open repro | none |
| `src/chainstate/store3a/tests/repro_s3a1.cpp` | `main()` | leaf-page corruption repro | none |
| `src/chainstate/store3a/tests/repro_s3a3.cpp` | `main()` | partial-append repro | none |
| `src/chainstate/tests/chainstate_slice_tests.cpp` | `main()` | Stage-2B T0..T17 suite | none |
| `src/chainstate/txloc/tests/inc_tests.cpp` | `main()` | TX-LOC harness | none |
| `src/cohortm_gensnapshot_tool.cpp` | `main()` | Cohort-M measurement | none |
| `src/fixtures_gen60.cpp` | `main()` | scale fixture generator | none |
| `src/fixtures_mkbar.cpp` | `main()` | scale base-tree helper | none |
| `src/fullresident_trackd_main.cpp` | `main()` | residency reproducer | none |
| `src/fullresident_trackd_probe.cpp` | library | probe for the same suite | none |
| `src/leveldb/util/crc32c_verify.cpp` | `main()` | crc32c bit-exactness | none |
| `src/obj_probe.cpp` | `main()` | residency probe | none |
| `src/p2pprop_selftest.cpp` | `main()` | p2pprop self-test (needs `-DP2PPROP_SELFTEST`) | none |
| `src/test/fixtures_gen_base_at_s.cpp` | `main()` | base-generation-at-S driver | none |
| `src/test/fixtures_gen_empty_base.cpp` | `main()` | empty-base generation driver | none |
| `src/test/fixtures_gen_scale.cpp` | `main()` | scale fixture generator | none |
| `src/test/fuzz/fuzz_block_header.cpp` | `LLVMFuzzerTestOneInput` | fuzz target | none (needs a libFuzzer driver) |
| `src/test/fuzz/fuzz_deserialize.cpp` | `LLVMFuzzerTestOneInput` | fuzz target | none |
| `src/test/fuzz/fuzz_script.cpp` | `LLVMFuzzerTestOneInput` | fuzz target | none |
| `src/txlat_selftest.cpp` | `main()` | txlat self-test | none |

**Why no CMake targets yet:** the previous attempt in the Makefile
(`standalone-tools` linking `$(OBJS)` minus `init.o`) failed with 102 link errors,
and the same recipe under CMake would fail the same way — the self-tests need
their own compile definitions (`p2pprop_selftest` requires `-DP2PPROP_SELFTEST`,
the crc32c harness needs the leveldb `util/` sources) and their own link sets.
Shipping an unverified target was explicitly avoided. **Status: OPEN**, with the
per-file facts above as the input for the next step.
