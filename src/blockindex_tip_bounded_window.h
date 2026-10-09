// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.

#ifndef INNOVA_BLOCKINDEX_TIP_BOUNDED_WINDOW_H
#define INNOVA_BLOCKINDEX_TIP_BOUNDED_WINDOW_H

// Repair #3: bounded-memory access to the append-only fixed-record tip stores.
//
// tip-records.dat / tip-derived.dat / tip-active.dat are fixed-record files
// whose entry for slot S lives at byte offset headerSize + S*entrySize. The
// tip authority therefore does NOT need O(chain-height) RAM: this class keeps
// only
//   (1) a bounded ring of the newest `ringCap` entries (suffix window), and
//   (2) a byte-budgeted LRU of positional-pread decodes,
// so any access below the window costs ONE positional pread and is returned
// BY VALUE. Total tip-authority record/derived/active RAM is
// O(ringCap) + O(lruBytes) + O(new appends), independent of chain height.
//
// Ordering contract with the owning BlockIndexTipAuthority:
//   - normal append: the caller appends+fsyncs the encoded entries (existing
//     R4 incremental path), then calls PushBack() with the newly committed
//     entries. Ring advance is O(new).
//   - CoW replacement (truncate / reorg stream a complete new file and rename
//     it over the store): the caller calls SetCount(newCount). The ring is
//     dropped; subsequent Gets refill lazily from positional preads, so RAM
//     stays bounded at every mutation.
//   - Open: InitializeCount() stats the file, opens the read handle, and
//     warms the ring with the newest min(ringCap, count) entries.
//
// Single-threaded: the owning BlockIndexTipAuthority is not internally locked
// (unchanged contract); all counters are mutable so const Gets stay const.

#include "fixed_blockindex_store.h"
#include "blockindex_derived_state.h"
#include "blockindex_activeindex.h"

#include <boost/filesystem/operations.hpp>

#include <stdio.h>
#include <string.h>

#include <deque>
#include <functional>
#include <list>
#include <string>
#include <unordered_map>
#include <vector>

struct TipWindowStats
{
    uint64_t count = 0;          // logical committed entry count (slots 0..count-1)
    size_t ringCap = 0;
    size_t ringResident = 0;     // entries held in the ring window
    uint64_t ringBaseSlot = 0;   // first slot covered by the ring
    size_t lruEntries = 0;
    size_t lruBytes = 0;
    size_t lruBytesCap = 0;
    uint64_t gets = 0, ringHits = 0, lruHits = 0, preadMisses = 0;
};

namespace blocktipwin {

// Byte-budgeted LRU keyed by slot; pure derived state, never authoritative.
class SlotByteLru
{
public:
    explicit SlotByteLru(size_t maxBytes) : maxBytes_(maxBytes) {}

    // Returns NULL when `slot` is not cached; otherwise points at the cached
    // payload bytes and refreshes the entry to MRU position.
    const unsigned char* Find(uint64_t slot) const
    {
        std::unordered_map<uint64_t, std::list<Item>::iterator>::const_iterator it =
            map_.find(slot);
        if (it == map_.end())
            return NULL;
        // move to front (MRU)
        std::list<Item>::iterator lit = it->second;
        list_.splice(list_.begin(), list_, lit);
        std::vector<unsigned char>& p = lit->payload;
        return p.empty() ? NULL : (const unsigned char*)&p[0];
    }

    void Put(uint64_t slot, const unsigned char* data, size_t size)
    {
        std::unordered_map<uint64_t, std::list<Item>::iterator>::iterator it =
            map_.find(slot);
        if (it != map_.end())
        {
            bytes_ -= it->second->payload.size();
            list_.erase(it->second);
            map_.erase(it);
        }
        if (size > maxBytes_)
            return; // single oversized entry can never fit
        Item item;
        item.slot = slot;
        item.payload.assign(data, data + size);
        bytes_ += size;
        list_.push_front(item);
        map_[slot] = list_.begin();
        while (bytes_ > maxBytes_ && !list_.empty())
        {
            bytes_ -= list_.back().payload.size();
            map_.erase(list_.back().slot);
            list_.pop_back();
        }
    }

    void Clear()
    {
        map_.clear();
        list_.clear();
        bytes_ = 0;
    }
    size_t Bytes() const { return bytes_; }
    size_t Entries() const { return map_.size(); }
    size_t Cap() const { return maxBytes_; }

private:
    struct Item
    {
        uint64_t slot = 0;
        std::vector<unsigned char> payload;
    };
    size_t maxBytes_;
    size_t bytes_ = 0;
    mutable std::list<Item> list_; // front == MRU
    mutable std::unordered_map<uint64_t, std::list<Item>::iterator> map_;
};

} // namespace blocktipwin

