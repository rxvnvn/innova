// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ibdheaderscheduler.h"
#include "util.h"

#include <algorithm>

CIbdHeaderNode::CIbdHeaderNode()
    : height(-1), state(QUARANTINED), permanently_quarantined(false)
{
}

CIbdHeaderNode::CIbdHeaderNode(const uint256& hashIn, const uint256& prevIn)
    : hash(hashIn), prev(prevIn), height(-1), state(QUARANTINED),
      permanently_quarantined(false)
{
}

CIbdHeaderGraph::CIbdHeaderGraph()
    : m_has_anchor(false), m_anchor_height(-1),
      m_fast_anchor_advances(0), m_full_reanchors(0)
{
}

void CIbdHeaderGraph::Clear()
{
    m_nodes.clear();
    m_has_anchor = false;
    m_anchor_hash = 0;
    m_anchor_height = -1;
    m_active_tip = 0;
}

bool CIbdHeaderGraph::SetAuthoritativeAnchor(const uint256& hash, int height)
{
    if (hash == 0 || height < 0)
        return false;

    Clear();
    CIbdHeaderNode anchor(hash, uint256(0));
    anchor.height = height;
    anchor.state = CIbdHeaderNode::AUTHORITATIVE_ANCHOR;
    m_nodes.insert(std::make_pair(hash, anchor));
    m_has_anchor = true;
    m_anchor_hash = hash;
    m_anchor_height = height;
    m_active_tip = hash;
    return true;
}

bool CIbdHeaderGraph::Reanchor(const uint256& hash, int height)
{
    if (!m_has_anchor || hash == 0 || height < 0)
        return false;
    if (hash == m_anchor_hash && height == m_anchor_height)
        return true;

    NodeMap::iterator oldAnchor = m_nodes.find(m_anchor_hash);
    NodeMap::iterator nextAnchor = m_nodes.find(hash);
    if (oldAnchor != m_nodes.end() && nextAnchor != m_nodes.end() &&
        height == m_anchor_height + 1 &&
        nextAnchor->second.prev == m_anchor_hash &&
        nextAnchor->second.height == height &&
        nextAnchor->second.state == CIbdHeaderNode::ACTIVE &&
        !nextAnchor->second.permanently_quarantined)
    {
        oldAnchor->second.state = CIbdHeaderNode::ELIGIBLE;
        nextAnchor->second.state = CIbdHeaderNode::AUTHORITATIVE_ANCHOR;
        m_anchor_hash = hash;
        m_anchor_height = height;
        ++m_fast_anchor_advances;
        return true;
    }

    ++m_full_reanchors;
    std::vector<uint256> suffix;
    if (IsDescendantOf(m_active_tip, hash))
        suffix = GetActiveWindow(hash, m_nodes.size());
    SetAuthoritativeAnchor(hash, height);
    for (std::vector<uint256>::const_iterator it = suffix.begin();
         it != suffix.end(); ++it)
    {
        const uint256 prev = it == suffix.begin() ? hash : *(it - 1);
        Insert(*it, prev);
    }
    return true;
}

CIbdHeaderGraph::InsertResult CIbdHeaderGraph::Insert(
    const uint256& hash, const uint256& prev)
{
    if (hash == 0 || prev == hash)
        return CONFLICT;

    NodeMap::iterator existing = m_nodes.find(hash);
    if (existing != m_nodes.end())
        return existing->second.prev == prev ? DUPLICATE : CONFLICT;

    CIbdHeaderNode node(hash, prev);
    std::pair<NodeMap::iterator, bool> inserted =
        m_nodes.insert(std::make_pair(hash, node));
    CIbdHeaderNode& stored = inserted.first->second;

    NodeMap::iterator parent = m_nodes.find(prev);
    if (parent != m_nodes.end())
    {
        parent->second.children.insert(hash);
        if (parent->second.IsAnchored() &&
            !parent->second.permanently_quarantined)
        {
            stored.height = parent->second.height + 1;
            stored.state = CIbdHeaderNode::ELIGIBLE;
        }
    }

    // A child may have arrived before this parent.  Attach and derive the
    // whole now-connected subtree without trusting any remote height.
    for (NodeMap::iterator it = m_nodes.begin(); it != m_nodes.end(); ++it)
    {
        if (it->first != hash && it->second.prev == hash)
            stored.children.insert(it->first);
    }
    ConnectDescendants(hash);

    const bool extendsActive = stored.IsUsable() && prev == m_active_tip;
    if (extendsActive)
    {
        MarkActivePath(hash);
        ExtendActiveTipIfUnambiguous();
    }

    const CIbdHeaderNode* resultNode = Lookup(hash);
    if (resultNode && resultNode->state == CIbdHeaderNode::ACTIVE)
        return INSERTED_ACTIVE;
    if (resultNode && resultNode->IsUsable())
        return INSERTED_ELIGIBLE;
    return INSERTED_QUARANTINED;
}

