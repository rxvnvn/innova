# Qt redesign map: Blackcoin More 26.x -> Innova Core

This audit uses the local Blackcoin More tree at `/home/user/blackcoin-more` as a Qt UI reference only. The Innova network, consensus, wallet backend, P2P, RPC, Berkeley DB wallet format, staking, PoW/PoS, chain data and existing Qt models stay authoritative.

## Component map

| Blackcoin More class/file | Innova class/file | Purpose | Safe to transfer now? | Innova models that must remain | Blackcoin dependencies not to transfer |
| --- | --- | --- | --- | --- | --- |
| `BitcoinGUI` (`src/qt/bitcoingui.*`) | `BitcoinGUI` (`src/qt/bitcoingui.*`) | Main window, menu bar, toolbar, tray and status widgets | Yes, only menu/toolbar organization and native Qt layout ideas | `ClientModel`, `WalletModel`, `TransactionTableModel`, `AddressTableModel`, `OptionsModel`, existing page objects | `interfaces::Node`, `WalletController`, `WalletFrame` ownership, modal overlay sync logic, multiwallet actions, PSBT actions |
| `BitcoinApplication` (`src/qt/bitcoin.cpp`, Blackcoin `main.cpp`) | Innova `BitcoinApplication` flow in `src/qt/bitcoin.cpp` | Qt app startup/shutdown and model wiring | No for this stage, audit only | Innova `AppInit2`, `ClientModel`, single-wallet lifecycle | Blackcoin node interfaces, wallet controller, create/open/restore wallet lifecycle, descriptor wallet startup |
| `WalletFrame` | No standalone analog; Innova uses `QStackedWidget` directly inside `BitcoinGUI` | Holds multiple `WalletView` instances and no-wallet screen | No | Innova direct page wiring and single wallet model | Multiwallet map, create/open/restore/close wallet flows, PSBT loader |
| `WalletView` | No standalone analog; Innova `BitcoinGUI` owns `OverviewPage`, `SendCoinsDialog`, `AddressBookPage`, `TransactionView` | Wallet page stack and page-to-model plumbing | No class transfer; useful as organization reference | Innova page instances and their existing model setters | Blackcoin `ReceiveCoinsDialog`, `RecentRequestsTableModel`, descriptor/SegWit address-type plumbing |
| `OverviewPage` | `OverviewPage` | Wallet summary and recent transactions | Later stage only, visual/layout comparison | Innova wallet balance fields, staking/shielded labels, transaction proxy | Blackcoin privacy mask and modern wallet assumptions |
| `SendCoinsDialog` | `SendCoinsDialog` | Send workflow | Later stage only, visual/layout comparison | Innova send logic, coin control, privacy tabs, wallet unlock flow | Blackcoin wallet interfaces, PSBT, descriptor coin selection |
| `ReceiveCoinsDialog` and `ReceiveRequestDialog` | Innova uses `AddressBookPage` with `ReceivingTab`; no direct receive dialog | Receive/request UI | Not in stage 1. Needs separate lifecycle analysis | Innova `AddressTableModel`, `WalletModel::getAddressTableModel`, current address generation | Blackcoin recent requests model, address type selection, SegWit/Taproot address generation |
| `TransactionView` | `TransactionView` | Transaction history filters, table, export and context menu | Later stage only, visual/layout comparison | Innova `TransactionFilterProxy`, `TransactionTableModel`, `WalletModel` | Blackcoin abandoned transaction state and newer wallet signals unless supported |
| `QAction` setup in `BitcoinGUI::createActions()` | `BitcoinGUI::createActions()` | Shared actions for toolbar, menu and tray | Yes, text/mnemonic cleanup only | Existing slots such as `gotoSendCoinsPage`, `gotoReceiveCoinsPage`, `showConsole` | New Blackcoin actions for PSBT, create/open/restore/migrate wallet, mask values |
| `QToolBar` in `BitcoinGUI::createToolBars()` | `BitcoinGUI::createToolBars()` | Main page navigation | Yes. Blackcoin uses four primary wallet actions | Existing page-switching actions | Wallet selector and multiwallet controls |
| `QMenuBar` / `QMenu` in `BitcoinGUI::createMenuBar()` | `BitcoinGUI::createMenuBar()` | Top-level app menus | Yes. Reorganize into File, Settings, Window, Help | Existing useful Innova actions | Blackcoin top-level wallet lifecycle and PSBT menu actions |
| Status bar widgets in `BitcoinGUI` | Status bar widgets in `BitcoinGUI` | Sync progress, connections, warnings, encryption and staking indicators | Only placement preservation. No sync/staking calculation changes in stage 1 | Existing `setNumConnections`, `setNumBlocks`, `setEncryptionStatus`, `updateStakingIcon` | Blackcoin modal overlay, headers sync state, proxy/network active plumbing |
| `bitcoin.qrc`, `res/icons`, `res/src` | `bitcoin.qrc`, Innova `res/icons`, `res/images` | Icons, app logo, translation resources | Existing Innova resources are sufficient for stage 1 | Innova logo aliases and current icon aliases | Blackcoin logo, ticker, branded assets, gold styling |
| Translation files | `src/qt/locale/bitcoin_*.ts` | UI localization | Stage 1 updates only English/Russian strings touched by menu/toolbar | Existing TS/QM build flow from qmake | Bulk-imported Blackcoin translations |
| Build manifests (`Makefile.am`, qmake/CMake equivalents) | `innova-qt.pro`, `innova.pro`, generated `Makefile`, `src/makefile.unix` | Build wiring | No manifest change needed in stage 1 | qmake GUI build, `src/makefile.unix` daemon build | Blackcoin Autotools/CMake/depends layout |

