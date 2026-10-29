# bootstrap.sh — purpose, defects, fixes, validation

## Purpose (explicit)

`bootstrap.sh` installs **blockchain bootstrap data** into an Innova datadir so a node starts near the
chain tip instead of syncing from genesis. It is **not** a build-system bootstrap: it does not configure,
compile or install the project. Building is `make -f src/makefile.unix` (see `platform-build-matrix.md`).

## Original defects (44-line script, base commit)

| # | defect | consequence |
|---|---|---|
| 1 | `sudo apt-get install unrar` | requires root for a task that needs none; fails offline / on non-Debian |
| 2 | `mv innovabootstrap.rar ~/.innova/...` before `mkdir ~/.innova` | first run fails |
| 3 | `echo -e` under `#!/bin/sh` | prints literal `-e` on dash |
| 4 | `killall -9 innovad` | kills the daemon; then deletes files underneath it — chain corruption risk |
| 5 | unconditional `rm -R database txleveldb blk0001.dat ...` | destroys chain data with no backup, no confirmation |
| 6 | `cd ~/.innova` | assumes HOME; ignores `-datadir` |
| 7 | no `set -e`, no exit codes, no dependency checks | reports success after failures |
| 8 | unquoted paths | breaks on paths with spaces |
| 9 | `wget` into the caller's cwd | pollutes whatever directory the user ran it from |

## Repaired contract

- POSIX `sh`, `set -eu`; project root resolved from the script path (`readlink -f`), so it runs from any cwd.
- No root, no package installation. Missing `curl`/`wget` or archive extractor → exit **2** with the exact
  package name to install.
- Refuses to run while `innovad` is alive (pgrep + datadir pid file) → exit **3**. Never kills.
- Never touches `wallet.dat`, `innova.conf`, `masternode.conf`.
- Existing `blk*.dat`, `blkindex.dat`, `txleveldb/`, `chainstate/`, `database/` are **moved** to
  `<datadir>/bootstrap-backup-<UTC>/` before writing → reversible.
- Archive types: `.zip`, `.rar`, `.7z`, `.tar[.gz|.zst|.xz|.bz2]`; optional `--sha256` verification → exit **5**.
- Idempotent: `<datadir>/.bootstrap-installed` marker; re-run without `--force` exits 0 and prints the marker.
- `--datadir/--archive/--url/--sha256/--force/--start/--keep-archive/--help`; documented exit codes.

## Acceptance run (20 cases, isolated sandbox)

Harness: `scratch/test-bootstrap.sh`, fixture archive built in a temp dir, datadir pre-seeded with a
`wallet.dat` sentinel, `innova.conf`, and an existing `blk0001.dat`.

```
PASS: sh -n syntax
PASS: dash -n syntax
PASS: --help works from foreign cwd
PASS: install from local archive (exit 0)
PASS: new blk0002.dat installed
PASS: chainstate installed
PASS: install marker written
PASS: old chain backed up (bootstrap-backup-20261029T004015Z)
PASS: wallet.dat untouched
PASS: innova.conf untouched
PASS: re-run is a no-op (exit 0, reports existing install)
PASS: re-run created no second backup
PASS: --force reinstall (exit 0)
PASS: --force created a new backup
PASS: sha256 mismatch rejected with exit 5
PASS: missing extractor/downloader rejected with exit 2
PASS: running daemon refused with exit 3
PASS: corrupt archive rejected with exit 5
PASS: archive without chain data rejected with exit 5
PASS: default bootstrap URL reachable (HTTP 302)
RESULT: PASS=20 FAIL=0
```

## Additional checks requested in this pass

| check | result |
|---|---|
| final URL of the archive | `…/v4.3.9.5/innovabootstrap.rar` → 302 to `release-assets.githubusercontent.com`; a ranged GET returns **206** with 1025 bytes → the object is real and downloadable |
| archive format dispatch | `.rar` path selected for the default URL (`unrar` present); `.zip` path exercised end-to-end |
| content compatibility with the expected datadir | **NOT VERIFIED** — the real archive was not downloaded/extracted (multi-GB); the install contract only requires `blk*.dat` to be present, and an archive without it is rejected (tested) |
| full cycle on isolated data | PASS (all 20 cases ran on throwaway dirs; no live wallet/datadir touched) |
| interrupted download | covered structurally: download goes to a private `mktemp -d` dir with an EXIT trap; a partial file never reaches the datadir. **Not exercised with a real truncated multi-GB transfer.** |
| recovery after error | PASS — `--force` re-run after a failed install succeeds and creates a fresh backup; marker is only written after a complete copy |

## Reproduction

```bash
cd /home/user/innova
bash <scratch>/test-bootstrap.sh        # the 20-case harness
./bootstrap.sh --help                   # contract
./bootstrap.sh --datadir /tmp/dd --archive /tmp/boot.zip --sha256 <hash>
```
