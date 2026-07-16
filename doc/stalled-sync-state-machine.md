# Stalled full-node sync: причина, state machine и восстановление

## Краткий вывод

Состояние

```text
local_height < max_peer_height
initialblockdownload = false/true
sync_peer = none
blocks_in_flight = 0
queued_getblocks = 0
```

возникает детерминированно из-за инвертированной проверки protocol version в
исходном `StartSync()`. Временный reject делает дефект видимым: ownership
полученного блока очищается до `ProcessBlock()`, блок не индексируется, pipeline
опустошается, а повторный выбор допустимого peer отбрасывает версии 43950/50000.

`version.h` задаёт диапазон, от которого блоки запрашивать нельзя:

```cpp
// only request blocks from nodes outside this range of versions
NOBLKS_VERSION_START = 70002;
NOBLKS_VERSION_END   = 70006;
```

Правильный eligible predicate теперь централизован в
`IsBlockSyncPeerVersion()`:

```text
version < 70002 || version >= 70006
```

Handshake в `ProcessMessage("version")` использует его правильно. Но исходный
`StartSync()` делал:

```cpp
if (pnode->nVersion < NOBLKS_VERSION_START ||
    pnode->nVersion >= NOBLKS_VERSION_END)
    continue;
```

То есть он отбрасывал всех допустимых peers и оставлял кандидатами только
запрещённые версии 70002…70005. Та же инверсия была в списке диагностических
кандидатов `LogSyncDiagnosticsMaybe()`.

Для реально наблюдавшихся версий результат однозначен:

| Peer version | Должен отдавать блоки | Исходный `StartSync()` |
|---:|---:|---:|
| 43950 | да | ошибочно отбрасывает |
| 50000 | да | ошибочно отбрасывает |
| 70002…70005 | нет | ошибочно допускает |

Поэтому GUI знает peer height, но `pnodeSync` остаётся `NULL`, ни у одного
подходящего peer не выставляется `fStartSync`, а опустевший pipeline не получает
нового владельца.

## Почему первичная загрузка иногда всё же начинается

Есть два разных пути запуска:

```text
version handshake
  -> correct outside-[70002,70006) predicate
  -> PushGetBlocks(pindexBest, 0)

ThreadMessageHandler -> StartSync
  -> inverted predicate (bug)
  -> no eligible 43950/50000 peer
  -> pnodeSync remains none
  -> fStartSync remains false
```

Handshake способен наполнить pipeline после подключения или рестарта. Это
объясняет burst после рестарта. Но после временного reject, timeout или полного
drain pipeline постоянный sync owner отсутствует, и повторный выбор не работает.
Дальнейший прогресс начинает зависеть от нового handshake/`inv` или от
неограниченного recovery traffic, а не от явного состояния «мы отстаём».

`ThreadMessageHandler2()` выполняет цикл примерно каждые 100 ms в idle режиме.
В начале цикла он вызывает `StartSync()` только если нет valid/recent
`pnodeSync`. В зафиксированном эпизоде `pnodeSync==NULL`, поэтому исходный
`StartSync()` регулярно видел peers, но каждый раз исключал 43950/50000
инвертированным version gate. Есть и второй dead path: если owner остаётся
connected/recent, но его request pipeline опустел, `StartSync()` вообще не
вызывается, потому что freshness owner не означает наличие работы.

Именно поэтому одного исправления predicate недостаточно для полного recovery.
Новый `MaybeQueueStalledSyncRecovery()` выполняется в конце каждого
message-handler tick независимо от входящего `inv`/`ping` и проверяет фактический
global pipeline. Он покрывает как `pnodeSync==NULL`, так и живого, но idle owner.
`initialblockdownload` в этом decision не используется как gate.

## Почему GUI показывает актуальную расчётную высоту

GUI вызывает `GetNumBlocksOfPeers()`, который читает `nBestKnownHeight`/peer
median. Выбор sync peer в этом вычислении не участвует.

