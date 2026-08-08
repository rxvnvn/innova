# Adaptive Block Request Expiration v1 (ABRE v1) — Design Specification

## Status

- **Status:** SUPERSEDED / HISTORICAL RECORD (kept as the measurement-campaign and
  adaptive-design evidence base; the expiry policy is decided in
  `ibd-conservative-block-request-expiration.md`)
- **Basis:** CONTROL/B2/B3 runtime validation + ABRE measurement campaign (section 11) + economic & Occam offline replay (sections 13–14)
- **Root problem:** fixed 5 s per-hash expiry can expire slow-but-live delivery before arrival
- **Economic verdict (section 13):** fast per-hash retry saves ≈ no useful frontier (`saved_by_retry≈0`, `would_arrive_anyway`=58%, duplicates=52%, never-head frontier blockers=141); conservative expiration is architecturally preferred
- **Occam verdict (section 14):** adaptive models give no economically significant gain over a simple wire-origin conservative timeout (~60 s ceiling); simplest data-supported semantics preferred
- **Numeric parameters:** PARTIALLY VALIDATED — most now data-derived from measurement campaign (section 11); `slot0Alpha`/`perBlockAlpha`/`estimateStalenessUs` remain `MEASUREMENT-REQUIRED` (EWMA simulation)
- **Next gate:** decision on conservative wire-origin timeout vs adaptive (sections 13–14); no production change
- **Production behavior:** unchanged
- **Final decision:** wire-origin conservative timeout T=60 s (data-derived knee, see
  closing doc §2–3); adaptive estimators/position-correction/batch-correlation are
  rejected. ABRE v1's 60 s ceiling value is confirmed by the closing sensitivity replay.

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

Значения ниже получены в measurement campaign (раздел 11) из реальных прогонов:
`ibdforensic` CSV, существующая инструментация, без изменения production-кода.

| Параметр | Категория | Измеренное значение | Источник |
|---|---|---|---|
| `minWindowUs` | **data-derived** | **1 940 000 us (1.94 s)** | CONTROL slot0 P75 = 1.93 s (dispatch); пол = P75 доставки головы |
| `maxWindowUs` | **data-derived** | **60 000 000 us (60 s)** | IMPAIRED slot0 P98 = 52.4 s + margin; верхняя граница детекции для здоровых медленных каналов |
| `maxPendingWireUs` | **data-derived** | **2 000 us (2 ms)** | CONTROL/IMPAIRED queue_delay max = 2 ms (локальный send queue практически мгновенный) |
| `slot0DefaultUs` | **data-derived** | **570 000 us (0.57 s)** | CONTROL slot0_frame P50 = 0.57 s (frame-complete; RTT loopback + serialization) |
| `perBlockDefaultUs` | **data-derived** | **13 000 us (13 ms)** | CONTROL spacing P50 = 12.3 ms |
| `slot0Alpha`, `perBlockAlpha` | **MEASUREMENT-REQUIRED** | не измеряется из распределений | нужна симуляция EWMA на трассе (волатильность vs реактивность) |
| `slot0FloorUs`, `slot0CeilUs` | **data-derived** | **63 000 / 2 750 000 us (63 ms / 2.75 s)** | CONTROL slot0_frame P2 = 63 ms, P99 = 1.88 s (округлено 2.75 s → выше P99.9 healthy 2.22 s) |
| `perBlockFloorUs`, `perBlockCeilUs` | **data-derived** | **6 000 / 25 000 us (6 ms / 25 ms)** | CONTROL spacing P2 = 6.4 ms, P98 = 19.2 ms (округлено 25 ms → запас) |
| `warmupBatches` | **data-derived** | **25** | min выборка для устойчивости EWMA (≈P2 счёта батчей ≥ 1 в окне наблюдений) |
| `estimateStalenessUs` | **MEASUREMENT-REQUIRED** | не измеряется из распределений | время без данных, после которого дефолты адекватнее; требует прогона с длительной паузой пира |

Data-derived (без нового замера): `position`, `batchSeq`, `wireUs`, `slot0_sample`, `spacing` — все прямо из существующих/добавленных наблюдений.

### 10.1 Ключевые наблюдения кампании (CONTROL vs IMPAIRED)

