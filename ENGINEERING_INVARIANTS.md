# ENGINEERING INVARIANTS — Innova V2 startup, storage and recovery

Версионируемый реестр защищённых инвариантов. Назначение: **однажды
исправленный и принятый дефект не должен возвращаться незаметно**.

Правила ведения:

1. Каждый инвариант имеет ID, определение, происхождение, эталонные метрики,
   обязательные проверки и статус.
2. Статус `PROVEN` ставится только по реальному исполнению (тест/замер), а не по
   наличию кода. Непроверенное остаётся `PARTIAL`/`UNPROVEN`.
3. Изменение порога, снятие проверки или признание результата устаревшим — это
   **изменение инварианта**: требует OWNER GO и записи в этом файле (дата, кто,
   почему). Молчаливое удаление запрещено.
4. Изменение, ломающее инвариант, **блокирует интеграцию** до исправления или
   отдельного решения OWNER.

Проверки: `scripts/startup_invariants_gate.sh` (+ пороги
`scripts/startup_gate_thresholds.json`), focused suites, функциональные
startup/recovery/reorg-стенды, тяжёлый cold-boot стенд.

---

## MEM-001 — жёсткий лимит памяти без swap и OOM

- **Определение.** Старт и работа узла на реальной цепи укладываются в
  `MemoryMax=1GiB`, `MemorySwapMax=0`: ни OOM-kill, ни swap, при этом
  anonymous-память процесса остаётся ограниченной (не «съеденной» page cache).
- **Область.** `innovad`, авторитетный V2 старт, steady-state, аварийные пути
  (derived rebuild).
- **Происхождение.** OWNER: требование эксплуатировать узел на 1 GiB без swap.
- **Исходный дефект/исправление.** Пики старта (копия setStakeSeen, O(N)
  derived rebuild) могли вывести cgroup за лимит; исправления — MEM-002, MEM-003.
- **Коммиты.** `lab/mem-safe-startup-20261116` (swap set, on-disk rebuild).
- **Обязательные проверки.**
  - gate `boot --vmhwm-kb` (порог `memory.max_vmhwm_kb` = 640 MiB);
  - тяжёлый стенд: cgroup `memory.events` (`oom_kill 0`), `memory.swap.current 0`;
  - отчёт фиксирует `Rss`, `RssAnon`, `RssFile`, `VmHWM`, `memory.current`,
    `memory.peak` отдельными строками (page cache ≠ память процесса).
- **Эталон.** 8 241 282 блока, HDD `/dev/sdb`, 1 GiB: `VmHWM 592.6 MiB`,
  `RssAnon 395 MiB`, `read_bytes 7.32 GB`, `oom_kill 0`, `swap 0` (baseline);
  после правок — см. `MEMORY_STARTUP_ACCEPTANCE.md`.
- **Обнаружение регрессии.** `memory.events` с `oom_kill>0`; `VmHWM` >
  порога; `RssAnon` выше эталона на >10%.
- **Статус.** `PROVEN` для высоты 8.24M.
- **Границы.** MEM-001 не утверждает бесконечный горизонт: единственный
  O(высота) resident — `setStakeSeen` (72 B/PoS-блок, ~23 B/блок высоты); при
  +10M блоков steady ≈ 785 MiB и 768-цель перестаёт выполняться. Это отдельное
  архитектурное решение (fold/retention), а не нарушение текущего инварианта.

## MEM-002 — отсутствие полного дублирования setStakeSeen при старте

- **Определение.** Множество `setStakeSeen` передаётся стартом во владение
  (`swap`/`move`), а не копируется: в процессе не существует двух деревьев
  одного и того же множества.
- **Область.** `AuthoritativeStartup` (файл `blockindex_authoritative_startup.cpp`),
  единственный путь заполнения.
- **Происхождение.** Этап MEMORY & BOUNDED STORAGE 2026-11-16: `setStakeSeen = ss`
  даёт второй RB-tree на 2 642 643 записи.
- **Исправление.** `setStakeSeen.swap(ss)`; содержимое побитово то же.
- **Коммиты.** `lab/mem-safe-startup-20261116`.
- **Обязательные проверки.** gate `boot --vmhwm-kb` (маркер `handoff=swap` в
  строке `stake_seen_source=` — структурная проверка передачи владения, не
  зависящая от RSS); focused suite `startup_memory_invariants_tests`
  (эквивалентность множества); сверка `setStakeSeen_count` через
  `getmemorydiagnostics` (2 642 643 на 8.24M).
- **Эталон.** Счётчик записей неизменен; VmHWM не растёт; PoS duplicate
  protection подтверждается reorg/PoS-тестами.