В старом `SYNCSTATE` поле `best_header` также не являлось local best-header chain:
оно печатало максимальный advertised peer height. Поэтому одновременно были
истинны:

```text
GUI/diagnostics: peer advertised height = 7802721
active chain:                         = 7802641
download owner:                       = none
```

Знание peer height и работоспособность block-download state machine — независимые
состояния. Исправленная диагностика печатает локальный `best_header` и отдельный
`max_peer_height`.

## Триггер: временно rejected block

В зафиксированном эпизоде блок 7802642 был получен, но не добавлен в block index:

```text
received block
  -> ClearBlockInFlight(hash)
  -> ProcessBlock
  -> AcceptBlock
  -> AddToBlockIndex / SetBestChainInner / ConnectBlock
  -> legacy "CheckBlock-POW": collateralnode payee absent
  -> SetBestChainInner failed and transaction rolled back
  -> AcceptBlock failed
  -> ProcessBlock failed
  -> block not indexed
  -> request ownership cleared/drained
```

Payee validation в этой задаче не изменяется. Для sync logic существенно только
то, что hash следующего блока остаётся неизвестным, а pipeline после результата
`rejected` может стать пустым. В этом состоянии recovery выполняет две связанные
операции у одного owner:

1. ставит один single-owner `getblocks` для повторного discovery;
2. один раз на recorded hash дополнительно ставит прямой `AskFor`, чтобы не ждать
   нового `inv`.

## Почему существующий stall recovery не является исправлением

Исходный recovery находится внутри `SendMessages(CNode* pto)` и хранит cooldown
по `pto->addrName`. Каждый peer с большей высотой независимо:

1. ждёт `nTimeSinceBlock > 15`;
2. удаляет **все** `MSG_BLOCK` из глобального `mapAlreadyAskedFor`;
3. вызывает `pto->PushGetBlocks(pindexBest, 0)`;
4. присваивает `pto->nLastBlockRecv = nNow`, хотя блок не был получен.

Это per-peer, а не global recovery. При N peers он допускает N `getblocks` в одну
волну и повторяет волну примерно каждые 15 секунд. Он также стирает ownership
других peers и фальсифицирует receive timestamp, используемый IBD/stall logic.

Фактический `debug.log` показывает storm. Например, при одной и той же высоте
7802125 события `STALL_RECOVERY` идут для peers 2, 3, 5, 6 и 7 между
`time_us=1784227932026344` и `1784227942010232`; новая волна для тех же peers
начинается с `1784227948061037`. В более позднем dead state счётчик исходящих
`getblocks` уже равен 303 непосредственно перед первым `getinfo` (строка 121602)
и достигает 318 после него (строка 121623), не изменив высоту 7802641. Это
очередная per-peer recovery wave, а не доказательство полезного RPC effect.

Такой механизм создаёт traffic, но не устанавливает владельца синхронизации и не
гарантирует повтор именно rejected hash.

## Целевая state machine

```text
UP_TO_DATE
  | peer_height > local_height
  v
AHEAD_IDLE
  | select exactly one eligible owner
  | queue one getblocks
  v
REQUESTING
  | first getdata sent / blocks in flight
  v
ACTIVE
  | accepted block -------------------------------> progress; stay ACTIVE
  | pipeline drained while still behind ----------> AHEAD_IDLE
  | recorded block rejected and not indexed ------> AHEAD_IDLE + recorded hash
  | owner disconnected/stale ----------------------> AHEAD_IDLE

AHEAD_IDLE + recorded hash
  | global stall timeout/cooldown elapsed
  | queue one getblocks + one-shot AskFor(recorded hash) to same owner
  v
REQUESTING
  | no progress and pipeline drains
  v
AHEAD_IDLE (generic getblocks recovery with capped exponential backoff)
```

`initialblockdownload` не должен быть единственным gate восстановления. В
зафиксированном состоянии он был `false`, хотя peers находились выше. Основные
условия должны выводиться из height/progress/ownership:

```text
peer_can_advance
AND no active global block request pipeline
AND no queued getblocks
AND no accepted-chain progress for recovery interval
AND global exponential cooldown expired
```

