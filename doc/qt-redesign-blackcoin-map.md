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
