// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "dag.h"
#include "main.h"
#include "txdb.h"
#include "finality.h"
#include "blockindex_residency_counters.h"
#include "blockindex_authoritative_startup.h"
#include "blockindex_authoritative_live.h"
#include "util.h"
#include "dag_tips_delta.h"
#include "dag_mutation_preview.h"
#include "dag_tip_selector.h"

#include <algorithm>
#include <queue>

CDAGManager g_dagManager;


// ---------------------------------------------------------------------------
// DAG Parent Commitment: coinbase OP_RETURN encoding
// ---------------------------------------------------------------------------

std::vector<uint256> ExtractDAGParents(const CScript& scriptCoinbase)
{
    std::vector<uint256> vResult;

    // Walk coinbase outputs looking for OP_RETURN with IDAG tag
    // The script format: OP_RETURN <push: tag(4) || count(1) || hashes(32*count)>
    CScript::const_iterator pc = scriptCoinbase.begin();
    if (pc >= scriptCoinbase.end())
        return vResult;

    opcodetype opcode;
    std::vector<unsigned char> vchData;
    if (!scriptCoinbase.GetOp(pc, opcode, vchData))
        return vResult;

    if (opcode != OP_RETURN)
        return vResult;

    if (!scriptCoinbase.GetOp(pc, opcode, vchData))
        return vResult;

    // Verify IDAG tag prefix
    if (vchData.size() < 5) // 4 tag + 1 count minimum
        return vResult;

    if (memcmp(vchData.data(), DAG_PARENT_TAG, 4) != 0)
        return vResult;

    unsigned int nCount = vchData[4];
    if (nCount == 0 || nCount > MAX_DAG_PARENTS)
        return vResult;

    unsigned int nExpectedSize = 5 + nCount * 32;
    if (vchData.size() < nExpectedSize)
        return vResult;

    for (unsigned int i = 0; i < nCount; i++)
    {
        uint256 hash;
        memcpy(hash.begin(), &vchData[5 + i * 32], 32);
        vResult.push_back(hash);
    }

    return vResult;
}

CScript BuildDAGParentScript(const std::vector<uint256>& vParents)
{
    if (vParents.empty() || vParents.size() > MAX_DAG_PARENTS)
        return CScript();

    std::vector<unsigned char> vchData;
    vchData.reserve(5 + vParents.size() * 32);

    vchData.insert(vchData.end(), DAG_PARENT_TAG, DAG_PARENT_TAG + 4);
    vchData.push_back((unsigned char)vParents.size());
    for (const uint256& hash : vParents)
    {
        const unsigned char* p = hash.begin();
        vchData.insert(vchData.end(), p, p + 32);
    }

    CScript script;
    script << OP_RETURN << vchData;
    return script;
}


// ---------------------------------------------------------------------------
// CDAGManager: Initialization
// ---------------------------------------------------------------------------

void CDAGManager::AddChildNoDuplicate(std::vector<uint256>& vChildren, const uint256& hashChild) const
{
    if (std::find(vChildren.begin(), vChildren.end(), hashChild) == vChildren.end())
        vChildren.push_back(hashChild);
}

void CDAGManager::InvalidateBlueSetCacheForBlock(const uint256& hashBlock) const
{
    mapBlueSetCache.erase(hashBlock);
}

bool CDAGManager::TrackedInsertTip(const uint256& hash)
{
    const std::pair<std::set<uint256>::iterator, bool> r = setDAGTips.insert(hash);
    // Live shadow deltas come exclusively from canonical CTxDB mutations.
    return r.second;
}

bool CDAGManager::TrackedEraseTip(const uint256& hash)
{
    if (setDAGTips.erase(hash) == 0)
        return false;
    // Legacy membership is maintained, but is not source authority.
    return true;
}

void CDAGManager::RebuildPendingChildIndex()
{
    mapPendingChildrenByParent.clear();
    for (auto& pair : mapDAGData)
        pair.second.vDAGChildren.clear();

    for (auto& pair : mapDAGData)
    {
        for (const uint256& hashParent : pair.second.vDAGParents)
        {
            auto pit = mapDAGData.find(hashParent);
            if (pit != mapDAGData.end())
                AddChildNoDuplicate(pit->second.vDAGChildren, pair.first);
            else
                mapPendingChildrenByParent[hashParent].insert(pair.first);
        }
    }

    setDAGTips.clear();
    for (const auto& pair : mapDAGData)
    {
        if (pair.second.vDAGChildren.empty())
            setDAGTips.insert(pair.first);
    }
}

bool CDAGManager::InitBlockDAGData(CBlockIndex* pindex, const std::vector<uint256>& vParents,
                                   const DagMutationPreview* mutationPreview)
{
    LOCK(cs_dag);

    if (!pindex || !pindex->phashBlock)
        return false;
    if (pindex->nHeight >= FORK_HEIGHT_DAG && pindex->IsProofOfStake())
        return false;

    uint256 hash = pindex->GetBlockHash();

    CBlockDAGData& data = mapDAGData[hash];
    data.vDAGParents = vParents;
    data.fBlue = true; // default, recolored by ColorBlock/ColorBlockDAGKnight
    data.nDAGScore = 0;
    data.nDAGOrder = -1;
    data.nInferredK = -1;

    InvalidateBlueSetCacheForBlock(hash);

    // Register as child of each parent
    for (const uint256& hashParent : vParents)
    {
        auto pit = mapDAGData.find(hashParent);
        if (pit != mapDAGData.end())
        {
            AddChildNoDuplicate(pit->second.vDAGChildren, hash);
            InvalidateBlueSetCacheForBlock(hashParent);
        }
        else
        {
            mapPendingChildrenByParent[hashParent].insert(hash);
        }
    }

    // Attach children that arrived earlier while this parent was missing.
    auto pendingIt = mapPendingChildrenByParent.find(hash);
    if (pendingIt != mapPendingChildrenByParent.end())
    {
        for (const uint256& hashChild : pendingIt->second)
        {
            AddChildNoDuplicate(data.vDAGChildren, hashChild);
            InvalidateBlueSetCacheForBlock(hashChild);
        }
        mapPendingChildrenByParent.erase(pendingIt);
    }

    // Update DAG tips: this block is a tip only if no earlier child referenced it.
    if (data.vDAGChildren.empty())
        TrackedInsertTip(hash);
    else
        TrackedEraseTip(hash);
    for (const uint256& hashParent : vParents)
        TrackedEraseTip(hashParent);

    static int64_t nLastRebuildTime = 0;
    if ((int)setDAGTips.size() > 64)
    {
        int64_t nNow = GetTimeMillis();
        if (nNow - nLastRebuildTime > 60000)
        {
            nLastRebuildTime = nNow;
            printf("InitBlockDAGData: tip flood detected (%d tips), triggering incremental rebuild\n",
                   (int)setDAGTips.size());
            // R2c.2s/S5: mutation-internal synchronous consumer — the explicit
            // transaction-scoped preview is threaded through; validation
            // failure fails the rebuild closed (the journal is latched).
            if (!RebuildDAGOrderIncremental(nPrunedBelowHeight, mutationPreview))
                return false;
        }
    }

    return true;
}


// ---------------------------------------------------------------------------
// CDAGManager: Tips and Best Tip Selection
// ---------------------------------------------------------------------------

std::vector<uint256> CDAGManager::GetDAGTips() const
{
    LOCK(cs_dag);
    return std::vector<uint256>(setDAGTips.begin(), setDAGTips.end());
}

CBlockIndex* CDAGManager::SelectBestDAGTip() const
{
    LOCK(cs_dag);

    CBlockIndex* pBest = NULL;
    uint256 nBestScore = 0;

    for (const uint256& hashTip : setDAGTips)
    {
        auto it = mapDAGData.find(hashTip);
        if (it == mapDAGData.end())
            continue;

        std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashTip);
        if (mi == mapBlockIndex.end())
            continue;

        CBlockIndex* pindex = mi->second;
        if (pindex->nHeight >= FORK_HEIGHT_DAG && pindex->IsProofOfStake())
            continue;

        if (pindexBest)
        {
            if (pindex->nHeight > pindexBest->nHeight)
                continue;
            const CBlockIndex* pWalk = pindexBest;
            while (pWalk && pWalk->nHeight > pindex->nHeight)
                pWalk = pWalk->pprev;
            if (pWalk != pindex)
                continue;
        }

        if (it->second.nDAGScore > nBestScore ||
            (it->second.nDAGScore == nBestScore && (!pBest || hashTip < pBest->GetBlockHash())))
        {
            nBestScore = it->second.nDAGScore;
            pBest = pindex;
        }
    }

    if (!pBest && pindexBest)
        pBest = pindexBest;

    return pBest;
}


// ---------------------------------------------------------------------------
// CDAGManager: GHOSTDAG Blue-Set Coloring (pre-DAGKNIGHT)
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// CDAGManager: F2 authoritative DAG parent-score resolution
// ---------------------------------------------------------------------------
// DAG PARENT SCORE TRUTH != mapDAGData/mapBlockIndex RESIDENCY. The two
// resolvers below are the ONE logical parent-score rule used by the coloring
// entry points; they differ only in which source is authoritative.

CDAGManager::DAGParentScoreResult
CDAGManager::ResolveDagParentScore(const uint256& hashParent,
                                   bool fAuthoritativeParentScore,
                                   std::string* error,
                                   DAGParentScorePolicy policy) const
{
    if (error) error->clear();
    if (!fAuthoritativeParentScore)
        return ResolveParentScoreLegacy(hashParent);
    return ResolveParentScoreAuthoritative(hashParent, error, policy);
}

// Legacy resident rule, preserved byte-for-byte from the historical loop body.
CDAGManager::DAGParentScoreResult
CDAGManager::ResolveParentScoreLegacy(const uint256& hashParent) const
{
    DAGParentScoreResult r;
    auto pit = mapDAGData.find(hashParent);
    if (pit != mapDAGData.end())
    {
        r.status = DAGParentScoreStatus::FOUND;
        r.score = pit->second.nDAGScore;
        return r;
    }
    // Pre-DAG parent: use accumulated chain trust as base score
    std::map<uint256, CBlockIndex*>& idx = RecolorBlockIndex();
    std::map<uint256, CBlockIndex*>::iterator mi = idx.find(hashParent);
    if (mi != idx.end() &&
        !(mi->second->nHeight >= FORK_HEIGHT_DAG && mi->second->IsProofOfStake()))
    {
        r.status = DAGParentScoreStatus::FOUND;
        r.score = mi->second->nChainTrust;
        return r;
    }
    // Legacy miss: the caller ranks this parent at score 0 (unchanged).
    r.status = DAGParentScoreStatus::NOT_FOUND;
    r.score = 0;
    return r;
}