Проверка выполняется периодически message-handler'ом, а не только при входящем
`inv`, `ping`, `pong` или RPC.

## Реализация восстановления

Единое transient-состояние recovery хранится в `CStalledSyncRecoveryState`.
Периодическая точка принятия решения — `MaybeQueueStalledSyncRecovery`.

Параметры независимого от RPC coordinator:

```text
-syncstalltimeout=<seconds>   # default 15, minimum 5
-syncstallcooldown=<seconds>  # initial default 15, minimum 5; then 2x...32x
```

Фактический decision tick:

1. message-handler сам выполняет `ExpireBlockInFlight()`, затем читает active tip
   и local height;
2. применяет общий helper version eligibility с семантикой
   `version < 70002 || version >= 70006` во handshake, `StartSync`, recovery и
   diagnostics;
3. считает pipeline активным, если хотя бы у одного eligible full-node peer есть
   `fStartSync`, block `mapAskFor`, `setBlocksInFlight` или queued
   `getBlocksIndex`;
4. при пустом pipeline и peer height выше local ждёт `-syncstalltimeout` и единый
   global cooldown;
5. выбирает одного connected, successfully-connected, non-client, non-one-shot
   peer с допустимой version и высотой выше local;
6. ставит ему ровно один `PushGetBlocks(active_tip, 0)` и назначает `pnodeSync`;
7. не очищает `mapAlreadyAskedFor` и не изменяет `nLastBlockRecv`;
8. при отсутствии progress увеличивает cooldown как 1x, 2x, 4x, 8x, 16x, затем
   не более 32x; permanent attempt cap нет;
9. смена local height на следующем tick сбрасывает generic attempt/backoff state.

Candidates сортируются тем же score/tie-break, что sync selection. Последовательные
recovery attempts без progress выбирают индекс
`(attempts - 1) % candidate_count`, поэтому один tick имеет одного owner, но после
очистки pipeline и следующего cooldown запрос контролируемо переходит к другому
подходящему peer вместо постоянного hammering первого.

Для rejected block хранится hash, время записи и one-shot flag. Запись создаётся
только после `ProcessBlock=false`, если до и после обработки block отсутствует в
index/orphan map и `nDoS==0`. На первом подходящем recovery tick тот же выбранный
owner получает один `AskFor(MSG_BLOCK, hash)` вместе с locator. Повторная запись
того же hash не сбрасывает one-shot flag; принятие блока очищает запись, а новый
rejected hash открывает новый one-shot.

Повторная проверка того же блока проходит неизменённые consensus/payee rules.
Recovery не превращает reject в accept и не помечает блок валидным заранее.

## Anti-spam invariants

Исправление обязано сохранять следующие инварианты:

1. **Один sync/recovery owner.** В один момент новый locator принадлежит одному
   peer; выбор глобальный, не per-peer.
2. **Один global cooldown.** Число подходящих peers не умножает частоту recovery.
3. **Пустой global pipeline перед recovery.** Любой block `mapAskFor`, in-flight,
   `fStartSync` или queued locator у любого eligible peer подавляет новый decision.
4. **Нет сброса `mapAlreadyAskedFor`.** Transaction и block entries сохраняются;
   `AskFor` использует существующий global request timestamp.
5. **Known-block filter остаётся в send path.** Перед `getdata` выполняется
   `AlreadyHave`, когда удаётся взять `cs_main`; accepted rejected-hash state также
   очищается немедленно.
6. **Нет cross-peer overlap.** Смена peer возможна только после полного drain
   global pipeline и следующего cooldown; попытки без progress циклически проходят
   sorted candidate list.
7. **Не более одного queued locator.** Пока любой `getBlocksIndex` не пуст или
   recovery locator ещё активен, второй не ставится.
8. **Progress resets generic recovery.** Изменение local height сбрасывает stall
   age и exponential backoff; принятие recorded hash сбрасывает direct retry.
