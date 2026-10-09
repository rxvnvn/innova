// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// PM1-P0-05 / R3G — derived-state replay primitive (see the header contract).
// The semantic core in ComputeDerivedFromParent is MOVED VERBATIM from
// blockindex_catchup_tool.cpp::ComputeDerived; the catch-up tool now calls
// this function so catch-up and corruption-rebuild share exactly one
// derived-semantics algorithm.

#include "blockindex_derived_replay.h"

#include "hash.h"
#include "kernel.h"
#include "main.h"
#include "util.h"
#include "bignum.h"

#include <boost/filesystem.hpp>

#include <cstdio>
#include <vector>

namespace fs = boost::filesystem;

// ---------------------------------------------------------------------------
// Semantic core — verbatim move from blockindex_catchup_tool.cpp::ComputeDerived
// (itself byte-for-byte the LM builder's M4 compute). The only edit is the
// availability-field preamble: this fn rewrites stakeModifierTime /
// HasStakeModifierTime and nSize / HasBlockSize on every path, so a caller
// that starts from a default-constructed entry cannot leak stale availability
// bits. The catch-up tool always produced entries from default-constructed
// values, so its behavior is unchanged.
// ---------------------------------------------------------------------------
void ComputeDerivedFromParent(const BlockIndexRecord& rec,
                              const BlockIndexDerivedEntry& parentDerived,
                              const std::string& blockDataDir,
                              BlockIndexDerivedEntry* out)
{
    // chainTrust = parentTrust + blockTrust, using the ONE authoritative
    // surviving trust rule (POEM entropy + post-DAG PoS zero), identical to the
    // regular builder, the LM builder and the live authoritative path.
    // PM1-P0-08: legacy nFlags PoS classification; the caller validates that it
    // is consistent with prevoutStake before this is reached.
    const uint256 parentTrust = parentDerived.chainTrust;
    const bool fPos = (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) != 0;
    const uint256 bt = GetAuthoritativeBlockTrustValue(rec.nBits, rec.height, fPos,
                                                       rec.hashProof, rec.hash);
    out->chainTrust = parentTrust + bt;

    // checksum - canonical kernel.cpp GetStakeModifierChecksum semantics: a
    // parentless NON-genesis block (non-canonical chain root / side-branch root)
    // carries a FORCED 0 checksum, and that forced 0 feeds the descendants
    // recurrence via parentChecksum below. Only a parentless block whose hash IS
    // GetGenesisBlockHash() takes the parentless (genesis-shape) recurrence.
    // Mirrors kernel.cpp GetStakeModifierChecksum, the V2 reader
    // (blockindex_v2_reader.cpp:447-451 / 374-378) and the Stage E builder guard
    // (blockindex_generation_builder.cpp:398). Without this, replay produced the
    // genesis-shape value for a non-genesis root and poisoned every descendant.
    if (rec.hashPrev == uint256(0) && rec.hash != GetGenesisBlockHash())
    {
        out->stakeModifierChecksum = 0;
    }
    else
    {
    unsigned int parentChecksum = parentDerived.stakeModifierChecksum;
    CDataStream ss(SER_GETHASH, 0);
    if (rec.hashPrev != uint256(0)) ss << parentChecksum;
    uint256 proof = (rec.nFlags & CBlockIndex::BLOCK_PROOF_OF_STAKE) ? rec.hashProof : uint256(0);
    ss << rec.nFlags << proof << rec.nStakeModifier;
    uint256 hc = Hash(ss.begin(), ss.end());
    hc >>= (256 - 32);
    out->stakeModifierChecksum = hc.Get64();
    }

    // memo
    out->SetHasStakeModifierTime(false);
    out->stakeModifierTime = 0;
    if (rec.nFlags & CBlockIndex::BLOCK_STAKE_MODIFIER)
    {
        out->SetHasStakeModifierTime(true);
        out->stakeModifierTime = (int64_t)rec.nTime;
    }
    else if (rec.hashPrev != uint256(0) && parentDerived.HasStakeModifierTime())
    {
        out->SetHasStakeModifierTime(true);
        out->stakeModifierTime = parentDerived.stakeModifierTime;
    }

    // nSize (from blk files when available; else unavailable -> 0)
    out->SetHasBlockSize(false);
    out->nSize = 0;
    if (!blockDataDir.empty() && rec.nFile > 0)
    {
        std::string blockFn = strprintf("blk%04u.dat", rec.nFile);
        fs::path blockPath = fs::path(blockDataDir) / blockFn;
        FILE* blockFile = fopen(blockPath.string().c_str(), "rb");
        if (blockFile)
        {
            if (fseeko(blockFile, (off_t)rec.nBlockPos, SEEK_SET) == 0)
            {
                try {
                    CBlock block;
                    CAutoFile filein(blockFile, SER_DISK, CLIENT_VERSION);
                    filein >> block;
                    if (block.GetHash() == rec.hash)
                    {
                        out->nSize = ::GetSerializeSize(block, SER_NETWORK, PROTOCOL_VERSION);
                        out->SetHasBlockSize(out->nSize > 0);
                    }
                } catch (...) { /* CAutoFile closed the FILE* */ }
            }
            else
                fclose(blockFile); // CAutoFile never constructed
        }
    }
}

