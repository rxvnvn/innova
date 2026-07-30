# Innova-Qt UI Polish Roadmap

Innova-Qt contains a functional but visually dated Qt interface inherited from an older Bitcoin-Qt codebase. UI polish should be performed incrementally. Each issue should be investigated and implemented as a separate minimal patch where practical. Behavioural bugs must not be treated as cosmetic issues. Existing wallet, transaction, mining, staking, and platform behaviour must be preserved. Every completed item should receive appropriate build and runtime validation.

## Working Principles

1. Reproduce and understand each issue before changing code.
2. Prefer minimal local changes over broad Qt refactoring.
3. Separate behavioural fixes from visual-only cleanup.
4. Avoid combining unrelated UI changes in one commit.
5. Preserve Linux, Windows, and macOS behaviour unless a change is explicitly platform-specific.
6. Check both normal and high-DPI rendering where practical.
7. Check translations and text expansion when changing layout.
8. Do not replace the full icon set without a separate design review.
9. Record user-visible changes in CHANGELOG.md.
10. Validate transaction and wallet updates against actual runtime behaviour, not screenshots alone.

## Priority Definitions

| Priority | Meaning |
|----------|---------|
| P0 | Data loss, transaction safety, or application unusability |
| P1 | Broken user-visible behaviour |
| P2 | Serious usability or readability issue |
| P3 | Visual consistency and layout polish |
| P4 | Optional modernization or technical debt |

## Current Backlog

### P2 — Transaction icons have insufficient contrast

**Observed behaviour:**

- Transaction status and type icons are very pale on the default light theme.
- Incoming, outgoing, mining/staking, lock, and confirmation icons can be difficult to distinguish.
- Some icons almost disappear against alternating table backgrounds.

**Investigation plan:**

- Identify all resources and delegates used by the overview and transaction views.
- Determine whether low contrast comes from source PNG/SVG assets, opacity, disabled icon mode, palette transformations, or delegate painting.
- Review icon appearance in normal, selected, inactive, confirmed, unconfirmed, conflicted, incoming, outgoing, mined, and staked states.
- Check whether the same icons are reused on dark themes.
- Prefer adjusting or replacing only affected resources rather than changing the whole icon system.
- Preserve semantic distinctions between transaction states.

**Acceptance criteria:**

- Icons remain visible on normal and alternating light rows.
- Selected rows remain readable.
- Statuses remain visually distinguishable.
- No regressions on dark palettes or high-DPI displays.

**Suggested future commit scope:**

    style(qt): improve transaction icon contrast

---

### P2 — Some transaction table text is too faint

**Observed behaviour:**

- Address text and some metadata are rendered in very light gray.
- Secondary text can be difficult to read on the default theme.
- Disabled-looking colors may be used for valid transaction data.

**Investigation plan:**

- Inspect `TransactionView`, `TransactionTableModel` roles, transaction delegate painting, and palette usage.
- Identify whether foreground colors come from `Qt::ForegroundRole`, explicit `QColor` values, disabled palette groups, or stylesheet rules.
- Preserve meaningful color distinctions for negative values, immature transactions, conflicted transactions, and inactive states.
- Avoid hard-coded colors where palette-aware colors are practical.

**Acceptance criteria:**

- Normal transaction metadata is clearly readable.
- Disabled, immature, conflicted, incoming, and outgoing states remain distinguishable.
- Selected row contrast is correct.

**Suggested future commit scope:**

    style(qt): improve transaction table readability

---

### P3 — Send page contains excessive horizontal and vertical spacing

**Observed behaviour:**

- Controls related to coin selection, custom change address, UTXO splitting, recipient fields, narration, balance, and send actions are spread too far apart.
- The page has large unused blank areas even at common window sizes.
- Related controls do not always appear visually grouped.

**Investigation plan:**

- Identify whether the layout is generated from a `.ui` file or assembled in C++.
- Inspect size policies, stretches, spacers, minimum heights, row stretches, and container margins.
- Determine which blank space is intentional for dynamic recipient rows.
- Keep room for multiple recipients without making the single-recipient view sparse.
- Verify translated labels, long addresses, validation messages, coin control text, custom change controls, UTXO splitting, narration, and shielded/private tabs.
- Avoid fixed pixel positioning.

**Acceptance criteria:**

- Related controls are visibly grouped.
- Single-recipient layout is more compact.
- Multiple recipients still expand correctly.
- No clipping occurs at 100%, 125%, 150%, or 200% scaling where testable.
- Russian and English labels fit.

**Suggested future commit scope:**

    style(qt): tighten send page layout

---

