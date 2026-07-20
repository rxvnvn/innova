# Stage 6B/6C — numeric verification status

Stage 6A documentation commit: `dfd02c7e75413eb4b66e1199af882a2d031319d4`.

## Executed evidence

### Preliminary moving-chain snapshot

The earlier scan at height `7,814,996` remains historical preliminary evidence and is not mixed with the stable scan below.

### Stable snapshot

After the daemon was stopped, a fresh filesystem copy of `txleveldb` was created and scanned. The scan completed with:

```text
blockindex records: 7,816,068
active records: 7,815,017
side records: 1,051
tip height: 7,815,016
tip hash: 00000021233a369416466ba2a0a6d10e5bfeb19b306150404f2f27220cbf5a74
malformed records: 0
height-link mismatches: 0
dag records: 0
```

This stable snapshot is the only input eligible for future baseline/no-DAG comparison.

The existing read-only LevelDB exporter was compiled from the repository and run against a separate 3.0G filesystem copy of `txleveldb`. The preliminary scan completed successfully and reported:

```text
blockindex records: 7,816,042
active records: 7,814,997
side records: 1,045
tip height: 7,814,996
dag records: 0
```

Both scans are newer than the Stage 2 snapshot (height 7,813,845), so each is identified by its observed tip rather than silently treated as the earlier snapshot. The exporter generated a binary index and raw DAG-prefix output; the raw scan found no `daglinks`, `epochstate`, or `dagcleanheight` records.

## Scope limitation

The executed exporter decodes block-index metadata and reconstructs active membership, but it does not export or calculate `nChainTrust`. `nChainTrust` is not serialized in the block-index value, and this run did not load the daemon's in-memory index or independently implement the PoW/PoS/PoEM trust formula. Consequently no numeric trust match, side-tip trust reconstruction, active-tip reconstruction by trust, or baseline/no-DAG comparison was performed.

```text
independent records checked: 0
trust matches: NOT TESTED
trust mismatches: NOT TESTED
baseline/no-DAG comparison: NOT TESTED
Blocker B: INCONCLUSIVE
```

The zero DAG-prefix count is supportive evidence for this particular copied snapshot only; it is not a decoded key-by-key proof of every possible historical DAG runtime input. No production code or consensus behavior was changed.

## Reproduction

The scan used the repository forensic exporter `contrib/forensics/idag_leveldb_export.cpp`, compiled as a standalone tool, with outputs outside the database copy. The input copy was made before opening LevelDB. Generated output was kept outside Git because the binary index is approximately 984 MiB.

## Required next work

Implement an independent audit module that reads the exported records and block headers, reproduces the exact trust formula (including PoS/PoEM branches), compares against baseline and no-DAG runtime values, and independently ranks all side-chain tips. Until that is executed, Blocker B remains `INCONCLUSIVE`.

## Final no-DAG runtime comparison

The syscall-traced no-DAG runtime export completed on the immutable snapshot at height 7,815,016 (tip `00000021233a369416466ba2a0a6d10e5bfeb19b306150404f2f27220cbf5a74`). The export contained 7,816,068 records (7,815,017 active and 1,051 side records). Its SHA-256 is `d4b1971dfda8debf90f87193773a4c1715bd34ee29fce8afd091cda77ab8ec69`, identical to the baseline runtime export.

A full hash-keyed comparison found 7,816,068 matched hashes and zero differences in height, parent hash, block trust, cumulative chain trust, or active flags. The active tip and side topology therefore matched exactly. Persistent DAG prefixes were absent (0 `daglinks`, `epochstate`, and `dagcleanheight` records).

The initial positional comparison and earlier truncated/missing artifacts were rejected methodology; only the complete hash-keyed artifacts are evidence. For this snapshot, DAG runtime was not required and IDAG did not affect block trust, `nChainTrust`, active-chain selection, or side-chain ordering. This conclusion is limited to the examined snapshot and does not assert behavior for hypothetical post-DAG records.

**Blocker B: CLOSED for the examined snapshot.**