std::vector<CIbdHeaderGraph::InsertResult> CIbdHeaderGraph::InsertBatch(
    const std::vector<std::pair<uint256, uint256> >& headers)
{
    std::vector<InsertResult> results(headers.size(), INSERTED_QUARANTINED);
    if (headers.empty()) return results;
    std::vector<uint256> inserted;
    inserted.reserve(headers.size());
    for (std::size_t i = 0; i < headers.size(); ++i) {
        const uint256& hash = headers[i].first;
        const uint256& prev = headers[i].second;
        if (hash == 0 || hash == prev) { results[i] = CONFLICT; continue; }
        NodeMap::iterator existing = m_nodes.find(hash);
        if (existing != m_nodes.end()) {
            results[i] = existing->second.prev == prev ? DUPLICATE : CONFLICT;
            continue;
        }
        m_nodes.insert(std::make_pair(hash, CIbdHeaderNode(hash, prev)));
        inserted.push_back(hash);
    }
    // Link only new nodes; do not scan the complete graph for each header.
    for (std::vector<uint256>::const_iterator it = inserted.begin();
         it != inserted.end(); ++it) {
        NodeMap::iterator node = m_nodes.find(*it);
        NodeMap::iterator parent = m_nodes.find(node->second.prev);
        if (parent != m_nodes.end()) parent->second.children.insert(node->first);
    }
    // Ordered responses permit one forward height propagation pass.
    for (std::size_t i = 0; i < headers.size(); ++i) {
        NodeMap::iterator node = m_nodes.find(headers[i].first);
        if (node == m_nodes.end() || node->second.height >= 0) continue;
        NodeMap::iterator parent = m_nodes.find(node->second.prev);
        if (parent != m_nodes.end() && parent->second.IsAnchored() &&
            !parent->second.permanently_quarantined) {
            node->second.height = parent->second.height + 1;
            node->second.state = CIbdHeaderNode::ELIGIBLE;
        }
    }
    uint256 candidateTip = m_active_tip;
    for (std::vector<uint256>::const_iterator it = inserted.begin();
         it != inserted.end(); ++it) {
        NodeMap::const_iterator node = m_nodes.find(*it);
        if (node != m_nodes.end() && node->second.IsUsable() &&
            IsDescendantOf(node->first, candidateTip)) candidateTip = node->first;
    }
    if (candidateTip != m_active_tip) {
        MarkActivePath(candidateTip);
        ExtendActiveTipIfUnambiguous();
    }
    for (std::size_t i = 0; i < headers.size(); ++i) {
        NodeMap::const_iterator node = m_nodes.find(headers[i].first);
        if (results[i] == DUPLICATE || results[i] == CONFLICT) continue;
        if (node != m_nodes.end() && node->second.state == CIbdHeaderNode::ACTIVE)
            results[i] = INSERTED_ACTIVE;
        else if (node != m_nodes.end() && node->second.IsUsable())
            results[i] = INSERTED_ELIGIBLE;
    }
    return results;
}

const CIbdHeaderNode* CIbdHeaderGraph::Lookup(const uint256& hash) const
{
    NodeMap::const_iterator it = m_nodes.find(hash);
    return it == m_nodes.end() ? NULL : &it->second;
}

bool CIbdHeaderGraph::GetAncestor(const uint256& hash, int targetHeight,
                                  uint256& ancestorOut) const
{
    const CIbdHeaderNode* node = Lookup(hash);
    if (!node || !node->IsAnchored() || targetHeight < m_anchor_height ||
        targetHeight > node->height)
        return false;

    while (node->height > targetHeight)
    {
        node = Lookup(node->prev);
        if (!node)
            return false;
    }
    ancestorOut = node->hash;
    return true;
}

