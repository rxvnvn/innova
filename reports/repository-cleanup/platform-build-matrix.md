# Platform build matrix

Statuses: **PASS** = real command executed successfully; **FAIL** = real command failed;
**NOT TESTED** = no toolchain / out of session budget; **STATIC** = manifest/graph check only.

| configuration | entry point | toolchain on this host | build command | result |
|---|---|---|---|---|
| Linux daemon `innovad` | `src/makefile.unix` | g++ 13, -j4 | `nice -n 19 ionice -c3 make -f makefile.unix -j4 innovad` | **PASS** — RC=0, `innovad` 173,945,928 B |
| Linux test binary | `src/makefile.unix` (`test_innova`) | g++ 13 | `… -j4 test_innova` | **PASS** — RC=0, 360,162,608 B |
| Linux CLI tool `blockindex-recovery-tool` | `makefile.unix:869` | g++ 13 | `… blockindex-recovery-cli` | **PASS** — built and used by `blockindex_recovery_tool_tests` (suite green) |
| Linux clean-checkout build graph | `makefile.unix` | — | `git worktree add` at HEAD + `make -n innovad` / `make -n test_innova` | **PASS (STATIC)** — rc=0, every prerequisite resolves |
| Qt GUI (Qt6, `USE_NATIVETOR=-`) | `innova-qt.pro` | qmake6, Qt 6.4.2 | `qmake6 …; make -j4` (shadow build in scratch) | **FAIL** — see below |
| Qt GUI (Qt6, NativeTor on) | `innova-qt.pro` | qmake6 | same | **FAIL** — vendored `src/tor/tortls.c` is incompatible with OpenSSL 3 (`SSL_SESSION` opaque, conflicting prototypes) |
| Windows (mingw cross) | `src/makefile.mingw`, `build-portable-win64.sh` | x86_64-w64-mingw32-g++ **MISSING** | — | **NOT TESTED** (manifest: 0 dead objects, `make -n` rc=0) |
| ARM | `src/makefile.arm` | arm/aarch64 g++ **MISSING** | — | **NOT TESTED** (manifest: 0 dead objects, `make -n` rc=0) |
| BSD | `src/makefile.bsd` | no BSD userland | — | **NOT TESTED** (manifest: 0 dead objects, conditionals balanced, `make -n` rc=0) |
| Packaging: snap | `snapcraft.yaml`, `snap/gui/` | snapcraft **MISSING** | — | **NOT TESTED** |
| Packaging: NSIS (Windows installer) | `share/setup.nsi` | makensis **MISSING** | — | **NOT TESTED** |

## Qt GUI: what was actually found

`qmake6 innova-qt.pro` on the base commit reports 27 missing sources, including nine DAG-era files
(`dag.cpp`, `dag_tip_frontier.cpp`, `dag_tip_frontier_metadata.cpp`, `dag_tip_live_overlay.cpp`,
`dag_tip_live_overlay.cpp`, `dag_logical_authority.cpp`, `dag_source_binding_verifier.cpp`,
`dag_tips_delta.cpp`, `blockindex_dag_restart_seam.cpp`) plus 18 removed headers. After removing those
entries locally, the build progressed and exposed real code defects, which **were fixed and are committed**:

| file | defect | fix |
|---|---|---|
| `src/qt/multisigdialog.cpp` | used removed `CTxDB`/`txdb.ReadDiskTx`/`FetchInputs` | prevouts read through `GetTransaction()`; mempool submit via `AcceptToMemoryPool()` |
| `src/qt/transactiondesc.cpp` | used removed `CTxDB::ReadDiskTx` | `GetTransaction()` |
| `src/qt/privacypage.cpp` | `onRefreshClicked()` declared, never defined (link error) | implemented: refresh + status text |

The `.pro` rewrite itself (112 missing BI-V2/Store3A sources, chainstate include paths, duplicate-entry
removal) reached the link stage but the file became structurally inconsistent and was **reverted** rather
than left half-applied. Qt therefore remains **FAIL / NOT VERIFIED** on this branch; the three source
fixes above are prerequisites for any future `.pro` repair and are kept.

## Manifest validation (structural, all four manifests)

| manifest | dead `obj/*.o` before | after | conditional balance | `make -n innovad` |
|---|---|---|---|---|
| `src/makefile.unix` | 0 | 0 | balanced | rc=0 |
| `src/makefile.arm` | 23 (42 entries) | 0 | balanced | rc=0 |
| `src/makefile.bsd` | 23 (21 entries) + 2 orphan `endif` | 0 | balanced | rc=0 |
| `src/makefile.mingw` | 23 (21 entries) | 0 | balanced | rc=0 |
