# `getinfo` и синхронизация: call graph, блокировки и границы причинности

## Результат аудита

`getinfo` не вызывает `StartSync`, `ProcessMessages`, `SendMessages`, `PushGetBlocks`,
`ProcessBlock`, `AcceptBlock` или `SetBestChain` и не назначает `pnodeSync`/`fStartSync`.
В обычном full-node режиме у него нет прямого пути, который должен возобновлять
загрузку основной цепи.

Аудит исходного кода обнаружил два query-side mutation, которые в этом патче
устранены: `IsInitialBlockDownload()` и `CNode::copyStats()` вызывали
`ExpireBlockInFlight()`. Теперь обслуживание timeout выполняется только в
message-handler, а оба query читают состояние без его очистки.

Сам `getinfo` всё же не является строго безэффектным:

* диспетчер держит `cs_main`, а затем `pwalletMain->cs_wallet` всё время выполнения
  RPC;
* `IsInitialBlockDownload()` пытается взять `cs_vNodes` и читает peer heights,
  `mapAskFor` и in-flight state;
* wallet getters заполняют mutable credit caches;
* `GetOldestKeyPoolTime()` временно резервирует ключ, возвращает его в keypool и
  при разблокированном кошельке может вызвать `TopUpKeyPool()` с генерацией ключей
  и записью wallet DB;
* в режиме `fHybridSPV` расчёт stake weight может дойти до
  `FetchBlockForStaking()` и отправить до трёх `getdata` для wallet staking data.
  Это отдельный SPV-путь, а не механизм full-chain sync.

Таким образом, после удаления query-side expiry `getinfo` не меняет transient
download state в normal full-node режиме. Он всё ещё способен влиять на
расписание потоков через outer locks, но найденное зависание вызывается не этим
RPC. Точная причина
описана в [stalled-sync-state-machine.md](stalled-sync-state-machine.md).

## Точка входа и порядок блокировок

Команда зарегистрирована в `vRPCCommands` так:

```text
{ "getinfo", &getinfo, true, false }
```

Последнее поле называется `unlocked`. Значение `false` означает, что
`CRPCTable::execute()` исполняет actor внутри:

```cpp
LOCK2(cs_main, pwalletMain->cs_wallet);
```

Фактический путь вызова:

```text
HTTP/Qt RPC caller
  -> JSONRequest::parse()
       -> printf("ThreadRPCServer time_us=... method=getinfo")
  -> CRPCTable::execute("getinfo")
       -> GetWarnings("rpc")
            -> cs_mapAlerts
       -> LOCK(cs_main)
       -> LOCK(pwalletMain->cs_wallet)
       -> getinfo()
       <- unlock pwalletMain->cs_wallet
       <- unlock cs_main
```

Строка `ThreadRPCServer time_us=... method=getinfo` печатается **до** ожидания
`cs_main` и `cs_wallet`; она фиксирует начало request, но не момент входа в actor
или освобождения блокировок. Эти границы дают `GETINFO_PROBE_LOCKED/END`.

`CCriticalSection` является `boost::recursive_mutex`. Поэтому повторные
`LOCK2(cs_main, cs_wallet)` внутри wallet getters не отпускают внешние locks и не
образуют scheduling point. Освобождение `cs_main` в конце RPC может дать уже
ожидающему потоку возможность продолжить, как обычное освобождение mutex, но RPC
не делает `notify` condition variable и не ставит сетевую работу в очередь.

## Call graph `getinfo`

Ниже перечислены прямые вызовы и транзитивные ветви, важные для locks, I/O или
изменения состояния. JSON formatting (`Pair`, `push_back`), простые преобразования
строк и арифметика опущены как не имеющие общей mutable state.