// B-1 provenance predicate for a post-DAG parent whose canonical row is absent.
//
// The ACCEPTED prune/erase lifecycle (CDAGManager::PruneDAGData) erases exactly
// the persisted vertices whose height is STRICTLY BELOW the prune line
// (`snap.height < nPruneBelow`) and persists that same line as the durable DAG
// PRUNE FLOOR (WriteDAGPruneFloor(nPruneBelow), in the SAME atomic commit).
// That marker has exactly ONE writer - the erase lifecycle - so a vertex below
// it is legitimately row-absent by the lifecycle's own contract, while any
// absence at or above it is unexplained loss.
//
// The predicate deliberately does NOT read `dagcleanheight`: that key is ALSO
// written by Shutdown() with the CURRENT TIP and erases nothing (and restored by
// the prune rollback), so "height < ReadDAGCleanHeight()" proves only that the
// vertex is old enough, not that it was erased. Reading it let a single clean
// shutdown turn arbitrary row loss anywhere below the tip into FOUND /
// PRUNED_BOUNDARY.
//
// Deliberately resident-free and value-only: no mapBlockIndex, no mapDAGData,
// no resident pointer. Returns false (=> the caller fails closed) whenever the
// erase marker is not certified or the requested vertex is not a known DAG-era
// block.
// R3 / C6 sections 1+3+5+6 — THE FINAL POSITIVE PRUNE PREDICATE.
//
// ROW_OBJECTIVELY_PRUNED(X) is admitted ONLY when the complete frozen positive
// predicate verifies. Deliberately resident-free and value-only: no mapBlockIndex,
// no mapDAGData, no resident pointer.
//
//   * a bare absence never proves a prune
//   * a floor alone never proves a prune (floor CORROBORATES only)
//   * an old erase marker never proves a prune (the legacy marker convention is
//     retired as evidence; the historical records are neither read nor rewritten)
//
// Every failure is ROW_MISSING_UNEXPLAINED (fail closed) — never "present", never
// "empty", never "zero state".
static bool DAGRowObjectivelyPruned(CTxDB& db, const BlockIndexSnapshot& snap,
                                   const uint256& hash, std::string* why)
{
    if (!snap.found || snap.hash != hash)
    {
        if (why) *why = "not a known authoritative vertex";
        return false;
    }
    if (snap.height < FORK_HEIGHT_DAG)
    {
        if (why) *why = "pre-DAG height: owned by the pre-DAG provider";
        return false;
    }
    // (0) The row must genuinely be ABSENT: this predicate attributes an absence and
    //     must never be reachable for a present row. A present row is FOUND (not a
    //     pruned boundary); a malformed row is CORRUPT and a storage failure is a
    //     STORAGE error, and neither may be laundered into prune provenance.
    {
        CBlockDAGData row;
        DAGRowTypedOutcome outcome = DAGRowTypedOutcome::ROW_PRESENT_VALID;
        std::string rowDetail;
        if (!db.ReadDAGLinksTyped(hash, &row, &outcome, &rowDetail))
        {
            if (why) *why = "typed DAG row read returned no classification";
            return false;
        }
        if (outcome != DAGRowTypedOutcome::ROW_MISSING_UNEXPLAINED)
        {
            if (why) *why = outcome == DAGRowTypedOutcome::ROW_PRESENT_VALID
                ? "row is present: a present row is not a pruned boundary"
                : (outcome == DAGRowTypedOutcome::ROW_CORRUPT
                    ? "malformed row: a corrupt row is never treated as a pruned absence"
                    : "row store failure: a storage/iterator failure is never an absence");
            return false;
        }
    }
    // (1) Custody continuity. An unsupported/foreign writer invalidates trusted custody
    //     even when the final application-visible rows look identical.
    if (db.GetDAGCustodyState() != DAGCustodyState::VERIFIED)
    {
        if (why) *why = db.GetDAGCustodyState() == DAGCustodyState::SUSPENDED
            ? "custody suspended (watermark continuity lost): prior prune evidence inadmissible"
            : "cross-session custody unavailable: coverage does not survive";
        return false;
    }
    // (2) Coverage certificate: positively certified domain, bound to this store,
    //     this custody epoch and the current journal/counter state.
    DAGCertVerifyResult certRes = DAGCertVerifyResult::NO_CERTIFICATE;
    std::string certErr;
    if (!db.VerifyDAGProvenanceCoverageShallow(&certRes, &certErr))
    {
        if (why) *why = "coverage certificate unreadable: " + certErr;
        return false;
    }
    if (certRes != DAGCertVerifyResult::OK)
    {
        if (why) *why = "coverage certificate does not verify (result=" +
                        std::to_string((int)certRes) + ")" + (certErr.empty() ? std::string() : ": " + certErr);
        return false;
    }
    DAGProvenanceCertificate cert;
    bool haveCert = false;
    if (!db.ReadDAGProvenanceCertificate(&cert, &haveCert) || !haveCert || cert.epoch == 0)
    {
        if (why) *why = "coverage certificate missing or carries no custody epoch";
        return false;
    }
    if (!db.IsDAGProvenanceDeepVerified())
    {
        if (why) *why = "certified domain not positively proven in this session (no certification scan)";
        return false;
    }
    // (3) Inside the certified domain.
    if (snap.height >= cert.hCert)
    {
        if (why) *why = "height outside the certified provenance domain [FORK_HEIGHT_DAG, " +
                        std::to_string(cert.hCert) + ")";
        return false;
    }
    // (4) Current durable incarnation for X.
    uint64_t incarnation = 0;
    bool haveIncarnation = false;
    if (!db.ReadDAGRowIncarnation(hash, &incarnation, &haveIncarnation))
    {
        if (why) *why = "row incarnation unreadable";
        return false;
    }
    if (!haveIncarnation || incarnation == 0)
    {
        if (why) *why = "no durable incarnation for X: no positive attribution is possible";
        return false;
    }
    // (5) The per-row index must bind the CURRENT incarnation, not some earlier one.
    uint64_t latestEvent = 0, latestIncarnation = 0;
    bool haveLatest = false;
    std::vector<uint64_t> rowEvents;
    if (!db.ReadDAGPruneLatest(hash, &latestEvent, &latestIncarnation, &rowEvents, &haveLatest))
    {
        if (why) *why = "per-row prune index unreadable";
        return false;
    }
    if (!haveLatest || latestEvent == 0 || latestIncarnation != incarnation)
    {
        if (why) *why = "no admissible prune evidence bound to the current incarnation " +
                        std::to_string(incarnation);
        return false;
    }
    // (6) The referenced event must exist, bind (X, N) and still be admissible.
    DAGPruneEvent ev;
    bool haveEvent = false;
    if (!db.ReadDAGPruneEvent(latestEvent, &ev, &haveEvent))
    {
        if (why) *why = "prune event unreadable";
        return false;
    }
    if (!haveEvent)
    {
        if (why) *why = "referenced prune event missing from the journal";
        return false;
    }
    if (ev.hash != hash || ev.incarnation != incarnation)
    {
        if (why) *why = "prune event does not bind (X, N) for the current incarnation";
        return false;
    }
    if (ev.superseded_by != 0)
    {
        if (why) *why = "prune event was superseded by incarnation " + std::to_string(ev.superseded_by);
        return false;
    }
    if (ev.epoch == 0 || ev.epoch != cert.epoch)
    {
        if (why) *why = "prune event epoch is not the certified custody epoch (event=" +
                        std::to_string(ev.epoch) + " cert=" + std::to_string(cert.epoch) + ")";
        return false;
    }
    // (7) Journal integrity: the certified head/length must still hold and must contain E.
    uint64_t head = 0, length = 0;
    uint256 headHash;
    if (!db.ReadDAGPruneJournal(&head, &length, &headHash))
    {
        if (why) *why = "prune journal unreadable";
        return false;
    }
    if (length != cert.journalLength || headHash != cert.journalHeadHash)
    {
        if (why) *why = "prune journal is not the certified journal (head/length mismatch)";
        return false;
    }
    if (latestEvent > head)
    {
        if (why) *why = "prune event id is beyond the journal head";
        return false;
    }
    // (8) Height corroboration: the event binds height(X) exactly.
    if (ev.height != snap.height)
    {
        if (why) *why = "prune event height does not match the authoritative height of X";
        return false;
    }
    // (9) Floor / clean-height corroboration (corroboration ONLY: it establishes nothing).
    int32_t floorValue = 0;
    int32_t cleanHeight = 0;
    const bool haveFloor = db.ReadDAGPruneFloor(floorValue);
    const bool haveClean = db.ReadDAGCleanHeight(cleanHeight);
    if (!haveFloor || floorValue <= 0)
    {
        if (why) *why = "no prune floor: the absence cannot be corroborated";
        return false;
    }
    if (ev.floor_after <= 0 || floorValue < ev.floor_after)
    {
        if (why) *why = "prune floor is below the event's post-prune floor (floor must not regress)";
        return false;
    }
    if (!(snap.height < ev.floor_after))
    {
        if (why) *why = "X is not below the event's post-prune floor";
        return false;
    }
    if (haveClean && cleanHeight < ev.height)
    {
        if (why) *why = "clean height is below the event height";
        return false;
    }
    std::string deepErr;
    DAGCertVerifyResult deepRes = DAGCertVerifyResult::NO_CERTIFICATE;
    if (!db.VerifyDAGProvenanceCoverage(&deepRes, &deepErr) || deepRes != DAGCertVerifyResult::OK)
    {
        if (why) *why = "coverage certification scan does not reproduce the certified digests (result=" +
                        std::to_string((int)deepRes) + ")" + (deepErr.empty() ? std::string() : ": " + deepErr);
        return false;
    }
    return true;
}

// R3 / C6 section 6 — test seam. Wraps the SAME static predicate used at the consensus
// site with a value-only snapshot so the frozen predicate (not a re-implementation) can
// be exercised directly against an isolated store.
bool DAGRowObjectivelyPrunedForTest(CTxDB& db, const uint256& hash, int32_t height, std::string* why)
{
    BlockIndexSnapshot snap;
    snap.found = true;
    snap.hash = hash;
    snap.height = height;
    return DAGRowObjectivelyPruned(db, snap, hash, why);
}

