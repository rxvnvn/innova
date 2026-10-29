# Unresolved / UNVERIFIED items

Nothing here is presented as done elsewhere in the report.

## 1. Sources outside every build manifest (24 files) — KEEP, consumer UNVERIFIED

Detected by build-graph inversion: `src/**/*.cpp` whose basename appears in none of
`makefile.unix`, `makefile.arm`, `makefile.bsd`, `makefile.mingw`, `innova-qt.pro`.

| file | entry point | purpose (from its own header comment) |
|---|---|---|
| `src/bootstrap_export.cpp` | `main()` | bootstrap/export utility |
| `src/chainstate/commitstore.cpp` | library (no main) | Stage-2B durable commit store implementation; referenced by `src/chainstate/tests/chainstate_slice_tests.cpp` |
| `src/chainstate/store3a/tests/inc_tests.cpp` | `main()` | Stage-3A focused store3a suite |
| `src/chainstate/store3a/tests/repro_f0_undo.cpp` | `main()` | undo-offset reproduction driver |
| `src/chainstate/store3a/tests/repro_r2_freshopen.cpp` | `main()` | fresh-open reproduction |
| `src/chainstate/store3a/tests/repro_s3a1.cpp` | `main()` | leaf-page corruption reproduction |
| `src/chainstate/store3a/tests/repro_s3a3.cpp` | `main()` | partial-append reproduction |
| `src/chainstate/tests/chainstate_slice_tests.cpp` | `main()` | Stage-2B T0..T17 suite |
| `src/chainstate/txloc/tests/inc_tests.cpp` | `main()` | TX-LOC standalone harness |
| `src/cohortm_gensnapshot_tool.cpp` | `main()` | Cohort-M measurement harness |
| `src/fixtures_gen60.cpp` | `main()` | scale fixture generator |
| `src/fixtures_mkbar.cpp` | `main()` | scale-run base-tree helper |
| `src/fullresident_trackd_main.cpp` | `main()` | long-run residency reproducer; named by `src/test/blockindex_authoritative_live_tests.cpp` |
| `src/fullresident_trackd_probe.cpp` | library | probe used by the same suite |
| `src/leveldb/util/crc32c_verify.cpp` | `main()` | crc32c bit-exactness harness |
| `src/obj_probe.cpp` | `main()` | object/residency probe |
| `src/p2pprop_selftest.cpp` | `main()` | p2pprop self-test; documented in `TODO.md` / `PROJECT_CANONICAL.md` |
| `src/test/fixtures_gen_base_at_s.cpp` | `main()` | base-generation-at-S driver |
| `src/test/fixtures_gen_empty_base.cpp` | `main()` | empty-base generation driver |
| `src/test/fixtures_gen_scale.cpp` | `main()` | scale fixture generator |
| `src/test/fuzz/fuzz_block_header.cpp` | `LLVMFuzzerTestOneInput` | fuzz target (libFuzzer, intentionally not in makefile) |
| `src/test/fuzz/fuzz_deserialize.cpp` | `LLVMFuzzerTestOneInput` | fuzz target |
| `src/test/fuzz/fuzz_script.cpp` | `LLVMFuzzerTestOneInput` | fuzz target |
| `src/txlat_selftest.cpp` | `main()` | txlat self-test |

**Why they are not in a default target:** they are developer/diagnostic/fuzz entry points that are built ad
hoc; the fuzz targets require a libFuzzer driver. **Risk of deletion:** they encode reproducible
diagnostics for storage and IBD behaviour; deleting them would remove the only executable reproduction for
several past defects. **Action to resolve:** decide per file whether it belongs in a `make …-tools` target
or should be retired; this pass deliberately did not delete them.

## 2. LOW-confidence inventory rows

`file-inventory.csv` has LOW confidence where the automated evidence sources found nothing: these are
mostly `.idx` caches, `.Po`, `.qm`, and files whose only relationship is a directory convention. Each row
carries `evidence=UNVERIFIED`; counts are in `README.md`. They are **not** counted as KEEP-with-evidence.

## 3. Qt GUI — FAIL / NOT VERIFIED

- `innova-qt.pro` on the base commit references 27 non-existent sources (9 of them DAG-specific) and 45
  non-existent headers; three Qt source files used the removed `CTxDB` API and one slot was undefined.
- The three source defects and the undefined slot **are fixed** in this branch.
- The `.pro` repair (remove dead entries, add `src/chainstate`/`src/chainstate/store3a` include paths, add
  112 BI-V2/Store3A sources) reached the link step but left the project file structurally inconsistent; it
  was reverted to avoid shipping a broken project. **Qt build status: FAIL.**
- To finish: redo the `.pro` edit as a generated diff (script-driven, with qmake rc checked after every
  step), then build and link.

## 4. NativeTor (`USE_NATIVETOR=1`)

The bundled `src/tor/tortls.c` does not compile against OpenSSL 3 (opaque `SSL_SESSION`, conflicting
prototypes). The documented build uses `USE_NATIVETOR=-`, which is what CI does. Not fixed here.

## 5. Platform builds NOT TESTED

Windows (mingw), ARM, BSD, snap, NSIS: no toolchain on this host. Their manifests are structurally valid
(0 dead objects, balanced conditionals, `make -n` rc=0) but **no object was compiled**. Do not read the
matrix as a build result.

## 6. bootstrap.sh — real archive content NOT VERIFIED

The published `.rar` was confirmed to exist and serve ranged bytes (HTTP 206), but it was not fully
downloaded/extracted (multi-GB). Archive-to-datadir compatibility is therefore **NOT VERIFIED**; the
install contract (requires `blk*.dat`, moves old chain aside, never touches the wallet) is proven with a
controlled archive.

## 7. Legacy installers kept with a limitation

`innovaqtubuntu.sh` and `installdaemon.sh` are interactive installers for Ubuntu 16.04/18.04/20.04 and
install `qt5-default`, which no longer exists on 22.04+. They work on the releases they name and were kept;
they are not current-host installers.

## 8. DAG/IDAG — NOT COMPLETED

See `dag-removal-verification.md`. Executable residue (generation-binding domain, dead work-identity field,
dead telemetry, test identifiers) is still on `master`. The paused branch holds the removal work but is not
proven green.

## 9. `PROJECT_CANONICAL.md` reference to `README`

The governance document (untracked in this repo) mentions `README`, which this pass moved to
`doc/project-status.md`. Governance files were not edited.
