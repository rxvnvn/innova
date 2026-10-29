# Root directory: BEFORE → AFTER

## BEFORE (2026-10-29, base 014eb2e2)

```
AGENTS.md                      local governance (git-excluded, owner-managed)
bootstrap.sh                   broken installer (47 lines)
build/                         58 MB  qmake/moc/uic output
build-portable-win64.sh        Windows cross-build driver
build.properties               version source for CI
build_prod/                    20 MB  production build output
build_test/                    52 MB  test build output
.cache/                        6.7 MB local cache
CHANGELOG.md
.circleci/                     CI (cloned upstream, autotools deps)
.codegraph/                    141 MB code-intelligence index
compile_commands.json          generated
compile_commands.json.qmake.bak generated
compile-win-qt5.bat            Qt 5.3.2 helper
contrib/
COPYING
DEPLOYMENT_READINESS_3f05a82e.md  one-off report in the root
doc/  docs/
.github/  hooks/
.gitignore
innova.pro                     duplicate qmake project (51 missing sources)
innova-qt.pro                  live Qt project (27 missing sources)
innovadprebuilt.sh
innovaqtubuntu.sh
INSTALL
installdaemon.sh
Makefile                       350 KB qmake output
Makefile.perf                  279 KB qmake output (source .pro deleted)
.opencode/                     agent tooling (+ 63 MB node_modules)
PROJECT_CANONICAL.md           local governance (git-excluded)
.qmake.stash                   qmake cache
qrc_bitcoin.cpp                8.1 MB rcc output
qrc_style.cpp                  193 KB rcc output
README                         2026-10-25 status note shadowing README.md
README.md                      project readme
share/  snap/  snapcraft.yaml  src/  TODO.md  .travis.yml  .vs/
```

## AFTER (this branch)

```
AGENTS.md                      unchanged (owner-managed, git-excluded)
bootstrap.sh                   repaired installer (documented contract, 20/20 cases)
build-portable-win64.sh        unchanged (Windows path, uses makefile.mingw)
build.properties               unchanged (CI version source)
CHANGELOG.md  COPYING  INSTALL  README.md  snapcraft.yaml
.circleci/                     upstream re-clone + autotools deps removed
contrib/  doc/  docs/  .github/  hooks/  share/  snap/  src/
.gitignore                     root + generated paths added
innova-qt.pro                  unchanged in this pass (see unresolved-items §Qt)
innovadprebuilt.sh  innovaqtubuntu.sh  installdaemon.sh   kept (legacy installers)
.opencode/                     kept (agent tooling; node_modules is gitignored)
PROJECT_CANONICAL.md  TODO.md  local governance (git-excluded)
reports/repository-cleanup/    this report set
```

## Root item disposition

| item | purpose | part of upstream project? | generated? | belongs outside the source tree? |
|---|---|---|---|---|
| `bootstrap.sh` | installs published chain bootstrap into a datadir | yes | no | no — it is a repo tool |
| `build-portable-win64.sh` | Windows/mingw cross-build driver | yes | no | no |
| `innova-qt.pro` | live Qt GUI project | yes | no | no |
| `innova.pro` | **removed** duplicate project | no (dead duplicate) | no | — |
| `snapcraft.yaml`, `snap/`, `share/`, `hooks/` | packaging/install assets | yes | no | no |
| `.github/`, `.circleci/` | CI configuration | yes | no | no |
| `.opencode/` | agent tooling config | no (local tooling) | partly (node_modules) | arguably yes; kept, node_modules gitignored |
| `PROJECT_CANONICAL.md`, `TODO.md`, `AGENTS.md` | owner governance | no | no | kept in place by owner decision (git-excluded) |
| `README.md` | project readme | yes | no | no |
| `doc/project-status.md` | moved from root `README` | yes | no | no |
| `reports/` | this report set | no | no | no (deliverable) |

All previously present generated trees (`build/`, `build_prod/`, `build_test/`, `.cache/`, `.codegraph/`,
`Makefile`, `Makefile.perf`, `qrc_*.cpp`, `compile_commands.json*`, `.qmake.stash`) are gone from the root
and covered by `.gitignore`.