- **Обнаружение регрессии.** Появление копии видно как скачок `VmHWM`/`RssAnon`
  на ~180 MiB в фазе материализации; `setStakeSeen_count` изменился.
- **Статус.** `PROVEN` (тест + замер после правки).
- **Разрешённое изменение.** Только сменой владения (swap/move) или
  архитектурным решением о хранении (ADR); копирование запрещено.

## MEM-003 — аварийный derived rebuild не требует O(N) RAM

- **Определение.** При повреждённом `derived.dat` rebuild выполняется в
  on-disk scratch-каталог вне иммутабельной генерации; resident-стоимость
  ограничена (битовые карты + одна запись + page cache), а не узлом на запись.
  Результат проходит обычную read-only валидацию (CRC по каждой записи).
- **Область.** `BlockIndexV2Reader::Open` (R3G путь), `blockindex_derived_replay`.
- **Происхождение.** 2026-11-16: `derivedRebuilt` = `std::map` на 8.24M записей
  (~790 MB) + O(N) hash→id и ancestry карты внутри closure.
- **Исправление.** `RebuildDerivedStoreBoundedFromAuthoritative` +
  `BlockIndexDerivedStateStore::CreateForRandomAccess/WriteAt`; родители
  резолвятся через `hashindex`; «уже выведен» — битовая карта N бит.
- **Коммиты.** `lab/mem-safe-startup-20261116`.
- **Обязательные проверки.** `startup_memory_invariants_tests`
  (parity, immutability, stale-scratch, fail-closed);
  `blockindex_crash_corruption_tests` (r3g1..r3g4); тяжёлый замер VmHWM на
  повреждённом 8.24M-наборе.
- **Эталон.** `entries == recordCount`; trust parity; generation bytes
  неизменны; `oom_kill 0`.
- **Обнаружение регрессии.** Появление `std::map`-материализации (рост
  `RssAnon` пропорционально записям), появление rebuild-маркера на здоровом
  наборе, изменение байтов генерации.
- **Статус.** `PROVEN` на фикстуре; тяжёлый замер — в приёмке этапа.

## BOOT-001 — минимально необходимое число полных проходов по генерации

- **Определение.** Старт делает **два** полных последовательных прохода по
  `records.dat`: (1) пересчёт корня генерации в `ValidateGeneration`;
  (2) integrity-скан при открытии ридера, на котором же собирается
  `setStakeSeen`. Третьего прохода нет.
- **Область.** `ValidateGeneration` → `RecomputeGenerationRootFromFiles`;
  `BlockIndexV2Reader::Open`; `AuthoritativeStartup`.
- **Происхождение.** 2026-11-16: три прохода (validation + scan + stake-seen),
  5.6 GB из 7.32 GB чтения.
- **Исправление.** `BlockIndexV2ReaderOptions::integrityScanVisitor` +
  `IntegrityScanRanOnOpen()`; startup собирает множество на скане; при
  memo-пропуске скана — честный fallback на стриминговый builder.
- **Коммиты.** `lab/mem-safe-startup-20261116`.
- **Обязательные проверки.** gate `boot` (ровно одна строка
  `stake_seen_source=`, значение `integrity_scan`); порог
  `boot.max_read_bytes_per_record` (780 B/record против 888 baseline);
  suite `startup_memory_invariants_tests` (в т.ч. честный fallback при memo).
- **Эталон.** 8.24M: `bytes/record` ≈ 660; `stake_seen_source=integrity_scan`.
- **Обнаружение регрессии.** `stake_seen_source=sequential_rebuild` на обычном
  старте; `bytes/record` > 780; появление нового полного прохода.
- **Статус.** `PROVEN`.
- **Разрешённое изменение.** Дальнейшее слияние проходов (например, корень +
  скан в один) — изменение валидационной архитектуры, только через ADR:
  порядок ошибок и fail-closed поверхность меняются.

## BOOT-002 — bounded memory при холодном старте

- **Определение.** Холодный старт не материализует историю: resident-структуры
  ограничены константами (окна tip, LRU, bitmap) плюс `setStakeSeen`.
- **Область.** Весь startup, включая аварийные пути.
- **Происхождение.** Серия A11/A12, R3G, 2026-11-16.
- **Проверки.** gate `boot --vmhwm-kb`; тяжёлый стенд (VmHWM + cgroup peak).
- **Эталон.** См. MEM-001.
- **Обнаружение регрессии.** Рост `RssAnon` пропорционально высоте на любом
  пути, кроме `setStakeSeen` (и `mapBlockIndex` в пределах горизонта).