bool CIbdHeaderGraph::IsDescendantOf(const uint256& hash,
                                     const uint256& ancestor) const
{
    const CIbdHeaderNode* node = Lookup(hash);
    const CIbdHeaderNode* ancestorNode = Lookup(ancestor);
    if (!node || !ancestorNode || !node->IsAnchored() ||
        !ancestorNode->IsAnchored() || ancestorNode->height > node->height)
        return false;
    uint256 found;
    return GetAncestor(hash, ancestorNode->height, found) && found == ancestor;
}

bool CIbdHeaderGraph::ActivateBranch(const uint256& tip)
{
    const CIbdHeaderNode* node = Lookup(tip);
    if (!node || !node->IsUsable() ||
        !IsDescendantOf(tip, m_anchor_hash))
        return false;
    MarkActivePath(tip);
    return true;
}

bool CIbdHeaderGraph::QuarantineBranch(const uint256& hash)
{
    NodeMap::iterator it = m_nodes.find(hash);
    if (it == m_nodes.end() || hash == m_anchor_hash)
        return false;

    uint256 fallback = it->second.prev;
    const bool affectsActive = IsDescendantOf(m_active_tip, hash);
    QuarantineDescendants(hash);
    if (affectsActive)
    {
        const CIbdHeaderNode* parent = Lookup(fallback);
        if (parent && parent->IsUsable())
            MarkActivePath(fallback);
        else
            MarkActivePath(m_anchor_hash);
    }
    return true;
}

const CIbdHeaderNode* CIbdHeaderGraph::ActiveTip() const
{
    return Lookup(m_active_tip);
}

bool CIbdHeaderGraph::GetActiveSuccessor(const uint256& hash,
                                          uint256& successorOut) const
{
    const CIbdHeaderNode* node = Lookup(hash);
    if (!node) return false;
    uint256 candidate;
    int usable = 0;
    for (std::set<uint256>::const_iterator it = node->children.begin();
         it != node->children.end(); ++it)
    {
        const CIbdHeaderNode* child = Lookup(*it);
        if (child && child->state == CIbdHeaderNode::ACTIVE &&
            child->IsUsable())
        {
            candidate = child->hash;
            ++usable;
        }
    }
    if (usable != 1) return false;
    successorOut = candidate;
    return true;
}

const CIbdHeaderNode* CIbdHeaderGraph::BestKnownEligibleTip() const
{
    const CIbdHeaderNode* best = NULL;
    for (NodeMap::const_iterator it = m_nodes.begin(); it != m_nodes.end(); ++it)
    {
        const CIbdHeaderNode& node = it->second;
        if (!node.IsUsable())
            continue;
        if (!best || node.height > best->height ||
            (node.height == best->height && node.hash < best->hash))
            best = &node;
    }
    return best;
}

std::vector<uint256> CIbdHeaderGraph::GetActiveWindow(
    const uint256& frontier, std::size_t windowSize) const
{
    std::vector<uint256> reversePath;
    if (windowSize == 0 || !IsDescendantOf(m_active_tip, frontier))
        return reversePath;

    const CIbdHeaderNode* node = ActiveTip();
    while (node && node->hash != frontier)
    {
        if (node->state != CIbdHeaderNode::ACTIVE || !node->IsUsable())
            return std::vector<uint256>();
        reversePath.push_back(node->hash);
        node = Lookup(node->prev);
    }
    if (!node)
        return std::vector<uint256>();

    std::reverse(reversePath.begin(), reversePath.end());
    if (reversePath.size() > windowSize)
        reversePath.resize(windowSize);
    return reversePath;
}

