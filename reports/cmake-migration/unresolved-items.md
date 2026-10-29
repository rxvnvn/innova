# Unresolved items

1. **Boost exit 200** — open; see `boost-exit200.md`. Not masked in CTest.
2. **NativeTor / OpenSSL 3** — open; see `nativetor-openssl3.md`.
3. **Qt GUI under CMake** — target defined and configures in principle, but not
   built in this pass. The qmake build was proven PASS in the previous pass.
4. **test_innova under CMake** — target defined (90 sources), not built; CTest
   registered 9 suites but did not execute them.
5. **9 CLI tools** — targets defined, none built in this pass.
6. **24 auxiliary sources** — no CMake targets; the failure mode of the Makefile
   attempt is recorded in `auxiliary-tools.md`.
7. **Cross-platform** — toolchains absent; Windows/ARM/BSD are NOT TESTED.
8. **`-fpermissive`** — kept for parity; the tree has code that only compiles with
   it (const-iterator increments were fixed, the remaining cases are narrowing
   conversions in test code).
9. **Old build system** — retained deliberately; removal requires CMake parity for
   the GUI, tests and tools.
10. **`build/` CMake tree** — configuration left in `build/cmake-linux-release`
    (gitignored); the source tree contains no generated files.
