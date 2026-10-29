# Repository cleanup — Innova Core

Branch `chore/repo-cleanup-bootstrap`, base `014eb2e289536352a3458cd53d955df841b26b88`.

## What this report set contains

| file | contents |
|---|---|
| `file-inventory.csv` | per-file register of the working tree (1614 rows, 15 columns) |
| `aggregated-manifest.txt` | full path listings for the three aggregated trees (.git, src/obj, node_modules) |
| `file-decisions.md` | decision taxonomy, counts, and the reasoning per decision class |
| `deleted-files.md` | every removed path with evidence and commit |
| `moved-files.md` | every moved path and the reference updates |
| `root-before-after.md` | root directory tree BEFORE and AFTER |
| `platform-build-matrix.md` | Linux / Windows / ARM / BSD / Qt / daemon / CLI / tests |
| `bootstrap-validation.md` | bootstrap.sh defects, fixes, and the 20-case acceptance run |
| `dag-removal-verification.md` | DAG/IDAG residue found, removed, and what is still open |
| `test-results.md` | suites run, PASS/FAIL, and the BASELINE FAIL |
| `unresolved-items.md` | every UNVERIFIED / NOT TESTED item, stated openly |

## Totals

- inventory rows: **1614** (1553 tracked, 0 untracked, 61 ignored)
- decisions: **{'FIX': 7, 'KEEP': 1546, 'IGNORE': 61}**
- confidence: **{'LOW': 163, 'HIGH': 887, 'MEDIUM': 564}**
- categories: **{'ci/metadata': 4, 'other': 227, 'documentation': 83, 'build/script': 23, 'contrib-tooling': 103, 'resource': 125, 'source': 416, 'header': 479, 'test': 106, 'build-output': 42, 'build-manifest': 6}**

## Commits in this branch

```
6f433ef6  bootstrap: make the chain bootstrap installer actually work
2a7cce14  repo: clean the root, drop tracked local state, ignore generated output
fa0b1275  build: remove dead platform build definitions and stale CI wiring
```
Stage-1/2/4 work is committed separately (see `deleted-files.md` and `dag-removal-verification.md` for the SHAs).

## How to reproduce

```bash
cd /home/user/innova
python3 <scratch>/gen-inventory.py        # regenerates file-inventory.csv from the live tree
git log --oneline 014eb2e2..HEAD          # the commits this report describes
git show --stat <sha>                     # per-commit contents
```

Confidence is assigned only from a checkable source: a manifest object list, a
`#include` site count, or a named referencing file. Anything else is LOW
(`UNVERIFIED`) and is listed in `unresolved-items.md` — never silently promoted.

## Explicit non-claims

- Windows, ARM and BSD **cross-compiles were not executed** (no toolchains installed). Their manifests were
  validated structurally only (`make -n`, object-to-source resolution, conditional balance). See the matrix.
- The Qt GUI build does **not** pass on this tree; the defects are identified and partly fixed, and the
  `.pro` repair was reverted rather than left half-applied. See `unresolved-items.md` §Qt.
- The published bootstrap archive was **not fully downloaded/extracted** (multi-GB); the download path,
  redirect handling, archive-type dispatch and the failure modes were tested with a controlled archive.
