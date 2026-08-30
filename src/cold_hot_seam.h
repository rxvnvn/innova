// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_COLD_HOT_SEAM_H
#define INNOVA_COLD_HOT_SEAM_H

#include "blockindex_navigation.h"
#include "blockindex_v2_reader.h"

/** A resolved value snapshot and its unambiguous navigation reference. */
struct ColdHotSeamSnapshot
{
    BlockIndexNavigationRef ref;
    BlockIndexSnapshot snapshot;

    bool IsValid() const { return ref.IsValid() && snapshot.found && snapshot.hash == ref.logical.GetHash(); }
};

/**
 * A fail-closed, read-only bridge between a pinned immutable V2 generation and
 * the live legacy active chain. Callers must hold cs_main for every method:
 * the hot resolver is the LegacyBlockIndexAccessor.
 *
 * It deliberately exposes BlockIndexNavigationRef, never a raw BlockIndexId.
 * This makes a process-local legacy ID unable to be used as a V2 RecordId.
 */
/** Test seam: a by-value hot oracle. Production uses the legacy adapter below;
 *  tests may inject an offline source oracle without CBlockIndex materialization. */
class ColdHotHotResolver
{
public:
    virtual ~ColdHotHotResolver() {}
    virtual BlockIndexSnapshot LookupByHash(const uint256& hash) const = 0;
    virtual BlockIndexSnapshot GetActiveByHeight(int height) const = 0;
    virtual BlockIndexSnapshot GetParentByHash(const uint256& hash) const = 0;
    virtual BlockIndexSnapshot GetNextActiveByHash(const uint256& hash) const = 0;
    virtual BlockIndexSnapshot GetTip() const = 0;
};

class ColdHotSeamNavigator
{
public:
    ColdHotSeamNavigator();

    bool Open(const std::string& v2Root, const BlockIndexV2ReaderOptions& options,
              std::string* error);
    void Close();
    bool IsOpen() const;

    // Test-only: oracle lifetime remains caller-owned; NULL restores production
    // LegacyBlockIndexAccessor behavior. Never called by production startup.
    void SetTestHotResolver(const ColdHotHotResolver* resolver);

    /** Require the pinned V2 generation to match CURRENT and the live active
     * chain at the generation tip. Does not auto-rebase. */
    bool VerifySeam(std::string* error) const;

    bool LookupCold(const BlockIndexLogicalId& logical, ColdHotSeamSnapshot* out,
                    std::string* error) const;
    bool LookupHot(const BlockIndexLogicalId& logical, ColdHotSeamSnapshot* out,
                   std::string* error) const;
    bool Resolve(const BlockIndexNavigationRef& ref, ColdHotSeamSnapshot* out,
                 std::string* error) const;

    bool GetParent(const BlockIndexNavigationRef& ref, ColdHotSeamSnapshot* out,
                   std::string* error) const;
    bool GetAncestor(const BlockIndexNavigationRef& ref, int targetHeight,
                     ColdHotSeamSnapshot* out, std::string* error) const;
    bool GetNextActive(const BlockIndexNavigationRef& ref, ColdHotSeamSnapshot* out,
                       std::string* error) const;
    bool GetStakingMetadata(const BlockIndexNavigationRef& ref,
                            BlockIndexStakingMetadata* out, std::string* error) const;

    /** Bounded modifier-time derivation only (no O(depth) checksum walk). */
    bool GetStakeModifierTime(const BlockIndexNavigationRef& ref,
                              int64_t* out, std::string* error) const;

    /** O(depth) checksum derivation (checkpoint validation; call sparingly). */
    bool GetStakeModifierChecksum(const BlockIndexNavigationRef& ref,
                                  unsigned int* out, std::string* error) const;

    BlockIndexSnapshot GetColdTip() const;
    BlockIndexSnapshot GetHotTip() const;
    uint64_t ColdGeneration() const;

private:
    bool MakeCold(const BlockIndexSnapshot& snapshot, ColdHotSeamSnapshot* out,
                  std::string* error) const;
    bool MakeHot(const BlockIndexSnapshot& snapshot, ColdHotSeamSnapshot* out,
                 std::string* error) const;
    bool IsAtColdTip(const BlockIndexSnapshot& snapshot) const;

    BlockIndexV2Reader coldReader;
    LegacyBlockIndexAccessor hotAccessor;
    const ColdHotHotResolver* testHotResolver;
    bool open;
};

#endif // INNOVA_COLD_HOT_SEAM_H