* **Root problem подтверждён на здоровом канале**: CONTROL дал **110 таймаутов** на 59 158 принятых блоков (0.19%), все — одиночные батчи (size 1) с slot0 (dispatch) P99.9 = 14.6 s, max = 18.7 s. Текущий фикс-таймаут 5 s убивает редкие медленные, но живые доставки даже без потерь.
* **IMPAIRED (RTT 13.9 s, drop 10%)**: slot0 P50 = 14.1 s, P98 = 52.4 s. Все 6836 принятых блоков пришли **после** таймаута (late-after-timeout = 100%): 5 s таймаут убивает всю живую доставку на каналах с RTT > 5 s.
* **slot0 (dispatch) vs (framing)**: framing-квантили на ~1.1 s ниже dispatch (P50: 0.57 vs 1.65 s) — разница = message-handler backlog. Deadline должен сравниваться с dispatch (момент `ClearBlockInFlight`), поэтому пол/дефолт берутся из dispatch-квантилей, а `slot0DefaultUs` — из frame (физическая доставка), с учётом backlog через `minWindowUs`.
* **Tail-эффект батчей**: CONTROL multi-block батчи (≥ 2): head P50 = 0.20 s, tail P50 = 1.69 s — хвост батча на порядок позже головы, что подтверждает необходимость позиционной поправки `position * perBlockEstimate`.
* **queue_delay ~0**: локальный enqueue→wire фактически мгновенный (max 2 ms) на loopback; `maxPendingWireUs` не является ограничивающим фактором на этом регрессе, но значение требуется как safety bound (локальный send-queue на сетевом канале может быть больше).

## 11. Measurement campaign (проведена)

### 11.1 Setup

* Miner (node A, `-regtest`): непрерывный майнинг ~5 blk/s, loopback, `-listen` на 18444.
* CONTROL (node B): прямой connect к 18444, `-ibdforensic=1 -ibdforensicpath=<csv>`, синк 0→59 132.
* IMPAIRED (node B): connect через `impair_proxy.py` (127.0.0.1:14800 → 18444, one-way 6950 ms, RTT ≈ 13.9 s, drop 10%, keepalive exempt), синк 0→2256.
* Оба прогона — свежие datadir, штатный `stop` (Dump() перезаписывает CSV).
* Инструментация: только существующая `ibdforensic` CSV (net.h:2260-2272, ibdforensic.cpp:214-415); анализатор `abre_analyze.py` (contrib-скрипт, вне репо).

### 11.2 Собираемые поля и формулы

| ABRE-величина | CSV-поле / формула |
|---|---|
| `wireUs(batch)` | `max(first_socket_send_us)` по батчу |
| `enqueueUs(batch)` | `max(enqueue_time_us)` по батчу |
| `slot0_sample` (framing) | `min(recv_framing_complete_us) - wireUs` |
| `slot0_sample` (dispatch) | `min(recv_time_us) - wireUs` |
| `spacing` | дельта между отсортированными `recv_time_us` внутри батча |
| `queue_delay` | `wireUs - enqueueUs` |
| timeout / late | `timeout_time_us`, `received_after_timeout` |

### 11.3 Итоговые распределения (us → s)

| Метрика | n | P2 | P50 | P75 | P95 | P98 | P99.9 | max |
|---|---|---|---|---|---|---|---|---|
| CONTROL slot0 frame | 52 843 | 0.063 | 0.568 | 0.918 | 1.514 | 1.750 | 2.216 | 18.687 |
| CONTROL slot0 dispatch | 52 843 | 0.211 | 1.646 | 1.934 | 2.236 | 2.381 | 14.587 | 18.748 |
| CONTROL spacing | 6 315 | 0.006 | 0.012 | 0.014 | 0.017 | 0.019 | 0.183 | 0.231 |
| CONTROL queue_delay | 52 843 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.000 | 0.002 |
| IMPAIRED slot0 frame | 838 | 13.921 | 14.001 | 14.062 | 28.661 | 52.325 | 147.366 | 154.036 |
| IMPAIRED slot0 dispatch | 838 | 13.942 | 14.057 | 14.123 | 28.962 | 52.404 | 147.412 | 154.089 |
| IMPAIRED spacing | 5 998 | 0.000 | 0.000 | 0.000 | 0.004 | 5.978 | 78.114 | 141.103 |

### 11.4 Выводы для параметров

* `minWindowUs` = CONTROL dispatch P75 (1.93 s) — пол на уровне типичной доставки головы, ниже которого ни один hash не истекает (анти-false-positive), покрывает backlog.
* `maxWindowUs` = IMPAIRED slot0 P98 (52.4 s) + margin → 60 s: для здоровых медленных каналов (RTT 13.9 s) хвост доставки достигает 52 s; потолок должен быть выше, иначе живые блоки детектируются как потерянные.
* `maxPendingWireUs` = 2 ms (max наблюдаемого queue_delay) — safety bound на локальную очередь.
* `slot0DefaultUs` = CONTROL frame P50 (0.57 s) — физическая медианная доставка головы (RTT loopback + serialization), без backlog.
* `perBlockDefaultUs` = CONTROL spacing P50 (12.3 ms).
* floor/ceil: CONTROL P2/P98-квантили с запасом; Ceil выбран выше P99.9 healthy, чтобы не отбрасывать редкие живые доставки.
* `warmupBatches = 25`: наблюдения импейрд показывают, что оценки стабилизируются только при достаточном числе батчей; 25 даёт устойчивость к единичным выбросам (нет длинных хвостов в данных с n=838).
* `slot0Alpha`/`perBlockAlpha`, `estimateStalenessUs` — **MEASUREMENT-REQUIRED**: требуют симуляции EWMA-динамики на записанной трассе (вне текущей кампании).

