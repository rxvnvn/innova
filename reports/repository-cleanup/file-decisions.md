# File decisions

Decision vocabulary: **KEEP** (stays, with a named consumer), **FIX** (changed by this pass),
**MOVE** (relocated), **DELETE** (removed), **IGNORE** (generated/local, gitignored, not source).

Counts from `file-inventory.csv`: `{'FIX': 7, 'KEEP': 1546, 'IGNORE': 61}`.

## Why KEEP is the majority (1546 files)

This repository has already been through several demolition passes, so few files are outright dead.
A tracked file is kept when the inventory can name a consumer:

- **compiled and linked** — the basename appears in an object list of `src/makefile.unix`, `makefile.arm`,
  `makefile.bsd` or `makefile.mingw` (evidence column, e.g. `src/makefile.unix:188 (object list)`);
- **included** — a header with N `#include` sites under `src/` (evidence column, N is the real count);
- **referenced** — a file named by another tracked file (evidence column names the referencing file);
- **entry point** — a GitHub Actions workflow (executed by GitHub, not referenced from the tree).

## Why the FIX set is small and specific

| path | fix | evidence |
|---|---|---|
| `bootstrap.sh` | rewritten installer | 20/20 acceptance cases, see `bootstrap-validation.md` |
| `.gitignore` | root + generated paths added | the deleted artifact classes no longer reappear |
| `src/makefile.arm/bsd/mingw` | dead object entries removed | every `obj/*.o` now maps to an existing source |
| `.github/workflows/ci.yml` | retired `idag` trigger removed | branch no longer exists in the project |
| `.circleci/config.yml` | upstream re-clone + autotools deps removed | it built `innova-foundation/innova`, not this repo |

## Why the IGNORE set is not "junk that was hidden"

`IGNORE` rows are files still present in the working tree that are build output or local state
(`src/obj/**`, compiled binaries, logs). They are not source, they are gitignored, and they were **not**
deleted wholesale because a working build tree is legitimately large: deleting `src/obj` forces a full
rebuild of ~370 translation units. Full path listings are in `aggregated-manifest.txt`.

## Decisions that were deliberately NOT taken

- **No deletion of the 24 sources outside every manifest.** They are standalone repro/fixture/fuzz/selftest
  tools; deleting them would require proving no developer workflow uses them, which the evidence does not
  support. They are listed in `unresolved-items.md` §1.
- **No deletion of `innovaqtubuntu.sh` / `installdaemon.sh`.** They are interactive installers for Ubuntu
  16.04/18.04/20.04 and reference `qt5-default` (absent from 22.04+), so they cannot run on current
  supported releases — but they do run on the releases they name. Reported as KEEP-with-limitation rather
  than removed.
- **No conversion of `make -n` into a build claim.** See `platform-build-matrix.md`.