9. **One-shot rejected retry.** Один hash напрямую ставится в `AskFor` не более
   одного раза; дальше остаётся обычный exponential getblocks re-discovery.
10. **Legacy compatibility.** 43950 и 50000 проходят один и тот же правильный
    outside-range predicate; 70002…70005 не выбираются.

## Связь с `getinfo`

`getinfo` действительно держит `cs_main`/wallet locks. Кроме того, аудит исходного
кода нашёл два реальных неявных coupling: IBD/copyStats выполняли in-flight expiry,
а слишком широкий `TRY_LOCK(cs_main)` в `SendMessages()` заставлял getdata loop
идти под `cs_main` только при успешном захвате. Поэтому исходная гипотеза о side
effect была обоснованной. Оба coupling удалены: query-side expiry перенесён в
message-handler, а lock ограничен явным inner scope перед getdata.

Фактическая хронология устанавливает causal bounds:

* первый `getinfo` в stalled episode не меняет высоту или pipeline;
* второй `getinfo` также оставляет высоту 7802641 и pipeline пустым;
* после рестарта sync доходит с 7802641 до 7802732 до первого `getinfo`;
* `sync_peer=none` напрямую следует из version predicate для observed peers;
* restart создаёт новые version handshakes, использующие правильный predicate.

Следовательно, видимое совпадение с RPC не является корнем. Scheduling effect
общего mutex должен быть виден через `GETINFO_PROBE_*`/`SYNCLOCK`, но корректность
downloader больше от RPC не зависит.

## Диагностические события

Все события, участвующие в доказательстве порядка, должны иметь
`time_us=GetTimeMicros()`; `SetBestChain date=` остаётся block time.

Минимальная последовательность для recovery test:

```text
SYNC_EVENT event=PROCESS_BLOCK_BEGIN hash=H
SYNC_EVENT event=ACCEPT_BLOCK_BEGIN hash=H
SYNC_EVENT event=CHECKBLOCK_POW_PAYEE_REJECT hash=H
SYNC_EVENT event=REJECT_RETRY_RECORDED hash=H
BLOCKREQTRACE event=BLOCK_RESULT result=rejected hash=H indexed_after=0
BLOCKREQTRACE event=GETBLOCKS_QUEUE peer=P begin_height=N
SYNC_EVENT event=REJECT_RETRY_SCHEDULED peer=P hash=H
SYNC_EVENT event=STALL_RECOVERY_OWNER peer=P rejected_retry=H
BLOCKREQTRACE event=GETDATA_SEND hash=H peer=P path=askfor source=reject-recovery
SYNC_EVENT event=PROCESS_BLOCK_BEGIN hash=H
SYNC_EVENT event=ACCEPT_BLOCK_BEGIN hash=H
SYNC_EVENT event=SETBESTCHAIN_COMMIT hash=H height=N
BLOCKREQTRACE event=BLOCK_RESULT result=accepted-active hash=H
```

И для пустого pipeline без rejected hash:

```text
SYNCSTATE time_us=... local_height=N best_header=N max_peer_height=N+K sync_peer=none blocks_in_flight=0 queued_getblocks=0
SYNC_EVENT event=STALL_RECOVERY_OWNER peer=P rejected_retry=0
BLOCKREQTRACE event=GETBLOCKS_QUEUE peer=P begin_height=N
BLOCKREQTRACE event=GETDATA_SEND ...
BLOCKREQTRACE event=BLOCK_RESULT result=accepted-active
SYNC_EVENT event=SETBESTCHAIN_COMMIT height=N+1
```

Все `SYNC_EVENT`, `SYNCSTATE` и `BLOCKREQTRACE` строки выше содержат
`time_us=GetTimeMicros()`. Существующие `GETBLOCKS_QUEUE`, `STALL_RECOVERY`,
`GETDATA_SEND` и `BLOCK_RESULT` сохранены для request lifecycle; отдельные
chain-processing events дают локальное время до consensus check и после commit.

## Regression tests

Component tests в `src/test/p2p_sync_tests.cpp` моделируют decision state без
реального RPC/socket timing.

