# Deep-Fork Recovery Runtime Validation

## Executive Summary

**Runtime verdict: CONFIRMED — autonomous chain recovery after restart with
the corrected binary from the observed 2,729-block Innova mainnet fork.**

A node on VPS `194.87.24.207`, using its preserved real forked datadir,
discovered and validated a stronger competing chain through normal P2P
operation. Innova's normal `SetBestChain` / `Reorganize` path disconnected
2,729 blocks from the previously active fork, connected 10,628 competing-chain
blocks, reached height 7,897,145, and continued synchronizing.

No datadir reset, `invalidateblock`, `reconsiderblock`, manual invalidation,
manual active-chain selection, or manual suffix import was used as the recovery
mechanism. Restarting the node with the corrected binary was an operator action;
the chain discovery, validation, trust comparison, and reorganization after
that restart were autonomous.

This document labels direct runtime observations as **FACT** and conclusions
drawn from source and history as **INFERENCE**. The result is strong evidence
for recovery from this observed class of deep fork. It is not a proof that
every possible future fork depth and topology is recoverable.

## Incident and Test Subject

**FACT — forensic test subject:** the recovery used the existing mainnet
datadir from the forked node, rather than a synthetic topology. Its initial
active tip was:

- height: 7,889,246
- hash: `00000000a5a366ce70b968ed40b5c246a636c14e86532fcd1b881955621bc541`

The datadir was deliberately preserved. The corrected binary included the
checkpoint, IBD classification, locator, and candidate-branch proof-of-stake
fixes described below.

## Confirmed Fork Boundary

**FACT — previously established fork boundary:** the last common block was:

- height: 7,886,517
- hash: `000000004ab5487d20b55d99a8a435e584d8c10c2d619c2b3d9cfe5729414d02`

Divergence began at height 7,886,518. Two known hashes at that height were:

- `b08b5c94826e264dd8b01e168277c68c18a73f5de1d03068bf381bed135ad11e`
- `003e7206967057672b61aeb5616446e596ba383b1b603bdb65352351e8099498`

The available evidence used for this document does not establish which of
these two hashes belonged to the eventually selected chain, so neither is
arbitrarily labelled “main” or “fork” here.

The initial active suffix was 2,729 blocks beyond the common ancestor:

```text
7,889,246 - 7,886,517 = 2,729
```

## Initial Failure Mode

**FACT:** before the fixes below, the node remained on the weaker active fork
despite healthy peers advertising a substantially higher chain. It could not
index enough of the competing suffix to make that branch eligible to win the
chain-trust comparison.

**INFERENCE from source and the staged runtime investigations:** recovery was
not blocked by one defect. Several independent gates prevented discovery,
admission, or branch-correct validation of the competing chain. Removing only
one gate exposed the next one.

## Recovery Blockers Identified

### IBD checkpoint trap

Commit `9e40433b16b88ab61af58efea06fa27e5faa69e8`
(`fix(checkpoints): allow competing chains during IBD`) removed an IBD-only
synchronized-checkpoint trap.

**INFERENCE from the pre-fix and current source:** the old IBD `CheckSync`
path derived an automatic synchronized-checkpoint boundary from the current
active best chain. Once fork F became active, a competing block below that
moving boundary could be rejected in `AcceptBlock` before `WriteToDisk` and
`AddToBlockIndex`. The competing branch could therefore not accumulate
`nChainTrust` and could never reach `Reorganize`.

The fix makes this synchronized-checkpoint policy check permissive during IBD.
It does not bypass hardened checkpoints: `AcceptBlock` still independently
calls `Checkpoints::CheckHardened`.

### IBD peer-snapshot lock contention

Commit `54e70b3c40ab535bf18637f7e9e2e4aba45992bd`
(`fix(ibd): make sync-state classification lock-safe`) corrected an unavailable
peer-snapshot case.

**INFERENCE from the pre-fix source:** `IsInitialBlockDownload` used
`TRY_LOCK(cs_vNodes)`. Failure to obtain the lock left the peer evidence empty,
which could turn “peer state unavailable” into `NOT_IBD`. Identical chain state
could consequently be classified differently according to lock availability.
The corrected semantics conservatively retain IBD when the peer snapshot is
unavailable, without replacing the try-lock with a blocking `cs_vNodes` lock.

### Stale ahead-peer evidence

