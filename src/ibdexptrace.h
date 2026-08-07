// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

#ifndef INNOVA_IBDEXPTRACE_H
#define INNOVA_IBDEXPTRACE_H

// First-runtime-falsification telemetry for the explanatory model
// (doc/forensics/getblocks-hashcontinue-batch-size-audit.md, Appendix).
//
// DIAGNOSTIC ONLY.  Gated by -ibdexptrace=1; otherwise every note() is a
// no-op.  There is no behavioral change: no scheduler, timeout, orphan-cap,
// scoring or ownership modification, and no new lock is held in any hot path
// that does not already serialize.  All state is relaxed-atomic counters plus
// a bounded per-hash tag map guarded by a single spinlock.
//
// The whole module is removable in one commit: the call sites below the
// "EXPTRACE HOOK" markers in net.h / main.cpp / net.cpp / init.cpp plus this
// header.

#include "uint256.h"
#include "util.h"

#include <stdint.h>

#include <atomic>
#include <cstdio>
#include <map>
#include <set>
#include <vector>

#include "sync.h"

namespace ibdexptrace {

// Request-origin classification, matching the model's origins.  A timed-out
// in-flight hash that is re-requested WITHOUT a fresh announcement is tagged
// TIMEOUT_REISSUE (the model asserts this never happens in the tree; the
// counter falsifies or confirms that claim directly).
enum ExpOrigin
{
    EXP_ORIGIN_BULK = 0,
    EXP_ORIGIN_RELAY,
    EXP_ORIGIN_ORPHAN_PARENT,
    EXP_ORIGIN_CONTINUATION,
    EXP_ORIGIN_TIMEOUT_REISSUE,
    EXP_ORIGIN_OTHER,
    EXP_ORIGIN_COUNT
};

inline const char* OriginName(int o)
{
    switch (o)
    {
    case EXP_ORIGIN_BULK: return "BULK";
    case EXP_ORIGIN_RELAY: return "RELAY";
    case EXP_ORIGIN_ORPHAN_PARENT: return "ORPHAN_PARENT";
    case EXP_ORIGIN_CONTINUATION: return "CONTINUATION";
    case EXP_ORIGIN_TIMEOUT_REISSUE: return "TIMEOUT_REISSUE";
    default: return "OTHER";
    }
}

// INV-batch origin (coarse, per the audit's Section 10 classification).
enum ExpInvOrigin
{
    EXP_INV_RESPONSE = 0,        // arrived while a getblocks cycle was outstanding
    EXP_INV_CONTINUATION,        // 1-item getblocks response (continuation/tip path)
    EXP_INV_RELAY                // unsolicited announcement
};

inline bool& EnabledFlag()
{
    static bool f = false;
    return f;
}

inline bool Enabled()
{
    return EnabledFlag();
}

inline void SetEnabled(bool fEnabled)
{
    EnabledFlag() = fEnabled;
}

struct State
{
    // lifecycle counters per origin: requested / received / connected
    std::atomic<int64_t> requested[EXP_ORIGIN_COUNT];
    std::atomic<int64_t> received[EXP_ORIGIN_COUNT];
    std::atomic<int64_t> connected[EXP_ORIGIN_COUNT];
    // current in-flight gauge per origin (snapshot source)
    std::atomic<int64_t> active[EXP_ORIGIN_COUNT];

    // INV-batch classification totals
    std::atomic<int64_t> inv_batches;
    std::atomic<int64_t> inv_blocks[3];        // by ExpInvOrigin
    std::atomic<int64_t> inv_1item[3];         // n==1 batches by ExpInvOrigin
    std::atomic<int64_t> inv_1item_blocks;     // blocks inside 1-item batches

    // getblocks decisions
    std::atomic<int64_t> gblocks_total;
    std::atomic<int64_t> gblocks_dedup;
    std::atomic<int64_t> gblocks_suppressed;

    // orphan occupancy / reject events (bounded: counters only, occupancy is
    // captured by the per-second snapshot)
    std::atomic<int64_t> orphan_add_total;
    std::atomic<int64_t> orphan_limit_reject;
    std::atomic<int64_t> orphan_reject_parent_unknown;

    // falsification probe: re-request of a hash that timed out in-flight
    std::atomic<int64_t> timeout_reask_count;

    // late delivery measured as received-after-expire (diagnostic)
    std::atomic<int64_t> late_received_count;

