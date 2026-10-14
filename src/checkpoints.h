// Copyright (c) 2009-2012 The Bitcoin developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.
#ifndef BITCOIN_CHECKPOINT_H
#define  BITCOIN_CHECKPOINT_H

#include <map>
#include "net.h"
#include "util.h"
#include "txdb.h"   // SyncCheckpointLoadStatus (V2-R2B FIX A typed seam)

#ifdef WIN32
#undef STRICT
#undef PERMISSIVE
#undef ADVISORY
#endif

class uint256;
class CBlockIndex;
class CSyncCheckpoint;

/** Block-chain checkpoints are compiled-in sanity checks.
 * They are updated every release or three.
 */
namespace Checkpoints
{
    typedef std::map<int, uint256> MapCheckpoints;
    /** Checkpointing mode */
    enum CPMode
    {
        // Scrict checkpoints policy, perform conflicts verification and resolve conflicts
        STRICT = 0,
        // Advisory checkpoints policy, perform conflicts verification but don't try to resolve them
        ADVISORY = 1,
        // Permissive checkpoints policy, don't perform any checking
        PERMISSIVE = 2
    };

    // Returns true if block passes checkpoint checks
    bool CheckHardened(int nHeight, const uint256& hash);

    // Return conservative estimate of total number of blocks, 0 if unknown
    int GetTotalBlocksEstimate();

    // Returns last CBlockIndex* in mapBlockIndex that is a checkpoint
    CBlockIndex* GetLastCheckpoint(const std::map<uint256, CBlockIndex*>& mapBlockIndex);

    extern uint256 hashSyncCheckpoint;
    extern uint256 hashPendingCheckpoint;
    extern CSyncCheckpoint checkpointMessage;
    extern uint256 hashInvalidCheckpoint;
    extern CCriticalSection cs_hashSyncCheckpoint;

    extern MapCheckpoints mapCheckpoints;
    extern MapCheckpoints mapCheckpointsTestnet;

    CBlockIndex* GetLastSyncCheckpoint();
    bool WriteSyncCheckpoint(const uint256& hashCheckpoint);
    bool AcceptPendingSyncCheckpoint();

    // V2-R2B (FIX A): at V2 authoritative startup the persisted synchronized
    // checkpoint (the value the legacy CTxDB::LoadBlockIndex read from key
    // "hashSyncCheckpoint") must be restored into hashSyncCheckpoint. Returns
    // SYNC_CHECKPOINT_LOADED / SYNC_CHECKPOINT_ABSENT / SYNC_CHECKPOINT_CORRUPT.
    // ABSENT leaves hashOut == 0, the canonical ppcoin "no synchronized
    // checkpoint" value -- NEVER a genesis substitution. CORRUPT is a durable
    // authority inconsistency and must fail closed at startup.
    SyncCheckpointLoadStatus LoadPersistedSyncCheckpoint(uint256& hashOut, std::string* error);

    // ---------------------------------------------------------------------------
    // V2-R3 (ROUND-2 P0): establishment result of the one-time chain-CREATION
    // genesis synchronized checkpoint (LEGACY PARITY, NOT an unconditional
    // fallback). Only CREATED performs a durable write.
    // ---------------------------------------------------------------------------
    enum SyncCheckpointEstablishmentResult
    {
        SYNC_CHECKPOINT_ESTABLISHMENT_CREATED = 0,           // fresh chain: genesis checkpoint persisted
        SYNC_CHECKPOINT_ESTABLISHMENT_NOT_APPLICABLE,         // persisted checkpoint exists / out of scope
        SYNC_CHECKPOINT_ESTABLISHMENT_RESIDUAL_ABSENT_ON_NONEMPTY, // loud-warning pass-through (documented)
        SYNC_CHECKPOINT_ESTABLISHMENT_REJECTED_PRECONDITION,  // caller precondition violated (caller must fail)
        SYNC_CHECKPOINT_ESTABLISHMENT_ERROR,                  // write failed (caller must fail startup)
    };