Commit `4404ec05c0aaa73c456204c5ca0791341484d276`
(`fix(ibd): preserve known ahead-peer evidence`) corrected a second false
`NOT_IBD` path.

**INFERENCE from the pre-fix source:** a connected peer's
`nBestKnownHeight` could remain far ahead while `nLastHeightUpdate` exceeded
the freshness interval. During historical recovery, the peer can continue
delivering blocks without advertising a new tip, so expiration of the height
update timestamp is not evidence that the local node caught up. Known
connected ahead-peer height is now retained as IBD evidence. The classification
can still leave IBD when the node is genuinely caught up; it is not a permanent
sticky-IBD latch.

### Deep-fork locator truncation

Commit `5966fe98a0f77f8a834b23a4cd00c9d0c157e32e`
(`fix(p2p): allow block locators to span deep forks`) removed the artificial
`nStep > 1024` termination from `CBlockLocator`.

**INFERENCE from source and the real fork depth:** although the locator used
exponential backoff, the cutoff stopped its walk before it included the known
non-genesis common ancestor of this 2,729-block fork. A healthy peer could then
resolve the locator only at genesis and announce old historical ranges already
known locally. Removing the cutoff retained logarithmic/exponential backoff
while allowing the walk to continue to genesis, so a sufficiently deep common
ancestor can be represented.

**FACT — qualitative runtime confirmation:** after deployment of this fix,
the node began receiving competing-chain blocks near the established divergence
region instead of repeatedly receiving only historical ranges from genesis.

### Candidate-branch PoS modifier context

Commit `43d189a9a2b5d4c3d9eb3ef617dadca6e3224f42`
(`fix(pos): validate stake context against candidate branch`) corrected two
related candidate-branch validation dependencies.

**INFERENCE from the pre-fix source:** legacy `GetKernelStakeModifier`
traversal used active-chain `pnext` links. While F remained active, validating
a proof-of-stake block on candidate branch M could therefore obtain modifier
context from F rather than M. The branch-aware path now traverses the candidate
block's `pindexPrev` ancestry.

### Candidate-branch stake-source transaction lookup

**INFERENCE from source:** `CTxDB::ReadDiskTx` uses the global transaction
index, which is populated by `ConnectBlock`, not merely by `WriteToDisk` or
`AddToBlockIndex`. A transaction in indexed-but-unconnected M was therefore
unavailable while validating a later M proof-of-stake block. This formed a
circular dependency:

```text
M becomes active
  -> ConnectBlock indexes M transactions globally
  -> a later M stake source becomes resolvable

but

the later M stake must validate
  -> M accumulates sufficient chain trust
  -> M can become active
```

The corrected lookup retains global `ReadDiskTx` as its fast path. When that
lookup cannot prove the source in the candidate context, it searches only the
candidate branch's unconnected ancestry, stopping at the active-chain ancestor.
It does not scan unrelated branches.

The security property is essential: a transaction does not become a valid
stake source merely because the same transaction ID exists somewhere in block
storage. The source must belong to the candidate block's ancestry.

## Observed Intermediate PoS Blockers

**FACT:** block
`7a770e77e22a4aaa2101e1946493449b1baeedf428fd23439816390a498fcb5c`,
reported by the explorer at height 7,886,749, repeatedly reached `AcceptBlock`
and failed proof-of-stake validation. The available runtime excerpt did not
identify its exact inner failing predicate, so this document does not assign
one.

**FACT:** for block
`c427e5103003db89d67b276f2dabd51eefa0345ad56c2ac91bf5d838fbe2eb0b`,
reported at height 7,886,815, runtime explicitly recorded:

```text
CheckProofOfStake() : INFO: read txPrev failed
```

followed by an `AcceptBlock(): check proof-of-stake failed` message. This is
direct evidence for the transaction-lookup class of failure. The excerpt did
not expose `prevout.hash`; by itself it does not prove which branch contained
that exact source transaction.

## Corrected Recovery Path

The current production path is:

```text
healthy peer
  -> deep CBlockLocator finds a common ancestor
  -> competing suffix is announced and downloaded
  -> ProcessBlock
  -> AcceptBlock
  -> candidate-branch proof-of-stake validation
  -> WriteToDisk
  -> AddToBlockIndex
  -> competing nChainTrust accumulates
  -> competing nChainTrust exceeds nBestChainTrust
  -> SetBestChain
  -> Reorganize
  -> disconnect F suffix
  -> connect M suffix
  -> continue normal synchronization
```

