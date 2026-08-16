// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_IBDHEADERSCHEDULER_H
#define INNOVA_IBDHEADERSCHEDULER_H

#include "uint256.h"

#include <cstddef>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

/**
 * Bounded header lookahead (Stage 3): the header graph may run at most
 * (window + CAP_MARGIN) ahead of the anchor (connected frontier).  Header
 * continuation stops once that cap is reached; fetching resumes when the
 * lookahead decays to (window + RESUME_MARGIN), keeping a delivery margin so
 * the ordered block pipeline window stays full.  The cap bounds both graph
 * memory and BuildContinuationLocator cost (O(lookahead) per continuation).
 * Synced from the configurable block window W (-ibdblockwindow) at runtime.
 */
static const std::size_t IBD_HEADER_LOOKAHEAD_CAP_MARGIN = 2000;
static const std::size_t IBD_HEADER_LOOKAHEAD_RESUME_MARGIN = 1000;

/**
 * Scheduler-only structural knowledge about one header hash.
 *
 * This type carries no proof or consensus-validity claim.  In particular it
 * is deliberately independent of CBlockIndex and mapBlockIndex.
 */
struct CIbdHeaderNode
{
    enum State
    {
        AUTHORITATIVE_ANCHOR,
        ACTIVE,
        ELIGIBLE,
        QUARANTINED
    };

    uint256 hash;
    uint256 prev;
    int height;
    State state;
    bool permanently_quarantined;
    std::set<uint256> children;

    CIbdHeaderNode();
    CIbdHeaderNode(const uint256& hashIn, const uint256& prevIn);

    bool IsAnchored() const { return height >= 0; }
    bool IsUsable() const
    {
        return IsAnchored() && !permanently_quarantined &&
               state != QUARANTINED;
    }
};

/**
 * Isolated provisional header graph for future IBD scheduling.
 *
 * Locking: none.  The owning scheduler must serialize every call (the Stage 1
 * design requires cs_main).  Tests use it single-threaded.
 *
 * Lifetime: nodes are owned by value in m_nodes and refer to relatives only by
 * hash.  No pointer into authoritative block-index state is retained.
 */
class CIbdHeaderGraph
{
public:
    enum InsertResult
    {
        INSERTED_ACTIVE,
        INSERTED_ELIGIBLE,
        INSERTED_QUARANTINED,
        DUPLICATE,
        CONFLICT
    };

    CIbdHeaderGraph();

    void Clear();
    bool Empty() const { return m_nodes.empty(); }
    std::size_t Size() const { return m_nodes.size(); }
    uint64_t FastAnchorAdvanceCount() const { return m_fast_anchor_advances; }
    uint64_t FullReanchorCount() const { return m_full_reanchors; }

    /**
     * Number of MarkActivePath invocations and of graph nodes whose state was
     * rewritten by the most recent (and cumulative) invocation.  Exposed for
     * deterministic tests and IBD diagnostics: forward extension must touch
     * only the inserted delta, never the accumulated header-graph size.
     */
    uint64_t MarkActivePathCalls() const { return m_mark_active_path_calls; }
    uint64_t MarkActivePathTouchedTotal() const
    { return m_mark_active_path_touched_total; }
    uint64_t LastMarkActivePathTouched() const
    { return m_last_mark_active_path_touched; }

    /**
     * Test-only switch routing MarkActivePath through the pre-optimization
     * full-graph implementation, so differential tests can prove the delta
     * implementation preserves observable state exactly.
     */
    void SetUseLegacyMarkActivePathForTesting(bool useLegacy)
    { m_use_legacy_mark_active_path = useLegacy; }

    /**
     * Number of successor probes performed by the most recent GetActiveWindow
     * call.  Exposed for deterministic tests: it must be bounded by
     * windowSize (independent of header-graph lookahead), and identical for
     * graphs that differ only in lookahead depth.
     */
    std::size_t LastGetActiveWindowSteps() const
    { return m_last_get_active_window_steps; }

    /** Reset the graph around one immutable authoritative hash/height anchor. */
    bool SetAuthoritativeAnchor(const uint256& hash, int height);
    /** Move the authoritative anchor, retaining only its active descendants. */
    bool Reanchor(const uint256& hash, int height);
    bool HasAnchor() const { return m_has_anchor; }
    const uint256& AnchorHash() const { return m_anchor_hash; }
    int AnchorHeight() const { return m_anchor_height; }

    /** Insert structural identity.  Height is always derived from ancestry. */
    InsertResult Insert(const uint256& hash, const uint256& prev);
    /** Insert one structurally contiguous response in one graph pass. */
    std::vector<InsertResult> InsertBatch(
        const std::vector<std::pair<uint256, uint256> >& headers);

    const CIbdHeaderNode* Lookup(const uint256& hash) const;

    /** Return hash's ancestor at targetHeight; self is allowed. */
    bool GetAncestor(const uint256& hash, int targetHeight,
                     uint256& ancestorOut) const;
    bool IsDescendantOf(const uint256& hash,
                        const uint256& ancestor) const;

    /**
     * Select an already anchored, non-quarantined branch as the active
     * scheduler path.  This is explicit scheduler eligibility, not fork choice.
     */
    bool ActivateBranch(const uint256& tip);

    /** Permanently quarantine hash and all known descendants. */
    bool QuarantineBranch(const uint256& hash);