- **Статус.** `PROVEN` (8.24M).

## BOOT-003 — измеримые пределы чтения, syscalls и времени

- **Определение.** Старт характеризуется измеренными пределами: read syscalls
  на запись и read bytes на запись **для фазы авторитетного блочного индекса**
  (метки `STARTUP_IO` `A_startup_enter` → `B_bootstrap_open`: проходы
  records.dat / derived.dat / active.dat), плюс wall time до RPC на именованном
  профиле стенда. Случайные точечные пробы не масштабируются с числом листьев
  фронтьера.
- **Область.** Последовательные проходы (`ReadAllSequential`,
  `VerifyIntegrity`, пересчёт корня), построение candidate frontier.
- **Почему метрика СКОУПЛЕНА (измерено 2026-11-16, 8.24M, тёплый прогон,
  точные дельты `/proc/self/io` по меткам).** Процесс-широкий `syscr` не
  является расходом чтения records.dat:

```text
компонент (фаза)                                 read syscalls   доля
LevelDB env / dbenv_open                              16 019        2.6%
bootstrap: 2 прохода records + derived + active        4 577        0.75%
  из них собственно sequential-проходы (1 MiB)         3 574
setStakeSeen (на скане)                                    2
candidate frontier (BOOT-004)                               6      0.001%
live-authority open: replay 70 614 post-S записей     497 837       81.7%
wallet load (BDB)                                     10 681        1.8%
Store3A txloc open                                    80 204       13.2%
остальное (tip/invalid/hreg/addrman/RPC)                  60
итого                                                609 524
```

  То есть 0.05 «syscalls на запись» в процесс-широком смысле **недостижим
  конструктивно** на этом datadir: пол из компонентов, не связанных с
  records.dat, равен ~0.073. Фактический расход на records.dat после BOOT-001/002
  — 0.00043 syscall/запись (3 574 syscall на 8 241 282 записи).
  Нормативное изменение определения вынесено на OWNER REVIEW (§ журнал,
  2026-11-16); численные пороги не менялись. Процесс-широкие значения
  по-прежнему измеряются и печатаются как информационные.
- **Происхождение.** 2026-11-16: 2.34M syscalls по 3.1 KB; 92k случайных
  проб в frontier (~300 s).
- **Исправление.** `SequentialIoBuffer` (1 MiB) в четырёх последовательных
  циклах; листья фронтьера опрашиваются в порядке ключа hashindex
  (`BOOT-003`, blockindex_candidate_startup_builder.cpp).
- **Проверки.** gate `boot --log <debug.log> --records N` (метки `STARTUP_IO`
  из `-startuptrace=1 -startupio=1`): `syscalls/record <= 0.05`,
  `bytes/record (rchar) <= 780`; `--seconds N --profile atlas-lab-sdb-hdd`:
  `cold_seconds <= reference * tolerance`; без меток gate откатывается на
  процесс-широкий счётчик и явно печатает, что это legacy-метрика.
- **Эталон.** Baseline до этапа: 0.284 syscalls/record, 888 B/record, 433 s
  (2 228 s исторически, под конкурирующим production I/O). После BOOT-001/002 и
  BOOT-004 (8.24M, HDD, 1 GiB, swap=0, кэш вытеснен): фаза индекса
  0.00056 syscall/record и 574 B/record; до RPC 41.3 s.
- **Обнаружение регрессии.** Порог фазы нарушен (возврат 4 KiB чтений даёт
  ~0.28, третий проход records.dat даёт ~802 B/record); время профиля выше
  42 s x 1.5.
- **Статус.** `PROVEN` (замеры этапа).
- **Разрешённое изменение.** Порог меняется только с OWNER GO.

## BOOT-004 — candidate frontier строится из уже выполняемых сканов

- **Определение.** Статический фронтьер кандидатов (`candidate-leaves.dat`,
  генерационно-связанный, отсортированный набор хэшей) обслуживается
  **проекцией на сканах, которые старт выполняет для целостности**: записи
  собираются на per-record скане `records.dat`, derived-записи — на
  per-entry проходе `derived.dat`. Никаких per-leaf случайных обращений
  (hashindex точка, чтение records, чтение derived) на этом пути нет.
  Разрешение `hash → RecordId` через hashindex и повторное чтение derived
  остаются в honest-fallback (memo уже верифицировал генерацию, старые
  генерации без derived/leaf-метаданных).
- **Область.** `V2BlockIndexStartupAuthority::Open` (читает leaf-набор ДО
  открытия ридера), `BlockIndexV2Reader::Open` (собирает проекции на двух
  сканах), `BlockIndexCandidateStartupBuilder::Build`.