namespace {

// The local error contract: every failure path must leave a non-empty reason.
inline void SetErr(std::string* error, const std::string& msg)
{
    if (error) *error = msg;
}

// Prefix an already-set error message, keeping it non-empty.
inline void WrapErr(std::string* error, const std::string& prefix)
{
    if (error)
        *error = prefix + (error->empty() ? std::string("(no detail)") : *error);
}

} // namespace

// ---------------------------------------------------------------------------
// ACTIVE-chain driver: seed parent -> committed active tip, streamed by
// height. Bounded state: one record + one derived entry in flight.
//
// RecordIds are NOT height-ordered (they are hash-sorted at build time), so
// topology is proven per step by height and hashPrev linkage only.
// ---------------------------------------------------------------------------
BlockIndexDerivedReplayStatus ReplayDerivedActiveChain(
    const FixedBlockIndexStore& store,
    const BlockIndexActiveIndex& active,
    const FixedBlockIndexManifest& manifest,
    const std::string& blockDataDir,
    BlockIndexId parentId,
    const BlockIndexDerivedEntry& seedDerived,
    std::map<BlockIndexId, BlockIndexDerivedEntry>* outEntries,
    uint64_t* outReplayed,
    std::string* error)
{
    if (!outEntries)
    {
        SetErr(error, "derived replay: null output map");
        return BLOCK_INDEX_DERIVED_REPLAY_INTERNAL;
    }
    if (outReplayed) *outReplayed = 0;

    // Seed backend: an existing record id, or the synthetic null-backend
    // parentId==0 (genesis-tail replay; its "parent" is the nonexistent height
    // -1 and a default derived entry — the ancestor universe is empty).
    BlockIndexRecord parentRec;
    if (parentId != BLOCK_INDEX_ID_INVALID)
    {
        if (!store.Read(parentId, &parentRec, error))
        {
            // An unreadable RECORD (authoritative input) can never be rebuilt.
            WrapErr(error, "derived replay: active seed record unreadable: ");
            return BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
        }
    }
    else
    {
        parentRec = BlockIndexRecord();
        parentRec.height = -1; // synthetic: no authoritative parent record
        parentRec.hash = uint256(0);
    }
    if (parentRec.height > manifest.committedTipHeight)
    {
        if (error) error->clear();
        return BLOCK_INDEX_DERIVED_REPLAY_OK; // nothing above the seed backend
    }

    BlockIndexDerivedEntry parentEntry = seedDerived;
    uint64_t replayed = 0;

    for (int h = parentRec.height + 1; h <= manifest.committedTipHeight; ++h)
    {
        BlockIndexId id = BLOCK_INDEX_ID_INVALID;
        if (!active.ReadEntry(h, &id, error))
            return BLOCK_INDEX_DERIVED_REPLAY_IO_ERROR;
        if (id == BLOCK_INDEX_ID_INVALID || id > manifest.recordCount)
        {
            SetErr(error, "derived replay: active entry invalid/beyond committed count at height "
                          + std::to_string(h));
            return BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
        }

        BlockIndexRecord rec;
        if (!store.Read(id, &rec, error))
        {
            WrapErr(error, "derived replay: active record unreadable at height "
                           + std::to_string(h) + ": ");
            return BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
        }
        if (rec.height != h)
        {
            SetErr(error, "derived replay: active height/record mismatch at height "
                          + std::to_string(h));
            return BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
        }
        // Exact predecessor linkage at EVERY step (including h==0, where the
        // required hashPrev is the null hash of the synthetic backend).
        if (rec.hashPrev != parentRec.hash)
        {
            SetErr(error, "derived replay: active predecessor linkage mismatch at height "
                          + std::to_string(h));
            return BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
        }

        BlockIndexDerivedEntry d;
        ComputeDerivedFromParent(rec, parentEntry, blockDataDir, &d);

        (*outEntries)[id] = d;
        parentEntry = d;
        parentRec = rec;
        ++replayed;
    }

    if (outReplayed) *outReplayed = replayed;
    if (error) error->clear();
    return BLOCK_INDEX_DERIVED_REPLAY_OK;
}