// Bounded positional window over ONE fixed-record store file.
template <class T>
class TipStoreWindow
{
public:
    typedef std::function<bool(const unsigned char*, size_t, T*, std::string*)>
        DecodeFn;
    typedef std::function<bool(uint64_t slot, const T&)> VisitFn;

    TipStoreWindow(const boost::filesystem::path& path, uint32_t headerSize,
                   uint32_t entrySize, size_t ringCap, size_t lruBytesCap,
                   DecodeFn decode)
        : path_(path), headerSize_(headerSize), entrySize_(entrySize),
          ringCap_(ringCap), lru_(lruBytesCap), decode_(decode)
    {
    }

    ~TipStoreWindow() { ReleaseHandle(); }

    TipStoreWindow(const TipStoreWindow&) = delete;
    TipStoreWindow& operator=(const TipStoreWindow&) = delete;
    // Move-assign is used ONLY by the owning Impl when InitializePaths re-points
    // the windows at the current store paths (identical caps + decoders).
    TipStoreWindow(TipStoreWindow&& o) { MoveFrom(o); }
    TipStoreWindow& operator=(TipStoreWindow&& o)
    {
        if (this != &o)
        {
            ReleaseHandle();
            MoveFrom(o);
        }
        return *this;
    }

    // ---- lifecycle ------------------------------------------------------

    // Open the read handle for `committedCount` logical entries and warm the
    // ring with the newest min(ringCap, count) entries streamed from disk.
    bool InitializeCount(uint64_t committedCount, std::string* error);
    // After a CoW file replacement (truncate/reorg rename): adopt the new
    // logical count, drop the ring (lazy refill), and refresh the snapshot.
    // The on-disk prefix relationship is the caller's responsibility.
    bool SetCount(uint64_t newCount, std::string* error);
    // Reset everything (Close()/redeploy); keeps path+caps.
    void Reset();

    uint64_t Count() const { return count_; }

    // ---- mutation (O(new)) ----------------------------------------------

    // Call AFTER the encoded entries for slots [count_, count_+n) are appended
    // + durable on disk. Advances the logical count and the ring by n.
    void PushBack(const T* items, size_t n);

    // Roll back the LAST logical entry (used by the failed-commit path so the
    // in-memory window never runs ahead of the committed tip.meta). Shrink is
    // always safe: a shrink is a pure prefix of the append history.
    void Pop();

    // ---- access (by-value) -----------------------------------------------

    // Decode entry `slot`; served by ring suffix, LRU, or ONE positional
    // pread. `slot` must be < Count().
    bool Get(uint64_t slot, T* out, std::string* error) const;

    // Sequential streaming decode over [beginSlot, endSlotExclusive) in
    // ascending order, O(1) auxiliary memory, cache-neutral (does not touch
    // the ring or the LRU). `visit` returning false stops early (not an
    // error). Fails closed on any short/undecodable read.
    bool StreamVisit(uint64_t beginSlot, uint64_t endSlotExclusive,
                     const VisitFn& visit, std::string* error) const;

    TipWindowStats Stats() const;

private:
    void MoveFrom(TipStoreWindow& o)
    {
        if (fh_) { fclose(fh_); fh_ = NULL; }
        path_ = o.path_;
        headerSize_ = o.headerSize_;
        entrySize_ = o.entrySize_;
        ringCap_ = o.ringCap_;
        // lru_ has no move: copy-by-value memberwise via assignment of the
        // whole object is unavailable; instead swap internals via a shallow
        // copy through std::swap of the full objects is impossible without a
        // move. Recreate the LRU with the same cap (cache loss is safe).
        lru_ = blocktipwin::SlotByteLru(o.lru_.Cap());
        decode_ = o.decode_;
        count_ = o.count_;
        ringBase_ = o.ringBase_;
        ring_.clear(); // ring is pure cache; drop it (lazy refill is correct)
        gets_ = 0; ringHits_ = 0; lruHits_ = 0; preadMisses_ = 0;
        sizeSnap_ = 0; mtimeSnap_ = 0; handleKnownFile_ = false; fh_ = NULL;
    }

    mutable FILE* fh_ = NULL; // positional-read handle
    mutable bool handleKnownFile_ = false;
    mutable uint64_t sizeSnap_ = 0;
    mutable long mtimeSnap_ = 0;
    mutable uint64_t gets_ = 0, ringHits_ = 0, lruHits_ = 0, preadMisses_ = 0;

    bool RefreshHandle(std::string* error) const; // stat + (re)open on change
    void ReleaseHandle() const;
    bool PreadDecode(uint64_t slot, T* out, std::string* error) const;