    // One-time genesis-checkpoint establishment for a GENUINELY NEW
    // authoritative chain. Preconditions (independently re-verified here):
    //   * `loadStatus` is the just-loaded persisted status and MUST be
    //     SYNC_CHECKPOINT_ABSENT — a real persisted checkpoint is NEVER
    //     overwritten (NOT_APPLICABLE otherwise);
    //   * the immutable base tip is at height 0 (authoritative mode);
    //   * the published active best IS the genesis anchor.
    // A persisted checkpoint + non-empty chain is the documented RESIDUAL
    // state -> RESIDUAL_ABSENT_ON_NONEMPTY (loud warning, canonical 0 kept,
    // no genesis substitution, no re-creation). On CREATED the in-process
    // Checkpoints::hashSyncCheckpoint is already published by WriteSyncCheckpoint.
    SyncCheckpointEstablishmentResult EstablishGenesisCheckpoint(
        const SyncCheckpointLoadStatus loadStatus,
        const uint256& hashGenesis,
        const int32_t baseTipHeight,
        const uint256& currentBestHash,
        std::string* error);

    bool CheckSync(const uint256& hashBlock, const CBlockIndex* pindexPrev);
    const CBlockIndex* AutoSelectSyncCheckpoint();

    // ---------------------------------------------------------------------
    // V2-R2B (FIX B): synchronized-checkpoint compatibility with
    // BY_VALUE_AUTHORITATIVE startup. mapBlockIndex holds only a bounded live
    // tail + anchors, so the legacy predicates below must resolve the
    // checkpoint and the candidate's ancestry by value instead of relying on
    // residency. Acceptance / conflict-rejection semantics are preserved
    // exactly; these are exposed for the deterministic compatibility tests.
    // ---------------------------------------------------------------------
    bool CheckSyncByValue(const uint256& hashBlock, const CBlockIndex* pindexPrev);

    // ROUND-6 TASK 3: tri-state result of by-value synchronized-checkpoint
    // validation. A TRANSIENT authority/storage failure is NOT a verdict on the
    // checkpoint: the pending-checkpoint consumer must RETAIN a known pending
    // checkpoint (and its message) and retry it, instead of clearing it as if it
    // were definitively unknown / invalid / conflicting / older.
    enum SyncCheckpointValidationOutcome
    {
        SYNC_CHECKPOINT_VALID = 0,
        SYNC_CHECKPOINT_DEFINITIVE_REJECT,
        SYNC_CHECKPOINT_TRANSIENT_FAILURE,
    };
    SyncCheckpointValidationOutcome ValidateSyncCheckpointDetailed(uint256 hashCheckpoint);
    bool ValidateSyncCheckpointByValue(uint256 hashCheckpoint);

    // Test-only memo invalidation seam (ROUND-4 P2; production never calls
    // it). Clears the per-tip extends-memo, the per-parent descendant-verdict
    // map and the per-epoch checkpoint resolution memo WITHOUT touching
    // hashSyncCheckpoint, so a test can replay the memo's RETRY path from a
    // known-EMPTY state. Defined in checkpoints.cpp.
    void ResetCheckpointMemoForTest(const uint256& tipHash, const uint256& epochCheckpoint);

    // ROUND-5 TASK C test seams (production never calls them): observe and
    // override the bounded per-epoch descendant-verdict cache.
    void SetCheckpointDescendantVerdictCapForTest(size_t cap);
    size_t GetCheckpointDescendantVerdictCapForTest();
    size_t DescendantVerdictCacheSizeForTest();

    // FIX B: by-value anti-spam work-gate anchors for the synced checkpoint
    // (the values ProcessBlock's weak-competitor gate previously read through a
    // resident CBlockIndex*). pnBits is the nBits of the last block of the
    // requested type at or below the checkpoint. Returns false when no
    // checkpoint is established or the anchors cannot be resolved by value.
    bool GetSyncCheckpointWorkGateByValue(bool fProofOfStake, int64_t* pnCheckpointTime,
                                           unsigned int* pnBits, int* pnHeight);

    // ROUND-6 TASKS 1/2: tri-state work-gate anchor search. The walk mirrors the
    // legacy GetLastBlockIndex semantics (stop on a proof-type match or at the
    // genesis) with NO arbitrary step cutoff, and reports a DEFINITIVE absence
    // distinctly from a TRANSIENT authority/storage failure, so ProcessBlock's
    // weak-competitor gate never silently disables the protection on uncertainty
    // and never anchors on a block of the WRONG proof type.
    enum SyncCheckpointWorkGateAnchor
    {
        SYNC_CHECKPOINT_WORKGATE_ANCHOR_VALID = 0,
        SYNC_CHECKPOINT_WORKGATE_ANCHOR_ABSENT,
        SYNC_CHECKPOINT_WORKGATE_ANCHOR_TRANSIENT,
    };
    SyncCheckpointWorkGateAnchor GetSyncCheckpointWorkGateAnchor(bool fProofOfStake, int64_t* pnCheckpointTime,
                                                                 unsigned int* pnBits, int* pnHeight,
                                                                 int* pnAnchorHeight = NULL);