    State()
    {
        for (int i = 0; i < EXP_ORIGIN_COUNT; ++i)
        {
            requested[i].store(0, std::memory_order_relaxed);
            received[i].store(0, std::memory_order_relaxed);
            connected[i].store(0, std::memory_order_relaxed);
            active[i].store(0, std::memory_order_relaxed);
        }
        for (int i = 0; i < 3; ++i)
        {
            inv_blocks[i].store(0, std::memory_order_relaxed);
            inv_1item[i].store(0, std::memory_order_relaxed);
        }
        inv_batches.store(0, std::memory_order_relaxed);
        inv_1item_blocks.store(0, std::memory_order_relaxed);
        gblocks_total.store(0, std::memory_order_relaxed);
        gblocks_dedup.store(0, std::memory_order_relaxed);
        gblocks_suppressed.store(0, std::memory_order_relaxed);
        orphan_add_total.store(0, std::memory_order_relaxed);
        orphan_limit_reject.store(0, std::memory_order_relaxed);
        orphan_reject_parent_unknown.store(0, std::memory_order_relaxed);
        timeout_reask_count.store(0, std::memory_order_relaxed);
        late_received_count.store(0, std::memory_order_relaxed);
    }
};

inline State& Get()
{
    static State s;
    return s;
}

// Per-hash request-origin tag map, bounded.  Tagged by INV classification
// (BULK/RELAY/CONTINUATION) and by the explicit orphan-parent AskFor; consumed
// at getdata send; the surviving request origin is re-tagged for the receive /
// connect attribution.  All access is under a single guard.
struct TagState
{
    std::map<uint256, int> hashOrigin;       // hash -> ExpOrigin
    std::set<uint256> expired;               // hashes that timed out in-flight
    std::map<uint256, int> recvOrigin;       // receive origin, consumed at connect
    CCriticalSection cs;

