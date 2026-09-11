#include "dag_source_binding_verifier.h"

#include "dag.h"
#include "serialize.h"

#include <boost/filesystem.hpp>
#include <leveldb/db.h>
#include <leveldb/iterator.h>
#include <openssl/sha.h>

#include <algorithm>
#include <fstream>
#include <queue>
#include <vector>
#include <cstring>

namespace {

struct RunRecord
{
    uint256 hash;
    std::vector<uint256> parents;
};

static size_t RecordBytes(const RunRecord& r)
{
    return 32 + sizeof(uint32_t) + r.parents.size() * 32;
}

static bool RecordLess(const RunRecord& a, const RunRecord& b)
{
    return a.hash < b.hash; // exact uint256::operator< used by std::map
}

static bool WriteRecord(std::ofstream& out, const RunRecord& r)
{
    uint32_t count = (uint32_t)r.parents.size();
    out.write((const char*)r.hash.begin(), 32);
    out.write((const char*)&count, sizeof(count));
    for (size_t i = 0; i < r.parents.size(); ++i)
        out.write((const char*)r.parents[i].begin(), 32);
    return out.good();
}

static bool ReadRecord(std::ifstream& in, RunRecord* out)
{
    char hash[32];
    in.read(hash, sizeof(hash));
    if (in.eof() && in.gcount() == 0)
        return false;
    if (in.gcount() != (std::streamsize)sizeof(hash))
        throw std::runtime_error("truncated run hash");
    memcpy(out->hash.begin(), hash, 32);
    uint32_t count = 0;
    in.read((char*)&count, sizeof(count));
    if (!in.good()) throw std::runtime_error("truncated run parent count");
    if (count > 1000000U) throw std::runtime_error("unreasonable DAG parent count");
    out->parents.clear();
    out->parents.resize(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        in.read((char*)out->parents[i].begin(), 32);
        if (!in.good()) throw std::runtime_error("truncated run parent hash");
    }
    return true;
}

static void FeedDigest(SHA256_CTX* ctx, const RunRecord& r)
{
    SHA256_Update(ctx, r.hash.begin(), 32);
    uint32_t count = (uint32_t)r.parents.size();
    SHA256_Update(ctx, &count, 4); // exact existing builder semantics
    for (size_t i = 0; i < r.parents.size(); ++i)
        SHA256_Update(ctx, r.parents[i].begin(), 32);
}

static void Fail(DagSourceBindingResult* r, DagSourceBindingStatus status, const std::string& error)
{
    r->status = status;
    r->error = error;
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

static bool DecodeDAGValue(const leveldb::Slice& slice, RunRecord* record)
{
    try
    {
        CDataStream value(SER_DISK, CLIENT_VERSION);
        value.write(slice.data(), slice.size());
        CBlockDAGData data;
        value >> data; // same full CBlockDAGData decode as the builder
        record->parents = data.vDAGParents;
        return true;
    }
    catch (const std::exception&) { return false; }
}

static bool WriteRun(const boost::filesystem::path& path,
                     std::vector<RunRecord>* records,
                     DagSourceBindingResult* result)
{
    std::sort(records->begin(), records->end(), RecordLess);
    std::ofstream out(path.string().c_str(), std::ios::binary | std::ios::trunc);
    if (!out.good()) return false;
    for (size_t i = 0; i < records->size(); ++i)
    {
        if (i && (*records)[i - 1].hash == (*records)[i].hash)
            return false;
        if (!WriteRecord(out, (*records)[i])) return false;
    }
    out.close();
    if (!out.good()) return false;
    result->temporaryBytesWritten += (uint64_t)boost::filesystem::file_size(path);
    return true;
}

static bool MergeRuns(const std::vector<boost::filesystem::path>& inputs,
                      const boost::filesystem::path* output,
                      SHA256_CTX* digest,
                      DagSourceBindingResult* result)
{
    struct Cursor { std::ifstream in; RunRecord record; bool valid; Cursor() : valid(false) {} };
    std::vector<Cursor> cursors(inputs.size());
    result->maxOpenRunsObserved = std::max<uint64_t>(result->maxOpenRunsObserved, inputs.size());
    for (size_t i = 0; i < inputs.size(); ++i)
    {
        cursors[i].in.open(inputs[i].string().c_str(), std::ios::binary);
        if (!cursors[i].in.good()) return false;
        cursors[i].valid = ReadRecord(cursors[i].in, &cursors[i].record);
    }
    std::ofstream out;
    if (output)
    {
        out.open(output->string().c_str(), std::ios::binary | std::ios::trunc);
        if (!out.good()) return false;
    }
    bool havePrevious = false;
    uint256 previous;
    while (true)
    {
        size_t best = inputs.size();
        for (size_t i = 0; i < cursors.size(); ++i)
            if (cursors[i].valid && (best == inputs.size() || cursors[i].record.hash < cursors[best].record.hash))
                best = i;
        if (best == inputs.size()) break;
        if (havePrevious && previous == cursors[best].record.hash)
            return false; // duplicate logical key is corrupt
        previous = cursors[best].record.hash;
        havePrevious = true;
        if (digest) FeedDigest(digest, cursors[best].record);
        if (output && !WriteRecord(out, cursors[best].record)) return false;
        cursors[best].valid = ReadRecord(cursors[best].in, &cursors[best].record);
    }
    if (output)
    {
        out.close();
        if (!out.good()) return false;
        result->temporaryBytesWritten += (uint64_t)boost::filesystem::file_size(*output);
    }
    return true;
}

} // namespace

DagSourceBindingResult DagSourceBindingVerifier::Verify(const std::string& dagLinksDir,
                                                        const unsigned char expectedDigest[32],
                                                        const DagSourceBindingVerifierOptions& options)
{
    DagSourceBindingResult result;
    boost::filesystem::path tempRoot;
    try
    {
        if (!expectedDigest)
        {
            Fail(&result, DAG_SOURCE_BINDING_AUTHORITY_FAILURE, "null expected DAG digest");
            return result;
        }
        if (options.chunkBytes == 0 || options.maxRecordsPerChunk == 0 || options.maxOpenRuns < 2)
        {
            Fail(&result, DAG_SOURCE_BINDING_AUTHORITY_FAILURE, "invalid external-sort bounds");
            return result;
        }
        if (options.tempParent.empty())
        {
            Fail(&result, DAG_SOURCE_BINDING_TEMP_IO_FAILURE, "temporary parent is not specified");
            return result;
        }
        leveldb::Options dbOptions;
        dbOptions.create_if_missing = false;
        dbOptions.error_if_exists = false;
        dbOptions.paranoid_checks = true;
        leveldb::DB* db = NULL;
        leveldb::Status openStatus = leveldb::DB::Open(dbOptions, dagLinksDir, &db);
        if (!openStatus.ok())
        {
            Fail(&result, DAG_SOURCE_BINDING_SOURCE_UNAVAILABLE, openStatus.ToString());
            return result;
        }

        boost::filesystem::path parent(options.tempParent);
        boost::filesystem::create_directories(parent);
        tempRoot = parent / boost::filesystem::unique_path("dag-bind-%%%%-%%%%-%%%%");
        boost::filesystem::create_directories(tempRoot);

        std::vector<boost::filesystem::path> runs;
        std::vector<RunRecord> chunk;
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
            RunRecord record;
            if (!DecodeDAGKey(key, &record.hash))
            {
                delete it; delete db;
                Fail(&result, DAG_SOURCE_BINDING_DECODE_FAILURE, "invalid daglinks key");
                throw std::runtime_error("decode failure");
            }
            if (!DecodeDAGValue(it->value(), &record))
            {
                delete it; delete db;
                Fail(&result, DAG_SOURCE_BINDING_DECODE_FAILURE, "invalid daglinks value");
                throw std::runtime_error("decode failure");
            }
            result.recordsProcessed++;
            result.bytesProcessed += key.size() + it->value().size();
            size_t bytes = RecordBytes(record);
            chunk.push_back(record);
            chunkBytes += bytes;
            result.peakChunkBytes = std::max<uint64_t>(result.peakChunkBytes, chunkBytes);
            result.peakChunkRecords = std::max<uint64_t>(result.peakChunkRecords, chunk.size());
            if (chunkBytes >= options.chunkBytes || chunk.size() >= options.maxRecordsPerChunk)
            {
                boost::filesystem::path run = tempRoot / (std::string("run-") + std::to_string(runNumber++) + ".bin");
                if (!WriteRun(run, &chunk, &result))
                {
                    delete it; delete db;
                    Fail(&result, DAG_SOURCE_BINDING_TEMP_IO_FAILURE, "cannot write sorted run");
                    throw std::runtime_error("temporary I/O failure");
                }
                runs.push_back(run); chunk.clear(); chunkBytes = 0;
            }
            it->Next();
        }
        leveldb::Status iteratorStatus = it->status();
        delete it; delete db;
        if (!iteratorStatus.ok())
        {
            Fail(&result, DAG_SOURCE_BINDING_SOURCE_CORRUPT, iteratorStatus.ToString());
            throw std::runtime_error("source iterator failure");
        }
        if (!chunk.empty())
        {
            boost::filesystem::path run = tempRoot / (std::string("run-") + std::to_string(runNumber++) + ".bin");
            if (!WriteRun(run, &chunk, &result))
            {
                Fail(&result, DAG_SOURCE_BINDING_TEMP_IO_FAILURE, "cannot write final sorted run");
                throw std::runtime_error("temporary I/O failure");
            }
            runs.push_back(run);
        }
        result.runCount = runs.size();

        while (runs.size() > options.maxOpenRuns)
        {
            std::vector<boost::filesystem::path> next;
            for (size_t begin = 0; begin < runs.size(); begin += options.maxOpenRuns)
            {
                size_t end = std::min(runs.size(), begin + options.maxOpenRuns);
                std::vector<boost::filesystem::path> group(runs.begin() + begin, runs.begin() + end);
                boost::filesystem::path merged = tempRoot / (std::string("merge-") + std::to_string(result.mergePasses) + "-" + std::to_string(begin) + ".bin");
                if (!MergeRuns(group, &merged, NULL, &result))
                {
                    Fail(&result, DAG_SOURCE_BINDING_TEMP_IO_FAILURE, "cannot merge sorted runs");
                    throw std::runtime_error("merge failure");
                }
                next.push_back(merged);
                for (size_t i = 0; i < group.size(); ++i) boost::filesystem::remove(group[i]);
            }
            runs.swap(next);
            result.mergePasses++;
        }

        SHA256_CTX digest;
        SHA256_Init(&digest);
        if (!runs.empty() && !MergeRuns(runs, NULL, &digest, &result))
        {
            Fail(&result, DAG_SOURCE_BINDING_SOURCE_CORRUPT, "duplicate or malformed sorted DAG record");
            throw std::runtime_error("final merge failure");
        }
        unsigned char actual[32];
        SHA256_Final(actual, &digest);
        if (memcmp(actual, expectedDigest, 32) != 0)
        {
            Fail(&result, DAG_SOURCE_BINDING_DIGEST_MISMATCH, "dagInputDigest mismatch");
            throw std::runtime_error("digest mismatch");
        }
        result.status = DAG_SOURCE_BINDING_VERIFIED;
        result.error.clear();
    }
    catch (const std::exception& ex)
    {
        if (result.status == DAG_SOURCE_BINDING_AUTHORITY_FAILURE)
            result.error = ex.what();
    }
    catch (...)
    {
        if (result.status == DAG_SOURCE_BINDING_AUTHORITY_FAILURE)
            result.error = "unknown verifier failure";
    }
    if (!tempRoot.empty())
    {
        try { boost::filesystem::remove_all(tempRoot); }
        catch (...) {}
    }
    return result;
}