```text
getinfo
  -> GetProxy(NET_IPV4)
  -> CWallet::GetStakeWeight
       -> CWallet::GetBalance
            -> CWalletTx::IsTrusted/GetAvailableCredit
                 -> credit-cache writes
       -> [full node] CWallet::SelectCoinsForStaking
            -> CWallet::AvailableCoinsForStaking
       -> [hybrid SPV] CWallet::SelectCoinsForStakingSPV
            -> CWallet::AvailableCoinsForStakingSPV
            -> MarkSPVUtxoSpent / AddToWallet, when applicable
            -> RequestBlockForStaking
                 -> FetchBlockForStaking
                      -> PushMessage("getdata"), up to 3 peers
       -> CTxDB::ReadTxIndex for selected stake coins
  -> LOCK(cs_vNodes) -> read vNodes.size() into local connection count
  -> FormatFullVersion
  -> CWallet::GetVersion
  -> CWallet::GetBalance
  -> CWallet::GetAnonBalance
  -> CWallet::GetNewMint
  -> CWallet::GetStake
  -> CWallet::GetUnconfirmedBalance
  -> CWallet::GetImmatureBalance
  -> ValueFromAmount for wallet amounts/fees
  -> GetTimeOffset
  -> CNode::GetTotalBytesRecv / GetTotalBytesSent
  -> bytesReadable
  -> GetDefaultDataDir / GetDataDir
  -> filesystem exists + read onion/hostname [native Tor only]
  -> GetDifficulty
       -> GetLastBlockIndex(pindexBest, proof type)
  -> GetPoWMHashPS
       -> walk pindexBest->pprev
       -> GetDifficulty
  -> GetPoSKernelPS
       -> walk pindexBest->pprev
       -> GetDifficulty
  -> CWallet::GetOldestKeyPoolTime
       -> ReserveKeyFromKeyPool
            -> TopUpKeyPool [unlocked wallet, if needed]
            -> CWalletDB::ReadPool
       -> ReturnKey
  -> CWallet::GetKeyPoolSize
  -> IsInitialBlockDownload
       -> TRY_LOCK(cs_vNodes)
       -> inspect fresh peer heights, mapAskFor and in-flight state
  -> CWallet::IsCrypted / IsLocked
  -> GetWarnings("statusbar")
       -> cs_mapAlerts
```

Прямые чтения без отдельного helper включают `nBestHeight`, `pindexBest`,
`pindexBest->nMoneySupply`, wallet/network flags,
`nReserveBalance`, `nTransactionFee`, `nMinimumInputValue` и `addrSeenByPeer`.
Чтения chain state защищены внешним `cs_main`; connection count теперь читается
под `cs_vNodes` в локальную переменную.

## Таблица блокировок и side effects

| Lock/state | Где берётся в пути `getinfo` | Чтение | Изменение/эффект |
|---|---|---|---|
| `cs_main` | блокирующий outer lock в `CRPCTable::execute`; рекурсивно в wallet helpers | `pindexBest`, `nBestHeight`, block index, depth/trust | Сам RPC chain state не меняет; unlock может пропустить ожидающий поток |
| `pwalletMain->cs_wallet` | второй outer lock; рекурсивно почти во всех wallet getters | `mapWallet`, balances, keypool, wallet version | credit caches; reserve/return key; возможен `TopUpKeyPool()` |
| `cs_vNodes` | blocking snapshot connection count в actor; рекурсивный `TRY_LOCK` в IBD; blocking в hybrid-SPV `FetchBlockForStaking` | connection count, peer heights и flags | в full-node ветке нет записи; hybrid-SPV может отправить `getdata` |
| `cs_mapRequests` (per peer) | **не берётся**; этот mutex защищает callback-map `mapRequests`, а не download queue | нет | к `mapAskFor` не относится |
| `mapAskFor` | отдельного mutex в этом дереве нет | IBD читает наличие элементов | после исправления query не пишет, но чтение всё ещё конкурирует с message-handler |
| `cs_mapAlreadyAskedFor` | **не берётся** | нет | нет |
| `cs_pnodeSync` | **не берётся** | нет | `pnodeSync` не назначается |
| `cs_collateralnodes` | **не берётся** | нет manager/list state | collateralnode sync не вызывается |
| `cs_mapTransactions` | такого lock в текущем дереве нет | — | — |
| `cs_proxyInfos` | `GetProxy` | proxy config | нет |
| `CNode::cs_totalBytesRecv/Sent` | byte counters | totals | нет |
| `cs_mapAlerts` | `GetWarnings("rpc")` до outer locks и `GetWarnings("statusbar")` внутри них | alerts/warnings | нет |
| `cs_nWalletUnlockTime` | только для encrypted wallet | unlock deadline | нет |
| `cs_KeyStore` | `IsLocked()` | master-key state | нет |
| `cs_spvutxos` | только hybrid-SPV stake branch | SPV UTXO cache | spent/available markers могут измениться |
| wallet/chain DB locks | keypool and stake-index reads | wallet pool, tx index | возможна wallet DB запись из `TopUpKeyPool()` |