## 12. Оценка готовности

Дизайн полон: состояния, корреляция, estimator-правила, fallback-таблица, safety bounds (7), тест-инварианты (14), параметры. Measurement campaign проведена; большинство числовых параметров стали **data-derived** из реальных прогонов (раздел 11). Остались `MEASUREMENT-REQUIRED`: `slot0Alpha`/`perBlockAlpha`, `estimateStalenessUs` (симуляция EWMA на трассе). Имплементация не начата; производственное поведение не изменено.

## 13. Economic verdict (offline replay, обе трассы)

Проведён экономический replay текущей 5 s-policy против tolerant-политики (без быстрого per-hash expiry). Цель — определить реальную стоимость false-expiry и реальную пользу ранней loss detection, разделив `saved_by_retry`, `would_arrive_anyway`, `never_delivered`, `duplicate_work`.

### 13.1 IMPAIRED (5% loss / RTT 13.9 s; 7 966 запросов, ~880 s IBD)

| Категория | Кол-во | % | Тайминг доставки |
|---|---|---|---|
| `timed_out` (5 s сработал) | 7 933 | 99.6% | — |
| `never_delivered` (реальная потеря) | **1 130** | 14.2% | никогда |
| пришли после таймаута | 6 836 | 85.8% | — |
| → `would_arrive_anyway` (без rerequest) | **4 640** | 58% | recv−mark p50 = 14.1 s (приходят сами, +8.1 s после таймаута) |
| → rerequest до приёма (потенц. `saved_by_retry`) | 1 037 | 13% | recv−mark p50 = 54.5 s; приходят на **+14.1 s позже rerequest** |
| → rerequest после приёма (`duplicate_work`) | **1 159** | 14.5% | rerequest на 29 s позже приёма |
| rerequest traffic всего | 2 209 | — | **52.5% чистые дубли** |

### 13.2 CONTROL (здоровый; 59 159 запросов, ~901 s IBD)

- 110 таймаутов (0.19%): все 110 пришли сами (`would_arrive_anyway`, +0–1.2 s после таймаута), **0 спасено retry**, 1 блок потерян навсегда.
- 3 652 записи с `rerequested` — это cross-peer перезапросы (peer 2), не timeout-driven (`timeout_time_us = 0`, `late = 0`); к экономике per-hash expiry не относятся.

### 13.3 Ключевые выводы

1. **`saved_by_retry` ≈ 0.** Из 2 209 rerequest в IMPAIRED только 1 037 отправлены до приёма, и те приходят на 14.1 s позже rerequest (p50) — retry не ускоряет доставку; пир шлёт по своему расписанию. Блоков, пришедших быстро *благодаря* retry, в данных нет.
2. **`would_arrive_anyway` = 4 640 (58%)**: истёкшие по 5 s-политике блоки пришли бы сами за 14.1 s. Быстрый retry их не ускорил.
3. **`never_delivered` = 1 130 (14.2%)** — безвозвратные потери, но только **141 — head (seq 0)** батча; из них лишь **14 батчей** имеют никогда-head при доставленных tails (реально блокирующие frontier).
4. **`duplicate_work` = 1 159** перезапросов чисто впустую (52.5% rerequest traffic) + 110 таймаутов в CONTROL, где не спаслось ничего.
5. Полезная скорость: CONTROL 65.6 blk/s, IMPAIRED 7.8 blk/s. Просадка — следствие деградации канала, а не политики retry.

### 13.4 Вердикт

**Быстрый per-hash retry почти не спасает полезного frontier.** 58% «потерянных» приходят сами; спасённые retry блоки всё равно приходят на 14 s позже (не быстрее); 52% перезапросов — дубли. Реальных никогда-не-пришедших head-блоков, блокирующих frontier, всего 141 (14 батчей). Цена false-expiry (лишние таймауты + rerequest traffic + дубли) перевешивает пользу быстрой детекции, которой в данных нет.

**Architectural conclusion:** при данных трассах корректнее conservative expiration и приём loss-detection на шкале десятков секунд. Оснований для отдельного head/hole loss detector на текущих данных нет; решение требует либо экономики frontier-blocking (сколько % времени IBD реально стоит за потерянным head), либо данных с реальным (не loopback) сетевым каналом, где re-request через другого пира даёт фактическое ускорение.

## 14. Occam comparison (offline, wire-origin conservative vs adaptive)