- **Происхождение.** 2026-11-16, замер на 8.24M: 30 633 листа × (hashindex
  точка + запись + derived) = 93 746 + 31 023 read syscalls, 209.9 s + 39.2 s
  (итого ~217 s из 259 s старта), при 6.85 ms на лист (латентность HDD).
- **Исправление.** `BlockIndexV2ReaderOptions::scanFilterHashes` +
  `collectDerivedForScanMatches`; проекции отдаются как
  `ScanRecordMatches()`/`ScanDerivedMatches()`; сборка размещает результаты по
  ИСХОДНОМУ индексу листа, поэтому порядок и содержимое фронтьера не зависят от
  порядка RecordId и от того, какой путь сработал.
- **Коммиты.** `lab/mem-safe-startup-20261116` (BOOT-004).
- **Обязательные проверки.** gate `boot`: ровно одна строка
  `BLOCKINDEX_CANDIDATE_FRONTIER leaf_source=` со значением `integrity_scan`;
  suite `blockindex_candidate_startup_builder_tests`
  (`boot004_scan_projection_parity_with_lookup_path`: паритет быстрого и
  fallback-путей по tipHashes/blocks/trust/best/operatorInvalid/hasData на
  генерации с боковой ветвью, плюс совпадение проекции с `LookupByHash` и
  `derived.Read`).
- **Эталон.** 8.24M: 30 633 листа, `candidate_tips=30633` (идентично),
  фаза кандидата 6 read syscalls / ~1.5 s (было ~217 s), полный старт до RPC
  41.3 s (было 259.3 s), VmHWM 413.2 MB (было 538.8 MB).
- **Обнаружение регрессии.** `leaf_source=hash_lookup` или `parent_marker` в
  обычном старте; рост VmHWM на ~30-60 MB (материализация снапшотов через LRU);
  возврат фазы кандидата к минутам на 8.24M.
- **Статус.** `PARTIAL — ИЗМЕРЕНО, НО НЕ ВКЛЮЧЕНО.` Оптимизация доказана на
  8.24M (паритет + замер), но на tier2-стенде обнаружена блокирующая регрессия:
  свежий нативный datadir (EMPTY_NEW → 31 блок → restart) с включённым фильтром
  возвращается на высоту 0 (mutable tip store читается пустым), тогда как тот же
  стенд с удалённым фильтром проходит restart/reorg(1,20)/restart. Фильтр в
  `V2BlockIndexStartupAuthority::Open` поэтому ОТКЛЮЧЁН; быстрый путь
  недостижим из продакшена и лог печатает `leaf_source=hash_lookup`, который
  гейт отвергает (незаметно включить путь нельзя). Включение допускается только
  после root cause + закрытия регрессии и повторного tier2/батареи.
- **Границы.** Проекция ограничена размером фронтьера (30633 записи ~6 MB),
  т.е. O(F), и не является O(высота). Fail-closed: лист, не найденный сканом,
  или нечитаемый `candidate-leaves.dat` при его наличии — отказ, не «пустой
  фронтьер».

## STORE-001 — единственный авторитетный active tip

- **Определение.** После старта ни одна высота не описана двумя авторитетными
  источниками: BI-V2 tip == Store3A tip (`S2.0 VERIFIED`), `mapBlockIndex`
  пуст/ограничен горизонтом, `fullResident_` отсутствует.
- **Происхождение.** ADR-0001 D-01, ADR-0025 C.
- **Проверки.** Маркер `S2.0 BI-V2 authoritative startup VERIFIED` (gate);
  `blockindex_window2_e2e_tests`, `blockindex_authoritative_restart_tests`.
- **Эталон.** tip 8 241 282/8 241 284 совпадает hash==hash.
- **Статус.** `PROVEN`.

## STORE-002 — отсутствие потери side-state при fold/recovery

- **Определение.** Side-записи не теряются молча: либо переносятся, либо
  операция отказывает fail-closed.
- **Статус.** `UNPROVEN` (и это осознанно): fold сегодня нерестартуем
  (C4/C5/C7/C8); замечание `ADR-FOLD-CONTRADICTION-NOTE.md` показывает, что
  текущая формулировка ADR недостижима. Реализация запрещена без OWNER GO.
- **Проверки.** Будущие: SIGKILL-матрица fold, проверка полноты side-набора.

## STORE-003 — атомарность и долговечность выбора генерации и привязки tip

- **Определение.** `CURRENT` + `tip.meta` образуют пару; crash в любом окне
  оставляет либо старую, либо новую пару; третьего состояния нет.