## First-stage decision

Blackcoin More 26.x separates `BitcoinGUI`, `WalletFrame`, and `WalletView`. Innova does not have that architecture: `BitcoinGUI` directly owns the central `QStackedWidget` and pages. For stage 1, the safe adaptation is therefore limited to reorganizing Innova `BitcoinGUI::createMenuBar()` and `BitcoinGUI::createToolBars()` while keeping all existing Innova page objects, actions, signals, slots and models.

The main toolbar is limited to:

- Overview
- Send
- Receive
- Transactions

Additional Innova pages remain reachable from the Window menu and are not removed from the central stack.

## Explicit non-transfers

The following Blackcoin More parts are intentionally not transferred:

- chain parameters, consensus, validation, net processing, RPC and wallet backend;
- `WalletController`, `WalletFrame`, `WalletView` classes as architecture;
- multiwallet create/open/restore/close/migrate actions;
- descriptor, SQLite, PSBT, SegWit/Taproot address-type and recent-request wallet flows;
- Blackcoin logo, ticker, branded icon resources, package names and user-agent strings;
- Blackcoin sync modal overlay and node interface plumbing.

## Next stages

1. Overview page: compare Blackcoin `OverviewPage` with Innova `OverviewPage`, then update layout and typography only while preserving balance/staking/shielded fields and `TransactionTableModel` wiring.
2. Receive workflow: audit whether Innova should keep `AddressBookPage::ReceivingTab` or gain a safe receive-request UI backed by existing address generation. Do not import Blackcoin recent requests or address-type support unless Innova backend supports it.
3. Send workflow: improve visual layout of `SendCoinsDialog` around existing Innova send, coin-control and privacy-tab logic.
4. Transactions page: modernize filters/table spacing and export placement while preserving `TransactionFilterProxy` and transaction model semantics.
5. Debug/window tools: only after the main wallet pages are stable, review RPC console and diagnostic tab navigation.
6. Wallet lifecycle: separate analysis before any create/open wallet UI; do not use Blackcoin multiwallet lifecycle as a drop-in replacement.

## Second-stage OverviewPage map

