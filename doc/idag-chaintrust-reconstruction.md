# Stage 6 — независимая реконструкция `nChainTrust`

## Stage 5 documentation commit

`f0894f552a5a7d5c34f43bc3d0e20f7cbf58a672` — `docs(idag): record runtime compatibility findings`.
Документационные артефакты Stage 5 зафиксированы отдельно; production consensus code этим коммитом не изменён.

## Semantic scope

LSP scope охватил `CBlockIndex::nChainTrust`, `CBlockIndex::GetBlockTrust`, `CTxDB::LoadBlockIndex`, `CBlock::AddToBlockIndex`, `CBlock::SetBestChain`, `CDiskBlockIndex::IMPLEMENT_SERIALIZE` и ссылки на `nChainTrust` в `main.cpp`, `txdb-leveldb.cpp`, `dag.cpp`, `miner.cpp` и RPC.

## Proven source semantics

`CDiskBlockIndex::IMPLEMENT_SERIALIZE` сериализует header/index fields (`hashNext`, file/position, height, monetary fields, flags, stake fields, proof, header, `hashPrev`, `blockHash`), но не сериализует `nChainTrust`. Поэтому в persistent record отсутствует stored trust value: `stored_nChainTrust = NOT_SERIALIZED`.

`CTxDB::LoadBlockIndex()` после чтения всех records сортирует `CBlockIndex` по height и выполняет:

```text
pindex->nChainTrust = (pindex->pprev ? pindex->pprev->nChainTrust : 0)
                    + pindex->GetBlockTrust();
```

`CBlock::AddToBlockIndex()` использует ту же формулу при создании индекса нового блока. `GetBlockTrust()`:

* возвращает 0 для non-positive compact target;
* возвращает 0 для PoS блока на высоте `>= FORK_HEIGHT_DAG`;
* на высоте `>= FORK_HEIGHT_POEM` использует `GetBlockEntropy(...)`;
* иначе возвращает `((2^256)/(target+1)).getuint256()`.

При post-DAG PoW block с непустыми IDAG parents production code позднее заменяет `nChainTrust` на `g_dagManager.ComputeDAGScore(pindexNew)`. Это отдельная post-gate ветка; её фактическое отсутствие в исследованной mainnet chain установлено Stage 2, но этот Stage 6 не выполнял полный numeric reconstruction.

Выбор best chain в `CBlock::AddToBlockIndex()` сравнивает `pindexNew->nChainTrust > nBestChainTrust`; `SetBestChain()` и reorg traversal также используют `nChainTrust`. При равенстве отдельного tie-break в этом сравнении нет. DAG manager имеет собственные score comparisons, но они относятся к DAG path и не доказывают linear mainnet selection.

## Export/reconstruction status

Полный read-only exporter, обрабатывающий все `7,813,846` active records и side-chain records, в этой попытке не выполнен. Existing placeholder `doc/forensics/idag-stage41-chaintrust-full.csv` не является результатом экспорта и не использован как evidence. Независимого сравнения baseline/no-DAG CSV и numeric reconstruction tip также нет.

Поэтому:

```text
Blocks checked: 0 by independent exporter
Active-chain trust matches: NOT ESTABLISHED
Side branches checked: 0 by independent exporter
DAG input required: UNKNOWN for full numeric reconstruction
Baseline/no-DAG difference: NOT TESTED
Blocker B: INCONCLUSIVE
```

## Why this is not a resolved blocker

Статический код доказывает формулу пересчёта и отсутствие serialized trust, но не доказывает на полном snapshot, что каждый loaded record и каждая side branch дают ожидаемое значение, nor that independent reconstruction selects the exact tip. Это требует отдельного read-only exporter/loader harness и полного запуска на копии datadir.

## Production safety

Production code не изменён. Не выполнялись изменения формулы trust, chain selection, block index migration, DAG fields или P2P behavior.