- **Происхождение.** ADR-0025 C.
- **Проверки.** `blockindex_crash_corruption_tests` (C1..C13, D1..D4);
  `blockindex_v2_reader` memo-поведение не влияет на коммит.
- **Статус.** `PARTIAL`: pair-атомарность для accept/restart — `PROVEN`;
  для fold (select→rebind) — `UNPROVEN` (механизм отсутствует).
- **Разрешённое изменение.** Только через ADR.

## REORG-001 — сохранение утверждённой глубины и корректности реорганизаций

- **Определение.** Reorg не теряет и не подделывает состояние: undo-цепочка,
  tree/UTXO, wallet settlement, tip-файлы согласованы; RAM не растёт.
- **Происхождение.** ADR-0016 (atomic reorg V→C→A), P0 wallet settlement fix.
- **Проверки.** `atomic_reorg_tests`, `pos_reorg_tests`, `reorg_314_*`;
  стенд: invalidate/reconsider depth 1/20 (regtest) и 1/100 (mainnet-копия).
- **Эталон.** d1 ≈ 4.8 s, d100 ≈ 150/81 s, RAM ±0.2 MB, undo/tree без потерь.
- **Статус.** `PROVEN` для измеренных глубин; глубже 100 на 8.24M не проверялось.
- **Границы.** Определение глубины — политика (см. ADR-FOLD-CONTRADICTION-NOTE).

## P2P-001 — доставка и переадресация при недоступности первичного пира

- **Определение.** Узел продолжает получать блоки при отказе первичного пира
  (переадресация запроса/повторный выбор источника), без потери tip.
- **Происхождение.** P2P sync failure triage, stall watch.
- **Проверки.** `p2p_sync_tests`, `p2p_stall_watch_tests`; lab-стенд с
  недоступным пиром.
- **Статус.** `PARTIAL` в этом этапе: код не менялся, изолированный сетевой
  стенд не поднимался (нет root для netns); регрессия маловероятна, но
  непроверенное не объявляется доказанным.

---

## Матрица гейтов

```text
tier1  каждый интеграционный коммит
       scripts/startup_invariants_gate.sh suites --tests <test_innova>
       scripts/startup_invariants_gate.sh boot --log <log> (regtest boot)
       покрывает: MEM-002, MEM-003 (fail-closed), BOOT-001, BOOT-002 (маркер),
                  STORE-001 (S2.0), STORE-003 (pair)

tier2  перед интеграцией релиза
       startup / restart / reorg(1,20) на regtest-стенде + POS-сценарии
       покрывает: REORG-001, STORE-001, MEM-001

tier3  перед релизным GO и после изменений горячего пути
       cold-boot 8.24M под MemoryMax=1G, swap=0 + пороги
       scripts/startup_gate_thresholds.json
       покрывает: MEM-001/002/003, BOOT-001/002/003
```

Порог ухудшения задаётся в `scripts/startup_gate_thresholds.json` и учитывает
шум: время сравнивается только на одном профиле стенда с допуском 30%,
структурные метрики (на запись) — без допуска, но на порядок выше нормы.
Разные условия кэша **не** считаются эквивалентными: прогон без вытеснения
кэша не может быть использован как «тёплый» baseline (измерено: тот же бинарь
даёт 433 s и 2 228 s).

## Журнал изменений инвариантов

```text
2026-11-16  создан реестр (MEM-001..003, BOOT-001..003, STORE-001..003,
            REORG-001, P2P-001) на базе integ/rel-p4 @ f2c79b8e.
            Пороги: memory.max_vmhwm_kb=655360, boot.max_read_syscalls_per_record=0.05,
            boot.max_read_bytes_per_record=780.
2026-11-16  BOOT-004 добавлен (frontier из сканов; 217 s -> 1.5 s, замер 8.24M);
            MEM-002 получил структурный маркер handoff=swap.
2026-11-16  BOOT-003: метрика СКОУПЛЕНА на фазу блочного индекса (метки
            STARTUP_IO), значения порогов НЕ изменены (0.05 / 780). Причина —
            измеренная атрибуция: процесс-широкие syscalls на 8.24M на 95%
            состоят из live-tip replay (497 837), txloc (80 204), LevelDB env
            (16 019) и wallet (10 681); records.dat читается за 3 574 syscall
            (0.00043/запись). Профиль стенда обновлён на принятую базу 42 s x 1.5
            (было 433 s x 1.3, что перестало обнаруживать возврат ~217 s).
            СТАТУС: изменение нормативного определения — PENDING OWNER REVIEW.
            До решения OWNER процесс-широкие числа печатаются как информационные.
```
