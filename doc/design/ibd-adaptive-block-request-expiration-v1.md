# Adaptive Block Request Expiration v1 (ABRE v1) — Design Specification

## Status

- **Status:** DESIGN / NOT IMPLEMENTED
- **Basis:** CONTROL/B2/B3 runtime validation
- **Root problem:** fixed 5 s per-hash expiry can expire slow-but-live delivery before arrival
- **Numeric parameters:** NOT YET VALIDATED
- **Next gate:** measurement campaign for all `MEASUREMENT-REQUIRED` parameters
- **Production behavior:** unchanged

## 0. Scope & goals

Заменить фиксированный `BLOCK_IN_FLIGHT_TIMEOUT = 5` (net.h:2171) на per-hash дедлайн, отсчитываемый от **фактического wire-send** батча, с позиционной поправкой и адаптивной оценкой per-block латентности. Цели: (a) исключить локальный queue-delay из окна, (b) исключить tail-таймауты здоровых батчей, (c) сохранить жёсткую верхнюю границу детекции потерянного блока, (d) не менять существующую семантику owner/gauges/late-delivery.

Вне скоупа v1: адаптация по throughput (модель F), sliding-window E, cross-peer приоритеты.

## 1. State model

### 1.1 Per-hash phases (in addition to existing owner states)

`QUEUED` → `IN_FLIGHT_PENDING_WIRE` → `IN_FLIGHT_WIRE_SENT` → (expire) → `EXPIRED` → late-delivery
                                             └→ (receive) → `RECEIVED`

- `QUEUED`: owner назначен (net.h:9631-9641), не помечен. Без изменений.
- `IN_FLIGHT_PENDING_WIRE`: hash добавлен в `setBlocksInFlight` через `MarkBlockInFlight`, но его батч ещё не получил первый `send()`.
- `IN_FLIGHT_WIRE_SENT`: `firstSendUs` батча проштампован (SocketSendData). С этого момента считается полный дедлайн.
- `EXPIRED` / `RECEIVED`: существующие пути (net.h:2185-2233, 2274-2316) — без изменений семантики.

Фаза выводится, а не хранится: hash в `WIRE_SENT` ⟺ `wireUs(batch(hash)) > 0`.

### 1.2 Per-batch state (новое, per-peer)

```
struct PendingWireBatch {
    uint64_t batchSeq;        // монотонный per-peer счётчик
    size_t   nBlocks;         // кол-во hashes в батче
    int64_t  enqueueUs;       // GetTimeMicros() в момент PushMessage("getdata")
    int64_t  wireUs;          // 0 = не отправлен; firstSendUs из SendMessageMeta
    int64_t  firstRecvUs;     // 0 = ещё ничего не получено (для estimator)
    int64_t  lastRecvUs;
    size_t   nRecv;
};
```
Контейнеры: `std::deque<PendingWireBatch> vPendingWireBatches` — порядок постановки == порядок getdata в `vSendMsg`; `std::map<uint256, uint64_t> mapBlockInFlightBatchSeq` — hash → batchSeq; `std::map<uint256, size_t> mapBlockInFlightPosition` — hash → позиция в батче (0-based). Локальный аккумулятор `curBatchSeq`/`curBatchPos` на период построения `vGetData`.

### 1.3 Per-peer estimator state (новое)

```
int64_t  slot0EstimateUs;       // EWMA, RTT + serialization первого блока
int64_t  perBlockEstimateUs;    // EWMA среднего межблочного интервала в батче
int64_t  lastEstimateUpdateUs;  // для staleness-декаи
size_t   warmupBatchesSeen;     // число батчей с валидными данными
```

## 2. Batch correlation & clock

