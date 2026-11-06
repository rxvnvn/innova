// ETAP-1: single-owner chain store context for the test harness.
//
// CONTRACT
//   * One authoritative Block Index V2 lives per PROCESS (it is opened by the
//     production startup path inside LoadBlockIndex -> InitBlockIndexAuthoritative;
//     this header never sets g_fAuthoritativeStartup itself).
//   * At most ONE Store3A may own that chain context at a time. A second
//     concurrent arm is a hard error, never an implicit nested arm.
//   * A suite that needs an INDEPENDENT chain must run in its own process /
//     datadir (the per-suite invocation already provides that), because the V2
//     authority is process-global by design.
//   * Ownership is released on destruction: observe state restored, store closed,
//     token freed. Lifecycle is deterministic and order-independent.
#ifndef INNOVA_TEST_CHAINSTORE_CONTEXT_H
#define INNOVA_TEST_CHAINSTORE_CONTEXT_H

#include "main.h"
#include "chainstate/observe_hook.h"
#include "chainstate/store3a/incstore.h"

#include <boost/filesystem.hpp>
#include <cstring>
#include <memory>
#include <string>

namespace chaintest {

// Process-wide ownership token. Plain bool under cs_main is enough: the harness
// is single-threaded for fixture setup/teardown.
inline bool& StoreOwnedFlag()
{
    static bool owned = false;
    return owned;
}

// True when a store is currently armed as the owner of this chain context.
inline bool StoreContextOwned()
{
    return StoreOwnedFlag();
}

// Lightweight ownership guard for suites that own their own Store3A inline:
// it enforces the one-owner-per-chain invariant (a nested/second arm fails fast)
// without changing the suite's own store lifecycle.
struct StoreOwnershipGuard
{
    bool owned;
    std::string err;
    StoreOwnershipGuard() : owned(false)
    {
        if (StoreOwnedFlag())
        {
            err = "another Store3A already owns this authoritative chain context";
            return;
        }
        StoreOwnedFlag() = true;
        owned = true;
    }
    ~StoreOwnershipGuard()
    {
        if (owned) StoreOwnedFlag() = false;
    }
    StoreOwnershipGuard(const StoreOwnershipGuard&) = delete;
    StoreOwnershipGuard& operator=(const StoreOwnershipGuard&) = delete;
};

// The single-owner Store3A context. Usage:
//   chaintest::ChainStoreContext ctx;            // own isolated dir
//   chaintest::ChainStoreContext ctx(dir);       // own caller-provided dir
//   BOOST_REQUIRE_MESSAGE(ctx.armed, ctx.err);   // fail fast, never nested
struct ChainStoreContext
{
    cs::store3a::CommitStore3A store;
    s16::ChainObserveState saved;
    boost::filesystem::path dir;
    bool armed;
    bool ownsToken;
    std::string err;

    explicit ChainStoreContext(const std::string& dirOverride = std::string())
        : armed(false), ownsToken(false)
    {
        namespace bfs = boost::filesystem;
        if (StoreOwnedFlag())
        {
            // A second Store3A must never silently own the same chain context.
            err = "another Store3A already owns this authoritative chain context";
            return;
        }
        if (dirOverride.empty())
        {
            dir = bfs::temp_directory_path() / bfs::unique_path("chainstore-%%%%-%%%%");
            bfs::create_directories(dir);
        }
        else
        {
            dir = bfs::path(dirOverride);
            bfs::create_directories(dir);
        }

        cs::ChainError oe;
        if (!store.open(dir.string(), &oe)) { err = "store open: " + oe.str(); return; }

        // Born-commit at the base/genesis identity when the store is fresh. This
        // is the production bootstrap shape (empty delta at the base height).
        if (store.tipHeight() < 0)
        {
            if (pindexGenesisBlock == NULL) { err = "no genesis block loaded"; store.close(); return; }
            cs::Delta birth;
            cs::Hash gh; memcpy(gh.data(), pindexGenesisBlock->GetBlockHash().begin(), 32);
            cs::ChainError be;
            if (!store.publish(birth, gh, 0, &be)) { err = "genesis born-commit: " + be.str(); store.close(); return; }
        }

        std::string recErr;
        if (!s16::ReconcileStore3AToChain(&store, &recErr))
        {
            err = "reconcile: " + recErr;
            store.close();
            return;
        }
        if (!s16::g_chainObserve.pending.empty() && !s16::g_chainObserve.FlushPending())
        {
            err = "barrier flush: " + s16::g_chainObserve.lastOpError;
            store.close();
            return;
        }

        saved = s16::g_chainObserve;
        s16::g_chainObserve.store = &store;
        s16::g_chainObserve.enabled = true;
        s16::g_chainObserve.checkAuthority = true;
        s16::g_chainObserve.hardFail = true;
        s16::g_chainObserve.batchSize = 1000;
        s16::g_chainObserve.batchChunk = 0;
        s16::g_chainObserve.pending.clear();
        s16::g_chainObserve.needsRecovery = false;

        StoreOwnedFlag() = true;
        ownsToken = true;
        armed = true;
    }

    ~ChainStoreContext()
    {
        reset();
    }

    void reset()
    {
        if (ownsToken)
        {
            s16::g_chainObserve = saved;
            if (store.opened()) store.close();
            StoreOwnedFlag() = false;
            ownsToken = false;
        }
        armed = false;
    }

    ChainStoreContext(const ChainStoreContext&) = delete;
    ChainStoreContext& operator=(const ChainStoreContext&) = delete;
};

} // namespace chaintest

#endif // INNOVA_TEST_CHAINSTORE_CONTEXT_H