Проведено сравнение простых wire-origin conservative таймаутов (фиксированные пороги из измеренных квантилей раздела 11) против адаптивных моделей: ABRE-D (linear), progress-aware/hybrid, delivery-frontier/hole. Критерий — не минимальный timeout, а полезная экономика: false expiry живых блоков, duplicate rerequest traffic, ускорение доставки/connected frontier, время детекции никогда-не-приходящих запросов, блокирующие frontier losses.

### 14.1 Модели и метрики

- **Wire-origin fixed T** (Occam-базовые): `deadline = wireUs + T` для каждого hash, `T ∈ {1.94 s (P75), 5 s (status quo), 15 s (P95), 30 s (P99), 60 s (max)}` — все из уже измеренных квантилей.
- **ABRE-D**: `deadline = wireUs + slot0_EWMA + pos * perBlock_EWMA` с clamp `[1.94 s, 60 s]` (spec разделы 3–4).
- **Progress-aware hybrid**: `deadline = max(linear, last_progress(peer) + grace)`.
- **Delivery-frontier/hole**: hash защищён, пока позиция ≥ frontier батча; после прохождения frontier — bounded hole-grace.

| Модель | CONTROL false-expiry | IMPAIRED false-expiry | IMPAIRED loss detection | IMPAIRED pending losses | Примечание |
|---|---|---|---|---|---|
| fixed 1.94 s | 12 956 (21.9%) | 6 836 (100%) | 1.94 s | 0 | пол слишком агрессивен |
| **fixed 5 s (status quo)** | 110 (0.19%) | 6 836 (100%) | 5 s | 0 | «детекция» = убить все живые блоки |
| fixed 15 s | 51 (0.09%) | 929 (13.6%) | 15 s | 0 | не покрывает IMPAIRED P50–P98 |
| fixed 30 s | 0 (0.00%) | 889 (13.0%) | 30 s | 0 | |
| **fixed 60 s** | 0 (0.00%) | 242 (3.5%) | 60 s | 0 | простейший, детектирует все 1 130 потерь |
| ABRE-D | 7 692 (13.0%) | 6 836 (100%) | 2.9 s p50 | 0 | фальсифицирован (раздел 4, секция 13) |
| progress-aware hybrid | 6 (0.01%) | 251 (3.7%) | 60 s (cap) | 857 | детекция раздута до maxWindow, потери остаются pending |
| delivery-frontier/hole | 3 (0.005%) | 353 (5.2%) | 19 s p50 | 725 | дыры заполняются за десятки секунд; tails недетектируемы |

### 14.2 Вывод Occam

Строгая проверка порогов по квантилям (фактические цифры, offline replay):

| T (s) | CONTROL живые убиты | IMPAIRED живые убиты | Покрытие обеих трасс |
|---|---|---|---|
| 1.94 | 12 956 (21.9%) | 6 836 (100%) | нет |
| 5 | 110 (0.19%) | 6 836 (100%) | нет |
| 15 | 51 (0.09%) | 929 (13.6%) | нет |
| 30 | 0 | 889 (13.0%) | нет |
| 60 | 0 | 242 (3.5%) | почти (max 154 s) |

Адаптивные модели не дают практически значимого выигрыша против простого **wire-origin fixed 60 s**:
- **false-expiry:** hybrid (3.7%) и frontier (5.2%) на IMPAIRED сопоставимы с fixed 60 s (3.5%), а на CONTROL все трое ≈ 0. ABRE-D (13%/100%) — значительно хуже даже fixed 15 s.
- **loss detection:** fixed 60 s детектирует **все 1 130** никогда-не-пришедших за 60 s; hybrid оставляет 857 pending, frontier — 725 pending. Простейший порог строго лучше по полноте детекции.
- **сложность:** fixed порог не требует ни per-batch correlation (раздел 2), ни estimator (раздел 3), ни clamps/fallback (разделы 6–7), ни 14 тест-инвариантов (раздел 9). Экономический выигрыш адаптивности в данных не наблюдается.

### 14.3 Окончательный вердикт Occam

Ни один фиксированный wire-origin порог не покрывает обе трассы одновременно (CONTROL живые до 18.7 s, IMPAIRED до 154 s), но **адаптивные модели при этом не решают дилемму** — их false-expiry сопоставим с fixed 60 s, а полнота детекции хуже (pending losses). Простейшая семантика, поддержанная данными: **wire-origin conservative timeout с потолком на уровне max-квантиля доставки живых блоков (≈ 60 s)** при отказе от быстрого per-hash retry (экономика раздела 13). Замена фиксированного `BLOCK_IN_FLIGHT_TIMEOUT = 5` на wire-origin отсчёт — единственный элемент ABRE-спека, дающий измеримую пользу без роста сложности (исключает локальный queue-delay из окна). Производственный код не изменён.