// Authoritative live rule. Score TRUTH comes from the authority, never from
// mapDAGData/mapBlockIndex residency, and score 0 is never substituted on
// failure.
CDAGManager::DAGParentScoreResult
CDAGManager::ResolveParentScoreAuthoritative(const uint256& hashParent,
                                             std::string* error,
                                             DAGParentScorePolicy policy) const
{
    DAGParentScoreResult r;

    BlockIndexAuthoritativeLive* live = GetAuthoritativeLiveAuthority();
    if (!live || !live->IsOpen())
    {
        r.status = DAGParentScoreStatus::FAILURE;
        if (error) *error = "DAG parent score: authoritative live authority unavailable";
        return r;
    }
    BlockIndexSnapshot snap;
    std::string e;
    const BlockIndexHotStatus st = live->ResolveBlockSnapshot(hashParent, &snap, &e);
    if (st == BlockIndexHotStatus::AUTHORITY_MISSING)
    {
        // NOT_FOUND is produced ONLY when the logical parent is genuinely absent
        // from the authority. AUTHORITY_MISSING alone cannot prove that: it also
        // covers an authority that is present but cannot expose the vertex. So
        // require the independent typed parent authority to confirm absence.
        // A vertex the authority DOES know (FOUND / NOT_ACTIVE) which the
        // composite view could not resolve is inconsistent metadata, and an
        // unavailable/changed-generational authority is a failure: both FAIL
        // CLOSED. Absence must never hide an unhealthy/revoked/mismatched source.
        BlockIndexAuthoritativeParentInfo pinfo;
        std::string perr;
        const BlockIndexAuthoritativeParentStatus pst =
            live->ResolveParentInfo(hashParent, &pinfo, &perr);
        if (pst == BLOCK_INDEX_AUTHORITATIVE_PARENT_NOT_FOUND)
        {
            // Legacy parity: a parent that is not an authority vertex at all is
            // ranked at score 0 exactly like the legacy `mapBlockIndex.find == end`
            // branch. This is NOT an authority failure.
            r.status = DAGParentScoreStatus::NOT_FOUND;
            r.score = 0;
            return r;
        }
        r.status = DAGParentScoreStatus::FAILURE;
        if (error) *error = "DAG parent score: parent absence not provable (status=" +
                            std::to_string((int)pst) + "): " +
                            (perr.empty() ? std::string("(no detail)") : perr);
        return r;
    }
    if (st != BlockIndexHotStatus::OK)
    {
        r.status = DAGParentScoreStatus::FAILURE;
        if (error) *error = "DAG parent score: parent metadata unavailable: " +
                            (e.empty() ? std::string("(no detail)") : e);
        return r;
    }

    // Post-DAG proof-of-stake exclusion: contributes no score. A legitimate
    // FOUND(0), never conflated with a resolution failure.
    if (snap.height >= FORK_HEIGHT_DAG && snap.fProofOfStake)
    {
        r.status = DAGParentScoreStatus::FOUND;
        r.score = 0;
        return r;
    }

    // Pre-DAG parent: the entropy-correct by-value accumulated trust that the
    // legacy ColorBlock fallback consumes. NEVER the reciprocal-only
    // derived.dat chainTrust, and no resident mapBlockIndex.
    if (snap.height < FORK_HEIGHT_DAG)
    {
        uint256 trust = 0;
        std::string terr;
        if (!GetAuthoritativeAccumulatedChainTrust(hashParent, &trust, &terr))
        {
            r.status = DAGParentScoreStatus::FAILURE;
            if (error) *error = "DAG parent score: pre-DAG accumulated trust unavailable: " +
                                (terr.empty() ? std::string("(no detail)") : terr);
            return r;
        }
        r.status = DAGParentScoreStatus::FOUND;
        r.score = trust;
        return r;
    }

    // Post-DAG proof-of-work parent: the exact former DAG-overwritten scalar.
    // Source order (B-1):
    //   1. the CERTIFIED-CURRENT canonical row, when the score authority is
    //      healthy and holds the row (FOUND_CANONICAL_SCORE);
    //   2. else, if the row's absence is explained by the ACCEPTED prune/erase
    //      lifecycle (a known DAG-era vertex strictly below the certified prune
    //      line), the value is reconstructed by value from immutable
    //      authoritative inputs (Option-R) (FOUND_PRUNED_BOUNDARY);
    //   3. else, if the retained set is merely NOT CERTIFIED in this session, the
    //      same exact reconstruction applies (a certificate is bound BY an add,
    //      so an uncertified session must still be able to color).
    // Revoked/corrupt/unavailable authority, an absence NOT explained by the
    // lifecycle (arbitrary row loss), and an unreconstructible scalar all FAIL
    // CLOSED. Never substitute 0; never fall back to residency.
    {
        CTxDB db("r");
        std::string herr;
        const CTxDB::DAGScoreAuthorityStatus ast = db.GetDAGScoreAuthorityStatus(&herr);
        // Positive evidence of damage (or an unreadable/absent store) fails closed
        // in BOTH policies: it is never "just" a stale certificate.
        if (ast == CTxDB::DAG_SCORE_AUTHORITY_CORRUPT ||
            ast == CTxDB::DAG_SCORE_AUTHORITY_UNAVAILABLE)
        {
            r.status = DAGParentScoreStatus::FAILURE;
            if (error) *error = "DAG parent score: score authority unhealthy: " +
                                (herr.empty() ? std::string("(no detail)") : herr);
            return r;
        }
        // Accepted direct-resolution contract: a revocation is FAILURE, never a
        // value. Only the accepting mutation may treat it as re-certifiable,
        // because that mutation's own commit republishes the certificate.
        if (ast == CTxDB::DAG_SCORE_AUTHORITY_REVOKED &&
            policy != DAGParentScorePolicy::MUTATION)
        {
            r.status = DAGParentScoreStatus::FAILURE;
            if (error) *error = "DAG parent score: score authority unhealthy: " +
                                (herr.empty() ? std::string("(no detail)") : herr);
            return r;
        }
        // R3 / C6 section 1 — TYPED ROW READ. The legacy bool read collapses
        // not-found, an I/O/iterator failure and a malformed payload into one
        // false, which this consensus path then read as "row absent" and handed to
        // the prune-attribution boundary. Those are three different facts: a
        // malformed row is CORRUPT, a storage failure is a STORAGE error, and only
        // a genuine absence may reach the boundary check. Both of the former fail
        // closed here (a corrupt row is never "missing"; a storage failure is never
        // "missing"), so no failure mode can be laundered into prune provenance.
        CBlockDAGData data;
        DAGRowTypedOutcome rowOutcome = DAGRowTypedOutcome::ROW_MISSING_UNEXPLAINED;
        std::string rowDetail;
        if (!db.ReadDAGLinksTyped(hashParent, &data, &rowOutcome, &rowDetail))
        {
            r.status = DAGParentScoreStatus::FAILURE;
            if (error) *error = "DAG parent score: typed DAG row read returned no classification for " +
                                hashParent.GetHex();
            return r;
        }
        if (rowOutcome == DAGRowTypedOutcome::STORAGE_ERROR)
        {
            r.status = DAGParentScoreStatus::FAILURE;
            if (error)
                *error = "DAG parent score: DAG row storage failure for post-DAG parent " +
                         hashParent.GetHex() + " (a storage/iterator failure is never an absence)" +
                         (rowDetail.empty() ? std::string() : ": " + rowDetail);
            return r;
        }
        if (rowOutcome == DAGRowTypedOutcome::ROW_CORRUPT)
        {
            r.status = DAGParentScoreStatus::FAILURE;
            if (error)
                *error = "DAG parent score: malformed DAG row for post-DAG parent " +
                         hashParent.GetHex() + " (a corrupt row is never treated as absent)" +
                         (rowDetail.empty() ? std::string() : ": " + rowDetail);
            return r;
        }
        const bool fRowPresent = (rowOutcome == DAGRowTypedOutcome::ROW_PRESENT_VALID);
        if (ast == CTxDB::DAG_SCORE_AUTHORITY_HEALTHY)
        {
            if (fRowPresent)
            {
                r.status = DAGParentScoreStatus::FOUND;
                r.score = data.nDAGScore;
                r.source = DAGParentScoreSource::CANONICAL_ROW;
                return r;
            }
            // Healthy certificate, row gone. R3 / C6: the absence is admitted as
            // ROW_OBJECTIVELY_PRUNED only when the COMPLETE frozen positive predicate
            // verifies (incarnation-bound prune event, admissible custody epoch,
            // certified domain, verified certificate, verified custody watermark,
            // corroborating floor). Every other missing row is ROW_MISSING_UNEXPLAINED
            // and fails closed: a bare absence, a floor alone and a legacy erase marker
            // prove nothing.
            std::string whyUnexplained;
            if (!DAGRowObjectivelyPruned(db, snap, hashParent, &whyUnexplained))
            {
                r.status = DAGParentScoreStatus::FAILURE;
                if (error)
                {
                    *error = "DAG parent score: canonical DAG score row absent for post-DAG parent " +
                             hashParent.GetHex() +
                             " (ROW_MISSING_UNEXPLAINED: no positive bound prune evidence)";
                    if (!whyUnexplained.empty()) *error += ": " + whyUnexplained;
                }
                return r;
            }
        }
        // Non-binding certificate (uncertified, or a revocation in the accepting
        // mutation) or an explained pruned boundary: reconstruct the exact former
        // scalar by value and bind it to one stable source identity.
        uint256 tokenBefore, tokenAfter;
        if (!db.ReadDAGSourceStateId(tokenBefore))
        {
            r.status = DAGParentScoreStatus::FAILURE;
            if (error) *error = "DAG parent score: boundary reconstruction requires a bound source token";
            return r;
        }
        BoundaryScoreResult bres;
        std::string berr;
        AuthoritativeDAGRecolorSource bsrc(db);
        const bool fReconstructed = bsrc.ReconstructBoundaryScore(hashParent, &bres, &berr);
        if (!fReconstructed ||
            !bres.valid || bres.hash != hashParent || bres.height != snap.height)
        {
            r.status = DAGParentScoreStatus::FAILURE;
            if (error) *error = "DAG parent score: boundary scalar reconstruction unavailable for post-DAG parent " +
                                hashParent.GetHex() + ": " +
                                (berr.empty() ? std::string("identity binding mismatch") : berr);
            return r;
        }
        // Source binding: the reconstruction must have been made at ONE stable
        // authoritative source identity (a concurrent source advance invalidates
        // the value rather than being silently accepted).
        if (!db.ReadDAGSourceStateId(tokenAfter) || tokenAfter != tokenBefore)
        {
            r.status = DAGParentScoreStatus::FAILURE;
            if (error) *error = "DAG parent score: boundary reconstruction source token changed for post-DAG parent " +
                                hashParent.GetHex();
            return r;
        }
        r.status = DAGParentScoreStatus::FOUND;
        r.score = bres.score;
        r.source = (ast == CTxDB::DAG_SCORE_AUTHORITY_HEALTHY)
                       ? DAGParentScoreSource::PRUNED_BOUNDARY
                       : DAGParentScoreSource::DEGRADED_CERTIFICATE_BOUNDARY;
        return r;
    }
}

void CDAGManager::ColorBlock(CBlockIndex* pindex)
{
    std::string error;
    (void)ColorBlockImpl(pindex, false, &error);
}

bool CDAGManager::ColorBlockAuthoritative(CBlockIndex* pindex, std::string* error)
{
    // The authoritative accept path. Mode is decided by the authoritative
    // startup flag; outside it this is exactly the legacy coloring and cannot
    // fail.
    return ColorBlockImpl(pindex, g_fAuthoritativeStartup, error);
}

bool CDAGManager::ColorBlockImpl(CBlockIndex* pindex, bool fAuthoritativeParentScore,
                                 std::string* error)
{
    if (error) error->clear();
    std::map<uint256, CBlockIndex*>& mapBlockIndex = RecolorBlockIndex();
    LOCK(cs_dag);

    if (!pindex || !pindex->phashBlock)
        return true;
    if (pindex->nHeight >= FORK_HEIGHT_DAG && pindex->IsProofOfStake())
        return true;

    uint256 hash = pindex->GetBlockHash();
    auto it = mapDAGData.find(hash);
    if (it == mapDAGData.end())
        return true;

    CBlockDAGData& data = it->second;
    const std::vector<uint256>& vParents = data.vDAGParents;

    if (vParents.empty())
    {
        // Genesis or pre-DAG block: always blue
        data.fBlue = true;
        data.nDAGScore = pindex->GetBlockTrust();
        return true;
    }

    // Find selected parent = parent with highest DAG score
    // Pre-DAG parents use their nChainTrust as effective DAG score
    uint256 hashSelectedParent;
    uint256 nBestParentScore = 0;

    for (const uint256& hashParent : vParents)
    {
        // F2: ONE logical parent-score resolver. Legacy/canvas mode is
        // byte-identical to the historical resident rule; authoritative mode
        // reads the certified authority and NEVER substitutes 0 on failure.
        // B-1: this is the ACCEPT-path coloring, so a non-binding certificate is
        // resolved by exact boundary reconstruction instead of bricking the
        // node; the mutation's commit re-certifies it.
        DAGParentScoreResult pres =
            ResolveDagParentScore(hashParent, fAuthoritativeParentScore, error,
                                  DAGParentScorePolicy::MUTATION);
        if (pres.status == DAGParentScoreStatus::FAILURE)
            return false;
        const uint256 nParentScore = pres.score;

        if (nParentScore > nBestParentScore ||
            (nParentScore == nBestParentScore && (hashSelectedParent == 0 || hashParent < hashSelectedParent)))
        {
            nBestParentScore = nParentScore;
            hashSelectedParent = hashParent;
        }
    }

    if (hashSelectedParent == 0)
    {
        // Fallback: use parent's chain trust + this block's trust
        if (pindex->pprev)
            data.nDAGScore = pindex->pprev->nChainTrust + pindex->GetBlockTrust();
        else
            data.nDAGScore = pindex->GetBlockTrust();
        data.fBlue = true;
        return true;
    }

    // Inherit blue set from selected parent
    std::set<uint256> blueSet = GetBlueSetCached(hashSelectedParent);
    // Cache selected parent's blue set before merge modifications (avoid redundant BFS)
    std::set<uint256> selectedParentBlue = blueSet;

    // For each merge parent, try to add its blue blocks
    for (const uint256& hashParent : vParents)
    {
        if (hashParent == hashSelectedParent)
            continue;

        auto pit = mapDAGData.find(hashParent);
        if (pit == mapDAGData.end())
            continue;

        // Get blue blocks reachable from this merge parent
        std::set<uint256> mergeBlue = GetBlueSetCached(hashParent);

        for (const uint256& hashCandidate : mergeBlue)
        {
            if (blueSet.count(hashCandidate))
                continue; // already in blue set

            // Check anticone size: |anticone(X) ∩ blue_set| <= GHOSTDAG_K
            int nAnticone = AnticoneSize(hashCandidate, blueSet);
            if (nAnticone <= GHOSTDAG_K)
            {
                blueSet.insert(hashCandidate);
                // Mark block as blue
                auto cit = mapDAGData.find(hashCandidate);
                if (cit != mapDAGData.end())
                    cit->second.fBlue = true;
            }
            else
            {
                // Mark as red
                auto cit = mapDAGData.find(hashCandidate);
                if (cit != mapDAGData.end())
                    cit->second.fBlue = false;
            }
        }
    }

    // This block itself is always blue
    data.fBlue = true;
    blueSet.insert(hash);

    // Compute DAG score incrementally:
    // score = selected_parent_score + this_block_trust
    //       + trust of newly-blue merge blocks (not already in selected parent's blue set)
    uint256 nScore = nBestParentScore + pindex->GetBlockTrust();

    // Add trust from newly-blue merge parent blocks (using cached selectedParentBlue)
    for (const uint256& hashBlue : blueSet)
    {
        if (hashBlue == hash)
            continue; // already counted above
        if (selectedParentBlue.count(hashBlue))
            continue; // already in selected parent's score

        std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlue);
        if (mi != mapBlockIndex.end())
        {
            if (mi->second->nHeight >= FORK_HEIGHT_DAG && mi->second->IsProofOfStake())
                continue;
            nScore = nScore + mi->second->GetBlockTrust();
        }
    }
    data.nDAGScore = nScore;
    return true;
}