1. Построение `vGetData` (main.cpp:11028-11105, 9625-9653): на первом mark в батче `curBatchSeq = ++peerBatchCounter`, `curBatchPos=0`. Каждый `MarkBlockInFlight(hash)` дополнительно пишет `mapBlockInFlightBatchSeq[hash]=curBatchSeq`, `mapBlockInFlightPosition[hash]=curBatchPos++`.
2. На каждый `PushMessage("getdata", ...)` (main.cpp:11112/11181/9684): `vPendingWireBatches.push_back({curBatchSeq, curBatchPos, GetTimeMicros(), 0,0,0,0})`, сброс аккумулятора. AskFor-флаш на 1000 (main.cpp:11105) автоматически рождает новый батч.
3. В `SocketSendData` (net.cpp:7104-7109), когда `mit->fStampPending` и `mit->command=="getdata"`: pop головы `vPendingWireBatches`, проставить `wireUs = GetTimeMicros()`, выставить `mit`-овую batchSeq (добавить поле `uint64_t batchSeq` в `SendMessageMeta`, net.h:1103) для инварианта корреляции.
4. `ExpireBlockInFlight(nNowUs=GetTimeMicros())`: подменяется миксекундным nNow (сейчас net.h:2169 использует `GetTime()` целые секунды). `mapBlockInFlightSince` (секунды) сохраняется для совместимости trace/`nSenderInFlightAge` (main.cpp:9794-9797).

Инвариант выравнивания: `|vPendingWireBatches|` == число неотправленных getdata в `vSendMsg`; каждая первой-send'нутая getdata строго соответствует голове очереди.

## 3. Estimators

Оба — per-peer EWMA, обновляются **только** по батчам с `wireUs>0` (защита от queue-шума) и только при приёме блоков этого батча.

### 3.1 `slot0EstimateUs`
Номинал дедлайна головного блока: время от первого байта getdata до прихода первого блока батча = RTT + serialization одного блока.
- Наблюдение: `slot0_sample = firstRecvUs(b) - wireUs(b)`.
- Обновление при первом приёме блока батча b.
- Валидность: `slot0_sample ∈ [slot0FloorUs, slot0CeilUs]`, иначе отброс (outlier clamp).
- `slot0EstimateUs = EWMA(slot0EstimateUs, slot0_sample, slot0Alpha)`.

### 3.2 `perBlockEstimateUs`
Номинал поправки на позицию: средний межблочный интервал внутри батча.
- Наблюдение при `nRecv ≥ 2`: `spacing = (lastRecvUs(b) - firstRecvUs(b)) / (nRecv-1)`.
- Валидность: `spacing ∈ [perBlockFloorUs, perBlockCeilUs]`, иначе отброс.
- `perBlockEstimateUs = EWMA(perBlockEstimateUs, spacing, perBlockAlpha)`.

### 3.3 Обновление
- Происходит в `ClearBlockInFlight` (main.cpp:9799), где уже есть `nTimeReceived`, и в batch-close при `nRecv==nBlocks`.
- `warmupBatchesSeen++` за каждый батч, давший хотя бы одно валидное наблюдение.
- Staleness: если `now - lastEstimateUpdateUs > estimateStalenessUs` → заменить оба оценки на дефолты и сбросить `warmupBatchesSeen=0`.

## 4. Deadline function

Для hash h, батч b(h), peer p:

```
nominal(h) = wireUs(b) + slot0Estimate_p + position(h) * perBlockEstimate_p
deadline(h) = clamp(nominal(h),
                    wireUs(b) + minWindowUs,        // floor
                    wireUs(b) + maxWindowUs)        // ceiling
```

- Позиция 0: `deadline ≈ wireUs + slot0` (аналог текущих 5 сек, но от wire и с запасом clamps).
- Хвост батча N: `deadline ≈ wireUs + slot0 + (N-1)*perBlock` → здоровый медленный пир не фальшивит.
- `maxWindowUs` — жёсткий предел: застрявший блок истекает не позже этого срока (гарантия детекции).
- `minWindowUs` — жёсткий минимум: даже при недооценке estimator блок не истекает раньше.

## 5. Expiration semantics