Ни `cs_vSend`, ни `cs_vRecvMsg` непосредственно actor не берёт в normal full-node
ветке. В hybrid-SPV ветке `PushMessage("getdata")` использует обычный send path.

## Lock-order и starvation

В normal full-node call graph порядок RPC равен `cs_main + cs_wallet`, затем
blocking `cs_vNodes` для connection snapshot; последующий IBD `TRY_LOCK` получает
тот же recursive mutex повторно. В аудите не найден противоположный blocking путь
`cs_vNodes -> cs_main/cs_wallet`: message-handler освобождает `cs_vNodes` после
создания ref-held copy, а cleanup/send paths используют `TRY_LOCK`. Поэтому не
найден lock cycle, который мог бы разрешаться только вызовом `getinfo`, но wait и
hold `cs_vNodes` теперь явно измеряются `SYNCLOCK location=getinfo`.

Message-handler вызывает `SendMessages` уже под peer `cs_vSend`, а внутри делает
`TRY_LOCK(cs_main)`. Это потенциально обратный порядок относительно hybrid-SPV
ветки `getinfo` (`cs_main -> peer send path`), но `TRY_LOCK` не ждёт и поэтому не
образует hard deadlock. Найденная проблема здесь была не циклом, а слишком широкой
областью жизни successful `TRY_LOCK`; она описана ниже и исправлена. Для
`ProcessMessage(block)` порядок начинается с `cs_main`; wallet notifications
следуют существующему порядку `cs_main -> cs_wallet`.

Для зафиксированного payee reject полный существенный порядок глубже:

```text
message handler: peer cs_vRecvMsg
  -> ProcessMessage(block): cs_main
     -> ConnectBlock: recursive cs_main + mempool.cs
        -> cs_collateralnodes (ranking/payee view; released)
        -> cs_vNodes (list-refresh broadcast)
           -> peer send path
```

Это объясняет, почему payee validation/list refresh может дать длинный hold
`cs_main`; `SYNCLOCK location=ProcessMessage(block)` измеряет весь участок от
ожидания chain lock до окончания `ProcessBlock`. Порядок совместим с normal RPC
`cs_main -> cs_wallet -> cs_vNodes`. Обратный send-path использует `TRY_LOCK`, а
socket cleanup использует peer `TRY_LOCK`, поэтому статический аудит не нашёл
blocking cycle. Длинный hold/starvation всё же возможен и отделён от ошибки выбора
sync peer.

Освобождение recursive mutex не выполняет специального wakeup P2P state machine.
Оно лишь позволяет одному из уже ожидающих threads продолжить; fairness
`boost::recursive_mutex` не гарантирует. Пороговая `SYNCLOCK`-диагностика поэтому
нужна для доказательства starvation, но сама не меняет порядок locks.

## Найденные lock/race проблемы и их исправление

### Query-side expiry удалён