// ---------------------------------------------------------------------------
// CDAGManager: DAG Linear Ordering
// ---------------------------------------------------------------------------

std::vector<uint256> CDAGManager::GetDAGLinearOrder(const uint256& hashTip, int nMaxBlocks) const
{
    LOCK(cs_dag);

    std::vector<uint256> vOrder;
    std::set<uint256> visited;

    // Follow selected-parent chain from tip to genesis
    // Bounded by mapDAGData size + cycle detection for safety
    std::vector<uint256> selectedChain;
    std::set<uint256> chainVisited;
    uint256 hashCurrent = hashTip;
    int nMaxChainLen = (int)mapDAGData.size() + 1;

    // If caller requests limited output, limit chain walk depth too
    if (nMaxBlocks > 0 && nMaxBlocks < nMaxChainLen)
        nMaxChainLen = nMaxBlocks;

    while (hashCurrent != 0 && nMaxChainLen > 0)
    {
        if (!chainVisited.insert(hashCurrent).second)
            break; // cycle detected — stop
        selectedChain.push_back(hashCurrent);
        hashCurrent = GetSelectedParent(hashCurrent);
        nMaxChainLen--;
    }

    // Reverse to go genesis->tip
    std::reverse(selectedChain.begin(), selectedChain.end());

    // At each step on the selected chain, insert newly-visible blocks
    for (const uint256& hashChainBlock : selectedChain)
    {
        if (!visited.insert(hashChainBlock).second)
            continue;

        auto it = mapDAGData.find(hashChainBlock);
        if (it == mapDAGData.end())
        {
            vOrder.push_back(hashChainBlock);
            continue;
        }

        // Collect merge parents' blocks not yet visited
        // Insert blue blocks first (topological), then red blocks
        std::vector<uint256> vBlueInsert;
        std::vector<uint256> vRedInsert;

        std::queue<uint256> queue;
        for (const uint256& hashParent : it->second.vDAGParents)
        {
            if (hashParent != GetSelectedParent(hashChainBlock))
                queue.push(hashParent);
        }

        std::set<uint256> queueVisited;
        while (!queue.empty())
        {
            uint256 h = queue.front();
            queue.pop();

            if (!queueVisited.insert(h).second)
                continue;
            if (visited.count(h))
                continue;

            visited.insert(h);

            auto dit = mapDAGData.find(h);
            if (dit != mapDAGData.end())
            {
                if (dit->second.fBlue)
                    vBlueInsert.push_back(h);
                else
                    vRedInsert.push_back(h);

                // Continue BFS through parents
                for (const uint256& hp : dit->second.vDAGParents)
                {
                    if (!visited.count(hp) && !queueVisited.count(hp))
                        queue.push(hp);
                }
            }
            else
            {
                vBlueInsert.push_back(h); // pre-DAG blocks treated as blue
            }
        }

        // Sort by hash for determinism within each color group
        std::sort(vBlueInsert.begin(), vBlueInsert.end());
        std::sort(vRedInsert.begin(), vRedInsert.end());

        // Insert: blue first, then red, then this chain block
        for (const uint256& h : vBlueInsert)
            vOrder.push_back(h);
        for (const uint256& h : vRedInsert)
            vOrder.push_back(h);
        vOrder.push_back(hashChainBlock);
    }

    return vOrder;
}


// ---------------------------------------------------------------------------
// CDAGManager: DAG Score Computation
// ---------------------------------------------------------------------------

uint256 CDAGManager::ComputeDAGScore(CBlockIndex* pindex)
{
    LOCK(cs_dag);

    if (!pindex || !pindex->phashBlock)
        return 0;
    if (pindex->nHeight >= FORK_HEIGHT_DAG && pindex->IsProofOfStake())
        return 0;

    uint256 hash = pindex->GetBlockHash();
    auto it = mapDAGData.find(hash);
    if (it != mapDAGData.end())
        return it->second.nDAGScore;

    // Pre-DAG block: use nChainTrust
    return pindex->nChainTrust;
}


// ---------------------------------------------------------------------------
// CDAGManager: Selected Parent
// ---------------------------------------------------------------------------

uint256 CDAGManager::GetSelectedParent(const uint256& hashBlock) const
{
    std::map<uint256, CBlockIndex*>& mapBlockIndex = RecolorBlockIndex();
    // No lock needed — caller should hold cs_dag
    auto it = mapDAGData.find(hashBlock);
    if (it == mapDAGData.end() || it->second.vDAGParents.empty())
        return 0;

    // Selected parent = parent with highest DAG score
    // Pre-DAG parents use nChainTrust as effective score
    uint256 hashBest;
    uint256 nBestScore = 0;

    for (const uint256& hashParent : it->second.vDAGParents)
    {
        uint256 nParentScore = 0;
        auto pit = mapDAGData.find(hashParent);
        if (pit != mapDAGData.end())
        {
            nParentScore = pit->second.nDAGScore;
        }
        else
        {
            // Pre-DAG parent: use chain trust
            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashParent);
            if (mi != mapBlockIndex.end() &&
                !(mi->second->nHeight >= FORK_HEIGHT_DAG && mi->second->IsProofOfStake()))
                nParentScore = mi->second->nChainTrust;
        }

        if (nParentScore > nBestScore ||
            (nParentScore == nBestScore && (hashBest == 0 || hashParent < hashBest)))
        {
            nBestScore = nParentScore;
            hashBest = hashParent;
        }
    }

    return hashBest;
}


// ---------------------------------------------------------------------------
// CDAGManager: Blue Set and Anticone helpers
// ---------------------------------------------------------------------------

std::set<uint256> CDAGManager::GetBlueSet(const uint256& hashBlock) const
{
    std::map<uint256, CBlockIndex*>& mapBlockIndex = RecolorBlockIndex();
    // No lock needed — caller should hold cs_dag
    // Bounded by DAG_MERGE_DEPTH * 4 to prevent DoS from deep BFS traversals
    static const int BLUESET_MAX_VISITED = DAG_MERGE_DEPTH * 4; // 256

    std::set<uint256> blueSet;
    std::set<uint256> visited;
    std::queue<uint256> queue;
    queue.push(hashBlock);

    while (!queue.empty())
    {
        uint256 h = queue.front();
        queue.pop();

        if (!visited.insert(h).second)
            continue;

        auto it = mapDAGData.find(h);
        if (it == mapDAGData.end())
        {
            // Deterministic boundary: any missing block at/above FORK_HEIGHT_DAG is a
            // pruned DAG block (stop BFS). Below FORK_HEIGHT_DAG is a genuine pre-DAG
            // block (add to blue set). This is deterministic regardless of local pruning state.
            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(h);
            if (mi != mapBlockIndex.end() && mi->second->nHeight >= FORK_HEIGHT_DAG)
                continue; // pruned DAG-era block — BFS boundary
            blueSet.insert(h); // genuine pre-DAG block
            continue;
        }

        if (it->second.fBlue)
            blueSet.insert(h);

        // Bounded BFS to prevent DoS
        if ((int)visited.size() >= BLUESET_MAX_VISITED)
            break;

        for (const uint256& hp : it->second.vDAGParents)
        {
            if (!visited.count(hp))
                queue.push(hp);
        }
    }

    return blueSet;
}

std::set<uint256> CDAGManager::GetBlueSetCached(const uint256& hashBlock) const
{
    // Check cache first
    auto cit = mapBlueSetCache.find(hashBlock);
    if (cit != mapBlueSetCache.end())
        return cit->second;

    // Compute and cache
    std::set<uint256> blueSet = GetBlueSet(hashBlock);

    // Evict oldest if cache full (simple eviction: clear half)
    if ((int)mapBlueSetCache.size() >= BLUESET_CACHE_MAX)
    {
        auto it = mapBlueSetCache.begin();
        int nToRemove = BLUESET_CACHE_MAX / 2;
        while (it != mapBlueSetCache.end() && nToRemove > 0)
        {
            it = mapBlueSetCache.erase(it);
            nToRemove--;
        }
    }

    mapBlueSetCache[hashBlock] = blueSet;
    return blueSet;
}

int CDAGManager::AnticoneSize(const uint256& hashBlock, const std::set<uint256>& blueSet) const
{
    // Anticone of X w.r.t. blue set: blocks in blueSet that are neither
    // ancestors nor descendants of X.

    auto itX = mapDAGData.find(hashBlock);
    if (itX == mapDAGData.end())
        return 0;

    // Get X's past set (ancestors) — computed once
    std::set<uint256> pastX = GetPastSet(hashBlock, DAG_MERGE_DEPTH * 2);

    // Get X's future set by checking which blueSet blocks have X in their past
    // Build a combined future set for efficiency: collect all blocks that have X as ancestor
    std::set<uint256> futureX;
    for (const uint256& hashBlue : blueSet)
    {
        if (hashBlue == hashBlock || pastX.count(hashBlue))
            continue;

        // Check if hashBlue has hashBlock in its past (i.e., X is ancestor of hashBlue)
        // Use bounded BFS from hashBlue back through parents
        std::set<uint256> visited;
        std::queue<uint256> q;
        auto bit = mapDAGData.find(hashBlue);
        if (bit == mapDAGData.end())
            continue;

        bool fFound = false;
        for (const uint256& hp : bit->second.vDAGParents)
            q.push(hp);

        int nSteps = 0;
        while (!q.empty() && nSteps < DAG_MERGE_DEPTH * 2)
        {
            uint256 h = q.front();
            q.pop();
            if (!visited.insert(h).second)
                continue;
            if (h == hashBlock)
            {
                fFound = true;
                break;
            }
            auto pit = mapDAGData.find(h);
            if (pit != mapDAGData.end())
            {
                for (const uint256& hp : pit->second.vDAGParents)
                {
                    if (!visited.count(hp))
                        q.push(hp);
                }
            }
            nSteps++;
        }

        if (fFound)
            futureX.insert(hashBlue);
    }

    // Anticone = blueSet - {X} - past(X) - future(X)
    int nAnticone = 0;
    for (const uint256& hashBlue : blueSet)
    {
        if (hashBlue == hashBlock)
            continue;
        if (pastX.count(hashBlue))
            continue;
        if (futureX.count(hashBlue))
            continue;
        nAnticone++;
    }

    return nAnticone;
}