В `ExpireBlockInFlight(nNowUs)`:
- `IN_FLIGHT_PENDING_WIRE`: истекает при `nNowUs - batch.enqueueUs > maxPendingWireUs`. Это **локальная** проблема (наш send-queue). Учёт: owner освобождается, `setBlocksInFlight` erase, gauges декрементятся (только `ACTIVE_DECREMENT_INFLIGHT_TIMEOUT` без изменения сути), **без** `RecordIbdBlockTimeout`/`timeout_score` и **без** late-delivery expectation. Блок доступен для перезапроса.
- `IN_FLIGHT_WIRE_SENT`: истекает при `nNowUs > deadline(h)`. Полный существующий путь (net.h:2185-2233): `RecordIbdBlockTimeout`, `timeout_score`, late-delivery, owner release, `EraseAlreadyAskedForIfUnowned`.
- Late-приход после expiry — существующий путь (net.h:2296-2311), без изменений.

## 6. Fallbacks

| Условие | Fallback |
|---|---|
| Warmup (валидных батчей < warmupBatches): оценки не доверены | `slot0=slot0DefaultUs`, `perBlock=perBlockDefaultUs` (детерминированно, инвариант I9) |
| Нет наблюдений вовсе (cold start) | дефолты + clamps |
| Estimator stale (staleness окно превышено) | сброс к дефолтам, warmup=0 |
| Батч в PENDING_WIRE дольше `maxPendingWireUs` | local-expire, без peer-наказания |
| `firstSendUs` недоступен (вырожденный кейс) | дедлайн от `enqueueUs + maxPendingWireUs` |
| Валидное наблюдение вне clamp-диапазона | отброс, оценка не трогается |

## 7. Safety bounds

1. `deadline(h) ≤ wireUs(b) + maxWindowUs` — жёсткий потолок (гарантия детекции).
2. `deadline(h) ≥ wireUs(b) + minWindowUs` — жёсткий пол (анти-false-positive).
3. `PENDING_WIRE` ограничен `maxPendingWireUs` от enqueue.
4. Estimator clamps: `slot0 ∈ [slot0FloorUs, slot0CeilUs]`, `perBlock ∈ [perBlockFloorUs, perBlockCeilUs]` — постусловие update-правила.
5. Capacity: `|setBlocksInFlight| ≤ nMaxBlocksInFlightPerPeer` (net.h:11042) — сохранено.
6. Один переход из IN_FLIGHT на hash (двойной декремент невозможен) — сохранено.

## 8. Telemetry

- `firstSendUs` уже есть (net.cpp:7106). Новое: `RecordBatchWireDelay(batchSeq, wireUs - enqueueUs)` — распределение локального queue-delay.
- `RecordDeadline(hash, position, slot0, perBlock, deadlineUs, wireUs)` — для верификации (Part 9 exp. 1,2).
- `RecordEarlyExpire()` / `RecordLateArrival()` — уже частично есть (`receivedAfterTimeout`, net.h:2294).
- Экспорт per-peer `slot0EstimateUs`, `perBlockEstimateUs`, `warmupBatchesSeen` в trace.

## 9. Test invariants (I1–I14)