До исправления `IsInitialBlockDownload()` вызывала `ExpireBlockInFlight()` для
каждого peer. Такой query удалял записи из `setBlocksInFlight` и
`mapBlockInFlightSince`; время expiry зависело от вызова `getinfo` или другого IBD
consumer. Аналогичный side effect был в `CNode::copyStats()`, поэтому
`getpeerinfo` и Qt `PeerTableModel` (опрос каждые 5 секунд) также могли выполнять
maintenance.

Патч удаляет оба вызова. `ExpireBlockInFlight()` теперь вызывается в
message-handler/отправке запросов, то есть query RPC и GUI больше не являются
частью download state machine. Это одновременно устраняет возможное уникальное
P2P-изменение `getinfo` в normal full-node пути.

IBD по-прежнему читает `mapAskFor` и in-flight поля без отдельного peer-state
mutex: `cs_mapRequests` защищает другой контейнер (`mapRequests`) и не является
lock для `mapAskFor`. Это остаточное ограничение наблюдаемости, но не RPC-side
mutation. `initialblockdownload` также может меняться из-за fresh-height/stale-time
эвристик без смены active chain.

### Неожиданная область жизни `TRY_LOCK(cs_main)` в `SendMessages`

До исправления `SendMessages()` объявляла `TRY_LOCK(cs_main, lockMain)` в scope
всей функции. Комментарий утверждал, что последующий getdata loop находится
outside `cs_main`, но при успешном `TRY_LOCK` recursive lock жил до return, и
getdata фактически выполнялся под ним. Если в этот момент RPC держал `cs_main`,
`TRY_LOCK` не срабатывал и тот же loop шёл без outer lock. Таким образом, любой
RPC с `cs_main`, включая `getinfo`, мог менять эту ветвь расписания, хотя не мог
создать отсутствующий `mapAskFor` или выбрать sync peer.

Outer `TRY_LOCK` помещён в явный inner scope; getdata loop теперь действительно
выполняется после его destruction. `SYNCLOCK location=SendMessages` показывает
длительность hold successful `TRY_LOCK`; его `wait_us` по определению близок к нулю.

### Незащищённые чтения

Race на `getinfo` connection count исправлен: `vNodes.size()` копируется под
`cs_vNodes`. `copyStats()`/IBD всё ещё читают часть peer sync fields под
`cs_vNodes`, хотя writers не везде используют этот lock. Cached probe отделяет
snapshot от RPC actor, чтобы диагностический RPC не становился частью state
machine.

## Что `getinfo` не делает

В normal full-node пути нет вызова или записи для:

* `pnodeSync`, `fStartSync`;
* `PushGetBlocks`, `getBlocksIndex`, `getBlocksHash`;
* планирования `AskFor`/`mapAskFor`;
* `mapAlreadyAskedFor`;
* collateralnode sync/list refresh;
* `ProcessMessages`, `SendMessages`, `ProcessBlock`, `AcceptBlock` или
  `SetBestChain`;
* condition-variable notification или deferred callback queue.

Следовательно, если после RPC появляется full-node `getblocks`/`getdata`, возможны:

1. совпадение с независимо работающим message-handler;
2. scheduling effect после unlock;
3. до исправления — смена lock-path в `SendMessages()` либо expiry через IBD;
4. отдельный hybrid-SPV staking `getdata` path.

Ни один из этих вариантов не объясняет состояние `sync_peer=none` для peers
версий 43950/50000.

## Проверка причинности по фактическому `debug.log`

Журнал `/home/user/data/.innova/debug.log` опровергает достаточную причинную связь
«вызов `getinfo` запускает продолжение»:

* строки 120951–120957: блок 7802642 отклонён, затем зафиксирован dead state
  `local_height=7802641 ... sync_peer=none blocks_in_flight=0 queued_getblocks=0`;
* строка 121602: непосредственно перед первым RPC уже отправлено 303 `getblocks`;
* строка 121620: первый зафиксированный `ThreadRPCServer method=getinfo`;
* строка 121623: после RPC счётчик `getblocks` вырос до 318, но высота остаётся
  7802641, pipeline пуст, `sync_peer=none`;
