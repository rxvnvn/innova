# Dependency matrix

Detected during `cmake -S . -B build/cmake-linux-release` on this host:

| dependency | required by | detection | version found | notes |
|---|---|---|---|---|
| Boost | core | `find_package(Boost 1.60 COMPONENTS filesystem program_options thread chrono [unit_test_framework])` | **1.83.0** | system package |
| OpenSSL | core | `find_package(OpenSSL)` | **3.0.13** | Crypto + SSL; NativeTor is incompatible with this version (see its report) |
| BerkeleyDB (db_cxx) | wallet | `find_path`/`find_library` | found | `innova::bdb` |
| libevent | core | `find_path`/`find_library` | found | `innova::event` |
| curl | core | `find_package(CURL)` | **8.5.0** | |
| zlib | core | `find_package(ZLIB)` | **1.3** | |
| MiniUPnPc | core | `find_library` | found | optional; disabled with `-DINNOVA_USE_UPNP=OFF` if missing |
| LevelDB | block index V2 hashindex | bundled `src/leveldb`, custom command + imported static libs | bundled | upstream ships no CMake build |
| Qt6 | GUI only | `find_package(Qt6 COMPONENTS Core Gui Widgets Network PrintSupport Concurrent DBus)` | 6.4.2 (host) | not configured in this pass |