| Blackcoin More class/file | Innova class/file | Purpose | Safe adaptation in stage 2? | Innova-specific state preserved | Blackcoin dependencies rejected |
| --- | --- | --- | --- | --- | --- |
| `OverviewPage` (`src/qt/overviewpage.*`) | `OverviewPage` (`src/qt/overviewpage.*`, `src/qt/forms/overviewpage.ui`) | Wallet balances, sync warning and recent transactions | Yes, layout, typography, spacing, row density and recent-transaction ordering only | Existing `WalletModel` getters for unlocked, locked, stake, unconfirmed, immature, watch-only and shielded balances | `interfaces::WalletBalances`, `WalletController`, privacy mask option, alerts plumbing, descriptor/private-key-disabled logic |
| `TxViewDelegate` inside `OverviewPage` | `TxViewDelegate` inside Innova `OverviewPage` | Paint compact recent transaction rows | Yes, elide long date/address/amount text and use a compact two-line row | Innova roles: `Qt::DecorationRole`, `DateRole`, `AmountRole`, `ConfirmedRole`, existing amount formatting | `RawDecorationRole`, watch-only decoration roles, `PlatformStyle::SingleColorIcon`, separator-style overloads unavailable in Innova |
| `TransactionOverviewWidget` | Innova `QListView listTransactions` | Recent transaction list sizing | No class transfer; only matching compact sizing behavior via existing `QListView` settings | Existing click signal maps through Innova `TransactionFilterProxy` and existing `transactionClicked` signal | New custom widget file and Blackcoin-specific table sizing assumptions |
| Blackcoin balance layout in `forms/overviewpage.ui` | Innova `forms/overviewpage.ui` | Visual hierarchy of balances and recent list | Yes, calmer headings and plain recent-transactions title | Innova shielded/private, locked, stake and watch-only labels are kept | Donation percentage, watch-only stake field, private-keys-disabled branch |
| Blackcoin out-of-sync buttons | Innova `labelWalletStatus`, `labelTransactionsStatus` | Warning placement | Placement/style only; visibility condition unchanged | Existing `showOutOfSyncWarning(bool)` calls and label widgets | Clickable warning button, node/client alert forwarding |

### Stage 2 adaptations

- The OverviewPage keeps Innova's existing `WalletModel`, `TransactionTableModel` and `TransactionFilterProxy` wiring.
- The available balance is made the primary amount, with total and secondary categories kept in the same balance block.
- Locked, stake, shielded/private, unconfirmed, immature and watch-only balances remain present and continue to use existing Innova values.
- Recent transactions now use a compact five-row presentation, date sorting, elided address/date/amount text and the existing transaction click path.
- The out-of-sync warning text remains driven by existing `showOutOfSyncWarning(bool)` logic; only local text/layout presentation is touched.
- The display-unit refresh now preserves the cached shielded balance when reformatting labels; this is a GUI refresh fix and does not change balance calculation.

### Stage 2 non-transfers

The following Blackcoin More pieces were intentionally not transferred:

- `interfaces::WalletBalances` and cached-balance lifecycle;
- `WalletController`, multiwallet, descriptor wallet and private-key-disabled branches;
- privacy masking controls and `OptionsModel::MaskValues`;
- `TransactionOverviewWidget` as a new class;
- alert forwarding and clickable out-of-sync warning buttons;
- donation percentage, watch-only stake and Blackcoin-specific transaction decoration roles;
- any wallet backend, staking backend, consensus, P2P, RPC, chainparams or validation code.

### Stage 2 limitations

- The current page remains a two-column Qt layout; fully responsive stacking for very narrow windows is left for a later UI pass if required.
- The page does not add a Blackcoin-style privacy mask because Innova has no matching existing OverviewPage privacy control.
- No dead Innova-specific balance widgets were removed; uncertain or active categories were preserved.
- The rest of the wallet pages keep their existing layouts until their dedicated stages.

### Proposed third stage

Modernize `SendCoinsDialog` only: audit Innova send, coin-control, privacy/shielded tabs and unlock flow, then improve spacing, labels, validation presentation and button layout while preserving Innova's existing transaction creation, staking, wallet unlock, address validation and fee logic.

## Stage 3: SendCoinsDialog

