# DAG/IDAG status

**This branch does not complete the DAG/IDAG removal.** The campaign was started earlier in the same
session and paused (stash `515c8661`, patch `scratch/dag-idag-wip-20261029T033347.patch`). This document
records what the cleanup pass *found and removed*, and what remains for the DAG campaign.

## Removed by this branch (DAG-owned build references)

| path | residue | action |
|---|---|---|
| `src/makefile.arm` | `obj/dag.o`, `obj/finality.o`, `obj/idns.o`, `obj/lelantus.o`, `obj/curvetree.o`, `obj/ipa.o`, `obj/ed25519_zk.o`, `obj/bulletproof_ac.o`, `obj/ringsig.o`, `obj/shielded*.o`, `obj/rpc*.o` (shielded/nyx/smessage), `obj/smessage.o`, `obj/spork.o`, `obj/null*.o`, `obj/namecoin.o`, `obj/zkproof.o`, `obj/core.o`, `obj/txdb-*.o` | 42 entries removed — no source exists for any of them |
| `src/makefile.bsd` | same family | 21 entries + 2 orphan `endif` removed |
| `src/makefile.mingw` | same family | 21 entries removed |
| `.github/workflows/ci.yml` | `idag` branch trigger | removed |
| `innova.pro` | dead duplicate project referencing DAG-era and removed sources | file removed |

## Still present in the tree (NOT removed here — belongs to the DAG campaign)

Measured with `git grep -E 'DAG|Dag|IDAG|Idag|dag' -- src/` at the end of the cleanup pass:

| site | file:line | nature | status |
|---|---|---|---|
| generation-binding domain literal `INNOVA_GENERATION_DAG_BINDING_V2` | `src/candidate_frontier_metadata.cpp:16` | **executable**: mixed into every V2 generation content root | OPEN — removed in the DAG branch (neutral `INNOVA_GENERATION_BINDING_V2` + deterministic re-bind) |
| `mixedDagInputDigest` | `src/blockindex_generation_builder.cpp:689`, `src/blockindex_generation_lifecycle.cpp:364` | local variable name (mechanism is current V2 candidate-frontier binding) | OPEN — renamed in the DAG branch |
| `hashDAGSourceState` | `src/miner.h:53`, `src/miner.cpp:1205` | work-identity field, never written in production | OPEN — removed in the DAG branch |
| `dag_epoch_commit_*` | `src/ibdactivepath.h:132-134`, `src/ibdactivepath.cpp:178-180,652` | write-only telemetry counters (never incremented) | OPEN — removed in the DAG branch |
| `dagcleanheight` | `src/init.cpp:156` | comment about an inert persisted key | OPEN — removed in the DAG branch |
| `obj/dag.o` in `src/makefile.arm/bsd/mingw` | — | dead build entries | **CLOSED here** |
| `obj/dag.cpp` in `innova-qt.pro` | line-list of missing sources | dead Qt source entry | reported; `.pro` repair reverted (see unresolved-items §Qt) |
| test externs `g_testDag*` | `src/test/blockindex_window2_e2e_tests.cpp:64-88` | unused declarations | OPEN — removed in the DAG branch |
| test case names / comments with `dag` | `blockindex_window2_e2e_tests.cpp`, `cpu_mining_tests.cpp`, `candidate_frontier_tests.cpp`, `blockindex_startup_seam_tests.cpp` and others | identifiers and historical narrative | OPEN — renamed/normalised in the DAG branch |
| historical forensic docs (`doc/idag-*.md`, `doc/forensics/*.patch`) | — | documentation of past work, no executable mechanism | KEPT deliberately (documentation, not architecture) |

## Mainnet consensus

No mainnet genesis, consensus parameter, checkpoint map (`mapCheckpoint`), fork height or validation rule
was changed by this branch. The only consensus-adjacent constant touched anywhere in this session was the
**testnet** genesis (in the paused DAG branch, not in this branch).

## Status

`DAG/IDAG = NOT COMPLETED`. The executable residue above still exists on `master`; the paused branch holds
the work that removes it and is **not** proven green (its test fixture was mid-repair). See
`unresolved-items.md` §DAG.


## Update — DAG/IDAG removal LANDED

Commit `d35cd97f`. Executable residue closed:

| item | before | after |
|---|---|---|
| generation-binding domain | `INNOVA_GENERATION_DAG_BINDING_V2` | `INNOVA_GENERATION_BINDING_V2` (V2-owned) |
| derived-data handling | old root unusable after the domain change | `-blockindexrebindbinding=1`: deterministic re-derivation from the generation's canonical files, structural validation before and after, idempotent |
| `mixedDagInputDigest` | builder + lifecycle | `candidateMixedInputDigest` |
| `hashDAGSourceState` | field + comparison | removed (never written in production) |
| `dag_epoch_commit_*` | 3 counters + log field | removed (never incremented) |
| `dagcleanheight` | comment/mechanism | removed |
| testnet genesis | `Innova Public IDAG Hidden Finality Testnet …` | `Innova Public Testnet | Neutral Genesis | V2 Store3A`, merkle `4f81629a…d6da3e`, nonce 709855, hash `0000080f…94e26b` |
| mainnet genesis/consensus | — | **unchanged** |
| fixture-triggered DAG refs | `g_testDag*`, DAG-named cases | removed / renamed |

Remaining `DAG`-token hits in `src/` are historical comments in test narratives and
one Swedish locale string (`dag` = "day"); no executable mechanism remains.
