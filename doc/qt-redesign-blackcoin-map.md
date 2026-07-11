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