| Blackcoin class / area | Innova class / area | Purpose | Safe to transfer | Innova-specific elements kept | Blackcoin dependencies rejected | Risks / constraints |
| --- | --- | --- | --- | --- | --- | --- |
| `SendCoinsDialog` | `SendCoinsDialog` | Rework the transparent send page layout and controls | Yes, for structure, spacing, button order, and presentation only | Existing `WalletModel`, `CoinControlDialog`, split UTXO controls, unlock flow, confirmation flow, privacy tabs, and send validation | PSBT, external signer, descriptor wallet, RBF, SegWit/Taproot, multiwallet, and any new fee or coin selection backend | Must not alter transaction creation, fee math, coin control logic, or private send modes |
| `SendCoinsEntry` | `SendCoinsEntry` | Compact recipient row arrangement | Yes, for layout and keyboard flow only | Address validation, URI parsing, narration semantics, label auto-fill, and existing recipient signals | Blackcoin subtract-fee, use-available-balance, or message/backend flows not present in Innova | Keep all recipient and validation semantics unchanged |
| `CoinControlDialog` presentation | Existing coin control widgets and labels | Make the send page coin-control section quieter and denser | Yes, for spacing and label presentation only | Current coin-control backend, custom change, and split UTXO handling | Blackcoin coin selection or fee estimator | Must not touch selection logic or fee calculation |
| Qt resources / translations | Existing Innova Qt resource and locale files | Ensure icons and text stay native and translated | Yes, if only existing assets are used | Existing Innova logo and language support | Blackcoin branding or new binary assets | Keep EN/RU aligned and avoid untranslated send-page strings |

### SendCoinsDialog audit notes

- The transparent send page is backed by the existing Innova wallet model and coin control logic.
- Privacy tabs remain intact because they call Innova-specific wallet/backend methods and are not safe to replace with Blackcoin flows.
- The safe transfer surface is limited to layout, spacing, hierarchy, button presentation, tab order, and text polish.
- Anything that would alter `CreateTransaction`, `CommitTransaction`, fee calculation, address validation, change handling, or unlock behavior is out of scope.

### Stage 4 planning boundary

The next stage should focus on `ReceiveCoinsDialog` and its related entry widgets only after a separate backend audit confirms that no wallet-lifecycle assumptions are required.

## Stage 4: Receive workflow

| Blackcoin class / area | Innova class / area | Purpose | Safe to transfer | Innova backend/models kept | Blackcoin dependencies rejected | Constraints |
| --- | --- | --- | --- | --- | --- | --- |
| `WalletView::gotoReceiveCoinsPage`, `ReceiveCoinsDialog` | `BitcoinGUI::gotoReceiveCoinsPage`, `AddressBookPage(AddressBookPage::ForEditing, AddressBookPage::ReceivingTab)` | Document the actual receive path and modernize the existing receiving-address page | Yes, only as UI presentation over the current address-book workflow | `WalletModel`, `AddressTableModel`, `EditAddressDialog`, `GUIUtil::parseBitcoinURI`, `QRCodeDialog` when compiled in, existing unlock path inside `AddressTableModel::addRow` | Blackcoin payment-request lifecycle, `RecentRequestsTableModel`, standalone request dialogs, wallet-controller assumptions, and any new backend storage | Do not invent a payment-request stack or alter address generation semantics |
| `ReceiveCoinsDialog` / `ReceiveRequestDialog` | Not present in Innova | Separate request creation, QR presentation, and recent requests history | No, not as a direct port | Existing receive-address list and QR dialog are enough for current functionality | `RecentRequestsTableModel`, payment-request persistence, URI/request workflows not present in Innova | Keep the stage scoped to the existing receiving-address list |
| `AddressBookPage` receiving tab | `AddressBookPage` receiving tab | Present, filter, copy, edit, and create receiving addresses | Yes, for layout, spacing, search/filter presentation, and button hierarchy only | Receiving entries in `AddressTableModel`, label edits, copy address, QR display, and existing `New Address` / shielded / SP / staking address actions | Blackcoin address-book selection flow, export-first receive UI, and its standalone request controls | Preserve all Innova-specific address types and unlock behavior |
| `QRImageWidget`, `receiverequestdialog.ui` | `QRCodeDialog` / conditional `showQRCode` button | Present an address as QR only when QR support is compiled in | Yes, for presentational flow only | Existing `QRCodeDialog` and `USE_QRCODE` guards | Blackcoin standalone receive request QR export stack | When QR is disabled, no dead buttons may remain |

### Receive workflow audit notes

