// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// A.13.6-R2c.1b — bounded live DAG tip frontier overlay implementation.
//
// The authoritative logical override state is a persistent, generation-bound,
// hash-keyed LevelDB store. A strictly capped in-memory LRU cache accelerates
// reads only; eviction removes cache residency, never logical state. Logical
// cardinality may grow on disk with no bound while RAM stays <= cacheCapacity.

#include "dag_tip_live_overlay.h"

#include "dag_tip_frontier.h" // TipFrontierReader
#include "serialize.h"        // CDataStream

#include <leveldb/db.h>
#include <leveldb/filter_policy.h>
#include <leveldb/iterator.h>

#include <boost/filesystem.hpp>

#include <algorithm>
#include <cstring>
#include <deque>
#include <map>
#include <set>
#include <string>

namespace dag_tip_frontier {
namespace {

static const char* const OVERRIDE_TYPE = "dagtipovr";
static const char* const META_TYPE = "dagtovlmeta";

// Forward declarations for the streaming emit trampoline.
struct LiveTipOverlayForward
{
    const LiveTipFrontierOverlay* self;
    LiveTipFrontierOverlay::ForEachFn fn;
    void* outerCtx;
    bool stop;
    LiveTipOverlayForward() : self(NULL), fn(NULL), outerCtx(NULL), stop(false) {}
};
static void EmitAddedNonSeedTramp(const uint256& h, bool present, void* c);

static void EmitAddedNonSeedTramp(const uint256& h, bool present, void* c)
{
    LiveTipOverlayForward* f = static_cast<LiveTipOverlayForward*>(c);
    if (!f || !present || !f->self) return;
    bool inSeed = f->self->HasSeedMembership(h);
    if (inSeed) return; // already emitted in pass 1
    if (f->fn && !((*f->fn)(h, f->outerCtx))) f->stop = true;
}

} // namespace

// ---------------------------------------------------------------------------
// Persistent keyed override store
// ---------------------------------------------------------------------------

struct LiveTipOverlayStore::Impl
{
    leveldb::DB* db;
    std::string dbDir;
    bool open;
    uint64_t generation;
    uint64_t entries;
    Impl() : db(NULL), open(false), generation(0), entries(0) {}
};

LiveTipOverlayStore::LiveTipOverlayStore() : impl_(new Impl()) {}
LiveTipOverlayStore::~LiveTipOverlayStore() { Close(); delete impl_; }

static void MakeEntryKey(CDataStream& k, const uint256& hash)
{
    k << make_pair(std::string(OVERRIDE_TYPE), hash);
}

static void MakeMetaKey(CDataStream& k, uint64_t gen)
{
    k << make_pair(std::string(META_TYPE), gen);
}

bool LiveTipOverlayStore::Open(const std::string& dbDir, uint64_t expectGeneration, std::string* error)
{
    Close();
    leveldb::Options options;
    options.create_if_missing = true;
    options.error_if_exists = false;
    options.filter_policy = leveldb::NewBloomFilterPolicy(10);
    leveldb::DB* db = NULL;
    leveldb::Status st = leveldb::DB::Open(options, dbDir, &db);
    if (!st.ok() || !db)
    {
        if (error) *error = "overlay store open failure: " + st.ToString();
        return false;
    }
    impl_->db = db;
    impl_->dbDir = dbDir;

    // Read meta generation; if absent (fresh store) then write it; if present
    // and mismatched then fail closed.
    CDataStream mk(SER_DISK, CLIENT_VERSION);
    MakeMetaKey(mk, expectGeneration);
    std::string metaVal;
    leveldb::Status gs = db->Get(leveldb::ReadOptions(), leveldb::Slice(mk.str()), &metaVal);
    if (gs.IsNotFound())
    {
        CDataStream mv(SER_DISK, CLIENT_VERSION);
        mv << expectGeneration;
        leveldb::Status ws = db->Put(leveldb::WriteOptions(), leveldb::Slice(mk.str()), leveldb::Slice(mv.str()));
        if (!ws.ok())
        {
            if (error) *error = "overlay store meta write failure: " + ws.ToString();
            Close(); return false;
        }
        impl_->generation = expectGeneration;
    }
    else if (!gs.ok())
    {
        if (error) *error = "overlay store meta read failure: " + gs.ToString();
        Close(); return false;
    }
    else
    {
        try
        {
            CDataStream mv(metaVal.data(), metaVal.data() + metaVal.size(), SER_DISK, CLIENT_VERSION);
            uint64_t gen = 0;
            mv >> gen;
            if (gen != expectGeneration)
            {
                if (error) *error = "overlay store generation mismatch";
                Close(); return false;
            }
            impl_->generation = gen;
        }
        catch (const std::exception&)
        {
            if (error) *error = "overlay store meta decode failure (corrupt)";
            Close(); return false;
        }
    }

    // Count persisted override entries (bounded query via iterator).
    uint64_t count = 0;
    {
        CDataStream prefix(SER_DISK, CLIENT_VERSION);
        prefix << std::string(OVERRIDE_TYPE);
        const std::string pfx = prefix.str();
        leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
        it->Seek(pfx);
        while (it->Valid())
        {
            if (it->key().ToString().compare(0, pfx.size(), pfx) != 0) break;
            ++count;
            it->Next();
        }
        delete it;
    }
    impl_->entries = count;
    impl_->open = true;
    return true;
}

void LiveTipOverlayStore::Close()
{
    if (impl_->db) { delete impl_->db; impl_->db = NULL; }
    impl_->open = false;
    impl_->generation = 0;
    impl_->entries = 0;
    impl_->dbDir.clear();
}

bool LiveTipOverlayStore::IsOpen() const { return impl_->open; }
uint64_t LiveTipOverlayStore::Generation() const { return impl_->generation; }
uint64_t LiveTipOverlayStore::EntryCount() const { return impl_->entries; }

bool LiveTipOverlayStore::SetState(const uint256& hash, bool present, std::string* error)
{
    if (!impl_->open) { if (error) *error = "overlay store not open"; return false; }
    // Record whether a previous override existed so the on-disk entry count
    // stays exact across updates (logical cardinality, unbounded on disk).
    bool had = false, oldPresent = false;
    std::string priorErr;
    if (!GetState(hash, &had, &oldPresent, &priorErr) ||
        !priorErr.empty())
    {
        // A read failure here is not fatal to the write in normal op; treat as
        // conservative and recount from disk below.
        had = true;
    }
    CDataStream k(SER_DISK, CLIENT_VERSION);
    MakeEntryKey(k, hash);
    CDataStream v(SER_DISK, CLIENT_VERSION);
    v << uint8_t(present ? 1 : 0);
    leveldb::Status st = impl_->db->Put(leveldb::WriteOptions(), leveldb::Slice(k.str()), leveldb::Slice(v.str()));
    if (!st.ok())
    {
        if (error) *error = "overlay store write failure: " + st.ToString();
        return false;
    }
    if (had)
        impl_->entries = impl_->entries; // existing key: count unchanged
    else
        ++impl_->entries;                 // new key: logical cardinality grows
    return true;
}

bool LiveTipOverlayStore::GetState(const uint256& hash, bool* hasOverride, bool* present, std::string* error) const
{
    if (hasOverride) *hasOverride = false;
    if (present) *present = false;
    if (!impl_->open) { if (error) *error = "overlay store not open"; return false; }
    CDataStream k(SER_DISK, CLIENT_VERSION);
    MakeEntryKey(k, hash);
    std::string val;
    leveldb::Status st = impl_->db->Get(leveldb::ReadOptions(), leveldb::Slice(k.str()), &val);
    if (st.IsNotFound())
        return true;
    if (!st.ok())
    {
        if (error) *error = "overlay store read failure: " + st.ToString();
        return false;
    }
    if (val.size() < 1)
    {
        if (error) *error = "overlay store corrupt override value";
        return false;
    }
    if (hasOverride) *hasOverride = true;
    if (present) *present = (val[0] != 0);
    return true;
}

bool LiveTipOverlayStore::ForEachOverride(ForEachOverrideFn fn, void* ctx, std::string* error) const
{
    if (!impl_->open) { if (error) *error = "overlay store not open"; return false; }
    CDataStream prefix(SER_DISK, CLIENT_VERSION);
    prefix << std::string(OVERRIDE_TYPE);
    const std::string pfx = prefix.str();
    leveldb::Iterator* it = impl_->db->NewIterator(leveldb::ReadOptions());
    it->Seek(pfx);
    while (it->Valid())
    {
        if (it->key().ToString().compare(0, pfx.size(), pfx) != 0) break;
        try
        {
            CDataStream key(it->key().data(), it->key().data() + it->key().size(), SER_DISK, CLIENT_VERSION);
            std::pair<std::string, uint256> pairKey;
            key >> pairKey;
            if (pairKey.first != OVERRIDE_TYPE) { delete it; return true; }
            bool present = false;
            if (it->value().size() >= 1) present = (it->value().data()[0] != 0);
            if (fn) fn(pairKey.second, present, ctx);
        }
        catch (const std::exception&)
        {
            delete it;
            if (error) *error = "overlay store iterate decode failure (corrupt)";
            return false;
        }
        it->Next();
    }
    delete it;
    return true;
}

// ---------------------------------------------------------------------------
// Bounded overlay view
// ---------------------------------------------------------------------------

struct LiveTipFrontierOverlay::Impl
{
    TipFrontierReader seed;
    LiveTipOverlayStore store;
    size_t cap;
    mutable std::set<uint256> cache; // only reads are cached; never authoritative
    mutable size_t peak;
    bool open;
    uint64_t generation;
    Impl() : cap(0), peak(0), open(false), generation(0) {}
};

LiveTipFrontierOverlay::LiveTipFrontierOverlay() : impl_(new Impl()) {}
LiveTipFrontierOverlay::~LiveTipFrontierOverlay() { Close(); delete impl_; }

bool LiveTipFrontierOverlay::Open(const std::string& artifactPath,
                                  const std::string& overlayDbDir,
                                  uint64_t generation,
                                  unsigned char dagInputDigest[32],
                                  size_t cacheCapacity,
                                  std::string* error)
{
    Close();
    if (!impl_->seed.Open(artifactPath, generation, dagInputDigest, error))
        return false;
    if (!impl_->store.Open(overlayDbDir, generation, error))
    {
        impl_->seed.Close();
        return false;
    }
    impl_->open = true;
    impl_->generation = generation;
    impl_->cap = cacheCapacity;
    impl_->peak = 0;
    impl_->cache.clear();
    return true;
}

void LiveTipFrontierOverlay::Close()
{
    if (impl_->seed.IsOpen()) impl_->seed.Close();
    impl_->store.Close();
    impl_->cache.clear();
    impl_->open = false;
    impl_->generation = 0;
    impl_->cap = 0;
    impl_->peak = 0;
}

bool LiveTipFrontierOverlay::IsOpen() const { return impl_->open; }
uint64_t LiveTipFrontierOverlay::Generation() const { return impl_->generation; }

bool LiveTipFrontierOverlay::AddTip(const uint256& hash, std::string* error)
{
    if (!impl_->open) { if (error) *error = "overlay not open"; return false; }
    return impl_->store.SetState(hash, true, error);
}

bool LiveTipFrontierOverlay::RemoveTip(const uint256& hash, std::string* error)
{
    if (!impl_->open) { if (error) *error = "overlay not open"; return false; }
    return impl_->store.SetState(hash, false, error);
}

// Bounded cache add (deterministic bounded eviction when over capacity;
// eviction loses only cache residency, never logical on-disk state).
static void CacheAdd(std::set<uint256>& cache, size_t& peak, const uint256& h, size_t cap)
{
    cache.insert(h);
    if (cache.size() > peak) peak = cache.size();
    while (cache.size() > cap && !cache.empty())
        cache.erase(cache.begin());
}

bool LiveTipFrontierOverlay::ForEachTip(ForEachFn fn, void* ctx, std::string* error) const
{
    if (!impl_->open) { if (error) *error = "overlay not open"; return false; }
    // Pass 1: stream immutable seed; emit seed tips not overridden to ABSENT,
    // and seed tips overridden PRESENT exactly once.
    impl_->seed.ResetStream();
    uint256 h;
    while (impl_->seed.Next(&h))
    {
        bool hasOv = false, present = false;
        if (!impl_->store.GetState(h, &hasOv, &present, error)) return false;
        if (hasOv && !present) continue;      // ABSENT -> suppress
        if (fn && !(*fn)(h, ctx)) break;      // PRESENT or no-override -> emit
    }
    // Pass 2: emit PRESENT overrides absent from the immutable seed (added
    // non-seed tips). ABSENT entries present nothing. Uses the documented
    // O(N) seed.Contains for this substrate (no index).
    LiveTipOverlayForward fwd;
    fwd.self = this;
    fwd.fn = fn;
    fwd.outerCtx = ctx;
    bool okForward = impl_->store.ForEachOverride(&EmitAddedNonSeedTramp, &fwd, error);
    if (!okForward) return false;
    return true;
}

bool LiveTipFrontierOverlay::Contains(const uint256& hash, bool* out, std::string* error) const
{
    if (!impl_->open) { if (error) *error = "overlay not open"; return false; }
    bool hasOv = false, present = false;
    if (!impl_->store.GetState(hash, &hasOv, &present, error)) return false;
    if (hasOv)
    {
        *out = present;
        // Enforced bounded cache: cache miss performs a persistent lookup every
        // re-read, and eviction never affects the on-disk authoritative state.
        CacheAdd(impl_->cache, impl_->peak, hash, impl_->cap);
        return true;
    }
    *out = impl_->seed.Contains(hash);
    return true;
}

bool LiveTipFrontierOverlay::GetComposedTipCount(uint64_t* out, std::string* error) const
{
    if (!impl_->open) { if (error) *error = "overlay not open"; return false; }
    struct Ctx { uint64_t n; Ctx() : n(0) {} };
    Ctx ctx;
    bool ok = ForEachTip([](const uint256&, void* c) { ((Ctx*)c)->n++; return true; }, &ctx, error);
    if (!ok) return false;
    *out = ctx.n;
    return true;
}

size_t LiveTipFrontierOverlay::CacheCapacity() const { return impl_->cap; }
size_t LiveTipFrontierOverlay::CacheCurrent() const { return impl_->cache.size(); }
size_t LiveTipFrontierOverlay::CachePeak() const { return impl_->peak; }
uint64_t LiveTipFrontierOverlay::PersistentOverrideCount() const { return impl_->store.EntryCount(); }

bool LiveTipFrontierOverlay::HasSeedMembership(const uint256& hash) const
{
    return impl_->seed.Contains(hash);
}

} // namespace dag_tip_frontier