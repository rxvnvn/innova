# CMake architecture

```
CMakeLists.txt                 project, options, build.h generation, subdirs
CMakePresets.json              5 configure presets + build/test presets
cmake/
  CompilerOptions.cmake        innova_apply_common_options(): warnings, -O2,
                               hardening (stack protector, relro/now), pthread
  PlatformOptions.cmake        innova_apply_platform_definitions(): OS macros,
                               IS_ARCH_64, scrypt assembly by architecture
  Dependencies.cmake           imported targets for every third-party library
  toolchains/mingw-w64.cmake   Windows cross toolchain
  toolchains/armhf.cmake       ARM 32-bit hard-float cross toolchain
src/CMakeLists.txt             innova_core (STATIC) + innovad + 9 CLI tools
src/core-sources.cmake         explicit production source list (no GLOB)
src/qt/CMakeLists.txt          Qt6 Widgets app, AUTOMOC/AUTOUIC/AUTORCC + lrelease
src/qt/qt-sources.cmake        explicit Qt source/header/form/translation lists
src/test/CMakeLists.txt        test_innova + CTest registration
src/test/test-sources.cmake    explicit test source list
```

Design rules applied:

- **No `file(GLOB)`** anywhere; every source is listed explicitly and generated
  from the authoritative manifests (`makefile.unix` link line, `TEST_OBJS`,
  `innova-qt.pro`) so the CMake graph is auditable against the old one.
- Third-party libraries are **imported targets** (`innova::bdb`, `innova::event`,
  `innova::miniupnpc`, `innova::leveldb`, `innova::memenv`, plus the upstream
  `Boost::*`, `OpenSSL::*`, `ZLIB::ZLIB`, `CURL::libcurl`).
- No global `include_directories()`/`add_definitions()`/`CMAKE_CXX_FLAGS` edits:
  everything is `target_*` on `innova_core` and propagated through its
  `PUBLIC` interface.
- One `innova_core` static library holds the production source graph; the daemon
  main (`init.cpp`), the Qt main and the Boost.Test main are **not** in it, so
  each executable has exactly one entry point.
- The bundled LevelDB keeps its upstream makefile and is built by a custom
  command into an imported static library (the project ships no CMake build).
- `build.h` is generated into `${CMAKE_BINARY_DIR}/obj` by `share/genbuild.sh`
  and never written into the source tree.