- The receive action in Innova does not open a separate request dialog. It switches the main stacked view to `AddressBookPage` in `ReceivingTab`.
- `AddressTableModel::addRow(AddressTableModel::Receive, ...)` is the actual address-creation path and still performs the wallet unlock and key-pool-backed address generation.
- `AddressBookPage` already exposes QR, copy address, and label editing for receiving entries, and it also carries Innova-specific address-type actions for shielded, silent payment, and staking addresses.
- Blackcoin More has a richer payment-request stack with `ReceiveCoinsDialog`, `ReceiveRequestDialog`, and `RecentRequestsTableModel`, but that stack is not present in Innova and should not be recreated without a backend lifecycle audit.
- The safe redesign boundary is limited to the current receiving-address list, its action ordering, its search/filter presentation, its empty state, and neutral Qt styling.

## Stage 5: Transactions page

| Blackcoin class / area | Innova class / area | Purpose | Safe to transfer | Innova models / behavior kept | Blackcoin dependencies rejected | Constraints |
| --- | --- | --- | --- | --- | --- | --- |
| `TransactionView` | `TransactionView` | Rework the transaction-history filter bar, table spacing, empty state, and export placement | Yes, for layout, spacing, button placement, and presentation only | Existing `TransactionTableModel`, `TransactionFilterProxy`, `TransactionDescDialog`, `WalletModel` connections, and CSV export semantics | Blackcoin watch-only filter, third-party explorer hooks, extra transaction roles, or any backend classification changes | Keep filter semantics, row ordering, and transaction meanings unchanged |
| `TransactionTableModel` presentation | `TransactionTableModel` | Improve table sizing, header balance, and row readability | Yes, only for presentation and header sizing | Existing columns, roles, status icons, amount formatting, and narration support | New roles, new transaction types, or any change to status/amount calculations | Do not alter data roles or transaction classification |
| `TransactionFilterProxy` | `TransactionFilterProxy` | Present date/type/search/min-amount filters in a cleaner layout | Yes, only for layout and control ordering | Current date range, type, address/label, minimum amount, and inactive-tx filtering logic | Blackcoin watch-only filter and txid-search/search-field semantics that are not present in Innova | Do not change filter calculations or add unsupported filters |
| `TransactionDescDialog` | `TransactionDescDialog` | Make the details dialog easier to read without touching transaction content | Yes, for margins, spacing, and window sizing only | Existing HTML description generated by `TransactionDesc` | Any change to description generation, content semantics, or backend details | Keep the dialog presentation-only |

### Transactions flow audit notes

- Innova opens the history page through `BitcoinGUI::gotoHistoryPage()`, which switches the stacked widget to `TransactionView`.
- The visible table is backed by the existing `TransactionTableModel` and filtered by `TransactionFilterProxy`. Its headers and viewport remain visible when the proxy has no rows; the empty-state text is presentation-only and does not replace the model.
- The model currently exposes the columns `Status`, `Date`, `Type`, `Address`, `Narration`, and `Amount`, with roles for type, date, long description, address, label, amount, txid, confirmation state, formatted amount, and status.
- Current transaction types in the UI are the existing Innova/Bitcoin-derived set: `Generated`, `RecvWithAddress`, `RecvFromOther`, `SendToAddress`, `SendToOther`, `SendToSelf`, and `Other`.
- The current filter surface is date, type, address/label search, and minimum amount; there is no separate watch-only filter in this Innova page.
- The context menu already supports copy address, copy label, copy amount, copy transaction ID, edit label, and transaction details.
- CSV export is already wired through the existing model roles and should remain unchanged.
- Blackcoin More adds extra transaction presentation patterns such as a watch-only filter, raw/full detail copying, third-party explorer actions, and different header polish, but those are not part of this Innova page and should not be added without backend support.


## Stage 6: Node / Debug Window