std::set<uint256> CDAGManager::GetPastSet(const uint256& hashBlock, int nMaxDepth) const
{
    std::map<uint256, CBlockIndex*>& mapBlockIndex = RecolorBlockIndex();
    // No lock needed — caller should hold cs_dag
    // Uses height-based depth (not BFS step count) for deterministic traversal
    std::set<uint256> past;
    std::queue<uint256> queue;

    auto it = mapDAGData.find(hashBlock);
    if (it == mapDAGData.end())
        return past;

    // Get starting block height for depth comparison
    int nStartHeight = -1;
    std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
    if (mi != mapBlockIndex.end())
        nStartHeight = mi->second->nHeight;

    for (const uint256& hp : it->second.vDAGParents)
        queue.push(hp);

    while (!queue.empty())
    {
        uint256 h = queue.front();
        queue.pop();

        if (!past.insert(h).second)
            continue;

        // Height-based depth check: stop when block is too far below start
        if (nStartHeight >= 0)
        {
            std::map<uint256, CBlockIndex*>::iterator mh = mapBlockIndex.find(h);
            if (mh != mapBlockIndex.end() && nStartHeight - mh->second->nHeight > nMaxDepth)
                continue; // don't expand parents beyond depth limit
        }

        auto pit = mapDAGData.find(h);
        if (pit != mapDAGData.end())
        {
            for (const uint256& hp : pit->second.vDAGParents)
            {
                if (!past.count(hp))
                    queue.push(hp);
            }
        }
    }

    return past;
}


// ---------------------------------------------------------------------------
// CDAGManager: Sibling Blocks (for conflict resolution in ConnectBlock)
// ---------------------------------------------------------------------------

std::set<uint256> CDAGManager::GetDAGSiblingBlocks(const uint256& hashBlock) const
{
    LOCK(cs_dag);

    std::set<uint256> siblings;
    auto it = mapDAGData.find(hashBlock);
    if (it == mapDAGData.end())
        return siblings;

    // Siblings = other children of our parents
    for (const uint256& hashParent : it->second.vDAGParents)
    {
        auto pit = mapDAGData.find(hashParent);
        if (pit == mapDAGData.end())
            continue;

        for (const uint256& hashChild : pit->second.vDAGChildren)
        {
            if (hashChild != hashBlock)
                siblings.insert(hashChild);
        }
    }

    return siblings;
}

bool CDAGManager::HasDAGData(const uint256& hash) const
{
    LOCK(cs_dag);
    return mapDAGData.count(hash) > 0;
}

bool CDAGManager::GetDAGData(const uint256& hash, CBlockDAGData& dataOut) const
{
    LOCK(cs_dag);
    auto it = mapDAGData.find(hash);
    if (it == mapDAGData.end())
        return false;
    dataOut = it->second;
    return true;
}


void CDAGManager::SetDAGDataForTest(const uint256& hash, const CBlockDAGData& data)
{
    LOCK(cs_dag);
    mapDAGData[hash] = data;
}


std::map<uint256, CBlockIndex*>& CDAGManager::RecolorBlockIndex() const
{
    return recolorBlockIndex ? *recolorBlockIndex : ::mapBlockIndex;
}

void CDAGManager::LoadRecolorCanvas(const std::map<uint256, CBlockDAGData>& records)
{
    LOCK(cs_dag);
    mapDAGData = records;
    mapBlueSetCache.clear();
}

uint256 CDAGManager::RecolorStateDigestForTest() const
{
    LOCK(cs_dag);
    CHashWriter digest(SER_GETHASH, 0);
    digest << mapDAGData << setDAGTips << mapPendingChildrenByParent
           << mapBlueSetCache << mapEpochState << setEpochBoundaryBlocks
           << nPrunedBelowHeight;
    return digest.GetHash();
}

void CDAGManager::ClearDAGDataForTest()
{
    LOCK(cs_dag);
    mapDAGData.clear();
    setDAGTips.clear();
    mapPendingChildrenByParent.clear();
    mapBlueSetCache.clear();
}


void CDAGManager::RemoveBlockDAGData(const uint256& hashBlock)
{
    LOCK(cs_dag);

    auto it = mapDAGData.find(hashBlock);
    if (it == mapDAGData.end())
        return;

    // Remove this block from its parents' child lists
    for (const uint256& hashParent : it->second.vDAGParents)
    {
        auto pit = mapDAGData.find(hashParent);
        if (pit != mapDAGData.end())
        {
            auto& children = pit->second.vDAGChildren;
            children.erase(std::remove(children.begin(), children.end(), hashBlock), children.end());
            // Parent may become a tip again if it has no other children
            if (children.empty())
                TrackedInsertTip(hashParent);
        }
        else
        {
            auto pendingIt = mapPendingChildrenByParent.find(hashParent);
            if (pendingIt != mapPendingChildrenByParent.end())
            {
                pendingIt->second.erase(hashBlock);
                if (pendingIt->second.empty())
                    mapPendingChildrenByParent.erase(pendingIt);
            }
        }
    }

    for (const uint256& hashChild : it->second.vDAGChildren)
    {
        mapPendingChildrenByParent[hashBlock].insert(hashChild);
        InvalidateBlueSetCacheForBlock(hashChild);
    }

    // Remove from tips and data
    TrackedEraseTip(hashBlock);
    mapDAGData.erase(it);
    InvalidateBlueSetCacheForBlock(hashBlock);
}


// ---------------------------------------------------------------------------
// CDAGManager: LevelDB Persistence
// ---------------------------------------------------------------------------

bool CDAGManager::WriteDAGLinks(CTxDB& txdb, const uint256& hash)
{
    LOCK(cs_dag);

    auto it = mapDAGData.find(hash);
    if (it == mapDAGData.end())
        return false;

    return txdb.WriteDAGLinks(hash, it->second);
}

bool CDAGManager::LoadDAGLinks(CTxDB& txdb)
{
    LOCK(cs_dag);

    mapDAGData.clear();
    setDAGTips.clear();
    mapPendingChildrenByParent.clear();

    // Load DAG links using efficient LevelDB prefix iteration
    txdb.IterateDAGLinks(mapDAGData);

    RebuildPendingChildIndex();

    if (!mapDAGData.empty())
        printf("LoadDAGLinks: loaded %d DAG entries, %d tips, %d pending parent links\n",
               (int)mapDAGData.size(), (int)setDAGTips.size(), (int)mapPendingChildrenByParent.size());

    return true;
}

bool CDAGManager::LoadEpochStates(CTxDB& txdb)
{
    LOCK(cs_dag);

    std::map<int, CEpochState> mapStates;
    if (!txdb.IterateEpochStates(mapStates))
        return false;

    std::map<int, CCurveTree> mapTrees;
    if (!txdb.IterateCurveTreeEpochs(mapTrees))
        return false;

    mapEpochState = mapStates;
    mapEpochCurveTrees = mapTrees;

    for (const auto& pair : mapEpochState)
    {
        if (pair.second.hashBoundaryBlock != 0)
            setEpochBoundaryBlocks.insert(pair.second.hashBoundaryBlock);
    }

    if (!mapEpochState.empty() || !mapEpochCurveTrees.empty())
        printf("LoadEpochStates: loaded %d epoch states and %d curve-tree snapshots\n",
               (int)mapEpochState.size(), (int)mapEpochCurveTrees.size());

    return true;
}


// ---------------------------------------------------------------------------
// CDAGManager: Restore canonical DAG score into CBlockIndex::nChainTrust
// ---------------------------------------------------------------------------

void CDAGManager::RestoreDAGTrustIntoChainTrust()
{
    g_res_restoredagtrust_calls++;
    LOCK(cs_dag);

    int nCount = 0;
    for (const auto& mi : mapBlockIndex)
    {
        CBlockIndex* pindex = mi.second;
        if (!pindex || pindex->nHeight < FORK_HEIGHT_DAG)
            continue;
        if (!pindex->IsProofOfWork())
            continue;

        auto it = mapDAGData.find(mi.first);
        if (it == mapDAGData.end() || it->second.nDAGScore == 0)
            continue;

        // Mirror the live overwrite at main.cpp:8813-8815
        pindex->nChainTrust = it->second.nDAGScore;
        nCount++;
    }

    if (nCount > 0)
        printf("RestoreDAGTrustIntoChainTrust: restored DAG score into nChainTrust for %d blocks\n", nCount);
}


// ---------------------------------------------------------------------------
// CDAGManager: Rebuild Ordering
// ---------------------------------------------------------------------------

bool CDAGManager::RebuildDAGOrder(const DagMutationPreview* mutationPreview)
{
    LOCK(cs_dag);

    // R2c.2s/S5: explicit transaction-scoped preview validation (fail closed).
    if (mutationPreview)
    {
        std::string previewError;
        DagMutationPreviewStatus pst = ValidateDagMutationPreviewForConsumer(
            mutationPreview, DAG_MUTATION_PREVIEW_CONSUMER_ORDER, &previewError);
        if (pst != DAG_MUTATION_PREVIEW_OK)
        {
            fprintf(stderr, "RebuildDAGOrder: S5 preview validation failed: %s\n",
                    DagMutationPreviewStatusName(pst));
            fflush(stderr);
            InvalidateDagTipDeltaTransaction();
            return false;
        }
    }

    // Clear blue set cache to avoid stale entries during rebuild
    mapBlueSetCache.clear();

    // Re-color all blocks and recompute scores
    // Process blocks in height order
    std::vector<std::pair<int, uint256>> vByHeight;

    for (const auto& pair : mapDAGData)
    {
        std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(pair.first);
        if (mi != mapBlockIndex.end())
            vByHeight.push_back(std::make_pair(mi->second->nHeight, pair.first));
    }

    std::sort(vByHeight.begin(), vByHeight.end());

    for (const auto& pair : vByHeight)
    {
        std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(pair.second);
        if (mi != mapBlockIndex.end())
        {
            // Phase 4: Fork-gate between GHOSTDAG and DAGKNIGHT coloring
            if (mi->second->nHeight >= FORK_HEIGHT_DAGKNIGHT)
                ColorBlockDAGKnight(mi->second);
            else
                ColorBlock(mi->second);
        }
    }

    // Assign linear ordering from best tip.
    // R2c.2/S6: authoritative primary selection (value-semantic). In
    // authoritative mode the winner hash comes from the accepted S5 preview
    // (fail closed on UNAVAILABLE); the legacy resident selector runs only in
    // genuine legacy mode (LEGACY status).
    uint256 hashBestTip(0);
    {
        std::string selError;
        DagTipSelectionResult sel = SelectDagTipForInternalConsumer(mutationPreview, &selError);
        if (sel.status == DAG_TIP_SELECTION_UNAVAILABLE)
        {
            fprintf(stderr, "RebuildDAGOrder: S6 authoritative selection unavailable: %s\n",
                    selError.c_str());
            fflush(stderr);
            InvalidateDagTipDeltaTransaction();
            return false;
        }
        if (sel.IsUsable())
        {
            hashBestTip = sel.hash;
        }
        else
        {
            CBlockIndex* pBestTip = SelectBestDAGTip();
            if (pBestTip && pBestTip->phashBlock)
                hashBestTip = pBestTip->GetBlockHash();
        }
    }
    if (hashBestTip != 0)
    {
        std::vector<uint256> vOrder = GetDAGLinearOrder(hashBestTip);
        for (int i = 0; i < (int)vOrder.size(); i++)
        {
            auto it = mapDAGData.find(vOrder[i]);
            if (it != mapDAGData.end())
                it->second.nDAGOrder = i;
        }
    }

    printf("RebuildDAGOrder: recolored and ordered %d DAG blocks\n", (int)vByHeight.size());
    return true;
}


// ---------------------------------------------------------------------------
// CDAGManager: Incremental Rebuild (only recolors blocks above nCleanHeight)
// ---------------------------------------------------------------------------