// ---------------------------------------------------------------------------
// SIDE-branch closure driver: deterministic DFS over unresolved records.
//
// Structural plane for the call (deleted on return; O(records) resident but
// by-value ids only — no CBlockIndex, no mapBlockIndex):
//   hashToId     hash -> RecordId      (one bounded streaming pass)
//   deriveAnchor RecordId -> parent id (0 = parentless branch root)
// ---------------------------------------------------------------------------
BlockIndexDerivedReplayStatus ReplayDerivedSideClosure(
    const FixedBlockIndexStore& store,
    const FixedBlockIndexManifest& manifest,
    const std::string& blockDataDir,
    std::map<BlockIndexId, BlockIndexDerivedEntry>* outEntries,
    uint64_t* outReplayed,
    std::string* error)
{
    if (!outEntries)
    {
        SetErr(error, "derived replay: null output map");
        return BLOCK_INDEX_DERIVED_REPLAY_INTERNAL;
    }
    if (outReplayed) *outReplayed = 0;

    // One streaming pass: hash -> RecordId. A duplicate block hash inside one
    // authoritative generation is topology corruption -> fail closed.
    std::map<uint256, BlockIndexId> hashToId;
    for (BlockIndexId rid = 1; rid <= manifest.recordCount; ++rid)
    {
        BlockIndexRecord r;
        if (!store.Read(rid, &r, error))
        {
            WrapErr(error, "derived replay: closure record unreadable (RecordId "
                           + std::to_string((unsigned long long)rid) + "): ");
            return BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
        }
        if (!hashToId.insert(std::make_pair(r.hash, rid)).second)
        {
            SetErr(error, "derived replay: duplicate record hash in authoritative store");
            return BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
        }
    }

    // Structural ancestry cache (RecordId -> parent RecordId, 0 = parentless).
    std::map<BlockIndexId, BlockIndexId> deriveAnchor;
    uint64_t replayed = 0;

    for (BlockIndexId id = 1; id <= manifest.recordCount; ++id)
    {
        if (outEntries->count(id))
            continue; // already derived (active pass or an earlier DFS)

        // --- DFS to the nearest derivation anchor --------------------------
        std::vector<BlockIndexId> stack; // stack[i+1] IS the parent of stack[i]
        BlockIndexId cur = id;
        bool anchorIsDerived = false; // `cur` already carries an entry

        for (;;)
        {
            if (outEntries->count(cur))
            {
                anchorIsDerived = true;
                break;
            }
            if (deriveAnchor.count(cur))
            {
                // Registered within THIS call but not yet derived is an
                // internal invariant breach (a successful DFS derives every
                // record it registered; a failed one publishes nothing).
                SetErr(error, "derived replay: registered ancestry without derived entry");
                return BLOCK_INDEX_DERIVED_REPLAY_INTERNAL;
            }

            BlockIndexRecord rec;
            if (!store.Read(cur, &rec, error))
            {
                WrapErr(error, "derived replay: closure record unreadable (RecordId "
                               + std::to_string((unsigned long long)cur) + "): ");
                return BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
            }
            if (stack.size() > (size_t)manifest.recordCount + 1)
            {
                SetErr(error, "derived replay: ancestry walk exceeded record count (cycle)");
                return BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
            }
            stack.push_back(cur);

            if (rec.hashPrev == uint256(0))
            {
                // True parentless branch root: derives from a default parent
                // entry (the exact genesis/branch-start shape).
                deriveAnchor[cur] = 0;
                break;
            }
            if (rec.hashPrev == rec.hash)
            {
                SetErr(error, "derived replay: self-referential record hashPrev");
                return BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
            }
            std::map<uint256, BlockIndexId>::const_iterator hit = hashToId.find(rec.hashPrev);
            if (hit == hashToId.end())
            {
                // Disconnected authoritative topology: a claimed parent whose
                // record does not exist. Same fail-closed contract the builder
                // (A.10.1b-fix3) enforces — never manufacture trust from a hole.
                SetErr(error, "derived replay: claimed parent "
                              + rec.hashPrev.GetHex() + " of "
                              + rec.hash.GetHex() + " absent from authoritative store");
                return BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
            }
            deriveAnchor[cur] = hit->second;
            cur = hit->second;
        }

        // --- Derive down the stack (deepest first) --------------------------
        BlockIndexDerivedEntry parent;
        if (anchorIsDerived)
        {
            std::map<BlockIndexId, BlockIndexDerivedEntry>::const_iterator fit = outEntries->find(cur);
            if (fit == outEntries->end())
            {
                SetErr(error, "derived replay: anchor entry missing (internal)");
                return BLOCK_INDEX_DERIVED_REPLAY_INTERNAL;
            }
            parent = fit->second;
        }
        else
        {
            // The DFS above can only terminate here on a branch root, so its
            // anchor is the parentless default.
            const BlockIndexId anchorId = deriveAnchor[stack.back()];
            if (anchorId != 0)
            {
                SetErr(error, "derived replay: non-root closure anchor (internal)");
                return BLOCK_INDEX_DERIVED_REPLAY_INTERNAL;
            }
            parent = BlockIndexDerivedEntry();
        }

        for (size_t i = stack.size(); i-- > 0; )
        {
            BlockIndexRecord rec;
            if (!store.Read(stack[i], &rec, error))
            {
                WrapErr(error, "derived replay: closure record unreadable (RecordId "
                               + std::to_string((unsigned long long)stack[i]) + "): ");
                return BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
            }
            BlockIndexDerivedEntry d;
            ComputeDerivedFromParent(rec, parent, blockDataDir, &d);
            (*outEntries)[stack[i]] = d;
            parent = d;
            ++replayed;
        }
    }

    if (outReplayed) *outReplayed = replayed;
    if (error) error->clear();
    return BLOCK_INDEX_DERIVED_REPLAY_OK;
}