| Blackcoin class / area | Innova class / area | Purpose | Safe to transfer | Innova models / behavior kept | Blackcoin dependencies rejected | Constraints |
| --- | --- | --- | --- | --- | --- | --- |
| `RPCConsole` / `debugwindow.ui` | `RPCConsole`, `rpcconsole.ui` | Modernize the node/debug window layout without changing RPC or networking logic | Yes, for tab ordering, spacing, splitters, labels, font presentation, and window title only | Existing `ClientModel`, `PeerTableModel`, `BanTableModel` model access, `TrafficGraphWidget`, console history, RPC executor thread, and node detail labels | `interfaces::Node`, new peer roles, wallet-controller integration, ban/disconnect actions, extra node statistics, and any RPC backend rewrite | Keep RPC execution, peer refresh, traffic counters, and connection logic unchanged |
| `TrafficGraphWidget` | `TrafficGraphWidget` | Present network traffic more clearly | Yes, for layout, range control, and placement only | Existing sent/received byte counters and graph sampling | Any new traffic sampling, node stats, or color semantics changes | Do not change byte accounting or reset behavior |
| `PeerTableModel` / peer detail panel | `PeerTableModel`, peer detail widgets in `rpcconsole.ui` | Present connected peers in a more readable table/detail split | Yes, for column sizing, splitter layout, and readable detail grouping | Existing peer columns (`Address:Port`, `User Agent`, `Sent`, `Recv`, `Height`, `Ping`) and detail fields from `CNodeCombinedStats` | Extra peer columns, disconnect/ban/unban actions, and new peer-state roles not present in Innova | Do not add fake values or unsupported peer controls |
| `BanTableModel` | `ClientModel::getBanTableModel()` only | Ban presentation exists as a backend model, but is not surfaced in the current Innova UI | No, not in this stage | Keep the model available for future use | Any new bans tab, ban actions, or backend behavior changes | Do not invent a bans workflow in the UI |

### Node / Debug window audit notes

- Innova currently exposes a four-tab node/debug dialog: `Information`, `Console`, `Network Traffic`, and `Peers`.
- The dialog is backed by `RPCConsole`, which remains a `QDialog` and drives a dedicated RPC executor thread in the GUI process.
- `ClientModel` already provides the node-facing data used by the dialog: connection count, block count, byte totals, last block time, peer model, ban model, startup/build/version strings, and the existing DAG/finality snapshot used on the Information tab.
- The peer table model in Innova is narrow: it provides only `Address:Port`, `User Agent`, `Sent`, `Recv`, `Height`, and `Ping`, and the detail panel fills from the cached `CNodeCombinedStats` snapshot.
- The existing UI has no safe hook for the richer Blackcoin/Bitcoin Core peer actions such as disconnect, ban, or unban, so those remain out of scope.
- Blackcoin More uses a more modern `debugwindow.ui` structure with splitters and denser presentation; that style can be borrowed only as layout guidance because its node and wallet abstractions are not present in Innova.

### Stage 6 transfer boundary

- Safe to transfer: tab titles, splitter-based layout, column sizing, group spacing, console font presentation, and neutral window chrome.
- Not safe to transfer: node backend abstractions, `interfaces::Node`, richer peer state roles, peer management actions, ban controls, or any traffic/connection accounting logic.
- The next step after this stage should remain outside the debug window and move to the remaining wallet pages only if the current wallet/backend audit permits it.


## Main window geometry and splash screen