std::vector<uint256> CIbdHeaderGraph::BuildContinuationLocator(
    std::size_t maxEntries) const
{
    std::vector<uint256> locator;
    if (!m_has_anchor || maxEntries == 0)
        return locator;
    if (maxEntries == 1 || m_active_tip == m_anchor_hash)
    {
        locator.push_back(m_anchor_hash);
        return locator;
    }
    const CIbdHeaderNode* cursor = ActiveTip();
    int step = 1;
    while (cursor && cursor->hash != m_anchor_hash &&
           locator.size() < maxEntries - 1)
    {
        if (cursor->state != CIbdHeaderNode::ACTIVE || !cursor->IsUsable())
            break;
        locator.push_back(cursor->hash);
        const int target = cursor->height - step;
        if (target <= m_anchor_height)
            break;
        uint256 ancestor;
        if (!GetAncestor(cursor->hash, target, ancestor))
            break;
        cursor = Lookup(ancestor);
        if (locator.size() > 10)
            step *= 2;
    }
    locator.push_back(m_anchor_hash);
    return locator;
}

bool CIbdHeaderGraph::CheckInvariants() const
{
    if (!m_has_anchor)
        return m_nodes.empty() && m_anchor_height == -1 && m_active_tip == 0;

    const CIbdHeaderNode* anchor = Lookup(m_anchor_hash);
    if (!anchor || anchor->height != m_anchor_height ||
        anchor->state != CIbdHeaderNode::AUTHORITATIVE_ANCHOR ||
        anchor->permanently_quarantined)
        return false;

    const CIbdHeaderNode* activeTip = ActiveTip();
    if (!activeTip || !activeTip->IsUsable() ||
        !IsDescendantOf(m_active_tip, m_anchor_hash))
        return false;

    for (NodeMap::const_iterator it = m_nodes.begin(); it != m_nodes.end(); ++it)
    {
        const CIbdHeaderNode& node = it->second;
        if (node.hash != it->first)
            return false;
        if (node.hash == m_anchor_hash)
            continue;

        const CIbdHeaderNode* parent = Lookup(node.prev);
        if (node.IsAnchored())
        {
            if (node.height >= m_anchor_height && ( !parent || !parent->IsAnchored() ||
                node.height != parent->height + 1))
                return false;
        }
        if (node.state == CIbdHeaderNode::ACTIVE &&
            (!node.IsUsable() || !IsDescendantOf(m_active_tip, node.hash)))
            return false;
        if (node.permanently_quarantined &&
            node.state != CIbdHeaderNode::QUARANTINED)
            return false;
        if (parent && parent->children.count(node.hash) == 0)
            return false;
    }
    return true;
}

void CIbdHeaderGraph::ConnectDescendants(const uint256& parentHash)
{
    NodeMap::iterator parent = m_nodes.find(parentHash);
    if (parent == m_nodes.end())
        return;

    for (std::set<uint256>::const_iterator childHash =
             parent->second.children.begin();
         childHash != parent->second.children.end(); ++childHash)
    {
        NodeMap::iterator child = m_nodes.find(*childHash);
        if (child == m_nodes.end())
            continue;
        if (child->second.permanently_quarantined ||
            !parent->second.IsAnchored() ||
            parent->second.permanently_quarantined)
        {
            child->second.height = -1;
            child->second.state = CIbdHeaderNode::QUARANTINED;
        }
        else
        {
            child->second.height = parent->second.height + 1;
            child->second.state = CIbdHeaderNode::ELIGIBLE;
        }
        ConnectDescendants(child->first);
    }
}

void CIbdHeaderGraph::ExtendActiveTipIfUnambiguous()
{
    while (true)
    {
        NodeMap::iterator tip = m_nodes.find(m_active_tip);
        if (tip == m_nodes.end())
            return;
        uint256 onlyChild;
        int usableChildren = 0;
        for (std::set<uint256>::const_iterator it = tip->second.children.begin();
             it != tip->second.children.end(); ++it)
        {
            const CIbdHeaderNode* child = Lookup(*it);
            if (child && child->IsUsable())
            {
                onlyChild = *it;
                ++usableChildren;
            }
        }
        if (usableChildren != 1)
            return;
        MarkActivePath(onlyChild);
    }
}

void CIbdHeaderGraph::MarkActivePath(const uint256& tip)
{
    for (NodeMap::iterator it = m_nodes.begin(); it != m_nodes.end(); ++it)
    {
        if (it->second.state == CIbdHeaderNode::ACTIVE)
            it->second.state = CIbdHeaderNode::ELIGIBLE;
    }

    uint256 cursor = tip;
    while (cursor != m_anchor_hash)
    {
        NodeMap::iterator it = m_nodes.find(cursor);
        if (it == m_nodes.end() || !it->second.IsUsable())
            return;
        it->second.state = CIbdHeaderNode::ACTIVE;
        cursor = it->second.prev;
    }
    m_active_tip = tip;
}

