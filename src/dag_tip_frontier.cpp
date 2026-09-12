// Copyright (c) 2019-2026 The Innova developers
// Distributed under the MIT/X11 software license.
//
// A.13.6-R2c.1 — immutable DAG tip frontier builder + reader implementation.

#include "dag_tip_frontier.h"

#include "dag.h"       // CBlockDAGData (decode)
#include "dag_source_binding_verifier.h" // reusable R1a bounded external-sort verifier
#include "serialize.h" // CDataStream

#include <boost/filesystem.hpp>
#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <openssl/sha.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <queue>
#include <vector>

namespace dag_tip_frontier {
namespace {

struct SortRecord
{
    uint256 hash;
    uint8_t seenNode;    // this hash exists as a daglinks node
    uint8_t seenParent;  // this hash is referenced as a DAG parent
};

static bool SortRecordLess(const SortRecord& a, const SortRecord& b)
{
    return a.hash < b.hash; // exact uint256::operator<
}

static bool WriteSortRecord(std::ofstream& out, const SortRecord& r)
{
    out.write((const char*)r.hash.begin(), 32);
    out.write((const char*)&r.seenNode, 1);
    out.write((const char*)&r.seenParent, 1);
    return out.good();
}

static bool ReadSortRecord(std::ifstream& in, SortRecord* out)
{
    char hash[32];
    in.read(hash, sizeof(hash));
    if (in.eof() && in.gcount() == 0)
        return false;
    if (in.gcount() != (std::streamsize)sizeof(hash))
        throw std::runtime_error("truncated run hash");
    memcpy(out->hash.begin(), hash, 32);
    in.read((char*)&out->seenNode, 1);
    in.read((char*)&out->seenParent, 1);
    if (!in.good())
        throw std::runtime_error("truncated run flags");
    return true;
}

static bool DecodeDAGKey(const leveldb::Slice& slice, uint256* hash)
{
    try
    {
        CDataStream key(SER_DISK, CLIENT_VERSION);
        key.write(slice.data(), slice.size());
        std::string type;
        key >> type;
        if (type != "daglinks") return false;
        key >> *hash;
        return key.size() == 0;
    }
    catch (const std::exception&) { return false; }
}

static bool DecodeDAGParents(const leveldb::Slice& slice, std::vector<uint256>* parents)
{
    try
    {
        CDataStream value(SER_DISK, CLIENT_VERSION);
        value.write(slice.data(), slice.size());
        CBlockDAGData data;
        value >> data; // same full CBlockDAGData decode as builder/verifier
        *parents = data.vDAGParents;
        return true;
    }
    catch (const std::exception&) { return false; }
}

// Write a chunk as a sorted run, folding adjacent duplicates by OR-ing flags.
static bool WriteRun(const boost::filesystem::path& path,
                     std::vector<SortRecord>* records,
                     BuildResult* result)
{
    std::sort(records->begin(), records->end(), SortRecordLess);
    std::ofstream out(path.string().c_str(), std::ios::binary | std::ios::trunc);
    if (!out.good()) return false;
    size_t i = 0;
    while (i < records->size())
    {
        SortRecord merged = (*records)[i];
        while (i + 1 < records->size() && (*records)[i + 1].hash == merged.hash)
        {
            ++i;
            merged.seenNode |= (*records)[i].seenNode;
            merged.seenParent |= (*records)[i].seenParent;
        }
        if (!WriteSortRecord(out, merged)) return false;
        ++i;
    }
    out.close();
    if (!out.good()) return false;
    result->temporaryBytesWritten += (uint64_t)boost::filesystem::file_size(path);
    return true;
}

struct Cursor
{
    std::ifstream in;
    SortRecord record;
    bool valid;
    Cursor() : valid(false) {}
};

// Merge up to maxOpenRuns sorted runs into a single sorted unique stream.
// Folds duplicate hashes by OR-ing their flags.
//
// emitTipsOnly=false (intermediate pass): writes every distinct record back as
// a merged run file (outRun path) so the stream can be re-merged.
// emitTipsOnly=true (final pass): emits only tip hashes (node && !parent) in
// ascending uint256 order to the tips file (may be NULL) feeding digest and
// tipCountOut.
static bool MergeRuns(const std::vector<boost::filesystem::path>& inputs,
                      bool emitTipsOnly,
                      const boost::filesystem::path* outRun,
                      const boost::filesystem::path* tipsOut,
                      SHA256_CTX* digest,   // final-pass frontier digest
                      uint64_t* tipCountOut,
                      BuildResult* result)
{
    std::vector<Cursor> cursors(inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        cursors[i].in.open(inputs[i].string().c_str(), std::ios::binary);
        if (!cursors[i].in.good()) return false;
        cursors[i].valid = ReadSortRecord(cursors[i].in, &cursors[i].record);
    }
    std::ofstream out;
    if (outRun)
    {
        out.open(outRun->string().c_str(), std::ios::binary | std::ios::trunc);
        if (!out.good()) return false;
    }
    std::ofstream tips;
    if (tipsOut)
    {
        tips.open(tipsOut->string().c_str(), std::ios::binary | std::ios::trunc);
        if (!tips.good()) return false;
    }
    bool have = false;
    uint256 cur;
    uint8_t curNode = 0, curParent = 0;
    uint64_t tipsCount = 0;
    while (true)
    {
        size_t best = inputs.size();
        for (size_t i = 0; i < cursors.size(); ++i)
            if (cursors[i].valid && (best == inputs.size() || cursors[i].record.hash < cursors[best].record.hash))
                best = i;
        if (best == inputs.size()) break;
        const SortRecord& rec = cursors[best].record;
        if (have && rec.hash == cur)
        {
            curNode |= rec.seenNode;
            curParent |= rec.seenParent;
        }
        else
        {
            if (have)
    {
        if (emitTipsOnly)
        {
            // A tip is a node that is never referenced as a parent.
            if (curNode && !curParent)
            {
                ++tipsCount;
                if (tips.good()) tips.write((const char*)cur.begin(), 32);
                if (digest) SHA256_Update(digest, cur.begin(), 32);
            }
        }
        else
        {
            SortRecord merged;
            merged.hash = cur; merged.seenNode = curNode; merged.seenParent = curParent;
            if (!WriteSortRecord(out, merged)) { /* handled after close */ }
        }
    }
            cur = rec.hash;
            curNode = rec.seenNode;
            curParent = rec.seenParent;
            have = true;
        }
        cursors[best].valid = ReadSortRecord(cursors[best].in, &cursors[best].record);
    }
    if (have)
    {
        if (emitTipsOnly)
        {
            if (curNode && !curParent)
            {
                ++tipsCount;
                if (tips.good()) tips.write((const char*)cur.begin(), 32);
                if (digest) SHA256_Update(digest, cur.begin(), 32);
            }
        }
        else
        {
            SortRecord merged;
            merged.hash = cur; merged.seenNode = curNode; merged.seenParent = curParent;
            if (!WriteSortRecord(out, merged)) { /* handled after close */ }
        }
    }
    if (tipCountOut) *tipCountOut = tipsCount;
    if (outRun)
    {
        out.close();
        if (!out.good()) return false;
    }
    if (tipsOut)
    {
        tips.close();
        if (!tips.good()) return false;
    }
    return true;
}

static void WriteU64LE(std::vector<unsigned char>& out, uint64_t v)
{
    for (int i = 0; i < 8; ++i) { out.push_back((unsigned char)(v & 0xff)); v >>= 8; }
}

} // namespace

// ---------------------------------------------------------------------------
// Public builder
// ---------------------------------------------------------------------------

bool BuildDagTipFrontier(const std::string& dagLinksDir,
                         const unsigned char expectedDigest[32],
                         uint64_t generation,
                         const std::string& outPath,
                         const BuildOptions& options,
                         BuildResult* result)
{
    if (!result) return false;
    result->status = DAG_TIP_FRONTIER_BUILD_AUTHORITY_FAILURE;
    boost::filesystem::path tempRoot;
    try
    {
        if (!expectedDigest)
        {
            result->error = "null expected DAG digest";
            return false;
        }
        if (options.chunkBytes == 0 || options.maxRecordsPerChunk == 0 || options.maxOpenRuns < 2)
        {
            result->error = "invalid external-sort bounds";
            return false;
        }
        if (options.tempParent.empty())
        {
            result->error = "temporary parent is not specified";
            return false;
        }

        // Reuse the R1a external-sort verifier to bind this build to the exact
        // DAG source digest (deterministic, order-correct, bounded external
        // sort over the sorted node stream). The canonical dagInputDigest is
        // defined over the SORTED daglinks node records, matching the
        // generation-builder's digest semantics; feeding it during raw LevelDB
        // iteration would be order-incorrect.
        {
            DagSourceBindingVerifierOptions vopts;
            vopts.tempParent = options.tempParent;
            DagSourceBindingResult binding = DagSourceBindingVerifier::Verify(
                dagLinksDir, expectedDigest, vopts);
            if (binding.status != DAG_SOURCE_BINDING_VERIFIED)
            {
                result->status = DAG_TIP_FRONTIER_BUILD_DECODE_FAILURE;
                result->error = "source dagInputDigest binding failed: " + binding.error;
                return false;
            }
        }

        leveldb::Options dbOptions;
        dbOptions.create_if_missing = false;
        dbOptions.error_if_exists = false;
        dbOptions.paranoid_checks = true;
        leveldb::DB* db = NULL;
        leveldb::Status openStatus = leveldb::DB::Open(dbOptions, dagLinksDir, &db);
        if (!openStatus.ok())
        {
            result->status = DAG_TIP_FRONTIER_BUILD_SOURCE_UNAVAILABLE;
            result->error = openStatus.ToString();
            return false;
        }

        boost::filesystem::path parent(options.tempParent);
        boost::filesystem::create_directories(parent);
        tempRoot = parent / boost::filesystem::unique_path("dag-frontier-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(tempRoot);

        std::vector<boost::filesystem::path> runs;
        std::vector<SortRecord> chunk;
        size_t chunkBytes = 0;
        leveldb::Iterator* it = db->NewIterator(leveldb::ReadOptions());
        CDataStream prefixStream(SER_DISK, CLIENT_VERSION);
        prefixStream << std::string("daglinks");
        const std::string prefix = prefixStream.str();
        it->Seek(prefix);
        uint64_t runNumber = 0;
        while (it->Valid())
        {
            const leveldb::Slice key = it->key();
            if (key.ToString().compare(0, prefix.size(), prefix) != 0) break;
            uint256 hash;
            std::vector<uint256> parents;
            if (!DecodeDAGKey(key, &hash))
            {
                delete it; delete db;
                result->status = DAG_TIP_FRONTIER_BUILD_DECODE_FAILURE;
                result->error = "invalid daglinks key";
                throw std::runtime_error("decode failure");
            }
            if (!DecodeDAGParents(it->value(), &parents))
            {
                delete it; delete db;
                result->status = DAG_TIP_FRONTIER_BUILD_DECODE_FAILURE;
                result->error = "invalid daglinks value";
                throw std::runtime_error("decode failure");
            }
            result->nodesProcessed++;
            size_t entryBytes = 32 + 2 + parents.size() * 32;
            SortRecord nodeRec;
            nodeRec.hash = hash; nodeRec.seenNode = 1; nodeRec.seenParent = 0;
            chunk.push_back(nodeRec);
            chunkBytes += entryBytes;
            for (size_t p = 0; p < parents.size(); ++p)
            {
                SortRecord parRec;
                parRec.hash = parents[p]; parRec.seenNode = 0; parRec.seenParent = 1;
                chunk.push_back(parRec);
                chunkBytes += entryBytes;
                result->parentRefsProcessed++;
            }
            result->peakChunkRecords = std::max<uint64_t>(result->peakChunkRecords, chunk.size());
            result->peakChunkBytes = std::max<uint64_t>(result->peakChunkBytes, chunkBytes);
            if (chunkBytes >= options.chunkBytes || chunk.size() >= options.maxRecordsPerChunk)
            {
                boost::filesystem::path run = tempRoot / (std::string("run-") + std::to_string(runNumber++) + ".bin");
                if (!WriteRun(run, &chunk, result))
                {
                    delete it; delete db;
                    result->status = DAG_TIP_FRONTIER_BUILD_TEMP_IO_FAILURE;
                    result->error = "cannot write sorted run";
                    throw std::runtime_error("temp io failure");
                }
                runs.push_back(run); chunk.clear(); chunkBytes = 0;
            }
            it->Next();
        }
        leveldb::Status iteratorStatus = it->status();
        delete it; delete db;
        if (!iteratorStatus.ok())
        {
            result->status = DAG_TIP_FRONTIER_BUILD_SOURCE_CORRUPT;
            result->error = iteratorStatus.ToString();
            throw std::runtime_error("source iterator failure");
        }
        if (!chunk.empty())
        {
            boost::filesystem::path run = tempRoot / (std::string("run-") + std::to_string(runNumber++) + ".bin");
            if (!WriteRun(run, &chunk, result))
            {
                result->status = DAG_TIP_FRONTIER_BUILD_TEMP_IO_FAILURE;
                result->error = "cannot write final sorted run";
                throw std::runtime_error("temp io failure");
            }
            runs.push_back(run);
        }
        chunk.clear(); chunkBytes = 0;
        result->runCount = runs.size();

        // External k-way merge passes: reduce to <= maxOpenRuns sorted unique
        // runs, preserving ALL distinct records (node+parent) in each merged
        // run so the final pass can re-derive tips.
        while (runs.size() > options.maxOpenRuns)
        {
            std::vector<boost::filesystem::path> next;
            for (size_t begin = 0; begin < runs.size(); begin += options.maxOpenRuns)
            {
                size_t end = std::min(runs.size(), begin + options.maxOpenRuns);
                std::vector<boost::filesystem::path> group(runs.begin() + begin, runs.begin() + end);
                boost::filesystem::path merged = tempRoot / (std::string("merge-") + std::to_string(result->mergePasses) + "-" + std::to_string(begin) + ".bin");
                uint64_t dummy = 0;
                if (!MergeRuns(group, false, &merged, NULL, NULL, &dummy, result))
                {
                    result->status = DAG_TIP_FRONTIER_BUILD_TEMP_IO_FAILURE;
                    result->error = "cannot merge sorted runs";
                    throw std::runtime_error("merge failure");
                }
                next.push_back(merged);
                for (size_t i = 0; i < group.size(); ++i) boost::filesystem::remove(group[i]);
            }
            runs.swap(next);
            result->mergePasses++;
        }

        // -------------------- Final pass: extract tips + digest ----------------
        // The artifact is assembled AFTER tips are known: header (magic/version/
        // generation/dagInputDigest/frontierDigest/tipCount) + sorted tip hashes.
        boost::filesystem::path tipsFile = tempRoot / "tips.bin";
        uint64_t tips = 0;
        SHA256_CTX digest;
        SHA256_Init(&digest);
        if (!runs.empty())
        {
            if (!MergeRuns(runs, true, NULL, &tipsFile, &digest, &tips, result))
            {
                result->status = DAG_TIP_FRONTIER_BUILD_TEMP_IO_FAILURE;
                result->error = "final merge failure";
                throw std::runtime_error("final merge failure");
            }
        }
        else
        {
            // No DAG nodes at all: empty tip set.
            std::ofstream emptyTips(tipsFile.string().c_str(), std::ios::binary | std::ios::trunc);
            emptyTips.close();
        }
        unsigned char frontierDigest[32];
        SHA256_Final(frontierDigest, &digest);

        // Assemble artifact = header + tips content.
        std::vector<unsigned char> header;
        header.insert(header.end(), (const unsigned char*)FRONTIER_MAGIC, (const unsigned char*)FRONTIER_MAGIC + 12);
        header.push_back(FRONTIER_VERSION);
        WriteU64LE(header, generation);
        header.insert(header.end(), expectedDigest, expectedDigest + 32);
        header.insert(header.end(), frontierDigest, frontierDigest + 32);
        WriteU64LE(header, tips);
        std::ifstream tipsIn(tipsFile.string().c_str(), std::ios::binary);
        std::vector<unsigned char> body((std::istreambuf_iterator<char>(tipsIn)), std::istreambuf_iterator<char>());
        tipsIn.close();
        if (body.size() != tips * 32)
        {
            result->status = DAG_TIP_FRONTIER_BUILD_TEMP_IO_FAILURE;
            result->error = "tips body size mismatch";
            throw std::runtime_error("tips body mismatch");
        }
        std::ofstream artifact(outPath.c_str(), std::ios::binary | std::ios::trunc);
        if (!artifact.good())
        {
            result->status = DAG_TIP_FRONTIER_BUILD_TEMP_IO_FAILURE;
            result->error = "cannot open artifact for write";
            throw std::runtime_error("artifact open failure");
        }
        artifact.write((const char*)header.data(), header.size());
        if (!body.empty()) artifact.write((const char*)body.data(), body.size());
        artifact.close();
        if (!artifact.good())
        {
            result->status = DAG_TIP_FRONTIER_BUILD_TEMP_IO_FAILURE;
            result->error = "artifact write failure";
            throw std::runtime_error("artifact write failure");
        }
        result->frontierTipCount = tips;
        result->artifactBytes = (uint64_t)header.size() + (uint64_t)body.size();
        result->status = DAG_TIP_FRONTIER_BUILD_OK;
        result->error.clear();
    }
    catch (const std::exception& ex)
    {
        if (result->status == DAG_TIP_FRONTIER_BUILD_AUTHORITY_FAILURE)
            result->error = ex.what();
    }
    catch (...)
    {
        if (result->status == DAG_TIP_FRONTIER_BUILD_AUTHORITY_FAILURE)
            result->error = "unknown builder failure";
    }
    if (!tempRoot.empty())
    {
        try { boost::filesystem::remove_all(tempRoot); }
        catch (...) {}
    }
    return result->status == DAG_TIP_FRONTIER_BUILD_OK;
}

// ---------------------------------------------------------------------------
// Header reader
// ---------------------------------------------------------------------------

namespace {

static uint64_t ReadU64LE(const unsigned char* p) { uint64_t v = 0; for (int i = 7; i >= 0; --i) v = (v << 8) | p[i]; return v; }

} // namespace

bool ReadDagTipFrontierHeader(const std::string& path,
                              uint64_t* generation,
                              unsigned char dagInputDigest[32],
                              unsigned char frontierDigest[32],
                              uint64_t* tipCount,
                              std::string* error)
{
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in.good())
    {
        if (error) *error = "cannot open frontier artifact";
        return false;
    }
    unsigned char magic[12];
    in.read((char*)magic, 12);
    if (in.gcount() != 12 || memcmp(magic, FRONTIER_MAGIC, 12) != 0)
    {
        if (error) *error = "frontier artifact magic mismatch";
        return false;
    }
    unsigned char version = 0;
    in.read((char*)&version, 1);
    if (version != FRONTIER_VERSION)
    {
        if (error) *error = "frontier artifact version unsupported";
        return false;
    }
    unsigned char genBytes[8], dig[32], fdig[32], tcBytes[8];
    in.read((char*)genBytes, 8);
    in.read((char*)dig, 32);
    in.read((char*)fdig, 32);
    in.read((char*)tcBytes, 8);
    if (!in.good())
    {
        if (error) *error = "frontier artifact header truncated";
        return false;
    }
    if (generation) *generation = ReadU64LE(genBytes);
    if (dagInputDigest) memcpy(dagInputDigest, dig, 32);
    if (frontierDigest) memcpy(frontierDigest, fdig, 32);
    if (tipCount) *tipCount = ReadU64LE(tcBytes);
    return true;
}

// ---------------------------------------------------------------------------
// Streaming reader
// ---------------------------------------------------------------------------

struct TipFrontierReader::Impl
{
    std::ifstream in;
    bool open;
    uint64_t generation;
    uint64_t tipCount;
    uint64_t remaining;
    unsigned char frontierDigest[32];
    unsigned char dagInput[32];
    Impl() : open(false), generation(0), tipCount(0), remaining(0) {}
};

TipFrontierReader::TipFrontierReader() : impl_(new Impl()) {}
TipFrontierReader::~TipFrontierReader() { Close(); delete impl_; }

bool TipFrontierReader::Open(const std::string& path, uint64_t expectGeneration,
                             const unsigned char expectDagInputDigest[32], std::string* error)
{
    Close();
    uint64_t gen = 0, tips = 0;
    unsigned char dag[32], fdig[32];
    if (!ReadDagTipFrontierHeader(path, &gen, dag, fdig, &tips, error))
        return false;
    if (gen != expectGeneration)
    {
        if (error) *error = "frontier generation mismatch";
        return false;
    }
    if (expectDagInputDigest && memcmp(dag, expectDagInputDigest, 32) != 0)
    {
        if (error) *error = "frontier dagInputDigest mismatch";
        return false;
    }
    impl_->in.open(path.c_str(), std::ios::binary);
    if (!impl_->in.good())
    {
        if (error) *error = "cannot open frontier artifact for stream";
        return false;
    }
    // Skip past the fixed-sized header (93 bytes).
    const std::streamoff hdr = 12 + 1 + 8 + 32 + 32 + 8;
    impl_->in.seekg(hdr);
    impl_->open = true;
    impl_->generation = gen;
    impl_->tipCount = tips;
    impl_->remaining = tips;
    memcpy(impl_->frontierDigest, fdig, 32);
    memcpy(impl_->dagInput, dag, 32);
    return true;
}

void TipFrontierReader::Close()
{
    if (impl_->in.is_open()) impl_->in.close();
    impl_->open = false;
    impl_->generation = 0;
    impl_->tipCount = 0;
    impl_->remaining = 0;
}

bool TipFrontierReader::IsOpen() const { return impl_->open; }
uint64_t TipFrontierReader::Generation() const { return impl_->generation; }
uint64_t TipFrontierReader::TipCount() const { return impl_->tipCount; }
bool TipFrontierReader::GetFrontierDigest(unsigned char out[32]) const
{
    if (!impl_->open) return false;
    memcpy(out, impl_->frontierDigest, 32);
    return true;
}

bool TipFrontierReader::GetBoundDagInputDigest(unsigned char out[32]) const
{
    if (!impl_->open) return false;
    memcpy(out, impl_->dagInput, 32);
    return true;
}

bool TipFrontierReader::Next(uint256* out)
{
    if (!impl_->open || impl_->remaining == 0) return false;
    unsigned char hash[32];
    impl_->in.read((char*)hash, 32);
    if (impl_->in.gcount() != 32)
    {
        Close();
        return false;
    }
    memcpy(out->begin(), hash, 32);
    --impl_->remaining;
    return true;
}

// Bounded contains(): single sorted artifact, linear scan with a fixed-size
// read window. O(N) over the artifact (no random index); documented as such.
bool TipFrontierReader::Contains(const uint256& hash)
{
    if (!impl_->open) return false;
    const std::streampos start = impl_->in.tellg();
    impl_->in.seekg(12 + 1 + 8 + 32 + 32 + 8); // header size
    bool found = false;
    for (uint64_t i = 0; i < impl_->tipCount; ++i)
    {
        unsigned char h[32];
        impl_->in.read((char*)h, 32);
        if (impl_->in.gcount() != 32) break;
        uint256 cur;
        memcpy(cur.begin(), h, 32);
        if (memcmp(cur.begin(), hash.begin(), 32) == 0) { found = true; break; }
    }
    impl_->in.seekg(start);
    return found;
}

void TipFrontierReader::ResetStream()
{
    if (!impl_->open) return;
    impl_->in.seekg(12 + 1 + 8 + 32 + 32 + 8);
    impl_->remaining = impl_->tipCount;
}

} // namespace dag_tip_frontier