- **I1 Clock-origin**: для hash в `WIRE_SENT`, возраст всегда `now - wireUs(b)`; ни один hash не истекает до первого `send()` своего батча.
- **I2 Floor**: `now - wireUs(b) < minWindowUs ⟹` hash жив (никогда не false-expire внутри floor).
- **I3 Ceiling**: `now - wireUs(b) ≥ maxWindowUs ⟹` hash истёк (детекция гарантирована к потолку).
- **I4 Local bound**: hash в `PENDING_WIRE` и `now - enqueueUs ≥ maxPendingWireUs ⟹` истёк или wire-sent.
- **I5 Single-expire**: каждый hash покидает IN_FLIGHT ровно один раз; двойной gauge-декремент невозможен.
- **I6 Late-delivery**: приход после expiry записывается один раз, один `ReleaseBlockRequestOwner`, без повторного декремента.
- **I7 No-local-penalty**: local-expire (I4) не инкрементирует `timeout_score`/`RecordIbdBlockTimeout`.
- **I8 Estimator bounds**: после любого update `slot0, perBlock ∈ [floor, ceil]`.
- **I9 Warmup determinism**: пока `warmupBatchesSeen < warmupBatches`, дедлайны вычисляются только из дефолтов и clamps (детерминированно).
- **I10 Determinism**: при идентичных `(wireUs, position, оценки)` дедлайн идентичен (без случайности).
- **I11 Alignment**: число первых-send getdata == число popped `vPendingWireBatches`; все hash одного батча делят `wireUs`.
- **I12 Capacity**: `|setBlocksInFlight| ≤ nMaxBlocksInFlightPerPeer` всегда.
- **I13 Head coverage**: на здоровом пире с измеренным `slot0`, позиция-0 не false-expire при фактической доставке ≤ `slot0 + tolerance`.
- **I14 Tail coverage**: батч из N блоков со spacing ≤ `perBlock` не даёт ни одного false-expire.

## 10. Parameter table

Все числовые параметры ниже либо data-derived, либо явно помечены `MEASUREMENT-REQUIRED`. Предложенные стартовые значения являются **только экспериментальными отправными точками** (experimental starting points) и НЕ являются defaults или рекомендациями; они требуют измерения и валидации.

| Параметр | Категория | Способ получения / эксперимент |
|---|---|---|
| `minWindowUs` | **MEASUREMENT-REQUIRED** | Гистограмма `slot0_sample` на регрессе; floor ≈ P50/P75 доставки головы. Экспериментальная отправная точка: 5 с (текущее фиксированное значение как консервативный init) |
| `maxWindowUs` | **MEASUREMENT-REQUIRED** | Эксперимент с вариацией порога; верхняя граница детекции потерянного блока. Экспериментальная отправная точка: 30 с |
| `maxPendingWireUs` | **MEASUREMENT-REQUIRED** | Гистограмма `wireUs-enqueueUs` (локальный queue-delay). Экспериментальная отправная точка: 1 с |
| `slot0DefaultUs` | **MEASUREMENT-REQUIRED** | Медиана RTT+первый блок на пуле пиров. Экспериментальная отправная точка: 2 с |
| `perBlockDefaultUs` | **MEASUREMENT-REQUIRED** | Медиана межблочного spacing при IBD. Экспериментальная отправная точка: 50 мс |
| `slot0Alpha`, `perBlockAlpha` | **MEASUREMENT-REQUIRED** | Симуляция на загруженном регрессе (волатильность vs реактивность) |
| `slot0FloorUs`, `slot0CeilUs` | **MEASUREMENT-REQUIRED** | Квантили (2%/98%) `slot0_sample` |
| `perBlockFloorUs`, `perBlockCeilUs` | **MEASUREMENT-REQUIRED** | Квантили (2%/98%) spacing |
| `warmupBatches` | **MEASUREMENT-REQUIRED** | Устойчивость оценок на трассе. Экспериментальная отправная точка: 3 батча |
| `estimateStalenessUs` | **MEASUREMENT-REQUIRED** | Время без данных, после которого дефолты адекватнее. Экспериментальная отправная точка: 60 с |

Data-derived (без нового замера): `position`, `batchSeq`, `wireUs`, `slot0_sample`, `spacing` — все прямо из существующих/добавленных наблюдений.

## 11. Оценка готовности

Дизайн полон: состояния, корреляция, estimator-правила, fallback-таблица, safety bounds (7), тест-инварианты (14), параметры. **Все** числовые параметры либо data-derived, либо помечены `MEASUREMENT-REQUIRED` — ни один не зафиксирован произвольно. Имплементация не начата; производственное поведение не изменено.