* строки 121878 и 121919: второй `getinfo` также не меняет высоту и пустой
  pipeline;
* после рестарта строки 136316–136407 принимают блоки 7802642…7802732;
* только затем, в строке 136423, появляется `ThreadRPCServer method=getinfo`.

То есть в первом случае `getinfo` не восстановил sync, а во втором восстановление
началось до `getinfo`. Старые строки не имеют local wall-clock timestamp, поэтому
они не дают длительность интервалов, но порядок строк достаточен, чтобы отвергнуть
RPC как необходимую и достаточную причину этих двух наблюдений.

## GUI polling

`innova-qt` не вызывает `getinfo` периодически. Команда лишь присутствует в
whitelist `WalletModel::executeRPC()` для явного GUI action.

Периодические внутренние чтения:

| Модель | Интервал | Действие и locks |
|---|---:|---|
| `ClientModel` | 1 s (`MODEL_UPDATE_DELAY`) | `TRY_LOCK(cs_main)`, затем `GetNumBlocksOfPeers()` с `TRY_LOCK(cs_vNodes)`; обновляет current/estimated height |
| `WalletModel` | 1 s | `TRY_LOCK(cs_main)`, затем `TRY_LOCK(cs_wallet)`; обновляет balances |
| `PeerTableModel` | 5 s | `TRY_LOCK(cs_vNodes)`, `copyStats()` для peers; после release отдельно `TRY_LOCK(cs_main)` для node-state stats |

До исправления последний путь отличал GUI от daemon: `copyStats()` выполнял
in-flight expiry каждые 5 секунд. После удаления этого side effect GUI polling
только наблюдает state; maintenance и recovery одинаковы для `innova-qt` и
`innovad`.

GUI получает «Расчётное число блоков» через `GetNumBlocksOfPeers()`: это максимум
свежих peer heights либо peer median/checkpoint estimate. Это не доказательство,
что downloader выбрал peer или знает/проиндексировал headers.

В старом журнале `SYNCSTATE best_header` фактически содержал
`max(nBestKnownHeight, nChainHeight)` по peers. Диагностика исправлена: теперь
`best_header` — локальная высота header/tip (для full node отдельной header chain
нет), а advertised maximum печатается отдельно как `max_peer_height`.

## Инструментирование эксперимента

Диагностика выключена по умолчанию и включается отдельно:

```text
-getinfosyncprobe=1
-synclockdiagnostics=1
-synclockthresholdms=<milliseconds>  # default 250, minimum 1
-blockrequesttrace=1
-logtimestamps=1
```

Назначение событий:

| Event | Точная граница |
|---|---|
| `ThreadRPCServer time_us=... method=getinfo` | JSON request распознан до ожидания actor locks |
| `GETINFO_PROBE_BEGIN` | непосредственно перед ожиданием outer locks |
| `GETINFO_PROBE_LOCKED` | `cs_main` и `cs_wallet` получены; `lock_wait_us` измеряет ожидание |
| `GETINFO_PROBE_END` | actor закончен и outer locks освобождены; длительность вычисляется по `request_start_us` |
| `SYNCLOCK` | wait или hold выше `-synclockthresholdms`; содержит `time_us`, thread id, location, locks, `wait_us` и `hold_us` |
| `BLOCKREQTRACE ... GETBLOCKS_QUEUE` | `PushGetBlocks` реально добавил locator в очередь |
| `BLOCKREQTRACE ... STALL_RECOVERY` | recovery решил инициировать запрос |
| `BLOCKREQTRACE ... GETDATA_SEND` | `getdata` записывается peer send path |
| `BLOCKREQTRACE ... BLOCK_RESULT` | результат `ProcessBlock` и наличие блока в index/active chain |
| `SYNC_EVENT ... PROCESS_BLOCK_BEGIN` | вход в `ProcessBlock` с hash/peer/local height |
| `SYNC_EVENT ... ACCEPT_BLOCK_BEGIN` | вход в `AcceptBlock` с hash |
| `SYNC_EVENT ... CHECKBLOCK_POW_PAYEE_REJECT` / `CHECKBLOCK_POS_PAYEE_REJECT` | точный collateralnode-payee reject |
| `SYNC_EVENT ... SETBESTCHAIN_COMMIT` | active tip успешно committed с hash/height |