bool CDAGManager::RebuildDAGOrderIncremental(int nCleanHeight, const DagMutationPreview* mutationPreview)
{
    LOCK(cs_dag);

    // R2c.2s/S5: explicit transaction-scoped preview validation (fail closed).
    if (mutationPreview)
    {
        std::string previewError;
        DagMutationPreviewStatus pst = ValidateDagMutationPreviewForConsumer(
            mutationPreview, DAG_MUTATION_PREVIEW_CONSUMER_REORDER, &previewError);
        if (pst != DAG_MUTATION_PREVIEW_OK)
        {
            fprintf(stderr, "RebuildDAGOrderIncremental: S5 preview validation failed: %s\n",
                    DagMutationPreviewStatusName(pst));
            fflush(stderr);
            InvalidateDagTipDeltaTransaction();
            return false;
        }
    }

    // Clear blue set cache to avoid stale entries during rebuild
    mapBlueSetCache.clear();

    std::vector<std::pair<int, uint256>> vByHeight;

    for (const auto& pair : mapDAGData)
    {
        std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(pair.first);
        if (mi != mapBlockIndex.end() && mi->second->nHeight > nCleanHeight)
            vByHeight.push_back(std::make_pair(mi->second->nHeight, pair.first));
    }

    std::sort(vByHeight.begin(), vByHeight.end());

    for (const auto& pair : vByHeight)
    {
        std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(pair.second);
        if (mi != mapBlockIndex.end())
        {
            // Phase 4: Fork-gate between GHOSTDAG and DAGKNIGHT coloring
            if (mi->second->nHeight >= FORK_HEIGHT_DAGKNIGHT)
                ColorBlockDAGKnight(mi->second);
            else
                ColorBlock(mi->second);
        }
    }

    // Assign linear ordering from best tip.
    // R2c.2/S6: authoritative primary selection (value-semantic); see
    // RebuildDAGOrder for the contract.
    uint256 hashBestTip(0);
    {
        std::string selError;
        DagTipSelectionResult sel = SelectDagTipForInternalConsumer(mutationPreview, &selError);
        if (sel.status == DAG_TIP_SELECTION_UNAVAILABLE)
        {
            fprintf(stderr, "RebuildDAGOrderIncremental: S6 authoritative selection unavailable: %s\n",
                    selError.c_str());
            fflush(stderr);
            InvalidateDagTipDeltaTransaction();
            return false;
        }
        if (sel.IsUsable())
        {
            hashBestTip = sel.hash;
        }
        else
        {
            CBlockIndex* pBestTip = SelectBestDAGTip();
            if (pBestTip && pBestTip->phashBlock)
                hashBestTip = pBestTip->GetBlockHash();
        }
    }
    if (hashBestTip != 0)
    {
        std::vector<uint256> vOrder = GetDAGLinearOrder(hashBestTip);
        for (int i = 0; i < (int)vOrder.size(); i++)
        {
            auto it = mapDAGData.find(vOrder[i]);
            if (it != mapDAGData.end())
                it->second.nDAGOrder = i;
        }
    }

    printf("RebuildDAGOrderIncremental: recolored %d blocks above height %d\n",
           (int)vByHeight.size(), nCleanHeight);
    return true;
}


// Test-only default-off source-prune seams; production retains DAG_PRUNE_DEPTH
// and normal LevelDB commit semantics.
int g_testDagPruneDepth = 0;
bool g_testFailDagPruneCommit = false;
// Test-only injection point for the S3 authoritative prune stages:
// 0=off, 1=post-selection (pre-batch), 2=pre-deletion, 3=post-deletion
// (child-count staged), 9=pre final commit. Engine-internal stages (recolor,
// full-field, token, certificates) are injected through the authoritative
// stage-barrier hook on the shared stage engine.
int g_testDagPruneFailStage = 0;

// ---------------------------------------------------------------------------
// CDAGManager: DAG Pruning
// ---------------------------------------------------------------------------

bool CDAGManager::PruneDAGData(CTxDB& txdb, int nHeight, DagPruneRollbackCapture* rollbackCapture,
                               const std::map<uint256,BlockIndexSnapshot>* chainedPending)
{
    LOCK(cs_dag);

    const int pruneDepth = g_testDagPruneDepth > 0 ? g_testDagPruneDepth : DAG_PRUNE_DEPTH;
    int nPruneBelow = nHeight - pruneDepth;
    if (nPruneBelow <= 0)
        return true; // nothing to prune

    int nPruned = 0;
    std::vector<uint256> vToErase;
    // R3 / C6 section 3: the height of each vertex being pruned, captured at
    // selection time from the SAME authoritative by-value resolution that already
    // decides prunability (or the legacy resident height). It is the only place
    // this value exists: the row payload carries no height, and the prune-event
    // journal entry must bind height(X) exactly. Parallel to vToErase.
    std::vector<int32_t> vToEraseHeights;

    if (g_fAuthoritativeStartup)
    {
        // S3 authoritative prune selection: the canonical persisted daglinks
        // source is the sole selector (never mapDAGData/mapBlockIndex
        // residency), heights resolve by value through the authoritative
        // resolver, and the real epoch-boundary exemption set is honored
        // exactly as in the legacy selection. A nonresident canonical record
        // is pruned by this path; a resident-only record is not.
        std::map<uint256, CBlockDAGData> persisted;
        std::string selErr;
        if (!txdb.IterateDAGLinksStrict(persisted, &selErr))
        {
            fprintf(stderr, "PruneDAGData: S3 prune selection failed: %s\n", selErr.c_str()); fflush(stderr);
            return false; // fail closed: never prune an unreadable canvas
        }
        for (const auto& pair : persisted)
        {
            // Don't prune epoch boundary blocks
            if (setEpochBoundaryBlocks.count(pair.first))
                continue;
            BlockIndexSnapshot snap;
            bool fHaveSnap = false;
            if (chainedPending)
            {
                // Mutation-scoped pending overlay first (exact same precedence as
                // the S3 enumerate/stage callers): a vertex committed by THIS
                // envelope (e.g. the block being added) is not yet resolvable
                // through generation/live authority but is known by value here.
                std::map<uint256,BlockIndexSnapshot>::const_iterator pit = chainedPending->find(pair.first);
                if (pit != chainedPending->end())
                {
                    snap = pit->second;
                    fHaveSnap = true;
                }
            }
            if (!fHaveSnap)
            {
                std::string resErr;
                if (!ResolveAuthoritativeBlockSnapshot(pair.first, &snap, &resErr) ||
                    !snap.found || snap.hash != pair.first)
                {
                    std::map<uint256, CBlockIndex*>::iterator dbgmi = mapBlockIndex.find(pair.first);
                    fprintf(stderr, "PruneDAGData: S3 prune height resolution failed for %s: %s (mapHeight=%d)\n",
                           pair.first.ToString().substr(0,20).c_str(), resErr.c_str(),
                           dbgmi != mapBlockIndex.end() ? dbgmi->second->nHeight : -1);
                    fflush(stderr);
                    return false;
                }
            }
            if (snap.height < nPruneBelow)
            {
                vToErase.push_back(pair.first);
                vToEraseHeights.push_back(snap.height);
            }
        }
    }
    else
    {
        for (const auto& pair : mapDAGData)
        {
            // Don't prune epoch boundary blocks
            if (setEpochBoundaryBlocks.count(pair.first))
                continue;

            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(pair.first);
            if (mi == mapBlockIndex.end())
                continue;

            if (mi->second->nHeight < nPruneBelow)
            {
                vToErase.push_back(pair.first);
                vToEraseHeights.push_back(mi->second->nHeight);
            }
        }
    }

    if (g_testDagPruneFailStage == 1)
    {
        fprintf(stderr, "PruneDAGData: injected selection-stage failure\n"); fflush(stderr);
        return false;
    }

    if (vToErase.empty())
        return true;

    // Phase 1: Write erasures + prune height + source token atomically.
    uint256 dagSourcePost;
    if (!txdb.MintDAGSourceStateId(dagSourcePost) || !txdb.TxnBegin())
    {
        fprintf(stderr, "PruneDAGData: S3 prune mint/TxnBegin failed\n"); fflush(stderr);
        return false;
    }

    if (g_fAuthoritativeStartup)
    {
        // S3 authoritative PRUNE physical source commit: canonical deletions,
        // post-prune canonical full-field reconciliation over surviving retained
        // vertices, exactly one SourceStateId advance, and BOTH certificates
        // bound to the same new token. One WriteBatch, one TxnCommit; staged
        // reads resolve through the batch (staged write > tombstone > disk); no
        // resident fallback anywhere in the decision or the derivation.
        if (rollbackCapture)
        {
            // Exact pre-image for a later SetBestChain-failure rollback: every
            // deleted durable row byte-for-byte plus the durable clean-height
            // marker state. Fail closed if a pre-image cannot be made.
            for (size_t i = 0; i < vToErase.size(); ++i)
            {
                CBlockDAGData pre;
                if (!txdb.ReadDAGLinks(vToErase[i], pre))
                {
                    fprintf(stderr, "PruneDAGData: S3 prune rollback pre-image read failed for %s\n",
                           vToErase[i].ToString().substr(0,20).c_str()); fflush(stderr);
                    txdb.TxnAbort();
                    return false;
                }
                rollbackCapture->records.push_back(std::make_pair(vToErase[i], pre));
                int preOrigin = -1; bool preOriginPresent = false;
                if (!txdb.ReadDAGRowErase(vToErase[i], &preOrigin, &preOriginPresent))
                {
                    fprintf(stderr, "PruneDAGData: S3 prune rollback row-erase pre-image read failed for %s\n",
                           vToErase[i].ToString().substr(0,20).c_str()); fflush(stderr);
                    txdb.TxnAbort();
                    return false;
                }
                rollbackCapture->rowEraseOrigins.push_back(preOriginPresent ? preOrigin : -1);
            }
            rollbackCapture->cleanHeightPresent = txdb.ReadDAGCleanHeight(rollbackCapture->cleanHeight);
            rollbackCapture->pruneFloorPresent = txdb.ReadDAGPruneFloor(rollbackCapture->pruneFloor);
        }
        if (g_testDagPruneFailStage == 2)
        {
            fprintf(stderr, "PruneDAGData: injected pre-deletion failure\n"); fflush(stderr);
            txdb.TxnAbort();
            return false;
        }
        // Token trace evidence: the durable token this physical prune commit
        // advances FROM (the ADD envelope's own source commit ran earlier).
        uint256 prunePreToken;
        txdb.ReadDAGSourceStateId(prunePreToken);
        if (vToEraseHeights.size() != vToErase.size())
        {
            fprintf(stderr, "PruneDAGData: S3 prune height/vertex capture mismatch (%d vs %d)\n",
                   (int)vToEraseHeights.size(), (int)vToErase.size()); fflush(stderr);
            txdb.TxnAbort();
            return false;
        }
        for (size_t i = 0; i < vToErase.size(); ++i)
            if (!txdb.EraseDAGLinks(vToErase[i], DAGRowEraseOrigin::PRUNE,
                                    vToEraseHeights[i], nPruneBelow))
            {
                fprintf(stderr, "PruneDAGData: S3 prune topology/child-count erase failed for %s\n",
                       vToErase[i].ToString().substr(0,20).c_str()); fflush(stderr);
                txdb.TxnAbort();
                return false;
            }
        if (g_testDagPruneFailStage == 3)
        {
            fprintf(stderr, "PruneDAGData: injected post-deletion (child-count staged) failure\n"); fflush(stderr);
            txdb.TxnAbort();
            return false;
        }
        std::vector<std::pair<int32_t,uint256>> stagedScope;
        std::string stageErr;
        if (!EnumerateAuthoritativeStagedScope(txdb, &stagedScope, chainedPending, &stageErr))
        {
            fprintf(stderr, "PruneDAGData: S3 prune scope enumeration failed: %s\n", stageErr.c_str()); fflush(stderr);
            txdb.TxnAbort();
            return false;
        }
        AuthoritativeDAGStageResult prRes;
        if (!StageAuthoritativeDAGScoreState(txdb, stagedScope, dagSourcePost, chainedPending, &prRes,
                                             &stageErr, /*diffOnlyWrites=*/true))
        {
            fprintf(stderr, "PruneDAGData: S3 prune authoritative staging failed: %s\n", stageErr.c_str()); fflush(stderr);
            txdb.TxnAbort();
            return false;
        }
        if (g_testDagPruneFailStage == 9)
        {
            fprintf(stderr, "PruneDAGData: injected final-commit failure\n"); fflush(stderr);
            txdb.TxnAbort();
            return false;
        }
        // Persist prune height so GetBlueSet boundary check survives restart.
        if (!txdb.WriteDAGCleanHeight(nPruneBelow))
        {
            fprintf(stderr, "PruneDAGData: S3 prune clean-height write failed\n"); fflush(stderr);
            txdb.TxnAbort();
            return false;
        }
        // F2 erase provenance: the same atomic commit persists the ERASE FLOOR of
        // THIS erasure. Sole writer of this key is the erase lifecycle, so a
        // below-floor absence is attributable to it; no non-erase site (Shutdown)
        // may advance it.
        if (!txdb.WriteDAGPruneFloor(nPruneBelow))
        {
            fprintf(stderr, "PruneDAGData: S3 prune erase-floor write failed\n"); fflush(stderr);
            txdb.TxnAbort();
            return false;
        }
        if (g_testFailDagPruneCommit || !txdb.TxnCommit())
        {
            fprintf(stderr, "PruneDAGData: S3 prune final commit failed\n"); fflush(stderr);
            return false;
        }
        if (rollbackCapture) rollbackCapture->committed = true;
        fprintf(stderr, "PruneDAGData: S3 prune committed below %d: deleted=%d surviving=%d changedFullFields=%d scopeClosure=%d token=%s preToken=%s\n",
               nPruneBelow, (int)vToErase.size(), (int)stagedScope.size(),
               (int)prRes.stagedFullFieldRecords, (int)prRes.stats.boundaryVertices,
               dagSourcePost.ToString().substr(0,20).c_str(),
               prunePreToken.ToString().substr(0,20).c_str()); fflush(stderr);
    }
    else
    {
        if (vToEraseHeights.size() != vToErase.size())
            return false;
        for (size_t i = 0; i < vToErase.size(); ++i)
            if (!txdb.EraseDAGLinks(vToErase[i], DAGRowEraseOrigin::PRUNE,
                                    vToEraseHeights[i], nPruneBelow)) { txdb.TxnAbort(); return false; }

        // Persist prune height so GetBlueSet boundary check survives restart.
        if (!txdb.WriteDAGCleanHeight(nPruneBelow) ||
            !txdb.WriteDAGPruneFloor(nPruneBelow) ||
            !txdb.WriteDAGSourceStateId(dagSourcePost) ||
            g_testFailDagPruneCommit || !txdb.TxnCommit())
            return false;
    }
    SetDagTipDeltaFinalSourceStateId(dagSourcePost);

    // Phase 2: Erase from memory only after LevelDB commit succeeds
    for (const uint256& hash : vToErase)
    {
        auto dit = mapDAGData.find(hash);
        if (dit != mapDAGData.end())
        {
            for (const uint256& hashParent : dit->second.vDAGParents)
            {
                auto pit = mapDAGData.find(hashParent);
                if (pit != mapDAGData.end())
                {
                    auto& children = pit->second.vDAGChildren;
                    children.erase(std::remove(children.begin(), children.end(), hash), children.end());
                    if (children.empty())
                        TrackedInsertTip(hashParent);
                }
                auto pendingIt = mapPendingChildrenByParent.find(hashParent);
                if (pendingIt != mapPendingChildrenByParent.end())
                {
                    pendingIt->second.erase(hash);
                    if (pendingIt->second.empty())
                        mapPendingChildrenByParent.erase(pendingIt);
                }
            }
            mapDAGData.erase(dit);
        }
        mapPendingChildrenByParent.erase(hash);
        TrackedEraseTip(hash);
        InvalidateBlueSetCacheForBlock(hash);
        nPruned++;
    }

    nPrunedBelowHeight = nPruneBelow;

    if (nPruned > 0)
        printf("PruneDAGData: pruned %d entries below height %d (%d remaining)\n",
               nPruned, nPruneBelow, (int)mapDAGData.size());

    return true;
}


