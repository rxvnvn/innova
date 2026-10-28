# Innova [INN]
Tribus Algo PoW/PoS Hybrid Cryptocurrency

![logo](https://i.imgur.com/0ieINKm.png)

[![License: GPL v3](https://img.shields.io/badge/License-MIT-blue.svg)](https://github.com/innova-foundation/innova/blob/master/COPYING)

[![GitHub version](https://img.shields.io/github/release/innova-foundation/innova.svg)](https://badge.fury.io/gh/innova-foundation%2Finnova)

[![GitHub Commit Activity](https://img.shields.io/github/commit-activity/m/innova-foundation/innova)](https://github.com/innova-foundation/innova/pulse)

[![Hits](https://hits.seeyoufarm.com/api/count/incr/badge.svg?url=https%3A%2F%2Fgithub.com%2Finnova-foundation%2Finnova&count_bg=%231283C4&title_bg=%23555555&icon=&icon_color=%231283C4&title=Hits+-+Daily%2FTotal&edge_flat=false)](https://hits.seeyoufarm.com)

![Discord Online Users](https://img.shields.io/discord/391676334956347395?label=Discord&color=%230F80C1)
[![Join Discord](https://img.shields.io/badge/Discord-Chat-blue.svg?logo=discord)](https://discord.gg/mNM59znzNG)

![GitHub code size in bytes](https://img.shields.io/github/languages/code-size/innova-foundation/innova.svg)
![GitHub repo size in bytes](https://img.shields.io/github/repo-size/innova-foundation/innova.svg)

[![Innova downloads](https://img.shields.io/github/downloads/innova-foundation/innova/total.svg?color=blue)](https://github.com/innova-foundation/innova/releases)
[![Innova latest release downloads](https://img.shields.io/github/downloads/innova-foundation/innova/latest/total?color=blue)](https://github.com/innova-foundation/innova/releases)

[![Innova Snapcraft](https://snapcraft.io/innova/badge.svg)](https://snapcraft.io/innova)

[![Github Actions](https://github.com/innova-foundation/innova/actions/workflows/build.yml/badge.svg)](https://github.com/innova-foundation/innova/actions)
[![CircleCI](https://circleci.com/gh/innova-foundation/innova.svg?style=shield)](https://app.circleci.com/pipelines/github/innova-foundation/innova)

<a href="https://twitter.com/intent/follow?screen_name=Innova_Fdn"><img src="https://img.shields.io/twitter/follow/Innova_Fdn?style=social&logo=twitter" alt="follow on Twitter"></a>

[Links](#links)

## Current development priority — 25 October 2026

The owner’s priority is recovery and stabilization of the legacy network after the recent hard fork. ASERT is deferred indefinitely. Completed Stage C/D remain historical research only; their results are preserved, but no Stage D.1, parameter selection, ASERT activation, DGW change, new hard fork, or mandatory client upgrade is planned. Parameters and activation remain unset. Resuming ASERT requires a new explicit owner decision.

Active work continues on P2P-01 block propagation, Block Index V2/Store3A stability and compatibility, and outstanding recovery (REC) tasks under their existing approval and verification requirements. Completed research and the accepted REC-03 operational recovery do not establish overall release readiness; the release verdict remains HOLD.

## Intro

Innova is a true optionally anonymous, untraceable, and secure hybrid cryptocurrency.

Ticker: INN

Innova [INN] is an anonymous, untraceable, energy efficient, Proof-of-Work (Tribus Algorithm created by carsenk) and Proof-of-Stake cryptocurrency.

## Supported Operating Systems

* Linux 64-bit
* Windows 64-bit
* macOS 10.11+

## Install Innova with Snap on any Linux Distro

* `sudo apt install snapd`
* `sudo snap install innova`

* `innova` for running the QT
* `innova.daemon` for running innovad

## Specifications

* Total number of coins: 18,000,000 INN
* Stake interest: 6% annual static inflation (source-verified: `COIN_YEAR_REWARD = 0.06 * COIN` = 6%, src/main.h:140)
* Ideal block time: ~15 seconds (source-verified: `nTargetSpacing = 15`, src/main.cpp:91)
* Confirmations: 10 blocks
* Maturity: 65 blocks (`nCoinbaseMaturity = 65`, src/main.cpp:96; the historically documented value 75 corresponds to the wallet spend-maturity heuristic `nCoinbaseMaturity + 10`, src/main.cpp:3884; verify against the reference release)
* Min stake age: 10 hours

* Cost of Hybrid Collateral Nodes: 25,000 INN
* Hybrid Collateral Node Reward: 65% of the current block reward
* P2P Port: 14530, Testnet Port: 15539
* RPC Port: 14531, Testnet RPC Port: 15531
* Collateral Node Port: 14539, Testnet Port: 15539

* INN Magic Number: 0xb73ff4fa
* P2P user-agent (subversion): `/innova:5.0.5/` (CLIENT_NAME `innova` [src/version.cpp:11] + FormatSubVersion [src/util.cpp:1806] + CLIENT_VERSION 5.0.5 [src/clientversion.h])
* BIP44 CoinType: 116
* Base58 Pubkey Decimal: 102
* Base58 Scriptkey Decimal: 137
* Base58 Privkey Decimal: 230

## Technology

* Hybrid PoW/PoS Collateral Nodes
* Stealth addresses
* Ring Signature / ANON transactions: consensus mechanism **removed** in this source (ANON transactions rejected unconditionally; the ring-sig deprecation fork gate is inert on mainnet)
* Native Optional Tor Onion Node (-nativetor=1)
* Multi-Signature Addresses & TXs
* Atomic Swaps using UTXOs (BIP65 CLTV)
* BIP39 Support (Coin Type 116) (documented historical claim; no BIP39/HD-derivation code path was found in this source tree at this HEAD — treat as unverified against source)
* Fast ~15 Second Block Times
* Tribus PoW Algorithm comprising of 3 NIST5 algorithms
* Tribus PoW/PoS Hybrid
* Full decentralization
* Name Value System supporting the IDNS for fully & truly decentralized blockchain domains

## CPU Mining

Innova Core includes optional built-in Tribus CPU mining. It is disabled by
default and is controlled through RPC. See [CPU mining](doc/cpu-mining.md) for
commands, thread limits, reward distribution, and safety notes.

## Privacy & Protocol Innovations (IIPs)

Innova Improvement Proposals (IIPs) formalize protocol innovations.
The authoritative historical specifications are in [docs/proposals/IIP_INDEX.md](docs/proposals/IIP_INDEX.md).

> **Status note (source-verified, 2026-10-25).** The table below documents the historical IIP
> proposals from IIP_INDEX.md. **None of them is active on the current mainnet build.** Every
> consensus fork gate — cold staking, CN payment validation, tighter timestamp drift, ring-sig
> deprecation, POEM — resolves to `MAINNET_EXPERIMENTAL_V5_DISABLED_HEIGHT = 999999999`
> (`src/main.h:138–221`), i.e. is inert on mainnet. IIP-0002/0004–0010 have **no corresponding
> consensus gate in this source at all** (not implemented). The ANON/ring-signature mechanism is
> **removed** (transactions rejected unconditionally). Fork heights below are the historical
> documented values from the spec and do **not** imply current activation. Do not treat a README
> or IIP_INDEX status as an activation authority; verify against source consensus gates.

| IIP | Title | Status vs current source | Fork Height (documented) |
|-----|-------|--------------------------|--------------------------|
| [IIP-0001](docs/proposals/IIP_INDEX.md#iip-0001-cold-staking-p2cs) | Cold Staking (P2CS) | Historical proposal; fork gate inert on mainnet (=999999999), not active | 7,100,000 |
| [IIP-0002](docs/proposals/IIP_INDEX.md#iip-0002-shielded-transactions) | Shielded Transactions (Pedersen + Bulletproofs + Lelantus) | Historical proposal; no consensus gate in source, not implemented | 8,000,000 |
| [IIP-0003](docs/proposals/IIP_INDEX.md#iip-0003-ring-signature-deprecation) | Ring Signature Deprecation | ANON/ring-sig consensus removed (unconditional reject); deprecation gate inert | 8,500,000 |
| [IIP-0004](docs/proposals/IIP_INDEX.md#iip-0004-dynamic-selective-privacy) | Dynamic Selective Privacy (8 modes) | Historical proposal; no consensus gate, not implemented | 8,600,000 |
| [IIP-0005](docs/proposals/IIP_INDEX.md#iip-0005-confidential-coinjoin) | Confidential CoinJoin | Historical proposal; no consensus gate, not implemented | 8,700,000 |
| [IIP-0006](docs/proposals/IIP_INDEX.md#iip-0006-fcmp-full-chain-membership-proofs) | FCMP++ Full-Chain Membership Proofs | Historical proposal; no consensus gate, not implemented | 9,000,000 |
| [IIP-0007](docs/proposals/IIP_INDEX.md#iip-0007-silent-payments-and-silent-shielding) | Silent Payments + Silent Shielding | Historical proposal; no consensus gate, not implemented | 8,000,000 |
| [IIP-0008](docs/proposals/IIP_INDEX.md#iip-0008-dandelion-network-privacy) | Dandelion++ Network Privacy | Historical proposal; no consensus gate, not implemented | 8,000,000 |
| [IIP-0009](docs/proposals/IIP_INDEX.md#iip-0009-nullstake-v1) | NullStake V1 (ZK Private Staking) | Historical proposal; no consensus gate, not implemented | 9,500,000 |
| [IIP-0010](docs/proposals/IIP_INDEX.md#iip-0010-nullstake-v2) | NullStake V2 (Poseidon2 + Bulletproof AC) | Historical proposal; no consensus gate, not implemented | 10,000,000 |

## Collateral Nodes

Innova supports two Collateral Node deployment models, both fully
supported:

* **Remote / controller** (legacy) — collateral wallet off the VPS, driven
  by `collateralnode.conf` + `start-alias`/`start-many`.
* **Self-contained local** — one node, exact
  `collateralnodeoutpoint=<txid>-<vout>`, no `collateralnode.conf` required.

See the operator guide for setup, verification, and troubleshooting:

* [Collateral Nodes operator guide](doc/collateralnode.md)

## Links

* Official Website(https://innova-foundation.com/)
* Innova Twitter(https://twitter.com/innovacoin)
* Innova Discord Chat(https://discord.gg/mNM59znzNG)
* Innova Telegram Chat(https://t.me/innova_foundation)

## installdaemon.sh

Compile the latest Innova Daemon (Headless Wallet) Ubuntu 16.04, Ubuntu 18.04, or 20.04

Compiles Innova Daemon Ubuntu 16.04, 18.04 or 20.04, Grabs latest chaindata, and populates innova.conf with addnodes or can update a previous compile to the latest master branch.  
```bash -c "$(wget -O - https://raw.githubusercontent.com/innova-foundation/innova/master/installdaemon.sh)"```  

To turn on nativetor in innova.conf  
```nativetor=1```  

## innovaqtubuntu.sh

Compile the latest Innova QT (Graphical Wallet) Ubuntu 16.04, Ubuntu 18.04, or 20.04

Credits to Buzzkillb for the creation of this bash script, original repository: https://github.com/buzzkillb/denarius-qt/

Compiles Innova QT Ubuntu 16.04, 18.04 or 20.04, Grabs latest chaindata, and populates innova.conf with addnodes or can update a previous compile to the latest master branch.  
```bash -c "$(wget -O - https://raw.githubusercontent.com/innova-foundation/innova/master/innovaqtubuntu.sh)"```  

To turn on nativetor in innova.conf  
```nativetor=1```   

## Development process


Developers work in their own trees, then submit pull requests when
they think their feature or bug fix is ready.

The patch will be accepted if there is broad consensus that it is a
good thing.  Developers should expect to rework and resubmit patches
if they don't match the project's coding conventions (see [coding.txt](doc/coding.txt))
or are controversial.

The master branch is regularly built and tested, but is not guaranteed
to be completely stable. Tags are regularly created to indicate new
stable release versions of Innova.

Feature branches are created when there are major new features being
worked on by several people.

From time to time a pull request will become outdated. If this occurs, and
the pull is no longer automatically mergeable; a comment on the pull will
be used to issue a warning of closure. The pull will be closed 15 days
after the warning if action is not taken by the author. Pull requests closed
in this manner will have their corresponding issue labeled 'stagnant'.

Issues with no commits will be given a similar warning, and closed after
15 days from their last activity. Issues closed in this manner will be
labeled 'stale'.
