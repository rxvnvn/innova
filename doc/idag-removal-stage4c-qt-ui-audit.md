# Innova Qt IDAG UI Removal Stage 4C Audit

## 1. Repository state

- Repository: `/home/user/innova`
- Branch: `master`
- HEAD: `d356f69`
- Subject: `refactor(idag): remove dead SupportingMass helper`

Tracked tree is clean apart from pre-existing untracked forensic/build artifacts.

## 2. Executive summary

The remaining Qt/IDAG surface is a local presentation layer. It is read-only,
polls backend state through `ClientModel::getDAGStatus()`, and does not mutate
consensus, validation, storage, wallet, mining, staking, P2P, or serialization
state.

The entire visible Qt IDAG graph is concentrated in four places:

- `src/qt/idagpage.{h,cpp}`: dedicated IDAG page widget;
- `src/qt/bitcoingui.{h,cpp}`: page construction, window menu/action wiring and
  status-bar tooltip block;
- `src/qt/rpcconsole.{h,cpp}`: embedded DAG status block in the info tab;
- `src/qt/clientmodel.{h,cpp}`: shared `DAGStatus` / `DAGBlockActivity`
  snapshot API and its backend reads.

The safe removal boundary is therefore the full Qt IDAG presentation graph, not
just the `IDAGPage` widget. Removing the page alone would leave the model live;
removing the model alone would break the page, tooltip and RPC console.

Recommended implementation order:

1. one coherent Stage 4C behavior-removal commit that removes the complete Qt
   IDAG presentation graph and its qmake source entries;
2. optional follow-up dead-resource cleanup only if any generic icon/helper
   residue is still caller-free after that patch.

## 3. Qt / IDAG file inventory

| File | Role | Current state | Removal notes |
|---|---|---|---|
| `src/qt/idagpage.h` | dedicated page widget declaration | live | removable with the page |
| `src/qt/idagpage.cpp` | dedicated page widget implementation | live | removable with the page |
| `src/qt/bitcoingui.h` | holds `IDAGPage*`, `QAction*`, `gotoIDAGPage()` | live | remove IDAG members/slot with navigation |
| `src/qt/bitcoingui.cpp` | constructs page, action, tooltip status block | live | remove page/action wiring and tooltip block together |
| `src/qt/clientmodel.h` | declares `DAGBlockActivity`, `DAGStatus`, `getDAGStatus()` | live | becomes caller-free after UI removal |
| `src/qt/clientmodel.cpp` | defines `DAGStatus()` and `getDAGStatus()` | live | becomes caller-free after UI removal |
| `src/qt/rpcconsole.h` | holds DAG status labels and `updateDAGInfo()` | live | remove DAG info block with the console UI cleanup |
| `src/qt/rpcconsole.cpp` | builds/updates the DAG info block | live | remove the block and its `getDAGStatus()` call |
| `innova-qt.pro` | qmake source list | live | remove `src/qt/idagpage.h/cpp` entries |
| `src/qt/bitcoin.qrc` | shared icon resources | no dedicated IDAG asset | optional follow-up only for dead generic icon residue |

There is no `src/qt/forms/idagpage.ui`, no dedicated `idagpage.qrc`, and no
Qt test that directly targets the IDAG page.

## 4. Symbol inventory

### `src/qt/clientmodel.h`

- `struct ClientModel::DAGBlockActivity`
- `struct ClientModel::DAGStatus`
- `ClientModel::DAGStatus getDAGStatus(int recentBlockCount = 0) const`

### `src/qt/clientmodel.cpp`

- `ClientModel::DAGStatus::DAGStatus()`
- `ClientModel::getDAGStatus(int recentBlockCount) const`

### `src/qt/idagpage.h`

- `class IDAGPage`
- `IDAGPage::IDAGPage(QWidget *parent = 0)`
- `IDAGPage::setModel(ClientModel *model)`
- `IDAGPage::refresh()`

### `src/qt/idagpage.cpp`

