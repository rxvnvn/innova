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
- **Обязательные проверки.** gate `boot --vmhwm-kb`; focused suite
  `startup_memory_invariants_tests` (эквивалентность множества); сверка
  `setStakeSeen_count` через `getmemorydiagnostics` (2 642 643 на 8.24M).
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
  на запись, read bytes на запись, wall time до `AUTHORITY_READY` и до RPC.
  Случайные точечные пробы не масштабируются с числом листьев фронтьера.
- **Область.** Последовательные проходы (`ReadAllSequential`,
  `VerifyIntegrity`, пересчёт корня), построение candidate frontier.
- **Происхождение.** 2026-11-16: 2.34M syscalls по 3.1 KB; 92k случайных
  проб в frontier (~300 s).
- **Исправление.** `SequentialIoBuffer` (1 MiB) в четырёх последовательных
  циклах; листья фронтьера опрашиваются в порядке ключа hashindex
  (`BOOT-003`, blockindex_candidate_startup_builder.cpp).
- **Проверки.** gate `boot --io <pid-io> --records N`:
  `syscalls/record <= 0.05`, `bytes/record <= 780`; профиль стенда для времени
  (`profiles.atlas-lab-sdb-hdd`, допуск 30%).
- **Эталон.** Baseline: 0.284 syscalls/record, 888 B/record, 433 s (сегодня),
  2 228 s (исторический cold под нагрузкой).
- **Обнаружение регрессии.** Порог нарушен; время профиля превышено в 1.3x.
- **Статус.** `PROVEN` (замеры этапа).
- **Разрешённое изменение.** Порог меняется только с OWNER GO.

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
```
