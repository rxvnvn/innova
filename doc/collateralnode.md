# Collateral Nodes

Innova supports **two** Collateral Node (CN) operating models. Both remain
supported; which one you choose depends on where you want the wallet that
holds the 25,000 INN collateral to live.

A Collateral Node is a network service that participates in Innova's
collateral-node ecosystem. To run one you must commit a **25,000 INN**
collateral output, hold the key for that output, and obtain a separate
**Collateral Node identity key**.

The exact 25,000 INN requirement and the 15-confirmation maturity rule below
are taken from the current source (`src/main.h`, `src/collateralnode.h`,
`src/activecollateralnode.cpp`). This document describes the operator
workflow for both models.

---

## Deployment Models

| | Model A — Remote / Controller | Model B — Self-Contained Local |
|---|---|---|
| Where the collateral wallet lives | Anywhere (often off the VPS) | Same host as the node |
| `collateralnode.conf` required? | Yes (defines aliases) | **No** |
| `collateralnodeoutpoint=<txid>-<vout>` used? | Optional / via alias | **Yes — exact output** |
| `collateralnode start-alias` / `start-many` | Yes (controller side) | **No** (auto-start) |
| Operational tradeoff | Collateral funds stay off the VPS; requires a controller + a separate CN process | Simpler, single machine; collateral wallet lives on the VPS |
| Intended use case | Operator does not want their collateral funds on the VPS | Operator accepts keeping the collateral wallet on the VPS for simpler operation |

Both models use the same collateral amount, the same CN identity key
concept, the same service-port/bind requirements, and the same
`collateralnode` RPC family. The difference is only **where the CN
registration is driven from**: Model A is driven from a controller wallet
via `collateralnode.conf` aliases; Model B is driven locally by the node
itself from `collateralnodeoutpoint`.

Choose Model A if you do **not** want the wallet controlling the collateral
funds residing on the VPS. Choose Model B if you intentionally accept
keeping the collateral wallet and the CN process on the same host.

---

## Shared concepts

### Collateral amount
A CN requires a collateral output of exactly **25,000 INN**
(`GetMNCollateral() * COIN`; `COLLATERALN_COLLATERAL`). The selected output
must be **wallet-owned**, **unspent**, **trusted** (confirmed), at least
**15 confirmations** old (`COLLATERALNODE_MIN_CONFIRMATIONS_NOPAY = 15`),
and of exactly the 25,000 INN value.

### Collateral output vs CN identity key — these are NOT interchangeable
- **Collateral output** = an existing UTXO in your wallet, exactly 25,000
  INN, identified by a **TXID + VOUT**. It proves ownership; it is what is
  registered as your CN.
- **CN identity key** (`collateralnodeprivkey`) = a **separate** private key
  used only to authenticate your CN's registration and pings. It is
  generated independently (see below) and is unrelated to the private key
  that owns the collateral output.

Never confuse the two. The collateral wallet's private key is **not** the
CN identity key.

### Ports (mainnet)
| Purpose | Port | Source |
|---|---|---|
| P2P (default) | 14530 | `GetDefaultPort()` |
| RPC | 14531 | `-rpcport` default |
| Collateral Node service | **14539 (fixed on mainnet)** | enforced by the CN handshake |

The Collateral Node service port is **fixed at 14539 on mainnet**: the
collateral-node handshake rejects CN sources on any other port, and CN
peers not on 14539 are disconnected. Set `collateralnodeaddr=<ip>:14539`
(do not pick a different port) and make sure the node is actually listening
on **14539** — see the bind guidance below.

---

## Self-Contained Local Mode (Model B)

This is the additive local mode. It needs **no `collateralnode.conf`** and
**no `start-alias`**.

### 1. Prerequisites
- A fully synchronized node (`initialblockdownload=false`).
- A wallet holding an output of exactly **25,000 INN**, at least **15
  confirmations** old.
- The wallet must own that output (its private key available to the node).
- The node must be reachable from the network on the CN service port.
- The required mainnet ports: P2P 14530, CN service port (e.g. 14539),
  RPC 14531 (private).

### 2. Create or identify the collateral
Your collateral is an existing, unspent, exactly-25,000-INN output in your
wallet. Identify it by **TXID** (the transaction id, a 64-character hex
string) and **VOUT** (the zero-based output index within that transaction).

To list the qualifying collateral outputs currently in your wallet:

```
innovad collateralnode outputs
```