    void CapMaps()
    {
        const size_t nCap = 250000;
        if (hashOrigin.size() > nCap)
            hashOrigin.clear();
        if (expired.size() > nCap)
            expired.clear();
        if (recvOrigin.size() > nCap)
            recvOrigin.clear();
    }
};

inline TagState& Tag()
{
    static TagState s;
    return s;
}

inline int ResolveRequestOrigin(const uint256& hash)
{
    LOCK(Tag().cs);
    // A re-request of a hash that timed out without a fresh announcement is
    // the falsification probe.  A new INV announcement clears the expired flag
    // (see NoteInv), so a re-request after a re-announcement tags under the
    // announcing origin instead.
    std::set<uint256>::iterator itExp = Tag().expired.find(hash);
    if (itExp != Tag().expired.end())
    {
        Tag().expired.erase(itExp);
        Get().timeout_reask_count.fetch_add(1, std::memory_order_relaxed);
        return EXP_ORIGIN_TIMEOUT_REISSUE;
    }
    std::map<uint256, int>::iterator it = Tag().hashOrigin.find(hash);
    if (it != Tag().hashOrigin.end())
    {
        const int o = it->second;
        Tag().hashOrigin.erase(it);
        return o;
    }
    return EXP_ORIGIN_OTHER;
}

// Called from CNode::MarkBlockInFlight: a getdata for `hash` was committed.
inline void NoteRequestSent(const uint256& hash)
{
    if (!Enabled())
        return;
    const int o = ResolveRequestOrigin(hash);
    Get().requested[o].fetch_add(1, std::memory_order_relaxed);
    Get().active[o].fetch_add(1, std::memory_order_relaxed);
    // Retag so the receive / timeout path can attribute the request origin.
    {
        LOCK(Tag().cs);
        Tag().hashOrigin[hash] = o;
    }
}

// Called from CNode::ClearBlockInFlight: the requested block arrived.
inline void NoteRequestReceived(const uint256& hash)
{
    if (!Enabled())
        return;
    int o = EXP_ORIGIN_OTHER;
    bool fWasExpired = false;
    {
        LOCK(Tag().cs);
        std::map<uint256, int>::iterator it = Tag().hashOrigin.find(hash);
        if (it != Tag().hashOrigin.end())
        {
            o = it->second;
            Tag().hashOrigin.erase(it);
        }
        // A block that arrives after its in-flight timeout is a late delivery;
        // its request origin was already consumed at the timeout.
        if (Tag().expired.count(hash))
        {
            fWasExpired = true;
            Tag().expired.erase(hash);
        }
    }
    if (fWasExpired)
        Get().late_received_count.fetch_add(1, std::memory_order_relaxed);
    Get().received[o].fetch_add(1, std::memory_order_relaxed);
    Get().active[o].fetch_sub(1, std::memory_order_relaxed);
    // Remember for the connect attribution; consumed by NoteBlockConnected.
    {
        LOCK(Tag().cs);
        Tag().recvOrigin[hash] = o;
    }
}

// Called from CNode::ExpireBlockInFlight / peer cleanup: the in-flight request
// for `hash` was released by timeout.
inline void NoteRequestTimeout(const uint256& hash)
{
    if (!Enabled())
        return;
    int o = EXP_ORIGIN_OTHER;
    {
        LOCK(Tag().cs);
        std::map<uint256, int>::iterator it = Tag().hashOrigin.find(hash);
        if (it != Tag().hashOrigin.end())
        {
            o = it->second;
            Tag().hashOrigin.erase(it);
        }
        Tag().expired.insert(hash);
    }
    Get().active[o].fetch_sub(1, std::memory_order_relaxed);
}

// Called when a received block becomes part of the active chain.
inline void NoteBlockConnected(const uint256& hash)
{
    if (!Enabled())
        return;
    int o = EXP_ORIGIN_OTHER;
    {
        LOCK(Tag().cs);
        std::map<uint256, int>::iterator it = Tag().recvOrigin.find(hash);
        if (it != Tag().recvOrigin.end())
        {
            o = it->second;
            Tag().recvOrigin.erase(it);
        }
    }
    Get().connected[o].fetch_add(1, std::memory_order_relaxed);
}

// A received block that did NOT become part of the active chain (orphan /
// rejected / non-main).  Frees its connect-attribution entry.
inline void NoteBlockNotConnected(const uint256& hash)
{
    if (!Enabled())
        return;
    LOCK(Tag().cs);
    Tag().recvOrigin.erase(hash);
}

// Called from the INV receive path.  Classifies the batch and tags every block
// hash with its announcing origin.
inline void NoteInv(int peer, int nBlockCount, bool fGetBlocksResponse,
                    bool fFrontierResponse, int nLocalHeight, int nPeerHeight,
                    const std::vector<uint256>& vBlockHashes,
                    const uint256& hashLastBatch)
{
    if (!Enabled())
        return;
    State& g = Get();
    g.inv_batches.fetch_add(1, std::memory_order_relaxed);
    int invOrigin;
    if (fGetBlocksResponse)
        invOrigin = (nBlockCount == 1) ? EXP_INV_CONTINUATION : EXP_INV_RESPONSE;
    else
        invOrigin = EXP_INV_RELAY;
    g.inv_blocks[invOrigin].fetch_add(nBlockCount, std::memory_order_relaxed);
    if (nBlockCount == 1)
    {
        g.inv_1item[invOrigin].fetch_add(1, std::memory_order_relaxed);
        g.inv_1item_blocks.fetch_add(1, std::memory_order_relaxed);
    }
    // Tag hashes; a fresh announcement adopts a previously timed-out hash.
    {
        LOCK(Tag().cs);
        const int reqOrigin =
            invOrigin == EXP_INV_RESPONSE ? EXP_ORIGIN_BULK
            : invOrigin == EXP_INV_CONTINUATION ? EXP_ORIGIN_CONTINUATION
                                                : EXP_ORIGIN_RELAY;
        for (size_t i = 0; i < vBlockHashes.size(); ++i)
        {
            Tag().hashOrigin[vBlockHashes[i]] = reqOrigin;
            Tag().expired.erase(vBlockHashes[i]);
        }
        Tag().CapMaps();
    }
    printf("EXPTRACE INV time_us=%lld peer=%d n=%d origin=%s local_h=%d "
           "peer_h=%d outstanding_gb=%d frontier=%d last_batch=%s\n",
           (long long)GetTimeMicros(), (int)peer, nBlockCount,
           invOrigin == EXP_INV_RESPONSE ? "RESPONSE"
           : invOrigin == EXP_INV_CONTINUATION ? "CONTINUATION" : "RELAY",
           nLocalHeight, nPeerHeight, fGetBlocksResponse ? 1 : 0,
           fFrontierResponse ? 1 : 0,
           hashLastBatch.ToString().substr(0, 16).c_str());
}

// Called from CNode::AskFor when the request is an explicit orphan-parent walk.
inline void NoteOrphanParentAsk(const uint256& hash)
{
    if (!Enabled())
        return;
    LOCK(Tag().cs);
    Tag().hashOrigin[hash] = EXP_ORIGIN_ORPHAN_PARENT;
}

// Called from the client-side getblocks decision (PushGetBlocks).
inline void NoteGetBlocks(int peer, int nLocatorHeight, int nPeerHeight,
                          int nGap, const char* pszSource, bool fDedupSkipped,
                          const uint256& hashStop)
{
    if (!Enabled())
        return;
    State& g = Get();
    g.gblocks_total.fetch_add(1, std::memory_order_relaxed);
    if (fDedupSkipped)
        g.gblocks_dedup.fetch_add(1, std::memory_order_relaxed);
    printf("EXPTRACE GBLOCK time_us=%lld peer=%d src=%s loc_h=%d peer_h=%d "
           "gap=%d dedup=%d stop=%s\n",
           (long long)GetTimeMicros(), (int)peer,
           pszSource ? pszSource : "OTHER",
           nLocatorHeight, nPeerHeight, nGap, fDedupSkipped ? 1 : 0,
           hashStop.ToString().substr(0, 16).c_str());
}

// Bounded orphan diagnostics: counter per add, printf only on cap-rejection.
inline void NoteOrphanAdd(int peer, int nPeerOrphans, int nGlobalOrphans)
{
    if (!Enabled())
        return;
    Get().orphan_add_total.fetch_add(1, std::memory_order_relaxed);
}

inline void NoteOrphanLimitReject(int peer, int nPeerOrphans, int nGlobalOrphans,
                                  bool fParentKnown)
{
    if (!Enabled())
        return;
    State& g = Get();
    g.orphan_limit_reject.fetch_add(1, std::memory_order_relaxed);
    if (!fParentKnown)
        g.orphan_reject_parent_unknown.fetch_add(1, std::memory_order_relaxed);
    printf("EXPTRACE ORPHAN_LIMIT time_us=%lld peer=%d peer_orphans=%d "
           "global_orphans=%d parent_known=%d\n",
           (long long)GetTimeMicros(), (int)peer, nPeerOrphans,
           nGlobalOrphans, fParentKnown ? 1 : 0);
}

// Per-second active-pipeline composition snapshot (gated internally).
inline void Emit1s(int64_t nNow, int nLocalHeight, int nOrphanGlobal)
{
    if (!Enabled())
        return;
    State& g = Get();
    printf("EXPTRACE ACTIVE time_us=%lld local_h=%d global_orphans=%d"
           " active_total=%lld",
           (long long)nNow, nLocalHeight, nOrphanGlobal,
           (long long)(g.active[0].load(std::memory_order_relaxed) +
                       g.active[1].load(std::memory_order_relaxed) +
                       g.active[2].load(std::memory_order_relaxed) +
                       g.active[3].load(std::memory_order_relaxed) +
                       g.active[4].load(std::memory_order_relaxed) +
                       g.active[5].load(std::memory_order_relaxed)));
    for (int i = 0; i < EXP_ORIGIN_COUNT; ++i)
        printf(" %s=%lld", OriginName(i),
               (long long)g.active[i].load(std::memory_order_relaxed));
    printf("\n");
}

// Final summary emitted at shutdown (from the caller's Shutdown path).
inline void EmitSummary(int64_t nNow)
{
    if (!Enabled())
        return;
    State& g = Get();
    printf("EXPTRACE SUMMARY time_us=%lld\n", (long long)nNow);
    for (int i = 0; i < EXP_ORIGIN_COUNT; ++i)
        printf("EXPTRACE ORIGIN %s req=%lld recv=%lld conn=%lld active=%lld\n",
               OriginName(i),
               (long long)g.requested[i].load(std::memory_order_relaxed),
               (long long)g.received[i].load(std::memory_order_relaxed),
               (long long)g.connected[i].load(std::memory_order_relaxed),
               (long long)g.active[i].load(std::memory_order_relaxed));
    printf("EXPTRACE INV batches=%lld blocks_resp=%lld blocks_cont=%lld "
           "blocks_relay=%lld 1item_resp=%lld 1item_cont=%lld 1item_relay=%lld\n",
           (long long)g.inv_batches.load(std::memory_order_relaxed),
           (long long)g.inv_blocks[0].load(std::memory_order_relaxed),
           (long long)g.inv_blocks[1].load(std::memory_order_relaxed),
           (long long)g.inv_blocks[2].load(std::memory_order_relaxed),
           (long long)g.inv_1item[0].load(std::memory_order_relaxed),
           (long long)g.inv_1item[1].load(std::memory_order_relaxed),
           (long long)g.inv_1item[2].load(std::memory_order_relaxed));
    printf("EXPTRACE GBLOCKS total=%lld dedup=%lld\n",
           (long long)g.gblocks_total.load(std::memory_order_relaxed),
           (long long)g.gblocks_dedup.load(std::memory_order_relaxed));
    printf("EXPTRACE ORPHAN add=%lld limit_reject=%lld parent_unknown=%lld\n",
           (long long)g.orphan_add_total.load(std::memory_order_relaxed),
           (long long)g.orphan_limit_reject.load(std::memory_order_relaxed),
           (long long)g.orphan_reject_parent_unknown.load(std::memory_order_relaxed));
    printf("EXPTRACE PROBE timeout_reask=%lld late_received=%lld\n",
           (long long)g.timeout_reask_count.load(std::memory_order_relaxed),
           (long long)g.late_received_count.load(std::memory_order_relaxed));
}

} // namespace ibdexptrace

#endif // INNOVA_IBDEXPTRACE_H