- constructor and helpers `addMetric`, `shortHash`, `formatBytes`,
  `formatPercent`, `setValue`
- timer-driven `refresh()`

### `src/qt/bitcoingui.h`

- forward declaration `IDAGPage`
- members `idagPage`, `idagAction`
- slot `gotoIDAGPage()`

### `src/qt/bitcoingui.cpp`

- `idagPage = new IDAGPage(this);`
- `centralWidget->addWidget(idagPage);`
- `idagAction = new QAction(...)`
- `connect(idagAction, SIGNAL(triggered()), this, SLOT(gotoIDAGPage()));`
- `window->addAction(idagAction);`
- `idagPage->setModel(clientModel);`
- status-bar tooltip block using `ClientModel::getDAGStatus()`
- `void BitcoinGUI::gotoIDAGPage()`

### `src/qt/rpcconsole.h`

- members `dagStatus`, `dagTips`, `dagInferredK`, `dagAdaptiveLimit`,
  `dagFinality`
- helper `updateDAGInfo()`

### `src/qt/rpcconsole.cpp`

- DAG info block construction in the constructor
- `updateDAGInfo()`
- `setNumBlocks()` call to `updateDAGInfo()`

## 5. Dependency graph

```text
BitcoinGUI constructor
  -> new IDAGPage(this)
  -> centralWidget->addWidget(idagPage)
  -> idagAction = new QAction(MakeToolbarIcon(GlyphBlock), "IDA&G", ...)
  -> QActionGroup / Window menu insertion
  -> connect(idagAction, triggered, gotoIDAGPage)

BitcoinGUI::setClientModel
  -> idagPage->setModel(clientModel)

IDAGPage
  -> 2 s QTimer -> refresh()
  -> clientModel->numBlocksChanged -> refresh()
  -> ClientModel::getDAGStatus(24)
     -> TRY_LOCK(cs_main)
     -> g_dagManager.GetDAGTips()
     -> g_dagManager.GetDAGEntryCount()
     -> g_dagManager.SelectBestDAGTip()
     -> g_dagManager.GetDAGData()
     -> g_finalityTracker queries
     -> recent block walk via pprev

BitcoinGUI::setNumBlocks
  -> ClientModel::getDAGStatus()
  -> sync tooltip augmentation

RPCConsole::setNumBlocks
  -> updateDAGInfo()
  -> ClientModel::getDAGStatus()
  -> embedded DAG info labels
```

The model is shared by all three Qt consumers. That makes the Qt removal graph
coherent: the page, tooltip and console block are one presentation surface.

## 6. ClientModel data flow

`ClientModel::getDAGStatus()` is public, non-mutating and signal-agnostic. It is
polled from UI code, not pushed from the backend.

| Symbol | Visibility | Callers | Backend reads | Behavior |
|---|---|---|---|---|
| `ClientModel::DAGStatus` | public nested struct | `IDAGPage`, `BitcoinGUI::setNumBlocks`, `RPCConsole::updateDAGInfo` | none directly; fields are filled by `getDAGStatus()` | read-only snapshot container |
| `ClientModel::DAGBlockActivity` | public nested struct | only `getDAGStatus()` | none directly | row container for recent block list |
| `ClientModel::getDAGStatus(int)` | public method | 3 live production callers | `g_dagManager`, `g_finalityTracker`, `cs_main`, `pindexBest` | non-blocking snapshot; returns inactive/empty state when unavailable |

Formatting is split:

- model: computes raw status, strings, counts, best tip/score, epoch/finality
  fields, recent-block rows;
- views: format bytes, percentages, truncated hashes, and present labels.

Threading/locking:

- `getDAGStatus()` uses `TRY_LOCK(cs_main, lockMain)`;
- `IDAGPage` refresh is timer-driven and also driven by `numBlocksChanged`;
- `BitcoinGUI::setNumBlocks` and `RPCConsole::setNumBlocks` are signal-driven
  through `ClientModel`.

