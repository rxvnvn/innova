# CMake migration — status report

Base: `b62aee3d4cdb66604f2791b8db148dca8be66958`.

| area | status |
|---|---|
| CMake infrastructure | **DONE** (root + cmake modules + presets + toolchains) |
| `innovad` from CMake | **PASS** — configure rc=0, build rc=0, binary runs |
| CLI tools (9) | targets defined; **NOT BUILT** in this pass |
| Qt GUI from CMake | target defined; **NOT BUILT** (`.pro` build is PASS from the previous pass) |
| `test_innova` from CMake | target defined; **NOT BUILT** (time budget) |
| CTest | **registered** (9 suites), **NOT EXECUTED** |
| Windows / ARM / BSD | configure files only; **NOT TESTED** (no toolchains) |
| Boost exit 200 | **OPEN** (`boost-exit200.md`) |
| NativeTor / OpenSSL 3 | **OPEN** (`nativetor-openssl3.md`) |
| Auxiliary 24 .cpp | **OPEN** (`auxiliary-tools.md`) |
| Old build system | **kept** — CMake is not yet equivalent for every target |

Files: `architecture.md`, `target-matrix.md`, `dependency-matrix.md`,
`platform-matrix.md`, `build-results.md`, `test-results.md`, `boost-exit200.md`,
`nativetor-openssl3.md`, `auxiliary-tools.md`, `migration-parity.md`,
`unresolved-items.md`.