    // ROUND-6 TASKS 1/2/3 test seams (production never calls them).
    //  - SetWorkGateAnchorTransientFailForTest: make the next `count` work-gate
    //    ANCHOR resolutions observe a TRANSIENT authority failure, exercising
    //    ProcessBlock's fail-closed work-gate branch deterministically without
    //    disturbing any other authoritative reader in the same process.
    //  - SetPendingCheckpointMessageForTest / PendingCheckpointMessageIsNullForTest:
    //    arm and observe the file-private pending checkpoint MESSAGE state so a
    //    regression can prove a transient validation failure preserves it.
    void SetWorkGateAnchorTransientFailForTest(int count);
    int WorkGateAnchorTransientFailCountForTest();
    // ROUND-7 seams: observational only (never consulted by production).
    int GetWorkGateAnchorResolveCountForTest();
    int64_t GetWorkGateAnchorWalkStepsForTest();
    void ResetWorkGateAnchorCountersForTest();
    size_t WorkGateAnchorMemoSizeForTest();
    void SetProcessSyncCheckpointSkipSignatureForTest(bool f);
    bool ProcessSyncCheckpointSkipSignatureForTest();
    void ClearWorkGateAnchorMemoForTest();
    //  - SetPendingValidationTransientFailForTest: force the next
    //    ValidateSyncCheckpointDetailed verdicts to TRANSIENT without touching
    //    any authoritative reader, so the pending-checkpoint CONSUMER path is
    //    exercised in isolation (the by-hash seam above would also break the
    //    fPendingKnown membership probe and short-circuit the consumer).
    void SetPendingValidationTransientFailForTest(int count);
    void SetPendingCheckpointMessageForTest(const uint256& hashCheckpoint);
    bool PendingCheckpointMessageIsNullForTest();
    uint256 PendingCheckpointMessageHashForTest();
    void ClearPendingCheckpointMessageForTest();

    bool WantedByPendingSyncCheckpoint(uint256 hashBlock);
    bool ResetSyncCheckpoint();
    void AskForPendingSyncCheckpoint(CNode* pfrom);
    bool SetCheckpointPrivKey(std::string strPrivKey);
    bool SendSyncCheckpoint(uint256 hashCheckpoint);
    bool IsMatureSyncCheckpoint();
}

// ppcoin: synchronized checkpoint
class CUnsignedSyncCheckpoint
{
public:
    int nVersion;
    uint256 hashCheckpoint;      // checkpoint block

    IMPLEMENT_SERIALIZE
    (
        READWRITE(this->nVersion);
        nVersion = this->nVersion;
        READWRITE(hashCheckpoint);
    )

    void SetNull()
    {
        nVersion = 1;
        hashCheckpoint = 0;
    }

    std::string ToString() const
    {
        return strprintf(
                "CSyncCheckpoint(\n"
                "    nVersion       = %d\n"
                "    hashCheckpoint = %s\n"
                ")\n",
            nVersion,
            hashCheckpoint.ToString().c_str());
    }

    void print() const
    {
        printf("%s", ToString().c_str());
    }
};

class CSyncCheckpoint : public CUnsignedSyncCheckpoint
{
public:
    static const std::string strMasterPubKey;
    static std::string strMasterPrivKey;

    std::vector<unsigned char> vchMsg;
    std::vector<unsigned char> vchSig;

    CSyncCheckpoint()
    {
        SetNull();
    }

    IMPLEMENT_SERIALIZE
    (
        READWRITE(vchMsg);
        READWRITE(vchSig);
    )

    void SetNull()
    {
        CUnsignedSyncCheckpoint::SetNull();
        vchMsg.clear();
        vchSig.clear();
    }

    bool IsNull() const
    {
        return (hashCheckpoint == 0);
    }

    uint256 GetHash() const
    {
        return SerializeHash(*this);
    }

    bool RelayTo(CNode* pnode) const
    {
        // returns true if wasn't already sent
        if (pnode->hashCheckpointKnown != hashCheckpoint)
        {
            pnode->hashCheckpointKnown = hashCheckpoint;
            pnode->PushMessage("checkpoint", *this);
            return true;
        }
        return false;
    }

    bool CheckSignature();
    bool ProcessSyncCheckpoint(CNode* pfrom);
    bool ProcessSyncCheckpointHeaders(CNode* pfrom);
};

#endif