## 7. Page/widget audit

### `IDAGPage`

- Read-only diagnostics page.
- Widgets: title, summary metrics, recent activity table.
- Timer: `QTimer refreshTimer`, 2-second interval.
- Model assignment: `setModel(ClientModel *)` disconnects old client model,
  connects `numBlocksChanged(int,int)` to `refresh()`, then refreshes.
- No wallet model, no direct backend access, no write path.
- No consensus, validation, storage, P2P, mining, staking or serialization side
  effects.

The page is only presentation. It is not a state-mutating UI.

## 8. Navigation and ownership

### Exact user-visible path

```text
Window menu / toolbar action
  -> idagAction
  -> connect(triggered, gotoIDAGPage)
  -> centralWidget->setCurrentWidget(idagPage)
```

### Ownership / layout

- `idagPage` is parented to `BitcoinGUI` and inserted into `QStackedWidget`.
- `idagAction` is parented to `BitcoinGUI`, added to the `QActionGroup`, and
  inserted into the Window menu.
- `setClientModel()` forwards the live model into the page.

### Index coupling

- The page is selected by `setCurrentWidget(idagPage)`, not by stack index.
- No other code in the audited path relies on `QStackedWidget` indices for the
  IDAG page.

### RPC console exposure

- Not navigation, but the console exposes a separate embedded DAG info block in
  its Info tab.
- That block is updated from `setNumBlocks()` and reads the same model snapshot.

## 9. Build-system and resource map

### qmake / build list

`innova-qt.pro` directly lists:

- `src/qt/idagpage.h`
- `src/qt/idagpage.cpp`

No dedicated `idagpage.ui` entry exists.

### Resources/icons

- The IDAG toolbar action uses `MakeToolbarIcon(GlyphBlock)` in
  `src/qt/bitcoingui.cpp`.
- The corresponding `GlyphBlock` enum case is in the same file.
- `src/qt/bitcoin.qrc` contains the generic `block` icon alias, but no
  IDAG-only resource file exists.

That icon path is only reachable through the IDAG toolbar action, so it is a
candidate for a follow-up dead-resource cleanup if the visible UI is removed.

### Translation catalogs

- All IDAG-facing strings are inline `tr()` calls in the C++ sources.
- There is no source-controlled IDAG translation file to edit.
- Generated `.qm` artifacts are build outputs, not source cleanup targets.

## 10. Tests and documentation references

### Tests

No active unit, GUI, or regtest test directly targets the Qt IDAG page or
`ClientModel::getDAGStatus()`.

### Historical / forensic references

The following documents mention the current Qt surface, but they are historical
evidence rather than active test coverage:

- `doc/idag-final-forensic-report.md`
- `doc/idag-removal-stage4-runtime-storage-audit.md`
- `doc/idag-removal-stage4b-rpc-qt-audit.md`

The audit documents are useful for context, but they do not make the Qt code
live.

## 11. Safe removal boundary

### A. UI files removable together

- `src/qt/idagpage.h`
- `src/qt/idagpage.cpp`
- `src/qt/bitcoingui.h`
- `src/qt/bitcoingui.cpp`
- `src/qt/rpcconsole.h`
- `src/qt/rpcconsole.cpp`
- `src/qt/clientmodel.h`
- `src/qt/clientmodel.cpp`

### B. Navigation/action code removable together

- `idagAction`
- `idagPage`
- `gotoIDAGPage()`
- `window->addAction(idagAction)`
- `centralWidget->addWidget(idagPage)`
- the sync-tooltip DAG status block in `BitcoinGUI::setNumBlocks()`
- the RPC-console DAG info block and its `updateDAGInfo()` helper

### C. ClientModel methods/types that become dead after UI removal

- `ClientModel::DAGStatus`
- `ClientModel::DAGBlockActivity`
- `ClientModel::DAGStatus::DAGStatus()`
- `ClientModel::getDAGStatus(int)`

