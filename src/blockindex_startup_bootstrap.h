// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

// A.10.1i — BlockIndexStartupBootstrap.
//
// Minimal authoritative-startup BOOTSTRAP OWNER substrate. For a SELECTED,
// validated, AUTHORITATIVE V2 generation, it binds together:
//
//   V2BlockIndexStartupAuthority   (AUTHORITY   — logical startup truth)
//   BlockIndexV2Reader             (by-value metadata source)
//   BlockIndexDerivedStateStore    (by-value derived chainTrust/size/stake)
//   BlockIndexAuthorityMaterializer(MATERIALIZATION — sparse CBlockIndex rebuild)
//   BlockIndexHotOwner             (RESIDENCY LIFETIME — pin / anchor)
//
// and pins the best-tip as a PERMANENT BOOTSTRAP ANCHOR, so that a future
// authoritative-startup cutover (D) can supply the resident best-tip handle
// without the legacy all-history CBlockIndex graph.
//
// THIS PHASE DOES NOT CUT OVER STARTUP. It builds the substrate only.
// Production default startup remains LEGACY_RESIDENT (init.cpp still runs
// legacy LoadBlockIndex()).
//
// Generation coherence is central: authority, reader, derived store,
// materializer, and owner must all bind the SAME selected generation, else
// construction FAILS CLOSED.
//
// Ownership contract: the exposed best-tip CBlockIndex* is valid for the
// bootstrap lifetime because it is both PINNED and PINNED-PERMANENT (anchored)
// on an owner-owned phashBlock. Every materialized object is owner-owned and
// subject to the existing HotOwner pin/anchor/evict policy.
//
// Lock contract: this bootstrap never acquires cs_main. BlockIndexHotOwner is
// a LEAF (never takes cs_main); materializer I/O runs OUTSIDE the owner lock.
// Lock order cs_main -> HotOwner leaf is preserved.

#ifndef INNOVA_BLOCKINDEX_STARTUP_BOOTSTRAP_H
#define INNOVA_BLOCKINDEX_STARTUP_BOOTSTRAP_H

#include "blockindex_startup_authority.h"
#include "blockindex_v2_reader.h"
#include "blockindex_derived_state.h"
#include "blockindex_authority_materializer.h"
#include "blockindex_hot_owner.h"
#include "blockindex_navigation.h"

#include <memory>
#include <string>

class BlockIndexStartupBootstrap
{
public:
    BlockIndexStartupBootstrap();
    ~BlockIndexStartupBootstrap();

    BlockIndexStartupBootstrap(const BlockIndexStartupBootstrap&) = delete;
    BlockIndexStartupBootstrap& operator=(const BlockIndexStartupBootstrap&) = delete;

    // Open + validate an AUTHORITATIVE generation at `root`, bind every
    // component to the SAME selected generation, and pin the best tip.
    // On ANY failure returns non-OK and sets *error; never partially opens and
    // never silently falls back to the legacy adapter.
    BlockIndexStartupStatus Open(const std::string& root,
                                 const BlockIndexV2ReaderOptions& options,
                                 std::string* error);
    void Close();
    bool IsOpen() const;

    // Bound generation (0 if not open).
    uint64_t Generation() const;

    // ----- authority (by-value) -----
    BlockIndexStartupAuthorityIdentity AuthorityIdentity() const;
    BlockIndexStartupResult GetBestTipRecord() const;

    // ----- pinned best-tip anchor -----
    BlockIndexLogicalId BestTipId() const;
    // Resident, pinned, anchored best-tip CBlockIndex* (valid for the bootstrap
    // lifetime because it is a PINNED-PERMANENT anchor). NULL if not open.
    CBlockIndex* BestTipObject() const;

    // ----- introspection / access (for tests and D readiness) -----
    const BlockIndexHotOwner& Owner() const { return *owner_; }
    BlockIndexHotOwner* OwnerPtr() { return owner_.get(); }
    const BlockIndexAuthorityMaterializer& Materializer() const { return *mat_; }
    // Reader/derived are owned by the authority (single open); expose via it.
    const BlockIndexV2Reader* ReaderPtr() const { return authority_.ReaderPtr(); }
    const BlockIndexDerivedStateStore* DerivedStorePtr() const { return authority_.DerivedStorePtr(); }
    const V2BlockIndexStartupAuthority& Authority() const { return authority_; }

private:
    V2BlockIndexStartupAuthority authority_;
    std::unique_ptr<BlockIndexAuthorityMaterializer> mat_;
    std::unique_ptr<BlockIndexHotOwner> owner_;
    BlockIndexHotHandle bestTipHandle_;
    uint64_t generation_;
    BlockIndexLogicalId bestTipId_;
    bool isOpen_;
};

#endif // INNOVA_BLOCKINDEX_STARTUP_BOOTSTRAP_H