    boost::filesystem::path path_;
    uint32_t headerSize_;
    uint32_t entrySize_;
    size_t ringCap_;
    mutable blocktipwin::SlotByteLru lru_;
    DecodeFn decode_;

    uint64_t count_ = 0;
    uint64_t ringBase_ = 0;    // first slot covered by ring_ (== count_ when empty)
    std::deque<T> ring_;       // ring_[k] == entry (ringBase_ + k)
};

// ---- implementation ---------------------------------------------------------

template <class T>
void TipStoreWindow<T>::ReleaseHandle() const
{
    if (fh_)
    {
        fclose(fh_);
        fh_ = NULL;
    }
    handleKnownFile_ = false;
}

template <class T>
bool TipStoreWindow<T>::RefreshHandle(std::string* error) const
{
    // The store file may have been replaced by a CoW rename since the last
    // access: detect via size/mtime snapshot and reopen. POSIX keeps an open
    // fd on an unlinked inode readable, so a stale handle would serve the OLD
    // file silently — the snapshot check guarantees we never do that.
    boost::system::error_code ec;
    uintmax_t sz = boost::filesystem::file_size(path_, ec);
    if (ec)
    {
        if (error) *error = "stat store failed: " + path_.string();
        ReleaseHandle();
        return false;
    }
    long mt = (long)boost::filesystem::last_write_time(path_, ec);
    if (ec)
    {
        if (error) *error = "stat store mtime failed: " + path_.string();
        ReleaseHandle();
        return false;
    }
    if (fh_ && handleKnownFile_ && sz == sizeSnap_ && mt == mtimeSnap_)
        return true; // same file, handle valid
    ReleaseHandle();
    fh_ = fopen(path_.string().c_str(), "rb");
    if (!fh_)
    {
        if (error) *error = "open store for read failed: " + path_.string();
        return false;
    }
    sizeSnap_ = sz;
    mtimeSnap_ = mt;
    handleKnownFile_ = true;
    return true;
}

template <class T>
bool TipStoreWindow<T>::PreadDecode(uint64_t slot, T* out, std::string* error) const
{
    if (!RefreshHandle(error))
        return false;
    const unsigned long off = headerSize_ + (unsigned long)slot * entrySize_;
    if (fseek(fh_, off, SEEK_SET) != 0)
    {
        if (error) *error = "seek store failed: " + path_.string();
        return false;
    }
    std::vector<unsigned char> buf(entrySize_);
    if (fread(&buf[0], 1, entrySize_, fh_) != entrySize_)
    {
        if (error)
            *error = "short positional read of store slot " +
                     std::to_string(slot) + ": " + path_.string();
        return false;
    }
    std::string derr;
    if (!decode_(&buf[0], buf.size(), out, &derr))
    {
        if (error)
            *error = "decode store slot " + std::to_string(slot) + ": " + derr;
        return false;
    }
    return true;
}

template <class T>
bool TipStoreWindow<T>::InitializeCount(uint64_t committedCount, std::string* error)
{
    ReleaseHandle();
    ring_.clear();
    lru_.Clear();
    count_ = committedCount;
    gets_ = ringHits_ = lruHits_ = preadMisses_ = 0;
    // Warm the suffix ring: newest up-to-ringCap entries streamed from disk.
    if (count_ == 0)
        return RefreshHandle(error); // ensure the file exists + is statable
    const uint64_t warm = count_ < (uint64_t)ringCap_ ? count_ : (uint64_t)ringCap_;
    const uint64_t begin = count_ - warm;
    ring_.clear();
    ringBase_ = begin;
    for (uint64_t s = begin; s < count_; ++s)
    {
        T v;
        if (!PreadDecode(s, &v, error))
        {
            ring_.clear();
            ringBase_ = count_;
            ReleaseHandle();
            return false;
        }
        ring_.push_back(v);
    }
    return true;
}

template <class T>
void TipStoreWindow<T>::Reset()
{
    ReleaseHandle();
    ring_.clear();
    ringBase_ = 0;
    count_ = 0;
    lru_.Clear();
    sizeSnap_ = 0;
    mtimeSnap_ = 0;
    handleKnownFile_ = false;
}

template <class T>
bool TipStoreWindow<T>::SetCount(uint64_t newCount, std::string* error)
{
    count_ = newCount;
    lru_.Clear();
    ring_.clear();
    ringBase_ = newCount; // empty ring: all access goes through preads
    // The caller calls SetCount AFTER a CoW rename replaced the store file. The
    // (size,mtime) staleness snapshot CANNOT be trusted here: a rename within
    // the same mtime second keeps size+mtime identical and would leave a stale
    // fd on the unlinked inode, silently serving the OLD file. Force a reopen.
    ReleaseHandle();
    if (!RefreshHandle(error))
        return false;
    return true;
}

