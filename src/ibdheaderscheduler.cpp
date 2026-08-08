// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#include "ibdheaderscheduler.h"

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
    : m_has_anchor(false), m_anchor_height(-1)
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
            if (!parent || !parent->IsAnchored() ||
                node.height != parent->height + 1)
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