### P3 — Overview page has excessive unused space and weak hierarchy

**Observed behaviour:**

- Balance information occupies a small part of a large empty page.
- Total, available, private, and unconfirmed balances do not form a strong visual hierarchy.
- Recent transactions occupy only a narrow area and their icons are faint.
- The page does not make effective use of common desktop window sizes.

**Investigation plan:**

- Inspect `OverviewPage` layout, recent transaction delegate, balance labels, stretches, margins, and fixed sizes.
- Determine whether older removed features left unused layout space.
- Improve grouping and alignment without introducing a full redesign.
- Preserve all balance categories that are still supported.
- Check long values and large money supplies.
- Verify privacy/shielded balances where applicable.

**Acceptance criteria:**

- Primary balance is immediately identifiable.
- Secondary balances remain readable but visually subordinate.
- Recent transactions use available space more effectively.
- Layout remains correct when resized.
- No wallet values are hidden.

**Suggested future commit scope:**

    style(qt): refine overview page layout

---

### P3 — Toolbar and navigation styling is visually dated

**Observed behaviour:**

- Main navigation actions use inconsistent icon contrast and spacing.
- Active and inactive states rely heavily on the platform's default toolbar appearance.
- The navigation bar feels visually disconnected from the page below it.

**Investigation plan:**

- Inspect `QToolBar` configuration, action icons, icon size, tool button style, spacing, margins, and checked state.
- Preserve keyboard shortcuts and accessible action names.
- Avoid replacing the toolbar with a custom widget unless necessary.
- Check Linux, Windows, and macOS native rendering.
- Keep the existing navigation order unless separately approved.

**Acceptance criteria:**

- Active section is easy to identify.
- Icons and labels remain readable.
- Native platform behaviour is preserved.
- Toolbar remains compact and keyboard accessible.

**Suggested future commit scope:**

    style(qt): refine main navigation toolbar

---

### P3 — Inconsistent language and untranslated interface text

**Observed examples:**

- Transactions
- Search
- Export
- Send
- Shield
- Private
- Unshield
- Silent Pay
- Narration
- Mined or Staked
- Placeholder text and labels may mix Russian and English.

**Investigation plan:**

- Inventory visible strings in the affected Qt pages.
- Determine which strings are missing from translation catalogs, marked incorrectly, dynamically constructed, or left untranslated.
- Do not translate protocol names, RPC names, feature names, or technical terms blindly.
- Update source strings and translation catalogs only in a dedicated task.
- Check text expansion after translation.

**Acceptance criteria:**

- The selected locale is applied consistently to ordinary interface text.
- Intentional product and protocol terms remain unchanged.
- No layout clipping is introduced.

**Suggested future commit scope:**

    i18n(qt): complete wallet interface translations

---

### P4 — Modern Qt signal and slot syntax

**Observed behaviour:**

- Much of the GUI still uses `SIGNAL()` and `SLOT()` macros.
- This is inherited technical debt and not currently a user-facing bug.

**Guidance:**

- Do not mix this modernization with visual changes or bug fixes.
- Migrate incrementally by component.
- Require a clean build and runtime signal verification.
- Avoid changing connection semantics accidentally.

**Suggested future commit scope:**

    refactor(qt): modernize signal connections in <component>

---

### P4 — Minor ownership and Qt idiom cleanup

**Potential areas:**

- Widgets created temporarily without a parent before being added to layouts.
- Dialog ownership and lifetime review.
- `foreach` replacement with C++ range-for.
- Redundant disconnect/reconnect patterns.
- UI calculations mixed into `BitcoinGUI`.

**Guidance:**

- Verify every reported issue against the actual code before changing it.
- Do not classify `QObject` parent-tree ownership as a leak without proving it.
- Keep cleanup separate from behavioural fixes and visual changes.

**Suggested future commit scope:**

    refactor(qt): cleanup ownership and idioms in <area>

---

## Completed Work

### Completed — Newly mined reward transactions appear without one-block delay

**Problem:**

- Generated coinbase transactions were hidden from the Qt source model for one block.
- During `ConnectBlock`, `SyncWithWallets → AddToWallet` fires `CT_NEW`/`CT_UPDATED` before `pindexBest` advances.
- `TransactionRecord::showTransaction()` returned `false` for coinbase transactions not yet in the active chain, suppressing the insertion.
- Recovery relied on `UpdatedTransaction(hashPrevBestCoinBase)` one block later, producing a deterministic delay.

**Resolution:**

- Removed the inherited `IsInMainChain` gate from `TransactionRecord::showTransaction()`.
- Inactive generated rewards continue to be represented via `NotAccepted` status and filtered by the default `TransactionFilterProxy`.

