# Parity with the existing build system

| aspect | `makefile.unix` | CMake | parity |
|---|---|---|---|
| production sources | `OBJS` list (115 objects on the `innovad` link line) | 114 explicit sources + `init.cpp` in the daemon target | **equal set** (the difference is `obj/build.h`, generated) |
| C++ standard | `-std=c++11` | `CMAKE_CXX_STANDARD 11`, extensions off | equal |
| optimisation | `-O2` | `-O2` in Release | equal |
| warnings | `-Wall -Wextra -Wno-*` (7 suppressions) | same list | equal |
| `-fpermissive` | present (7 uses) | present, documented as debt | equal |
| hardening | `-fstack-protector-all -Wstack-protector`, `-Wl,-z,relro -Wl,-z,now`, `_FORTIFY_SOURCE=2` | same | equal |
| definitions | `BOOST_SPIRIT_THREADSAFE`, `IS_ARCH_64`, `USE_LEVELDB`, `HAVE_BUILD_INFO`, `USE_UPNP=1` | same | equal |
| debug info | `-g` always | `-g` only in Debug/RelWithDebInfo | **differs on purpose** — this is why the CMake binary is 8.1 MB vs 173.9 MB for the `-g` build; binary size is not used as an equivalence argument |
| LevelDB | built by its own makefile, linked as `.a` | same, via a custom command + imported target | equal |
| Qt GUI | `innova-qt.pro` + qmake | `src/qt/CMakeLists.txt` (Qt6, AUTOMOC/AUTOUIC/AUTORCC) | **not yet built** |
| tests | `TEST_OBJS` + `test_innova` target | `test_innova` target + CTest | **not yet built** |
| CLI tools | one target per tool | same 9 tools as CMake executables | defined, not built |

**Verdict:** the daemon is at parity and builds; the GUI and the test binary are
migrated in structure but not yet proven under CMake. The old build system stays in
place — removing it now would delete the only *proven* path for the GUI and tests,
which the task explicitly forbids.