Три probe events связываются одинаковым `request_start_us`. Они не обходят
`vNodes` и chain state из RPC thread: message-handler периодически строит
read-only cached snapshot, а RPC копирует его под отдельным
`cs_getInfoProbeSnapshot`. Поля `snapshot_time_us` и `snapshot_age_us` явно
показывают возраст, поэтому диагностическая выборка сама не становится P2P
триггером. Snapshot builder использует `TRY_LOCK(cs_main)` и пропускает update при
contention; `LogSyncDiagnosticsMaybe()` так же пропускает весь periodic log вместо
ожидания chain lock. Snapshot содержит:

```text
time_us request_start_us lock_wait_us snapshot_time_us snapshot_age_us
local_height best_header max_peer_height initialblockdownload connections
sync_peer blocks_in_flight queued_getblocks total_askfor mapAskFor_size
mapAlreadyAskedFor_size
last_block_receive_time last_accepted_block_time last_getblocks_time last_getdata_time
collateralnode_sync_state collateralnode_list_height collateralnode_list_state
collateralnode_list_count collateralnode_median_count
recovery_last_progress_time recovery_last_attempt_time recovery_attempts
rejected_retry_hash rejected_retry_time
peer{id,height,fStartSync,askfor,inflight,queued_getblocks,last_block_recv,
     last_getblocks,last_getdata}
```

В текущем collateralnode subsystem нет надёжного общего sync-state/list-height
API, поэтому snapshot честно пишет `collateralnode_sync_state=unavailable` и
`collateralnode_list_height=-1`. `collateralnode_list_state=1` означает, что
неблокирующий snapshot получил `cs_collateralnodes` (0 — данные недоступны в этом
tick), а не отдельную фазу sync; count/median валидны только при значении 1.
`SYNCSTATE` и новые chain events используют `GetTimeMicros()`. Старая строка
`SetBestChain ... date=` сохранена без изменения: `date` остаётся временем блока.
`SYNCLOCK` инструментирует не каждый mutex, а только blocking/long-lived границы:
generic RPC dispatcher, `getinfo`/`getpeerinfo` peer snapshot,
`ProcessMessage(block)` и `SendMessages`. Nonblocking periodic probe/diagnostics
намеренно не входят в coverage; их пропуск виден по `snapshot_age_us` и отсутствию
новой `SYNCSTATE` строки.

## Контрольные RPC

В этом дереве контрольные команды имеют разные lock/side-effect профили:

| RPC | Generic `cs_main + cs_wallet` | Peer lock/mutation |
|---|---:|---|
| `getblockcount` | нет (`unlocked=true`) | нет |
| `getbestblockhash` | нет (`unlocked=true`) | нет |
| `getpeerinfo` | нет | `cs_vNodes` + read-only `copyStats`; `SYNCLOCK location=getpeerinfo` |
| `getblockchaininfo` | да (`unlocked=false`) | read-only IBD inspection |
| `getinfo` | да (`unlocked=false`) | `cs_vNodes` connection snapshot + read-only IBD; wallet side effects; hybrid-SPV special case |

Эксперимент должен выдерживать одинаковый quiet interval до каждого control RPC
и сравнивать первое последующее `GETBLOCKS_QUEUE`, `GETDATA_SEND`, `BLOCK_RESULT`
и `SETBESTCHAIN_COMMIT` по `time_us`. Детерминированная ошибка version gate уже
исправлена общим helper и не зависит от результата scheduling-теста.
