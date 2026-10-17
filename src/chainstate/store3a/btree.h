// Stage 3A-R1 - copy-on-write paged B+tree with Merkle node hashing.
// Header: on-disk format + API. Keys are 36-byte outpoint keys; values are opaque
// byte blobs (Coin::encode()). Not part of any production build.
//
// R1: every normal traversal (get/put/erase/prefix scan/BIP30) verifies each page
// against its authenticated hash from the parent (and the root against the committed
// root hash) BEFORE consuming it -> O(depth) verification, corrupt pages fail closed.
#pragma once
#include "chainstate.h"
#include <array>
#include <deque>
#include <vector>
#include <string>

namespace cs { namespace store3a {

static const size_t PAGE_SIZE = 4096;
enum : uint8_t { NT_LEAF = 1, NT_INTERNAL = 2 };

typedef std::array<uint8_t, 36> Key;

inline int keyCmp(const Key& a, const Key& b) {
    int c = memcmp(a.data(), b.data(), 36);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}

struct LeafEntry { Key key; std::vector<uint8_t> val; };

struct Node {
    uint8_t type = 0;
    std::vector<LeafEntry> entries;        // leaf
    std::vector<uint32_t> children;        // internal: count+1 ids
    std::vector<Hash> chashes;             // internal: count+1 child node hashes
    std::vector<Key> keys;                 // internal: count separators
};

// Merkle node hash (values depend only on the node's own page contents).
Hash leafHash(const Node& n);
Hash internalHash(const Node& n);
size_t leafSize(const Node& n);
size_t internalSize(const Node& n);
bool encodeNode(const Node& n, uint8_t* buf);     // false if it does not fit PAGE_SIZE
bool decodeNode(const uint8_t* buf, Node* n, ChainError* e);

struct SplitResult { bool split = false; Key sep{}; uint32_t rightId = 0; Hash rightHash{}; };

class BTree {
public:
    ~BTree() { close(); }
    void close();
    bool create(const std::string& path, ChainError* e);   // new empty tree (page 0 = empty leaf)
    bool open(const std::string& path, ChainError* e);      // open page file; drops an unreferenced trailing partial page
    bool fsync(ChainError* e);

    bool setRoot(uint32_t id, const Hash& h, ChainError* e); // load root page; verify hash (O(1))
    bool checkRootHash(uint32_t id, const Hash& h, ChainError* e) const; // verify without mutating
    bool loadPage0AsRoot(ChainError* e);                     // fresh empty store (page 0)
    uint32_t rootId() const { return rootId_; }
    const Hash& rootHash() const { return rootHash_; }
    uint32_t nextPageId() const { return nextId_; }
    int fd() const { return fd_; }
    const std::string& path() const { return path_; }
    bool rootIsLeaf() const { return rootIsLeaf_; }
    size_t rootCount() const { return rootCount_; }
    bool truncatedTail() const { return truncatedTail_; }     // R1: an unreferenced partial page was dropped at open
    uint64_t pageReads() const { return reads_; }              // R1 instrumentation
    uint64_t pageWrites() const { return writes_; }

    bool get(const Key& k, std::vector<uint8_t>* val, bool* found, ChainError* e) const;
    bool existsTxidPrefix(const uint8_t* txid32, bool* found, ChainError* e) const; // BIP30
    bool keysWithTxidPrefix(const uint8_t* txid32, std::vector<Key>* out, ChainError* e) const;
    bool put(const Key& k, const std::vector<uint8_t>& val, ChainError* e);
    bool erase(const Key& k, ChainError* e);
    // Free-page reuse (S1.7-G): pages retired by CoW commits are reclaimed and reused once
    // their generation is acknowledged, so the append-only file stops growing unboundedly
    // (no on-disk format change). Call retireMovedToFree() after each acknowledged commit.
    void retireMovedToFree();                      // move this commit's retired pages into the free-list
    void clearRetired();                          // drop any staged retired set (used on commit rollback)
    size_t freeListSize() const { return freeList_.size(); }
    bool collectAll(std::vector<LeafEntry>* out, ChainError* e) const;
    bool verifyAll(ChainError* e) const;                     // full traversal (test only)

    // R1 test hook: the next page append writes only `bytes` (< PAGE_SIZE) bytes and fails (simulated short write).
    void injectPartialAppendOnce(size_t bytes) { partialAppendBytes_ = bytes; }
    // R2 test hook: let the first (n-1) appends succeed, then fail the n-th with a short write.
    void injectPartialAppendOn(size_t n) { partialAppendOn_ = n; }

private:
    bool readPage(uint32_t id, uint8_t* buf, ChainError* e) const;
    // read + decode a page and verify it against its authenticated hash (O(page size)).
    bool readNodeVerified(uint32_t id, const Hash& expect, Node* n, ChainError* e) const;
    bool appendPage(const uint8_t* buf, uint32_t* outId, ChainError* e);
    bool modRec(uint32_t id, const Hash& expect, const Key& k, const std::vector<uint8_t>* val,
                uint32_t* outId, Hash* outHash, SplitResult* split, ChainError* e);
    bool collectRec(uint32_t id, const Hash& expect, std::vector<LeafEntry>* out, ChainError* e) const;
    bool verifyRec(uint32_t id, const Hash& expect, ChainError* e) const;
    bool keysPrefixRec(uint32_t id, const Hash& expect, const Key& lo, const Key& hi, std::vector<Key>* out, ChainError* e) const;

    int fd_ = -1;
    std::string path_;
    uint32_t nextId_ = 0;
    uint32_t rootId_ = 0;
    Hash rootHash_{};
    bool rootIsLeaf_ = true;
    size_t rootCount_ = 0;
    bool truncatedTail_ = false;
    size_t partialAppendBytes_ = 0;
    size_t partialAppendOn_ = 0;
    mutable uint64_t reads_ = 0;
    uint64_t writes_ = 0;
    std::vector<uint32_t> commitRetired_;   // pages retired by the current commit (CoW rewrites)
    std::deque<uint32_t> freeList_;          // reusable pages unreachable from ev. current root
};

}} // namespace cs::store3a