void CIbdHeaderGraph::QuarantineDescendants(const uint256& hash)
{
    NodeMap::iterator node = m_nodes.find(hash);
    if (node == m_nodes.end())
        return;
    node->second.permanently_quarantined = true;
    node->second.state = CIbdHeaderNode::QUARANTINED;
    for (std::set<uint256>::const_iterator it = node->second.children.begin();
         it != node->second.children.end(); ++it)
        QuarantineDescendants(*it);
}

CIbdHeadersObserver::Counters::Counters()
    : headerRequests(0), headerResponses(0), accepted(0), duplicates(0),
      disconnected(0), quarantined(0), activeBranchSwitches(0), anchorUpdates(0)
{
    std::fill(&classified[0][0], &classified[0][0] + 15, 0);
}

CIbdHeadersObserver::CIbdHeadersObserver(std::size_t windowSize)
    : m_enabled(false), m_window_size(windowSize)
{
}

void CIbdHeadersObserver::SetEnabled(bool enabled)
{
    if (m_enabled == enabled) return;
    Clear();
    m_enabled = enabled;
}

void CIbdHeadersObserver::Clear()
{
    m_graph.Clear();
    m_outstanding_peers.clear();
    m_sources.clear();
    m_counters = Counters();
}
bool CIbdHeadersObserver::UpdateAnchor(const uint256& hash, int height)
{
    if (!m_enabled || hash == 0 || height < 0) return false;
    if (m_graph.HasAnchor() && m_graph.AnchorHash() == hash &&
        m_graph.AnchorHeight() == height) return true;
    const int oldHeight = m_graph.AnchorHeight();
    const uint64_t fastBefore = m_graph.FastAnchorAdvanceCount();
    const uint64_t fullBefore = m_graph.FullReanchorCount();
    const int64_t startUs = GetTimeMicros();
    bool ok = m_graph.HasAnchor() ? m_graph.Reanchor(hash, height) :
                                   m_graph.SetAuthoritativeAnchor(hash, height);
    if (ok) {
        ++m_counters.anchorUpdates;
        for (std::map<uint256, std::set<int64_t> >::iterator it = m_sources.begin();
             it != m_sources.end();)
            if (!m_graph.Lookup(it->first)) m_sources.erase(it++); else ++it;
        printf("IBD_HEADER_ANCHOR event=update old_height=%d new_height=%d duration_us=%lld fast=%llu full=%llu\n",
               oldHeight, height, (long long)(GetTimeMicros() - startUs),
               (unsigned long long)(m_graph.FastAnchorAdvanceCount() - fastBefore),
               (unsigned long long)(m_graph.FullReanchorCount() - fullBefore));
    }
    return ok;
}

void CIbdHeadersObserver::MarkHeaderRequest(int64_t peer)
{
    if (!m_enabled) return;
    m_outstanding_peers.insert(peer); ++m_counters.headerRequests;
}

bool CIbdHeadersObserver::IsHeaderResponseExpected(int64_t peer) const
{ return m_enabled && m_outstanding_peers.count(peer) != 0; }

void CIbdHeadersObserver::RemovePeer(int64_t peer)
{
    m_outstanding_peers.erase(peer);
    for (std::map<uint256, std::set<int64_t> >::iterator it = m_sources.begin();
         it != m_sources.end(); ++it)
        it->second.erase(peer);
}

