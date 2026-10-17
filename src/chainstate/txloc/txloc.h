// TX-LOC -- historical transaction LOCATION index (Item 1 of the V2 cutover).
//
// Replaces the legacy leveldb "tx" -> CTxIndex (txid-locator) read path for
// historical transaction lookup (getrawtransaction / gettransaction, wallet
// ReadTxIndex/ContainsTx, mempool/p2p presence). It stores EXACTLY what legacy
// historical lookup needs -- txid -> CDiskTxPos(nFile, nBlockPos, nTxPos) +
// nHeight, so a fully-spent (and duplicate-txid) transaction can be located and
// read back from the authoritative blk*.dat archive. It is NOT a UTXO or
// consensus authority: Store3A owns coin state; BI-V2 owns the block index.
//
// Semantics (derived index -- must NOT become a second authority):
//   * absent          -> rebuild once by scanning the archive (or bulk feed);
//   * present + valid -> serve reads;
//   * present + INCONSISTENT/corrupt -> FAIL CLOSED (refuse; never silently
//     rebuild, never serve a wrong tx).
//
// Format (read-optimized, write-once per build/compaction):
//   txloc.dat : magic('TXLC') + version(le32) + recordWidth(le32) +
//               recordCount(le64) + N sorted records, ascending by the 32-byte
//               txid as unsigned big-endian.  Each record is fixed-width:
//               32B txid + 4B nFile + 4B nBlockPos + 4B nTxPos + 4B nHeight
//               = 48 B.
//   txloc.dir : a coarse directory: one boundary key every DIR_STEP records,
//               ASCII/le, rebuilt by the builder and read at open (small).
//
// Lookup = binary search over the record range using the directory as a first
// guess (bounded random-access pread reads; constant RAM for the directory vs
// record count). The full record set is never loaded into RAM.
//
// This slice covers the BULK build -> sorted main file -> directory -> lookup
// -> restart -> fail-closed path (the migration/read path). The IBD incremental
// ingest (write-ahead + in-order compaction) is the next slice.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string>
#include <vector>
#include <functional>

namespace txloc {

// A located transaction record (mirrors legacy CDiskTxPos + height).
struct Loc {
    uint32_t nFile = 0;
    uint32_t nBlockPos = 0;
    uint32_t nTxPos = 0;
    int32_t  nHeight = 0;
};

class ChainError {
public:
    std::string msg;
    int code = 0;               // 0 none; 1 IO; 2 CORRUPT; 3 NOT_FOUND; 4 NOT_OPEN
    std::string str() const { return msg; }
};

enum {
    TXLOC_MAGIC     = 0x434C5854u,        // 'TXLC'
    TXLOC_VERSION   = 1,
    TXLOC_RECWIDTH  = 48,                  // 32 + 4 + 4 + 4 + 4
    DIR_STEP        = 4096,
    ERR_NONE        = 0, ERR_IO = 1, ERR_CORRUPT = 2, ERR_NOT_FOUND = 3, ERR_NOT_OPEN = 4
};

// Header: magic le32, version le32, recordWidth le32, recordCount le64 = 20 B.
struct Header {
    uint32_t magic = TXLOC_MAGIC;
    uint32_t version = TXLOC_VERSION;
    uint32_t recordWidth = TXLOC_RECWIDTH;
    uint64_t recordCount = 0;
};

// Build a sorted, deduplicated location index from an arbitrary (height-ordered
// during IBD, or archive-order during migration) feed of (txid, loc). Keeps the
// FIRST occurrence per txid (earliest on-chain position; matches legacy
// AddTxIndex semantics). The caller polls `feed(txidOut, locOut)` for each next
// record; it returns true and fills the out-params while a record remains, then
// false when exhausted. Output written atomically: temp then rename. On failure
// the target dir is left byte-unchanged (fail-closed). Returns false on any IO
// error.
bool buildIndex(const std::string& indexPath,
                const std::function<bool(unsigned char* txidOut /*32B*/,
                                         Loc* locOut)>& feed,
                ChainError* e);

// Open for reads. Refuses (fail-closed) a present-but-invalid index; signals
// chains with absent=false when the index does not exist yet.
class TxLocator {
public:
    TxLocator() {}
    ~TxLocator();
    bool open(const std::string& indexPath, bool* absent, ChainError* e);
    void close();
    bool get(const unsigned char* txid /*32B*/, Loc* out, bool* found, ChainError* e) const;
    uint64_t count() const { return recordCount_; }
    bool opened() const { return fd_ >= 0; }
private:
    bool readHeader(ChainError* e);
    bool readRecord(uint64_t idx, unsigned char* txidOut, Loc* locOut, ChainError* e) const;
    int fd_ = -1;
    uint64_t recordCount_ = 0;
    uint32_t recordWidth_ = 0;
    std::string indexPath_;
    std::vector<unsigned char> dirKeys_;   // boundary txid[32] every DIR_STEP
};

} // namespace txloc