| Reference area | Innova implementation | Safe presentation adaptation | Preserved behavior | Rejected dependencies |
| --- | --- | --- | --- | --- |
| Blackcoin More `BitcoinGUI` geometry | `BitcoinGUI::BitcoinGUI`, `BitcoinGUI::~BitcoinGUI` | Restore `MainWindowGeometry`; otherwise use a centered, screen-bounded `1280 x 800` default and a target minimum of `1024 x 640` | Existing application identity, page ownership, tray behavior, models, and close semantics | Blackcoin `interfaces::Node`, `WalletFrame`, `WalletController`, and multiwallet startup |
| Blackcoin More primary toolbar | `BitcoinGUI::createToolBars` | Keep the four existing navigation actions and reduce icon size, margins, and spacing | Existing `QAction` objects, shortcuts, signals, slots, and Window-menu access to other pages | Wallet selector, multiwallet controls, branded styling, and global QSS |
| Blackcoin More typography density | `OverviewPage` local presentation | Use the system font with `DemiBold` only for the two Overview section headings, and normal weight for every balance value and sync warning | Existing balance labels, values, update signals, and warning visibility logic | Global font overrides, bundled fonts, fixed application-wide point sizes, and model changes |
| Innova balance-grid alignment | `OverviewPage` / `overviewpage.ui` | Remove the unused third value column so available, staking, shielded, pending, immature, locked, and watch-only amounts share one right edge | Existing balance sources, visibility conditions, and staking updates | Any staking calculation or model change |
| Innova status indicators | `BitcoinGUI` status labels | Tint the existing alpha masks with `QPalette::WindowText` or `QPalette::Highlight` so icons remain legible on light and dark native themes | Existing connection, Tor, sync, encryption, staking, and collateral-node state logic and tooltips | New states, polling, counters, or status backend changes |
| Innova Transactions empty state | `TransactionView` local table subclass | Keep headers and the real `QTableView` visible while painting the existing empty-state text in its viewport | Existing `TransactionTableModel`, proxy filters, selections, columns, export, and context actions | New model roles, placeholder rows, or transaction backend changes |
| Historical Innova secondary toolbar | Removed in stage 1 from `BitcoinGUI::createToolBars` | Confirm that the persistent bottom action strip remains absent | `exportAction`, `encryptWalletAction`, and `changePassphraseAction` remain live and model-driven | Reintroducing a duplicate global wallet-action bar |
| Blackcoin More `SplashScreen` presentation | Innova splash creation and init callback in `src/qt/bitcoin.cpp` | Compose a compact `560 x 320` HiDPI pixmap from the existing Innova logo, system palette, client version, copyright, and current startup message | Existing `uiInterface.InitMessage` subscription, startup ordering, finish wait, shutdown message, and `-splash` / `-min` behavior | Blackcoin branding, `interfaces::Node`, wallet handlers, shutdown controls, and startup backend |
| Blackcoin splash dimensions and hierarchy | Innova `bitcoin.qrc` and existing `:/icons/innova` resource | Remove the photographic `488 x 559` splash PNG and its resource entry | Existing Innova logo resource and resource build flow | Blackcoin logo, gold palette, external assets, fonts, and advertising URLs |

### Typography audit

- Blackcoin More does not set a custom global UI family; ordinary widgets inherit `QApplication::font()` from the desktop theme. Innova now follows the same system-font behavior.
- Blackcoin More separately bundles `RobotoMono-Bold.ttf` and applies `Roboto Mono Bold` to Overview balance values through an option. That font resource and option are intentionally not transferred because Innova balance values are required to remain in the standard system font and this stage must not add bundled fonts or settings.
- Only the two Overview section headings use the relative `QFont::DemiBold` weight; all balance values and sync warnings retain normal system size and weight.

### Geometry audit

- Before this stage, Innova always calculated a fresh size as 75 percent of screen width capped at 1200 and 80 percent of screen height capped at 900, with an `850 x 600` minimum. No `saveGeometry` or `restoreGeometry` path existed.
- The new path reads `QSettings::MainWindowGeometry`. A valid saved value is restored and bypasses the centered default; the normal Qt minimum-size constraint still applies. Qt `restoreGeometry` retains the existing protection against off-screen restored windows.
- The default is `1280 x 800`, bounded to the current primary screen available geometry with a small margin. The target minimum is `1024 x 640` and is itself bounded for screens smaller than that target.

### Wallet action and menu audit

- The old lower strip was the `secondaryToolbar` in pre-stage-1 `BitcoinGUI::createToolBars`, containing Export, Encrypt Wallet, and Change Passphrase. Stage 1 removed that toolbar while retaining the actions.
- Export remains in File and is rebound by existing page-navigation slots to Transactions, Address Book, receiving addresses, or staking-input export where supported. Transactions also keeps its page-local Export button.
- Encrypt Wallet and Change Passphrase remain in Settings and continue to receive their existing enabled/checked state updates from `setEncryptionStatus`.
- The source top-level menu is `&Window`; EN maps it to Window and RU maps it to `&Окно`. No Tools menu or duplicate backend action is introduced.

### Splash transfer boundary

- The splash now uses application/system fonts and palette colors, the existing Innova icon, and `FormatFullVersion()`. No new image or font file is added.
- Startup and shutdown messages still arrive through the existing callback. Only alignment, color, reserved message space, and static identity text are changed.
- No startup stage, wallet loading, block loading, synchronization, shutdown, RPC, P2P, consensus, or model code is transferred from Blackcoin More.
