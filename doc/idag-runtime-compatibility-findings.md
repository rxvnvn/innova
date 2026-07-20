# IDAG runtime compatibility findings

Статус: forensic baseline для будущего staged removal. Production cleanup на основании этого документа ещё не выполнен.

## Scope and evidence

Проверена копия mainnet datadir со snapshot height `7,813,845`, tip `3eeb41e301c3c904bc6e10409385ad766e871c6630b7cd4cbca8175450861bcd`. Оригинальный datadir daemon-ом не запускался. Эксперименты выполнялись в изолированном worktree и копиях datadir; конкретные локальные пути намеренно не публикуются.

## Experimental binary

* binary: experimental `src/innovad` в изолированном worktree
* SHA-256: `a89fd6f5816120456acfaff8df930c862a5136496a430779430b69d14c41bc90`
* marker: `IDAG_STAGE41B_DISABLE_RUNTIME`
* experimental HEAD: `6509b10ab119c683527a7d8edbbd0e0ac40c61e5`
* patch snapshot: `doc/forensics/idag-experimental-nodag.patch`

Экспериментальный patch отключал только загрузку DAG persistent state в `CTxDB::LoadBlockIndex()` и post-gate DAG runtime branch в `CBlock::AddToBlockIndex()`. Он также содержит несущественное удаление пустой строки в `main.cpp`; это не является частью production design.

## Proven offline compatibility

Для копии существующей базы доказано:

* C1 index load: PASS;
* C2 offline no-DAG startup: PASS;
* C3 clean stop: PASS;
* C4 no-DAG restart: PASS;
* C5 baseline rollback: PASS.

RPC достигал ready state, высота и tip оставались неизменными, `reindex` не требовался. После остановки no-DAG binary baseline binary открыл ту же копию без schema error. Это доказательство ограничено исследованным snapshot и конкретными бинарниками.

## Continuation evidence

Сохранённая uncontrolled-but-real live-chain continuation evidence продвинула no-DAG binary с height `7,813,845` до `7,814,116`; принято 271 active-chain block. В логах присутствуют `AcceptBlock`/`SetBestChain`. Следующий блок был отклонён с ошибкой `Did not find this payee in the collateralnode list`. Это collateralnode-payee blocker, а не доказанный DAG failure.

Отдельный controlled peer experiment с одним known-good endpoint получил handshake и advertised height `7,814,313`, но не доставил блоки; local height осталась `7,813,845`. Endpoint и credentials в документ не включены. Статус: blocked by peer/sync behavior, not a DAG-runtime failure.

## DAG-prefix evidence

Raw byte scan копии базы дал: `daglinks: 0 hits`, `epochstate: 0 hits`, `dagcleanheight: 0 hits`. Это supportive but not exhaustive evidence: decoded key-by-key LevelDB inventory не выполнялся. Поэтому данный результат не разрешает удалять storage compatibility code сам по себе.

## What is and is not established

`PROVEN for examined snapshot`: обычная mainnet block index открывается без DAG runtime initialization, без reindex и с обратимостью к baseline binary. `STRONG INFERENCE`: исторический mainnet runtime не нуждается в DAG-derived persistent records, поскольку Stage 2 не обнаружил их, а C1–C5 прошли.

`UNKNOWN`: полная decoded inventory LevelDB prefixes, поведение experimental testnet/regtest databases, полный P2P compatibility surface (`getdagtips`/`dagtips` и DAG-expanded `getblocks`), nChainTrust independent reconstruction и CEpochState boundary.

## Permitted next changes

До отдельного доказательства разрешены только узкие documentation/forensic changes. Не разрешены: удаление `CEpochState`, изменение ordinary `nChainTrust`, удаление DB serialization/prefix handling, изменение testnet/regtest semantics, массовое удаление DAG classes или P2P dispatch.

Первый production slice не выбран: P2P tests и broader getblocks compatibility ещё не закрыты, а экспериментальный patch затрагивает shared startup path. Следующий production review checkpoint должен быть после отдельного semantic audit и functional P2P tests.

## Reproduction references

Предыдущие Stage 1–4 отчёты находятся в `doc/idag-stage*.md`; raw/машинные результаты — в `doc/forensics/`. Экспериментальные datadir copies и logs не являются production state и не входят в этот документ.
