// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Serving-side getheaders dedup protection.
//
// When -headersservededup is enabled, a peer that re-sends an identical
// (locator, hashStop, active-tip) request within a short TTL is not served
// again; the request is dropped without writing anything to the socket.
// State is kept per CNode in CNode::mapHeadersServedDedup, so peers are
// independent and the state dies with the connection.  Default behavior is
// completely unchanged when the flag is off.
#ifndef INNOVA_HEADERSERVEDDEDUP_H
#define INNOVA_HEADERSERVEDDEDUP_H

#include <stdint.h>

#include <map>

#include "net.h"
#include "uint256.h"

// Age (seconds) after which a previously served request fingerprint may be
// served again.  Deliberately far below GETHEADERS_REQUEST_TIMEOUT (60 s,
// net.h): it collapses a peer that re-asks the same full 2000-header window
// many times per second (~4.3 pages/s measured), yet a legitimate lost-response
// retry (which lands ~60 s out) is never inside the TTL and so is always
// served.  8 s keeps a genuinely stuck peer's very first retry window short
// while still collapsing several amplification rounds per unchanged tip.
static const int64_t HEADER_SERVED_DEDUP_TTL = 8;

// Hard per-peer bound on tracked fingerprints.  Prevents unbounded growth even
// under a peer that rapidly cycles through many distinct locators.
static const size_t HEADER_SERVED_DEDUP_CAP = 64;

// Set the module-scope enable flag (called once from AppInit2 before
// networking threads start; afterwards immutable).
void InitHeadersServedDedup(bool fEnabled);

bool HeadersServedDedupEnabled();

// Fingerprint of the full request plus its response context: serialization of
// the entire locator (SER_NETWORK/PROTOCOL_VERSION) plus hashStop plus the
// ACTIVE CHAIN TIP HASH at serve time.  Any change in locator, hashStop, or
// active tip (advance or reorg) yields a different fingerprint.  Must be
// computed while cs_main is held so the tip argument is valid.
uint256 HeaderServedDedupFingerprint(const CBlockLocator& locator,
                                     const uint256& hashStop,
                                     const uint256& tipHash);

// True when the entry was served within HEADER_SERVED_DEDUP_TTL of nNowUs.
bool HeaderServedDedupEntryFresh(const CNode::CHeadersServedDedupEntry& entry,
                                 int64_t nNowUs);

// Record a served response for fp at time nNowUs.  Purges expired entries and
// enforces HEADER_SERVED_DEDUP_CAP (evicting the oldest entry when full and
// fp is new), so the map never grows without bound.
void HeaderServedDedupUpsert(std::map<uint256, CNode::CHeadersServedDedupEntry>& map,
                             const uint256& fp, int64_t nNowUs,
                             uint32_t nHeadersCount, uint64_t nBytes);

#endif // INNOVA_HEADERSERVEDDEDUP_H