// ---------------------------------------------------------------------------
// Reader rebuild composition (called ONLY from BlockIndexV2Reader::Open on a
// PRESENT-BUT-CORRUPT derived companion, and DIRECTLY by the R3G tests).
// ---------------------------------------------------------------------------
bool RebuildAllDerivedFromAuthoritative(
    const FixedBlockIndexStore& store,
    const BlockIndexActiveIndex& active,
    const FixedBlockIndexManifest& manifest,
    const std::string& blockDataDir,
    std::map<BlockIndexId, BlockIndexDerivedEntry>* outEntries,
    BlockIndexDerivedReplayStatus* status,
    std::string* error)
{
    if (!outEntries)
    {
        if (status) *status = BLOCK_INDEX_DERIVED_REPLAY_INTERNAL;
        SetErr(error, "derived rebuild: null output map");
        return false;
    }
    outEntries->clear();
    if (error) error->clear();
    // R3G (PM1-P0-05): one-shot interruption injection (reuses the existing
    // test-only durability failpoint primitive; no new framework). An
    // immutable-generation rebuild is in-memory -- it performs no FileCommit/
    // Rename/SyncDirectory -- so its only interruptible I/O boundary is the
    // replay itself. A consumed failpoint makes the rebuild fail CLOSED
    // (nothing partial is served); disarming yields the clean deterministic
    // retry with exact parity.
    if (DurabilityFailpointConsumeForTesting("DERIVED_REBUILD"))
    {
        SetErr(error, "derived rebuild interrupted by injected failpoint");
        if (status) *status = BLOCK_INDEX_DERIVED_REPLAY_INTERNAL;
        return false;
    }

    const bool hasActive =
        manifest.committedTipHeight >= 0 && manifest.recordCount > 0;
    uint64_t replayed = 0;

    // --- Pass 1: the authoritative active chain -----------------------------
    if (hasActive)
    {
        // Authoritative seed for the first post-seed record: the SYNTHETIC
        // NULL BACKEND (height -1, null hash, default derived entry) — the
        // parentless shape the authoritative genesis record itself proves
        // (hashPrev == 0). No chain-trust is inherited from any persisted
        // source: the first active record's own parentless semantics derive
        // exactly what the builder derived at genesis.
        static const BlockIndexDerivedEntry nullSeed = BlockIndexDerivedEntry();
        const BlockIndexDerivedReplayStatus st = ReplayDerivedActiveChain(
            store, active, manifest, blockDataDir,
            /*parentId=*/BLOCK_INDEX_ID_INVALID, nullSeed,
            outEntries, &replayed, error);
        if (st != BLOCK_INDEX_DERIVED_REPLAY_OK)
        {
            if (status) *status = st;
            SetErr(error, std::string("derived rebuild: active replay failed: ")
                          + (error && !error->empty() ? *error : "(no detail)"));
            return false;
        }
    }

    // --- Pass 2: full side-branch closure ------------------------------------
    {
        BlockIndexDerivedReplayStatus st = ReplayDerivedSideClosure(
            store, manifest, blockDataDir, outEntries, &replayed, error);
        if (st != BLOCK_INDEX_DERIVED_REPLAY_OK)
        {
            if (status) *status = st;
            SetErr(error, std::string("derived rebuild: side closure failed: ")
                          + (error && !error->empty() ? *error : "(no detail)"));
            return false;
        }
    }

    // Coverage proof: the rebuilt map must cover EVERY committed record.
    if (outEntries->size() != (size_t)manifest.recordCount)
    {
        if (status) *status = BLOCK_INDEX_DERIVED_REPLAY_CORRUPT;
        SetErr(error, "derived rebuild: rebuilt coverage below committed record count");
        return false;
    }
    if (status) *status = BLOCK_INDEX_DERIVED_REPLAY_OK;
    return true;
}