// ---------------------------------------------------------------------------
// CDAGManager: Epoch State Computation
// ---------------------------------------------------------------------------

bool CDAGManager::ComputeEpochState(int nEpoch, int nEpochInterval, const DagMutationPreview* mutationPreview)
{
    LOCK(cs_dag);

    // R2c.2s/S5: mutation-internal synchronous consumer. The owning envelope
    // threads its explicit transaction-scoped preview; validation failure is
    // fail-closed (the journal is latched and the caller aborts).
    if (mutationPreview)
    {
        std::string previewError;
        DagMutationPreviewStatus pst = ValidateDagMutationPreviewForConsumer(
            mutationPreview, DAG_MUTATION_PREVIEW_CONSUMER_EPOCH, &previewError);
        if (pst != DAG_MUTATION_PREVIEW_OK)
        {
            fprintf(stderr, "ComputeEpochState: S5 preview validation failed: %s\n",
                    DagMutationPreviewStatusName(pst));
            fflush(stderr);
            InvalidateDagTipDeltaTransaction();
            return false;
        }
    }

    CEpochState state;
    state.nEpoch = nEpoch;
    // Use adjacent epoch boundaries so the epoch that contains the DAG fork is
    // truncated deterministically instead of extending across the fork.
    state.nHeightStart = GetEpochBoundaryHeight(nEpoch, nEpoch * nEpochInterval);
    int nNextEpochStart = GetEpochBoundaryHeight(nEpoch + 1, (nEpoch + 1) * nEpochInterval);
    if (nNextEpochStart > state.nHeightStart)
        state.nHeightEnd = nNextEpochStart - 1;
    else
        state.nHeightEnd = state.nHeightStart + nEpochInterval - 1;
    state.nBlockCount = 0;
    state.nTxCount = 0;
    state.nTotalTrust = 0;
    state.fFinalized = false;

    // Post-DAG epoch boundaries follow the selected-parent chain, not a
    // height-sorted side effect of local arrival order.
    // R2c.2/S6: authoritative primary selection (value-semantic) +, in
    // authoritative mode, value-semantic boundary resolution: by-value
    // selected-parent walk with the authoritative active-at-height fallback;
    // fail closed on unresolvable boundaries. The legacy resident walk +
    // FindBlockByHeight remain only for genuine legacy mode.
    uint256 hashBestTip(0);
    uint256 hashBoundary(0);
    {
        std::string selError;
        DagTipSelectionResult sel = SelectDagTipForInternalConsumer(mutationPreview, &selError);
        if (sel.status == DAG_TIP_SELECTION_UNAVAILABLE)
        {
            fprintf(stderr, "ComputeEpochState: S6 authoritative selection unavailable: %s\n",
                    selError.c_str());
            fflush(stderr);
            InvalidateDagTipDeltaTransaction();
            return false;
        }
        if (sel.IsUsable())
        {
            hashBestTip = sel.hash;
            std::string berr;
            DagTipSelectionResult bres =
                ResolveAuthoritativeBoundaryAtHeight(hashBestTip, state.nHeightEnd, &berr);
            if (bres.status == DAG_TIP_SELECTION_UNAVAILABLE)
            {
                fprintf(stderr, "ComputeEpochState: S6 authoritative boundary unavailable: %s\n",
                        berr.c_str());
                fflush(stderr);
                InvalidateDagTipDeltaTransaction();
                return false;
            }
            hashBoundary = bres.hash;
        }
        else
        {
            CBlockIndex* pBoundary = NULL;
            CBlockIndex* pBestTip = SelectBestDAGTip();
            if (pBestTip && pBestTip->phashBlock)
                hashBestTip = pBestTip->GetBlockHash();
            if (pBestTip && pBestTip->nHeight >= state.nHeightEnd)
            {
                CBlockIndex* pWalk = pBestTip;
                std::set<uint256> setVisited;
                while (pWalk && pWalk->nHeight > state.nHeightEnd && pWalk->phashBlock)
                {
                    if (!setVisited.insert(pWalk->GetBlockHash()).second)
                        break;
                    uint256 hashParent = GetSelectedParent(pWalk->GetBlockHash());
                    std::map<uint256, CBlockIndex*>::iterator miParent = mapBlockIndex.find(hashParent);
                    if (miParent == mapBlockIndex.end())
                        break;
                    pWalk = miParent->second;
                }
                if (pWalk && pWalk->nHeight == state.nHeightEnd)
                    pBoundary = pWalk;
            }
            if (!pBoundary)
                pBoundary = FindBlockByHeight(state.nHeightEnd);
            if (pBoundary && pBoundary->phashBlock)
                hashBoundary = pBoundary->GetBlockHash();
        }
    }
    if (hashBoundary != 0)
    {
        state.hashBoundaryBlock = hashBoundary;
        setEpochBoundaryBlocks.insert(hashBoundary);
    }

    std::set<uint256> setOrdered;
    if (hashBestTip != 0)
    {
        std::vector<uint256> vOrder = GetDAGLinearOrder(hashBestTip);
        for (const uint256& hashBlock : vOrder)
        {
            std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
            if (mi == mapBlockIndex.end())
                continue;
            if (mi->second->nHeight < state.nHeightStart || mi->second->nHeight > state.nHeightEnd)
                continue;
            if (mi->second->nHeight >= FORK_HEIGHT_DAG && mi->second->IsProofOfStake())
                continue;
            if (!setOrdered.insert(hashBlock).second)
                continue;
            state.vBlockHashes.push_back(hashBlock);
        }
    }

    // Include any DAG-era blocks missing from the selected order using stored
    // DAG order as deterministic fallback.
    std::vector<std::pair<int, uint256>> vEpochBlocks;
    for (const auto& pair : mapDAGData)
    {
        std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(pair.first);
        if (mi == mapBlockIndex.end())
            continue;

        int nBlockHeight = mi->second->nHeight;
        if (nBlockHeight >= state.nHeightStart && nBlockHeight <= state.nHeightEnd)
        {
            if (mi->second->nHeight >= FORK_HEIGHT_DAG && mi->second->IsProofOfStake())
                continue;
            if (!setOrdered.count(pair.first))
                vEpochBlocks.push_back(std::make_pair(pair.second.nDAGOrder, pair.first));
            state.nBlockCount++;

            if (pair.second.fBlue)
                state.nTotalTrust = state.nTotalTrust + mi->second->GetBlockTrust();
        }
    }

    std::sort(vEpochBlocks.begin(), vEpochBlocks.end());
    for (const auto& pair : vEpochBlocks)
        state.vBlockHashes.push_back(pair.second);

    // Epoch-root privacy state is derived from deterministic DAG order.
    // Starting from the previous persisted epoch snapshot avoids mutable
    // per-block curve-tree state after the epoch-root FCMP fork.
    CCurveTree epochCurveTree;
    bool fHavePriorEpochSnapshot = false;
    for (int nPrevEpoch = nEpoch - 1; nPrevEpoch >= 0; nPrevEpoch--)
    {
        std::map<int, CCurveTree>::const_iterator itTree = mapEpochCurveTrees.find(nPrevEpoch);
        if (itTree != mapEpochCurveTrees.end())
        {
            epochCurveTree = itTree->second;
            fHavePriorEpochSnapshot = true;
            break;
        }
    }
    if (!fHavePriorEpochSnapshot)
    {
        CTxDB txdb("r");
        txdb.ReadCurveTree(epochCurveTree);
    }
    if (!epochCurveTree.IsEmpty())
        epochCurveTree.RebuildParentNodes();

    CHashWriter nullifierRootHasher(SER_GETHASH, 0);
    nullifierRootHasher << std::string("Innova/IDAG/EpochNullifierRoot/v1");
    if (nEpoch > 0 && mapEpochState.count(nEpoch - 1))
        nullifierRootHasher << mapEpochState[nEpoch - 1].hashNullifierRoot;
    else
        nullifierRootHasher << uint256(0);
    nullifierRootHasher << nEpoch;

    std::set<uint256> setSeenShieldedNullifiers;
    CFinalityTallyCertificate bestCert;
    bool fHaveBestCert = false;
    bool fInsertEpochOutputs = fHavePriorEpochSnapshot || state.nHeightEnd >= FORK_HEIGHT_EPOCH_ROOT_FCMP;

    for (const uint256& hashBlock : state.vBlockHashes)
    {
        std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlock);
        if (mi == mapBlockIndex.end())
            continue;

        CBlock block;
        if (!block.ReadFromDisk(mi->second))
            continue;

        for (const CTransaction& tx : block.vtx)
        {
            for (const CShieldedOutputDescription& output : tx.vShieldedOutput)
            {
                if (fInsertEpochOutputs)
                    epochCurveTree.InsertLeaf(output.cv);
            }

            for (const CShieldedSpendDescription& spend : tx.vShieldedSpend)
            {
                if (setSeenShieldedNullifiers.insert(spend.nullifier).second)
                    nullifierRootHasher << spend.nullifier;
            }
        }

        std::vector<CFinalityTallyCertificate> vCerts = ExtractFinalityTallyCertificatesFromBlock(block);
        for (const CFinalityTallyCertificate& cert : vCerts)
        {
            if (cert.nEpoch != nEpoch)
                continue;
            if (!fHaveBestCert ||
                cert.nTier > bestCert.nTier ||
                (cert.nTier == bestCert.nTier && cert.GetHash() < bestCert.GetHash()))
            {
                bestCert = cert;
                fHaveBestCert = true;
            }
        }
    }

    if (!epochCurveTree.IsEmpty())
        epochCurveTree.RebuildParentNodes();
    state.hashCurveRoot = epochCurveTree.GetRoot();
    state.hashNullifierRoot = nullifierRootHasher.GetHash();
    state.nFinalityTier = (int)g_finalityTracker.GetFinalityTier();
    state.nConsecutiveHardCount = g_finalityTracker.GetConsecutiveHardEpochCount();
    state.fFinalized = g_finalityTracker.IsFinalized(state.nHeightEnd);
    if (fHaveBestCert)
        state.hashFinalityCertificate = bestCert.GetHash();

    // Transaction counting deferred to RPC layer (getepochinfo) to avoid
    // blocking block processing with disk I/O at every epoch boundary.
    // nTxCount = -1 signals "not yet counted"; RPC can populate on demand.
    state.nTxCount = -1;

    // Check finality
    state.fFinalized = g_finalityTracker.IsFinalized(state.nHeightEnd);

    mapEpochState[nEpoch] = state;
    mapEpochCurveTrees[nEpoch] = epochCurveTree;

    printf("ComputeEpochState: epoch %d (%d-%d), %d blocks, %d txs, curve_root=%s, finalized=%d\n",
           nEpoch, state.nHeightStart, state.nHeightEnd,
           state.nBlockCount, state.nTxCount,
           state.hashCurveRoot.ToString().substr(0,10).c_str(),
           state.fFinalized);

    return true;
}