### D. Build-system entries

- `innova-qt.pro`: remove `src/qt/idagpage.h` and `src/qt/idagpage.cpp`

### E. Resources/icons/forms

- No IDAG-specific `.ui` form exists.
- `GlyphBlock` / `MakeToolbarIcon(GlyphBlock)` and the `block` icon alias are
  dead-resource candidates after the visible UI is removed.

### F. Tests/docs

- Preserve historical docs and forensic artifacts.
- No source test edit is required for the current Qt audit boundary.

### G. Backend DAG code that must remain

Even after Qt removal, the following backend pieces remain live elsewhere and
must not be inferred removable from the UI audit:

- `g_dagManager`
- `GetDAGTips()`
- `GetDAGEntryCount()`
- `SelectBestDAGTip()`
- `GetDAGData()`
- `g_finalityTracker`
- generic DAG/mining/finality/epoch/backend code used outside Qt

## 12. Recommended implementation sequencing

### Recommended choice: Option 1

One coherent Stage 4C commit should remove the entire Qt IDAG presentation
graph:

- page widget;
- action/navigation wiring;
- status-bar DAG tooltip block;
- RPC-console DAG info block;
- `ClientModel::DAGStatus` / `getDAGStatus()` and their row container types;
- qmake source entries.

This is the cleanest boundary because the three callers are coupled through the
same model snapshot API.

### Optional follow-up cleanup

If the implementation keeps the generic toolbar glyph and icon alias around for
minimal churn, a later dead-resource cleanup can remove:

- `GlyphBlock`
- `MakeToolbarIcon(GlyphBlock)` case
- `src/qt/bitcoin.qrc` `block` alias

That cleanup is not required to prove the user-visible Qt removal is safe.

## 13. Compatibility and risk assessment

### Lost user-visible functionality

- the IDAG page disappears from the Window menu / toolbar;
- the sync tooltip no longer shows IDAG status;
- the RPC console info tab no longer shows the embedded DAG block;
- `ClientModel::getDAGStatus()` is removed from the Qt presentation layer.

### Unchanged compatibility

- CLI/RPC dispatch: unchanged;
- wallet: unchanged;
- consensus / validation: unchanged;
- storage / startup: unchanged;
- P2P: unchanged;
- mining / staking: unchanged;
- backend DAG runtime: unchanged.

### Qt-internal ABI/API impact

- local Qt C++ API changes are expected;
- no external ABI compatibility promise is made for unused Qt internals.

### Main regression risks

- stale menu/action wiring left behind after removing the page;
- dangling `connect()` to `gotoIDAGPage()` or `updateDAGInfo()`;
- stale `QStackedWidget` or label member references;
- missing `moc` or qmake source-list updates;
- `ClientModel::getDAGStatus()` left caller-free after the UI removal;
- dead `GlyphBlock` / `block` resource residue left intentionally for a later
  cleanup stage.

## 14. Validation plan for implementation

For the eventual Stage 4C patch:

1. `git diff --check`
2. LSP diagnostics for all modified Qt files
3. Clean `innovad` build
4. `make -f makefile.unix -j"$(nproc)" test_innova`
5. `./test_innova` and confirm the full unit count/result
6. Clean `innova-qt` build
7. Qt runtime smoke on a safe disposable datadir or regtest if available:
   - launch GUI;
   - verify the Window menu no longer exposes the IDAG page;
   - verify neighboring navigation items still work;
   - open RPC console and confirm the DAG info block is absent;
   - inspect logs for QObject connect warnings or missing resources;
   - shut down cleanly.

Runtime smoke should not interfere with any user-owned datadir or running Qt
process.

## 15. Explicit keep-list

Keep these surfaces out of Stage 4C:

- backend DAG runtime and storage code;
- `getepochinfo`;
- epoch/finality / FCMP backend code;
- wallet / shielded / mining / staking logic;
- P2P and block validation;
- generic non-IDAG Qt infrastructure.