This returns a map of `txid -> vout` for available 25,000-INN outputs. Pick
the one you want to commit and note its `TXID` and `VOUT`.

A general wallet UTXO listing is also available via `listunspent` (it lists
all unspent wallet outputs; filter for the 25,000 INN one).

> Terminology: **TXID** identifies the transaction, **VOUT** identifies the
> specific output inside it. Together `TXID-VOUT` uniquely identifies the
> collateral output.

### 3. CN identity key
Generate a fresh Collateral Node identity key with:

```
innovad collateralnode genkey
```

This prints a new wallet-import-format (WIF) private key. Use it ONLY as
`collateralnodeprivkey` in the config. It is **not** the key to your
collateral; keep it separate from your collateral wallet's private key.

> Never put a real private key in documentation, config samples, or commit
> messages. In the example below `<CN_IDENTITY_PRIVATE_KEY>` is a
> placeholder.

### 4. innova.conf

A complete minimal self-contained configuration (replace every
`<PLACEHOLDER>`):

```
server=1
daemon=1
listen=1

# P2P
port=14530

# Collateral Node
collateralnode=1
collateralnodeaddr=<PUBLIC_IP>:14539
collateralnodeprivkey=<CN_IDENTITY_PRIVATE_KEY>
collateralnodeoutpoint=<TXID>-<VOUT>

# RPC (keep private)
rpcuser=<RPCUSER>
rpcpassword=<RPCPASSWORD>
rpcport=14531
rpcallowip=127.0.0.1

# Listeners / binds — IMPORTANT, see below
bind=0.0.0.0:14530
bind=[::]:14530
bind=0.0.0.0:14539
bind=[::]:14539
```

**Listeners / binds — read this carefully.**
- Without any `-bind`, the node binds the P2P port (from `-port`, default
  14530) on `0.0.0.0` and `[::]` automatically.
- **If you specify any `-bind`, that automatic default is replaced** by
  exactly the `-bind` entries you list. The node then listens **only** on
  those binds.
- Therefore, to expose both the normal P2P port (14530) **and** the CN
  service port (14539), you must list **all four** binds explicitly (IPv4 +
  IPv6 for each), as in the example above. If you only bind the CN port,
  peers can no longer reach your P2P listener; if you only bind the P2P
  port, the CN service port is not listening and incoming CN connections
  will fail.
- IPv6 addresses use `[host]:port` notation (`bind=[::]:14539` binds the
  IPv6 any-address on 14539).

### 5. Firewall / network
Open the inbound ports you are listening on:
- **P2P**: 14530 (TCP)
- **Collateral Node service**: the CN port you advertise, e.g. **14539** (TCP)
- **RPC**: 14531 — **do not expose this publicly**. Keep RPC restricted to
  the node's own host/trusted network (`rpcallowip=127.0.0.1` is the
  conservative default). This matches the project's recommendation that RPC
  is a private, authenticated interface.

### 6. Start
Start the node normally:

```
innovad
```

With `collateralnode=1` and `collateralnodeoutpoint` configured, the node
**auto-starts** its Collateral Node: it periodically selects the exact
configured output, registers, and begins pinging. **You do not need**
`collateralnode.conf` and you do **not** need `collateralnode start-alias`
for this model.

### 7. Verification
After startup, use the current RPCs (all confirmed in this version):

```
innovad collateralnode status        # local vin/service/payment_address/network_status
innovad collateralnode outputs       # listed qualifying 25,000-INN outputs
innovad collateralnode list          # network list (use "full" for details)
innovad collateralnode count         # number of CNs known
innovad collateralnode current       # current winner CN
innovad getinfo                      # blocks / connections / initialblockdownload / collateralnode
innovad getblockchaininfo            # chain, blocks, initialblockdownload
innovad gettxout <TXID> <VOUT>       # confirms the collateral is still unspent
```

Expected semantics (not exact transient values):
- `collateralnode status` `vin` should reference the configured `TXID-VOUT`,
  and `service` should be your advertised `ip:port`.
- `network_status` should eventually become `active` (or `registered`).
- The collateral should remain **unspent** (`gettxout` returns `null` if it
  is spent).
- The chain should be synchronized (`initialblockdownload=false`).

**Known status wording:** a self-contained node may report its local status
as `"collateralnode started remotely"` even though it is running in the
self-contained local model. This is **legacy status wording** carried over
from the hot/cold (remote) state machinery inside the CN code; it is cosmetic
and does **not** mean your configuration is wrong. Treat `network_status`
and the presence of your `vin`/`service` as the authoritative signals.