bool CDAGManager::WriteEpochState(CTxDB& txdb, int nEpoch)
{
    LOCK(cs_dag);

    auto it = mapEpochState.find(nEpoch);
    if (it == mapEpochState.end())
        return false;

    if (!txdb.WriteEpochState(nEpoch, it->second))
        return false;

    auto itTree = mapEpochCurveTrees.find(nEpoch);
    if (itTree != mapEpochCurveTrees.end())
    {
        if (!txdb.WriteCurveTreeAtEpoch(nEpoch, itTree->second))
            return false;
    }

    return true;
}

bool CDAGManager::GetEpochState(int nEpoch, CEpochState& stateOut) const
{
    LOCK(cs_dag);

    auto it = mapEpochState.find(nEpoch);
    if (it == mapEpochState.end())
        return false;

    stateOut = it->second;
    return true;
}

bool CDAGManager::GetLastFinalizedEpochState(CEpochState& stateOut) const
{
    int nFinalizedEpoch = GetEpochForHeight(g_finalityTracker.GetFinalizedHeight());

    LOCK(cs_dag);

    for (int nEpoch = nFinalizedEpoch; nEpoch >= 0; nEpoch--)
    {
        std::map<int, CEpochState>::const_iterator it = mapEpochState.find(nEpoch);
        if (it == mapEpochState.end())
            continue;
        if (it->second.hashCurveRoot == 0)
            continue;
        stateOut = it->second;
        return true;
    }

    return false;
}

int CDAGManager::GetDAGEntryCount() const
{
    LOCK(cs_dag);
    return (int)mapDAGData.size();
}


void CDAGManager::SetPrunedBelowHeight(int nHeight)
{
    LOCK(cs_dag);
    nPrunedBelowHeight = nHeight;
}


// ---------------------------------------------------------------------------
// CDAGManager: DAGKNIGHT Adaptive Ordering (Phase 4)
// ---------------------------------------------------------------------------

int CDAGManager::InferLocalK(const uint256& hashBlock) const
{
    // No lock — caller holds cs_dag
    // Determinism: use each ancestor's already-stored nInferredK (computed at
    // their own coloring time) rather than recomputing against a stale blue set.
    // For the current block, compute its own anticone against its selected parent.
    auto it = mapDAGData.find(hashBlock);
    if (it == mapDAGData.end())
        return 0;

    uint256 hashSelectedParent = GetSelectedParent(hashBlock);
    if (hashSelectedParent == 0)
        return 0;

    // Compute this block's anticone against its own selected parent's blue set
    std::set<uint256> blueSet = GetBlueSetCached(hashSelectedParent);
    int nAnticone = AnticoneSize(hashBlock, blueSet);

    // Clamp seed to ceiling to prevent single outlier from dominating EMA
    int nSeedAnticone = std::min(nAnticone, DAGKNIGHT_K_CEILING);

    // Sample stored nInferredK from ancestors (deterministic — values were
    // computed at coloring time before any pruning occurred)
    // Use EMA smoothing for stable k estimation
    int nEMAk = nSeedAnticone * 256; // fixed-point (*256), clamped seed
    uint256 hashWalk = hashSelectedParent;
    int nSamples = 0;

    while (nSamples < DAGKNIGHT_K_SAMPLE_DEPTH && hashWalk != 0)
    {
        auto wit = mapDAGData.find(hashWalk);
        if (wit == mapDAGData.end())
            break;

        if (wit->second.nInferredK >= 0)
        {
            // EMA: k_new = alpha * sample + (1 - alpha) * k_old
            nEMAk = (DAGKNIGHT_K_EMA_ALPHA * wit->second.nInferredK * 256
                     + (256 - DAGKNIGHT_K_EMA_ALPHA) * nEMAk) / 256;

        }

        hashWalk = GetSelectedParent(hashWalk);
        nSamples++;
    }

    // Use EMA estimate only (not max — max is dominated by outliers, allowing k inflation)
    int nResult = (nEMAk + 128) / 256; // round from fixed-point

    // Apply floor and ceiling
    if (nResult < DAGKNIGHT_K_FLOOR)
        nResult = DAGKNIGHT_K_FLOOR;
    if (nResult > DAGKNIGHT_K_CEILING)
        nResult = DAGKNIGHT_K_CEILING;

    return nResult;
}



void CDAGManager::ColorBlockDAGKnight(CBlockIndex* pindex)
{
    std::string error;
    (void)ColorBlockDAGKnightImpl(pindex, false, &error);
}

bool CDAGManager::ColorBlockDAGKnightAuthoritative(CBlockIndex* pindex, std::string* error)
{
    return ColorBlockDAGKnightImpl(pindex, g_fAuthoritativeStartup, error);
}

bool CDAGManager::ColorBlockDAGKnightImpl(CBlockIndex* pindex, bool fAuthoritativeParentScore,
                                          std::string* error)
{
    if (error) error->clear();
    std::map<uint256, CBlockIndex*>& mapBlockIndex = RecolorBlockIndex();
    LOCK(cs_dag);

    if (!pindex || !pindex->phashBlock)
        return true;
    if (pindex->nHeight >= FORK_HEIGHT_DAG && pindex->IsProofOfStake())
        return true;

    uint256 hash = pindex->GetBlockHash();
    auto it = mapDAGData.find(hash);
    if (it == mapDAGData.end())
        return true;

    CBlockDAGData& data = it->second;
    const std::vector<uint256>& vParents = data.vDAGParents;

    if (vParents.empty())
    {
        data.fBlue = true;
        data.nDAGScore = pindex->GetBlockTrust();
        data.nInferredK = 0;
        return true;
    }

    uint256 hashSelectedParent;
    uint256 nBestParentScore = 0;

    for (const uint256& hashParent : vParents)
    {
        // F2: ONE logical parent-score resolver (see ColorBlockImpl). B-1: the
        // same accept-path policy applies.
        DAGParentScoreResult pres =
            ResolveDagParentScore(hashParent, fAuthoritativeParentScore, error,
                                  DAGParentScorePolicy::MUTATION);
        if (pres.status == DAGParentScoreStatus::FAILURE)
            return false;
        const uint256 nParentScore = pres.score;

        bool fIsPrimary = (hashParent == vParents[0]);
        if (nParentScore > nBestParentScore ||
            (nParentScore == nBestParentScore && (hashSelectedParent == 0 ||
             (fIsPrimary ? true : hashParent < hashSelectedParent))))
        {
            nBestParentScore = nParentScore;
            hashSelectedParent = hashParent;
        }
    }

    if (hashSelectedParent == 0)
    {
        if (pindex->pprev)
            data.nDAGScore = pindex->pprev->nChainTrust + pindex->GetBlockTrust();
        else
            data.nDAGScore = pindex->GetBlockTrust();
        data.fBlue = true;
        data.nInferredK = 0;
        return true;
    }

    // DAGKNIGHT: Infer local k from DAG structure
    int nLocalK = InferLocalK(hash);
    if (nLocalK < DAGKNIGHT_K_FLOOR)
    {
        printf("ColorBlockDAGKnight: inferred k %d below floor %d for %s, clamping\n",
               nLocalK, DAGKNIGHT_K_FLOOR, hash.ToString().substr(0,20).c_str());
        nLocalK = DAGKNIGHT_K_FLOOR;
    }
    data.nInferredK = nLocalK;

    // Inherit blue set from selected parent
    std::set<uint256> blueSet = GetBlueSet(hashSelectedParent);
    std::set<uint256> selectedParentBlue = blueSet;

    // Merge parents' blue blocks using adaptive k
    for (const uint256& hashParent : vParents)
    {
        if (hashParent == hashSelectedParent)
            continue;

        auto pit = mapDAGData.find(hashParent);
        if (pit == mapDAGData.end())
            continue;

        std::set<uint256> mergeBlue = GetBlueSet(hashParent);

        for (const uint256& hashCandidate : mergeBlue)
        {
            if (blueSet.count(hashCandidate))
                continue;

            // DAGKNIGHT: Use inferred k instead of fixed GHOSTDAG_K
            int nAnticone = AnticoneSize(hashCandidate, blueSet);
            if (nAnticone <= nLocalK)
            {
                blueSet.insert(hashCandidate);
                auto cit = mapDAGData.find(hashCandidate);
                if (cit != mapDAGData.end())
                    cit->second.fBlue = true;
            }
            else
            {
                auto cit = mapDAGData.find(hashCandidate);
                if (cit != mapDAGData.end())
                    cit->second.fBlue = false;
            }
        }
    }

    // This block is always blue
    data.fBlue = true;
    blueSet.insert(hash);

    // Compute score: selected parent score + this block trust + newly-blue merge blocks
    uint256 nScore = nBestParentScore + pindex->GetBlockTrust();
    for (const uint256& hashBlue : blueSet)
    {
        if (hashBlue == hash)
            continue;
        if (selectedParentBlue.count(hashBlue))
            continue;
        std::map<uint256, CBlockIndex*>::iterator mi = mapBlockIndex.find(hashBlue);
        if (mi != mapBlockIndex.end() &&
            !(mi->second->nHeight >= FORK_HEIGHT_DAG && mi->second->IsProofOfStake()))
            nScore = nScore + mi->second->GetBlockTrust();
    }
    data.nDAGScore = nScore;
    return true;
}
