# Target matrix

| target | type | sources | links | definitions | platforms | build command | result |
|---|---|---|---|---|---|---|---|
| `innova_core` | static lib | 114 (explicit list, from the `innovad` link line) | Boost fs/po/thread/chrono, OpenSSL, BDB, libevent, bundled LevelDB+memenv, curl, zlib, MiniUPnPc | `BOOST_SPIRIT_THREADSAFE`, `USE_LEVELDB`, `HAVE_BUILD_INFO`, `USE_UPNP=1`, `IS_ARCH_64` (64-bit) | Linux/Windows/BSD/macOS | `cmake --build <b> --target innova_core` | **PASS** |
| `innovad` | executable | `src/init.cpp` + `innova_core` | as above | as above | Linux/Windows/BSD/macOS | `cmake --build <b> --target innovad` | **PASS** (rc=0) |
| `innova-qt` | executable | 55 Qt cpp + 54 headers + 20 forms + `bitcoin.qrc` + `innova_core` | Qt6 Core/Gui/Widgets/Network/PrintSupport/Concurrent/DBus | `QT_GUI`, `USE_QRCODE` | Linux/Windows/macOS | `cmake --build <b> --target innova-qt` | **NOT BUILT** in this pass |
| `test_innova` | executable | 90 test sources + `innova_core` | Boost.Test | as core | all | `cmake --build <b> --target test_innova` | **NOT BUILT** in this pass |
| `blockindex-recovery-tool` | executable | `src/blockindex_recovery_tool_cli.cpp` | `innova_core` | as core | all | target defined | **NOT BUILT** |
| `blockindex-catchup-tool` | executable | `src/blockindex_catchup_tool_cli.cpp` | `innova_core` | as core | all | target defined | **NOT BUILT** |
| `innova-blockindex-builder` | executable | `src/blockindex_generation_builder_tool.cpp` | `innova_core` | as core | all | target defined | **NOT BUILT** |
| `innova-blockindex-builder-lm` | executable | `src/blockindex_generation_builder_lm_tool.cpp` | `innova_core` | as core | all | target defined | **NOT BUILT** |
| `innova-txloc-build` | executable | `src/txloc_build_tool.cpp` | `innova_core` | as core | all | target defined | **NOT BUILT** |
| `innova-lm-verify` | executable | `src/lm_verify_tool.cpp` | `innova_core` | as core | all | target defined | **NOT BUILT** |
| `innova-undo-replay` | executable | `src/undo_replay_tool.cpp` | `innova_core` | as core | all | target defined | **NOT BUILT** |
| `innova-lm-fixture` | executable | `src/lm_fixture_tool.cpp` | `innova_core` | as core | all | target defined | **NOT BUILT** |
| `innova-blockindex-navdiff` | executable | `src/blockindex_navigation_diff_tool.cpp` | `innova_core` | as core | all | target defined | **NOT BUILT** |

"NOT BUILT" means the target exists and configures but no build was executed in
this pass; it is not a claim of failure or success.