CIbdHeadersObserver::HeaderResult CIbdHeadersObserver::ObserveHeaders(
    int64_t peer, const std::vector<std::pair<uint256, uint256> >& headers,
    std::size_t continuationBatchSize)
{
    HeaderResult result;
    if (!m_enabled || !m_graph.HasAnchor()) return result;
    result.expectedResponse = m_outstanding_peers.erase(peer) != 0;
    ++m_counters.headerResponses;
    const uint256 oldTip = m_graph.ActiveTip() ? m_graph.ActiveTip()->hash : uint256(0);
    const std::vector<CIbdHeaderGraph::InsertResult> inserted =
        m_graph.InsertBatch(headers);
    for (std::size_t i = 0; i < headers.size(); ++i)
    {
        m_sources[headers[i].first].insert(peer);
        if (inserted[i] == CIbdHeaderGraph::DUPLICATE) ++m_counters.duplicates;
        else if (inserted[i] == CIbdHeaderGraph::INSERTED_ACTIVE ||
                 inserted[i] == CIbdHeaderGraph::INSERTED_ELIGIBLE) ++m_counters.accepted;
        else { ++m_counters.disconnected; ++m_counters.quarantined; }
    }
    const uint256 newTip = m_graph.ActiveTip() ? m_graph.ActiveTip()->hash : uint256(0);
    if (oldTip != newTip && oldTip != m_graph.AnchorHash() &&
        !m_graph.IsDescendantOf(newTip, oldTip))
        ++m_counters.activeBranchSwitches;
    result.continueHeaders = result.expectedResponse && oldTip != newTip &&
                             continuationBatchSize > 0 &&
                             headers.size() >= continuationBatchSize;
    if (result.continueHeaders) result.continuationLocator = m_graph.BuildContinuationLocator();
    return result;
}

std::vector<uint256> CIbdHeadersObserver::PredictedWindow() const
{
    if (!m_enabled || !m_graph.HasAnchor()) return std::vector<uint256>();
    return m_graph.GetActiveWindow(m_graph.AnchorHash(), m_window_size);
}

std::vector<uint256> CIbdHeadersObserver::PredictedWindowFromFrontier(
    const uint256& frontier) const
{
    if (!m_enabled || !m_graph.HasAnchor()) return std::vector<uint256>();
    return m_graph.GetActiveWindow(frontier, m_window_size);
}

CIbdHeadersObserver::Classification CIbdHeadersObserver::Classify(
    const uint256& hash, int authoritativeHeight) const
{
    if (!m_enabled || !m_graph.HasAnchor()) return UNKNOWN_TO_GRAPH;
    if (authoritativeHeight >= 0 && authoritativeHeight <= m_graph.AnchorHeight())
        return BEFORE_WINDOW;
    const CIbdHeaderNode* node = m_graph.Lookup(hash);
    if (!node) return UNKNOWN_TO_GRAPH;
    if (node->hash == m_graph.AnchorHash()) return BEFORE_WINDOW;
    if (node->state != CIbdHeaderNode::ACTIVE || !node->IsUsable())
        return OFF_ACTIVE_BRANCH;
    if (node->height <= m_graph.AnchorHeight())
        return BEFORE_WINDOW;
    if ((std::size_t)(node->height - m_graph.AnchorHeight()) > m_window_size)
        return AFTER_WINDOW;
    return IN_PREDICTED_WINDOW;
}

void CIbdHeadersObserver::RecordClassification(
    unsigned int eventKind, Classification classification)
{
    if (m_enabled && eventKind < 3 && classification <= UNKNOWN_TO_GRAPH)
        ++m_counters.classified[eventKind][classification];
}

std::size_t CIbdHeadersObserver::PeerSupport(const uint256& hash) const
{
    std::map<uint256, std::set<int64_t> >::const_iterator it = m_sources.find(hash);
    return it == m_sources.end() ? 0 : it->second.size();
}

std::vector<int64_t> CIbdHeadersObserver::HeaderSources(const uint256& hash) const
{
    std::vector<int64_t> out;
    std::map<uint256, std::set<int64_t> >::const_iterator it = m_sources.find(hash);
    if (it == m_sources.end())
        return out;
    out.assign(it->second.begin(), it->second.end());
    return out;
}

const char* CIbdHeadersObserver::ClassificationName(Classification c)
{
    switch (c) {
    case IN_PREDICTED_WINDOW: return "IN_PREDICTED_WINDOW";
    case BEFORE_WINDOW: return "BEFORE_WINDOW";
    case AFTER_WINDOW: return "AFTER_WINDOW";
    case OFF_ACTIVE_BRANCH: return "OFF_ACTIVE_BRANCH";
    default: return "UNKNOWN_TO_GRAPH";
    }
}
