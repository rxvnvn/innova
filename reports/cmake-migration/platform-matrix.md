# Platform matrix

| configuration | entry point | toolchain on host | configure | build |
|---|---|---|---|---|
| Linux x86_64 (daemon+CLI) | `CMakePresets.json: linux-release` | g++ 13, cmake 3.28 | **PASS** (rc=0) | **PASS** (`innovad`, rc=0) |
| Linux x86_64 + Qt + tests | `CMakePresets.json: linux-qt` | Qt 6.4.2 present | not run | **NOT TESTED** |
| Windows x86_64 (MinGW-w64) | `cmake/toolchains/mingw-w64.cmake` | `x86_64-w64-mingw32-g++` **MISSING** | **NOT TESTED** | **NOT TESTED** |
| ARM 32-bit HF | `cmake/toolchains/armhf.cmake` | `arm-linux-gnueabihf-g++` **MISSING** | **NOT TESTED** | **NOT TESTED** |
| BSD | `CMakePresets.json: bsd` (native on a BSD host) | no BSD userland | **NOT TESTED** | **NOT TESTED** |
| macOS | `PlatformOptions.cmake` branch | none | **NOT TESTED** | **NOT TESTED** |

Platform handling is centralized in `cmake/PlatformOptions.cmake`: OS macros
(`LINUX`/`BSD`/`WIN32`/`MAC_OSX`), `IS_ARCH_64`, the system-library set per OS and
the scrypt assembly selection by `CMAKE_SYSTEM_PROCESSOR`. Nothing platform
specific leaks into the shared target definitions.

`configure PASS` is never reported as `build PASS`; for the three cross targets no
configure was even attempted because the compilers are absent.
