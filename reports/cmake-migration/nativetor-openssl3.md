# NativeTor / OpenSSL 3 — open blocker

Exact incompatibilities in the vendored Tor TLS layer against OpenSSL 3.0.13:

| location | problem |
|---|---|
| `src/tor/tortls.h:140` | redeclares `SSL_SESSION_get_master_key` with a non-const `SSL_SESSION*`; OpenSSL 3 declares it with `const SSL_SESSION*` → conflicting types |
| `src/tor/tortls.c:1516` | reads `session->ciphers`; `SSL_SESSION` is opaque in OpenSSL 3 |
| `src/tor/tortls.c:2410` | redeclares `SSL_get_client_random` with `SSL*` instead of `const SSL*` |
| `src/tor/tortls.c:2423` | redeclares `SSL_get_server_random` with `SSL*` instead of `const SSL*` |
| `src/tor/tortls.c:1770` | `SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION` used in a preprocessor expression; no longer a macro constant |

No internal struct field access, certificate check or TLS policy was weakened and
the component was not removed.

## CMake integration

NativeTor is now an explicit, OFF-by-default option:
`-DINNOVA_USE_NATIVETOR=ON` adds `src/tor/tor_main.c`,
`src/ed25519/donna/ed25519_tor.c`, `USE_NATIVETOR=1` and `src/tor` to the include
path. It is **not** required for any other configuration, so it cannot block a
CMake build of the daemon, the GUI or the tests.

Status: **NOT FIXED / NOT VERIFIED**. A real port to the OpenSSL 3 accessor API is
required and cannot be validated in this environment.
