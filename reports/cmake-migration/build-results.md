# Build results (real commands, real exit codes)

All commands ran with `-j4`, `nice -n 19`, `ionice -c3`.

```
$ cmake -S . -B build/cmake-linux-release -DCMAKE_BUILD_TYPE=Release -DINNOVA_BUILD_QT=OFF
-- Found Boost 1.83.0 (filesystem program_options thread chrono unit_test_framework)
-- Found OpenSSL 3.0.13   -- Found ZLIB 1.3   -- Found CURL 8.5.0
-- Configuring done / Generating done
rc=0

$ cmake --build build/cmake-linux-release --target innovad -j4
rc=0
-rwxr-xr-x  8140008  build/cmake-linux-release/src/innovad

$ build/cmake-linux-release/src/innovad --help | grep -c '^  -'
132

$ build/cmake-linux-release/src/innovad -version
Innova: Unable to bind to 0.0.0.0:14530 on this computer. Innova is probably already running.
Innova: Failed to listen on any port. Use -listen=0 if you want this.
```

The `-version` output shows the daemon reaching its listen phase (port 14530 is
occupied by the operator's live node, which this task must not touch) — i.e. the
binary starts, parses arguments and initialises networking.

Symbol presence in `libinnova_core.a` (`nm -C | grep -c`):
`BlockIndexManager` 54, `CommitStore3A` 84, `txloc` 56, `g_txloc` 3.

## Fixes required by the stricter build

| file | problem | fix |
|---|---|---|
| `src/main.cpp:9552,9555` | `const std::set<uint256>::const_iterator` was incremented (accepted only because the legacy build passes `-fpermissive`) | iterator declared non-const — the correct form |
| `src/test/blockindex_live_acceptance_tests.cpp:557` | `int` returned by `AcceptActive()` assigned to `BlockIndexTipStatus` | explicit cast |

`-fpermissive` is deliberately kept in `innova_apply_common_options()` for parity
with `makefile.unix`, which uses it in 7 places; tightening it is recorded as
technical debt in `unresolved-items.md`.