    const CIbdHeaderNode* ActiveTip() const;
    const CIbdHeaderNode* BestKnownEligibleTip() const;

    /**
     * Enumerate the active path strictly after frontier, capped at windowSize.
     * Returns empty if frontier is not on the active path.
     *
     * Cost is O(windowSize): the walk advances forward from frontier via the
     * unique ACTIVE successor (GetActiveSuccessor) and never traverses the
     * header-graph lookahead between frontier and the active tip.
     */
    std::vector<uint256> GetActiveWindow(const uint256& frontier,
                                         std::size_t windowSize) const;

    bool GetActiveSuccessor(const uint256& hash, uint256& successorOut) const;

    /** Newest provisional hashes first; authoritative anchor last. */
    std::vector<uint256> BuildContinuationLocator(
        std::size_t maxEntries = 32) const;

    /** Expensive consistency check intended for deterministic tests. */
    bool CheckInvariants() const;

private:
    typedef std::map<uint256, CIbdHeaderNode> NodeMap;

    NodeMap m_nodes;
    bool m_has_anchor;
    uint256 m_anchor_hash;
    int m_anchor_height;
    uint256 m_active_tip;
    uint64_t m_fast_anchor_advances;
    uint64_t m_full_reanchors;
    mutable std::size_t m_last_get_active_window_steps;
    bool m_use_legacy_mark_active_path;
    uint64_t m_mark_active_path_calls;
    uint64_t m_mark_active_path_touched_total;
    uint64_t m_last_mark_active_path_touched;

    void ConnectDescendants(const uint256& parentHash);
    void ExtendActiveTipIfUnambiguous();
    void MarkActivePath(const uint256& tip);
    void MarkActivePathLegacy(const uint256& tip);
    void QuarantineDescendants(const uint256& hash);
};


/** Stage-1 observation state. It never selects, requests, or owns blocks.
 * Locking: none. Runtime callers hold cs_main; tests are single-threaded. */
class CIbdHeadersObserver
{
public:
    enum Classification { IN_PREDICTED_WINDOW, BEFORE_WINDOW, AFTER_WINDOW,
                          OFF_ACTIVE_BRANCH, UNKNOWN_TO_GRAPH };
    struct Counters
    {
        uint64_t headerRequests, headerResponses, accepted, duplicates;
        uint64_t disconnected, quarantined, activeBranchSwitches, anchorUpdates;
        uint64_t classified[3][5];
        Counters();
    };
    struct HeaderResult
    {
        bool expectedResponse, continueHeaders;
        std::vector<uint256> continuationLocator;
        HeaderResult() : expectedResponse(false), continueHeaders(false) {}
    };

    explicit CIbdHeadersObserver(std::size_t windowSize = 512);
    void SetEnabled(bool enabled);
    bool Enabled() const { return m_enabled; }
    void Clear();
    bool UpdateAnchor(const uint256& hash, int height);
    void MarkHeaderRequest(int64_t peer);
    bool IsHeaderResponseExpected(int64_t peer) const;
    void RemovePeer(int64_t peer);
    HeaderResult ObserveHeaders(int64_t peer,
        const std::vector<std::pair<uint256, uint256> >& headers,
        std::size_t continuationBatchSize = 2000);
    Classification Classify(const uint256& hash, int authoritativeHeight = -1) const;
    void RecordClassification(unsigned int eventKind, Classification classification);
    std::vector<uint256> PredictedWindow() const;
    std::vector<uint256> PredictedWindowFromFrontier(const uint256& frontier) const;
    std::size_t PeerSupport(const uint256& hash) const;
    std::vector<int64_t> HeaderSources(const uint256& hash) const;
    std::map<int64_t, int> ActiveHeaderSourceClaims() const;
    const CIbdHeaderGraph& Graph() const { return m_graph; }
    const Counters& Stats() const { return m_counters; }
    std::size_t WindowSize() const { return m_window_size; }

    /**
     * Stage 3 bounded lookahead: cap is the maximum lookahead (graph tip
     * height - anchor height) at which header continuation stops; resume is
     * the lookahead at which the runtime re-fetches headers.  Synced from the
     * configurable block window W: cap = W + CAP_MARGIN, resume = W +
     * RESUME_MARGIN.
     */
    void SetLookaheadCap(std::size_t cap, std::size_t resume)
    { m_lookahead_cap = cap; m_lookahead_resume = resume; }
    std::size_t LookaheadCap() const { return m_lookahead_cap; }
    std::size_t LookaheadResume() const { return m_lookahead_resume; }

    /**
     * Number of m_sources entries examined during the most recent
     * UpdateAnchor call.  Exposed for deterministic regression tests: normal
     * fast anchor advancement must not examine any source record, regardless
     * of the accumulated header-graph size.
     */
    std::size_t LastAnchorSourceSweepExamined() const
    { return m_lastAnchorSourceSweepExamined; }

    static const char* ClassificationName(Classification classification);

private:
    bool m_enabled;
    std::size_t m_window_size;
    std::size_t m_lookahead_cap;
    std::size_t m_lookahead_resume;
    CIbdHeaderGraph m_graph;
    std::set<int64_t> m_outstanding_peers;
    std::map<uint256, std::set<int64_t> > m_sources;
    std::map<int64_t, uint256> m_active_source_claims;
    Counters m_counters;
    std::size_t m_lastAnchorSourceSweepExamined;
};

#endif // INNOVA_IBDHEADERSCHEDULER_H