**INFERENCE from source:** height alone does not choose the winner.
`AddToBlockIndex` accumulates chain trust, and `SetBestChain` is invoked when
the new branch exceeds `nBestChainTrust`.

## Live Mainnet Recovery

**FACT — operator-provided live runtime excerpt:** the final run recorded the
following exact messages:

```text
REORGANIZE: Disconnect 2729 blocks
REORGANIZE: Connect 10628 blocks
REORGANIZE: done
```

It then recorded `SetBestChain` reaching height 7,897,145 and continued forward
through subsequent heights.

The disconnect count independently reproduces the established fork boundary:

```text
7,889,246 - 2,729 = 7,886,517
```

The connect count independently reproduces the first observed
post-reorganization tip:

```text
7,886,517 + 10,628 = 7,897,145
```

This agreement is unusually strong runtime evidence: the reorganization depth
lands exactly on the last common block established before the recovery run,
and the connected suffix lands exactly on the first observed recovered tip.

The final-run excerpt was supplied with the operational report. No matching
locally archived final-run log was found during this documentation audit, so
this document does not invent a timestamp, artifact path, or additional block
hashes for those lines.

## Evidence Boundaries

### Directly observed or previously established facts

- The preserved datadir began with the active tip at height 7,889,246 and the
  hash recorded above.
- The last common block was previously established at height 7,886,517 with
  the hash recorded above.
- The live run reported disconnecting 2,729 blocks, connecting 10,628 blocks,
  completing `Reorganize`, reaching height 7,897,145, and continuing forward.
- The recovery mechanism did not use datadir recreation, block invalidation or
  reconsideration, manual active-chain selection, or manual suffix import.
- The two intermediate PoS failures were observed with the evidence limits
  stated above.

### Source-backed interpretation

- The checkpoint, IBD classification, locator, and PoS changes remove distinct
  gates in discovery, admission, and candidate-branch validation.
- After branch-correct validation and indexing, accumulated chain trust caused
  the normal `SetBestChain` / `Reorganize` path to select the stronger chain.
- The matching arithmetic strongly corroborates that the observed
  reorganization crossed the previously established fork boundary.

### Not established by this validation

- Which of the two recorded hashes at height 7,886,518 was on each branch.
- The exact internal PoS predicate that rejected the height-7,886,749 block.
- The exact branch containing the height-7,886,815 block's `txPrev`, because
  the runtime excerpt did not contain its prevout hash.
- A guarantee of recovery for every future fork depth, topology, peer set, or
  failure mode.
- Resolution or optimality of separate IBD scheduler and throughput behavior.

## Regression Validation

Before the real-datadir retest, focused regressions covered and passed:

- synchronized-checkpoint behavior during IBD;
- IBD classification with unavailable peer snapshots;
- preservation of connected ahead-peer evidence;
- a block locator spanning a fork deeper than 2,729 blocks;
- candidate-branch stake-modifier selection; and
- candidate-branch stake-source lookup, including rejection of an unrelated
  branch's transaction.

The P2P suite passed. The full suite passed 439 of 439 cases on rerun. The first
full-suite run encountered the unrelated `pinglifecycle_tests` one-microsecond
timing mismatch `3000001 != 3000000`; it did not recur on rerun. Both `innovad`
and `innova-qt` builds passed.

## Final Verdict

**CONFIRMED: autonomous recovery from the observed 2,729-block Innova mainnet
fork.**

A node using the corrected build and preserved real forked datadir discovered
and validated a stronger competing chain, then invoked the normal
`SetBestChain` / `Reorganize` path without manual chain manipulation.
Synchronization continued on the recovered active chain. This establishes
correctness and recoverability for the observed failure class; it does not
establish universal recovery or throughput guarantees.

## Remaining Work and Non-Goals

The separate scheduler-on IBD throughput and stall investigation remains open.
This recovery result does not establish that IBD performance is optimal, that
scheduler stalls or `recovery_skip_pipeline_active_after_timeout` are solved,
that any target blocks-per-minute rate has been achieved, or that observed
throughput is an architectural limit. Those are performance and scheduling
questions separate from the correctness result documented here.