### 8. Exact-outpoint behavior
When `collateralnodeoutpoint=<TXID>-<VOUT>` is set, the node uses **exactly
that** collateral output. It will **not** silently substitute another
25,000-INN output. If the output is:
- missing / absent from the wallet,
- malformed (see troubleshooting),
- already spent,
- the wrong amount (not exactly 25,000 INN),
- not owned by the wallet,
- lacking an available key,
- or otherwise unusable,

...the self-contained CN fails to become capable and reports a precise
reason (visible via `collateralnode status` / `notCapableReason` and the
debug log). It does **not** pick a different output as a fallback.

### 9. Lock behavior — "not wallet-locked" is NOT "safe from spending"
The self-contained CN lifecycle deliberately does **not** place the
configured collateral output into the wallet's locked-coin set
(`LockCoin`). This is intentional, so the CN does not mutate your wallet's
lock state.

**Important:** "not wallet-locked" does **not** mean the collateral cannot
be spent. It is simply not auto-locked by the CN. As an operator you must
still treat the 25,000 INN collateral output as **reserved**: do not spend
it while you are operating the Collateral Node. If you spend it, the CN
loses its collateral and will stop functioning.

### 10. Troubleshooting

| Symptom | Source-supported check |
|---|---|
| Startup aborts with `Invalid -collateralnodeoutpoint: ...` | The `<TXID>-<VOUT>` string is malformed. TXID must be exactly 64 hexadecimal chars; VOUT must be a non-negative integer ≤ 2147483647. |
| Wrong VOUT | `collateralnode status` `vin` differs from your intended output, or status does not become active. Re-check the `-<VOUT>` index of the 25,000-INN output (`collateralnode outputs` lists available ones). |
| Collateral transaction not found | The wallet does not contain that TXID. Verify it is a wallet output (`gettxout`, `collateralnode outputs`). |
| Collateral already spent | `gettxout <TXID> <VOUT>` returns `null`. The output is no longer unspent. |
| Incorrect collateral amount | Only an exactly-25,000-INN output qualifies (`collateralnode outputs` lists only those). |
| Wallet does not own output | The output is not a wallet-owned (spendable) output. The node cannot sign the collateral proof. |
| Collateral key unavailable | The private key for the collateral output is unavailable in the (unlocked) wallet. Unlock/import the key. |
| Node not synchronized | `getinfo` / `getblockchaininfo` show `initialblockdownload=true`. The CN waits for sync. |
| Service port unreachable | Verify the node is actually listening on the advertised CN port (see bind guidance in §4) and that the firewall opens it. |
| CN not appearing in the list | Address reachability / registration propagation; check `collateralnode status` `network_status` and the debug log. |
| Status shows legacy `"collateralnode started remotely"` wording | Documented cosmetic wording from the hot/cold state machinery — not a config error (§7). |
| `vin` differs from the configured outpoint | **Serious configuration/runtime inconsistency.** Confirm `collateralnodeoutpoint` is set exactly, the node restarted with it, and treat the mismatch as a misconfiguration to fix, not ignore. |

---

## Remote / Controller Mode (Model A) — legacy, fully supported

Use this model when you do **not** want the wallet that holds the collateral
funds to reside on the VPS.

Key points:
- The wallet holding the 25,000 INN collateral can be kept **off the VPS**
  (a "cold" wallet/controller).
- A `collateralnode.conf` file defines one or more CN **aliases**, each
  with an alias name, the CN service address, the CN identity key, and the
  collateral `txid`/`outputIndex`.
- The controller wallet drives registration:
  `collateralnode start-alias <alias>` (or `start-many` to start all).
- The remote/server node receives the registration and serves the CN
  (hot/cold remote operation).
- `cnconflock` controls whether the configured collateral outputs are
  locked in the controlling wallet at startup (default on).

Legacy behavior is unchanged. Use this model whenever you want the
collateral funds kept away from the VPS.

---

## Summary of the configuration surface (both models)

| Key | Model A | Model B |
|---|---|---|
| `collateralnode=1` | yes | yes |
| `collateralnodeaddr=<ip>:<port>` | yes | yes |
| `collateralnodeprivkey=<CN identity key>` | yes | yes |
| `collateralnodeoutpoint=<txid>-<vout>` | no (alias supplies it) | **yes (exact)** |
| `collateralnode.conf` | yes | no |
| `collateralnode start-alias/start-many` | yes (controller) | no |