Отдельные purity tests помещают искусственно просроченный hash в in-flight state,
вызывают `copyStats()` или `IsInitialBlockDownload()` и проверяют, что query не
удалил его. Это regression coverage удаления RPC/GUI-side expiry.

### Empty-pipeline recovery

```text
local_height < peer_height
max_peer_height > local_height
IBD = false
sync_peer = none
global in-flight = 0
global queued getblocks = 0
```

Проверяется, что первый tick только устанавливает progress baseline, tick после
stall timeout ставит ровно один locator одному из двух peers, а active pipeline и
cooldown подавляют повтор. Отдельный test прогоняет backoff 2x…32x и доказывает,
что после достижения cap recovery остаётся возможным, но не превращается в storm.

### Rejected-block recovery

1. записать `BLOCK_RESULT rejected`, `indexed_after=0` для следующего hash;
2. очистить pipeline;
3. оставить peer выше local height;
4. не генерировать новый `inv`;
5. продвинуть mock time за cooldown;
6. ожидать один direct `AskFor` того же hash и один locator у того же owner;
7. повторно записать тот же reject, очистить тестовый pipeline и убедиться, что
   второй direct `AskFor` не появляется, generic locator остаётся возможным и
   следующий attempt выбирает другого owner без overlap;
8. отдельно очистить recorded hash как после accept и проверить, что generic
   recovery не ставит прямой retry уже принятого hash.

### RPC independence

Recovery API не принимает RPC state и вызывается каждым циклом message-handler;
component tests достигают `AHEAD_IDLE -> REQUESTING` без любого RPC. Отдельный
tест выполняет `getinfo`, `getblockcount`, `getbestblockhash`, `getpeerinfo` и
`getblockchaininfo` через настоящий `tableRPC.execute()` и проверяет неизменность
`fStartSync`, queued locator, `mapAskFor`, in-flight и `mapAlreadyAskedFor`.
Инструментированный live control сравнивает те же RPC по `time_us`, как описано в
`getinfo-sync-side-effects.md`.

### Duplicate-traffic regression

Проверяются:

* repeated decision ticks;
* несколько одновременно подходящих peers;
* уже queued locator;
* block ownership (`mapAskFor`/in-flight) у текущего или другого peer;
* controlled owner rotation только после полного drain и cooldown;
* отсутствие progress на нескольких exponential-backoff интервалах.

Покрытые component cases показывают один locator, отсутствие cross-peer direct
schedule и отсутствие повторного direct retry того же hash. Проверка already-known
hash остаётся в существующем `AlreadyHave` send path; socket-level trace должна
дополнительно подтвердить ноль реально отправленных duplicate `getdata`.

### Version boundary

Табличный тест минимум для версий 43950, 50000, 70001, 70002, 70005 и 70006.
Handshake, `StartSync`, recovery и diagnostics должны возвращать одинаковую
eligibility.

## Ограничения

* Эта работа не меняет collateralnode payee validation. Повтор может стать
  успешным только если внешнее collateralnode state уже стало достаточным.
* Старый журнал доказывает порядок, но не длительности вокруг RPC: там нет local
  microsecond timestamp у `ThreadRPCServer`/`SetBestChain`. Новая сборка добавляет
  `time_us`, но старую историю ретроспективно восстановить нельзя.
* `best_header` в старом `SYNCSTATE` — misnomer для max peer height. Новая строка
  разделяет `best_header` и `max_peer_height`.
* Query-side mutations IBD/copyStats удалены, но несколько peer fields всё ещё
  читаются без единого dedicated state mutex; cached probe явно сообщает возраст
  snapshot и не используется для recovery decisions.
* Collateralnode manager не предоставляет надёжные sync-state/list-height поля:
  probe пишет их как `unavailable`/`-1`, сохраняя доступные list count/state.
* Component harness доказывает deterministic recovery и throttling. Несколько
  повторов live RPC-control эксперимента на естественно stalled старой базе всё
  равно нужны для статистического ответа о scheduler effect конкретной платформы.