template <class T>
void TipStoreWindow<T>::PushBack(const T* items, size_t n)
{
    for (size_t k = 0; k < n; ++k)
    {
        ring_.push_back(items[k]);
        ++count_;
        while (ring_.size() > ringCap_)
        {
            ring_.pop_front();
            ++ringBase_;
        }
    }
}

template <class T>
void TipStoreWindow<T>::Pop()
{
    if (count_ == 0)
        return;
    --count_;
    // Drop any ring entries beyond the (shrunk) count; the ring still holds a
    // contiguous suffix (slot algebra ringBase_..ringBase_+size-1 == the OLD
    // top), so the newest KEPT entry survives as the new suffix top.
    while (!ring_.empty() && ringBase_ + ring_.size() > count_)
        ring_.pop_back();
    if (ring_.empty())
        ringBase_ = count_;
}

template <class T>
bool TipStoreWindow<T>::Get(uint64_t slot, T* out, std::string* error) const
{
    ++gets_;
    if (slot >= count_)
    {
        if (error) *error = "store slot out of committed range";
        return false;
    }
    if (!ring_.empty() && slot >= ringBase_ && slot - ringBase_ < ring_.size())
    {
        ++ringHits_;
        *out = ring_[(size_t)(slot - ringBase_)]; // by value
        return true;
    }
    // LRU of positional decodes (pure derived cache).
    if (const unsigned char* hit = lru_.Find(slot))
    {
        ++lruHits_;
        std::string derr;
        if (!decode_(hit, entrySize_, out, &derr))
        {
            if (error) *error = "decode cached store slot: " + derr;
            return false;
        }
        return true;
    }
    ++preadMisses_;
    // One positional pread; insert the RAW bytes into the byte LRU afterwards
    // so the cached payload is the exact encoded entry (decode reuses it
    // without a second disk touch).
    if (!RefreshHandle(error))
        return false;
    const unsigned long off = headerSize_ + (unsigned long)slot * entrySize_;
    if (fseek(fh_, off, SEEK_SET) != 0)
    {
        if (error) *error = "seek store failed: " + path_.string();
        return false;
    }
    std::vector<unsigned char> buf(entrySize_);
    if (fread(&buf[0], 1, entrySize_, fh_) != entrySize_)
    {
        if (error)
            *error = "short positional read of store slot " +
                     std::to_string(slot) + ": " + path_.string();
        return false;
    }
    std::string derr;
    if (!decode_(&buf[0], buf.size(), out, &derr))
    {
        if (error)
            *error = "decode store slot " + std::to_string(slot) + ": " + derr;
        return false;
    }
    lru_.Put(slot, &buf[0], buf.size());
    return true;
}

template <class T>
bool TipStoreWindow<T>::StreamVisit(uint64_t beginSlot, uint64_t endSlotExclusive,
                                    const VisitFn& visit, std::string* error) const
{
    if (endSlotExclusive > count_)
    {
        if (error) *error = "stream range exceeds committed count";
        return false;
    }
    if (!RefreshHandle(error))
        return false;
    for (uint64_t s = beginSlot; s < endSlotExclusive; ++s)
    {
        const unsigned long off = headerSize_ + (unsigned long)s * entrySize_;
        if (fseek(fh_, off, SEEK_SET) != 0)
        {
            if (error) *error = "seek store failed: " + path_.string();
            return false;
        }
        std::vector<unsigned char> buf(entrySize_);
        if (fread(&buf[0], 1, entrySize_, fh_) != entrySize_)
        {
            if (error)
                *error = "short sequential read of store slot " +
                         std::to_string(s) + ": " + path_.string();
            return false;
        }
        T v;
        std::string derr;
        if (!decode_(&buf[0], buf.size(), &v, &derr))
        {
            if (error)
                *error = "decode store slot " + std::to_string(s) + ": " + derr;
            return false;
        }
        if (!visit(s, v))
            return true; // caller-requested early stop
    }
    return true;
}

template <class T>
TipWindowStats TipStoreWindow<T>::Stats() const
{
    TipWindowStats st;
    st.count = count_;
    st.ringCap = ringCap_;
    st.ringResident = ring_.size();
    st.ringBaseSlot = ringBase_;
    st.lruEntries = lru_.Entries();
    st.lruBytes = lru_.Bytes();
    st.lruBytesCap = lru_.Cap();
    st.gets = gets_;
    st.ringHits = ringHits_;
    st.lruHits = lruHits_;
    st.preadMisses = preadMisses_;
    return st;
}

#endif // INNOVA_BLOCKINDEX_TIP_BOUNDED_WINDOW_H