**Validation:**

- `TransactionRecord::showTransaction()` now returns `true` unconditionally; confirmed by unit test with an unconfirmed coinbase `CWalletTx`.
- Clean production and test Qt builds pass.
- Staking/coinstake behaviour was not affected (the gate only checked `IsCoinBase()`).
- `KernelRecord::showTransaction()` was not modified.

---

### Completed — Restore the main window from the system tray on X11

**Problem:**

- After minimize-to-tray, Qt retained `Qt::WindowMinimized` in `windowState()`.
- `hide()` did not clear that stale state.
- During restoration on X11, `changeEvent` observed the minimized transition and hid the window again.

**Resolution:**

- Clear the stale minimized/maximized state while the window is still hidden.
- Show the window only after its state is normalized.

**Validation:**

- Innova-Qt builds successfully.
- The main window restores from the tray on Linux/GNOME/X11.
- Repeated minimize, hide, and restore cycles work.
- No external `wmctrl` or `xdotool` workaround is required.

---

## Follow-Up Items (Not Addressed)

The following observations were noted during the P1 investigation but were not caused by the same root cause. They remain open.

### P1 — Staking/minting view may delay or miss generated rewards

`KernelRecord::showTransaction()` contains a separate `!wtx.IsInMainChain()` gate that affects the staking/minting overview page (`MintingView`). This gate is unrelated to the main transaction list fix. Evaluate separately.

### P2 — Possible transaction notification loss under TRY_LOCK failure

`TransactionTableModel::updateWalletTransaction()` acquires `TRY_LOCK(cs_main)`. If the lock fails, the notification is silently dropped. The transaction may still appear on the next `modelReset` or page switch. Evaluate whether this causes visible delays or missed rows.

### P3 — Confirmation refresh timing after block connect

After a new block connects, existing transaction confirmations may lag by one model update cycle. Investigate whether `NotifyTransactionChanged` is being emitted for every affected transaction or only for the newly generated coinbase.

---

## Suggested Implementation Order

1. **Transaction icon contrast** — daily usability, moderate effort.
2. **Transaction text readability** — daily usability, moderate effort.
3. **Send page spacing** — layout polish, noticeable at every use.
4. **Overview page hierarchy and spacing** — layout polish, first-impression page.
5. **Translation consistency** — i18n fix, affects all locales.
6. **Toolbar polish** — visual refresh of primary navigation.
7. **Optional Qt modernization** — no user-facing value, schedule last.

Behavioural work comes before cosmetic work. Resolve remaining P1/P2 items before P3/P4.

## Validation Matrix

| Check | Requirement |
|-------|-------------|
| `innovad` clean build | Must pass |
| `innova-qt` clean build | Must pass |
| `test_innova` | Must pass |
| GUI startup | No crash |
| Normal restore from tray | Correct window state |
| Minimize-to-tray | Window hidden, tray icon visible |
| Close-to-tray (where enabled) | Window hidden on close |
| Incoming transaction display | Row appears in transaction list |
| Outgoing transaction display | Row appears in transaction list |
| Mined transaction display | Row appears without one-block delay (fix applied) |
| Staked transaction display | Row appears promptly during staking (KernelRecord gate not yet evaluated) |
| Confirmation updates | Confirmations increment correctly |
| Wallet locked state | Lock icon, send disabled |
| Wallet unlocked state | Send enabled |
| Russian locale | Labels translated, no clipping |
| English locale | No regressions |
| Normal DPI (96) | Layout correct |
| High DPI (192+) | Layout correct where available |
| Light palette | Text and icons readable |
| Dark palette (where supported) | Text and icons readable |
| Linux / X11 | All tray and window operations correct |
| Linux / Wayland (where available) | Basic correctness |
| Windows | No regressions |
| macOS (where available) | No regressions |

Do not claim unsupported platforms were tested. This matrix is a future checklist.

## Commit Discipline

Use the following commit prefixes consistently:

| Prefix | Purpose |
|--------|---------|
| `fix(qt):` | Behavioural bugs |
| `style(qt):` | Visual-only changes |
| `i18n(qt):` | Translations |
| `refactor(qt):` | Internal modernization |
| `docs(qt):` | Roadmap updates |

One commit should normally address one roadmap item.

## Roadmap Maintenance

- Mark items **completed** only after implementation and validation.
- Add a reference to the commit hash when available.
- Move completed entries into the **Completed Work** section.
- Add user-visible fixes to `CHANGELOG.md`.
- Keep forensic details in separate documents when investigation becomes substantial.
- Do not let this document become a raw dump of every UI idea.
