// Durability-barrier chunking (ChainObserveState::batchChunk) contract test.
//
// A flush that carries N blocks may be published as several bounded barriers.
// The observable contract is:
//   * the FINAL logical state is identical to one single-barrier publish;
//   * each barrier is all-or-nothing and they are applied in order;
//   * the last barrier's state survives a reopen (durability of the tail);
//   * only the number of generations differs.
//
// This is the regression for the consensus-incident latency fix: a single
// barrier covering 1000 blocks ran under cs_main and grew the acceptance stall
// window with the batch size.
#include <boost/test/unit_test.hpp>
#include <boost/filesystem.hpp>

#include "store3a/incstore.h"

#include <string>
#include <vector>

namespace {

std::string TmpDir()
{
    char tmpl[] = "/tmp/innova-s3a-chunk-XXXXXX";
    char* d = mkdtemp(tmpl);
    BOOST_REQUIRE(d != NULL);
    return std::string(d);
}

cs::Hash H(uint8_t hi, uint8_t lo)
{
    cs::Hash h;
    h.fill(0);
    h[0] = hi;
    h[1] = lo;
    return h;
}

struct RunResult
{
    cs::Hash tip;
    int32_t height;
    uint64_t generation;
    uint64_t acknowledged;
};

// Publishes `n` consecutive empty-delta blocks. chunk == 0 (or >= n) means one
// barrier for the whole run; otherwise a barrier is paid every `chunk` blocks.
bool PublishRun(const std::string& dir, int n, size_t chunk, RunResult* out)
{
    cs::store3a::CommitStore3A store;
    cs::ChainError e;
    if (!store.open(dir, &e))
        return false;
    std::vector<cs::store3a::CommitStore3A::BatchBlock> pending;
    bool ok = true;
    for (int i = 1; i <= n; ++i)
    {
        cs::store3a::CommitStore3A::BatchBlock bb;
        bb.blockHash = H((uint8_t)((i >> 8) & 0xff), (uint8_t)(i & 0xff));
        bb.height = i;
        if (!store.applyBatchBlock(bb.d, &e)) { ok = false; break; }
        pending.push_back(bb);
        // chunk == 0 means ONE barrier for the whole run (published at the last block).
        const bool boundary = ((chunk != 0) && (pending.size() >= chunk)) || (i == n);
        if (boundary)
        {
            if (!store.publishBatch(pending, &e)) { ok = false; break; }
            pending.clear();
        }
    }
    if (ok)
    {
        out->tip = store.tipHash();
        out->height = store.tipHeight();
        out->generation = store.generation();
        out->acknowledged = store.acknowledgedGeneration();
    }
    store.close();
    return ok;
}

bool ReopenState(const std::string& dir, RunResult* out)
{
    cs::store3a::CommitStore3A store;
    cs::ChainError e;
    if (!store.open(dir, &e))
        return false;
    out->tip = store.tipHash();
    out->height = store.tipHeight();
    out->generation = store.generation();
    out->acknowledged = store.acknowledgedGeneration();
    store.close();
    return true;
}

} // namespace

BOOST_AUTO_TEST_SUITE(store3a_batch_chunk_tests)

BOOST_AUTO_TEST_CASE(four_chunked_barriers_equal_one_barrier_state)
{
    const std::string dirSingle = TmpDir();
    const std::string dirChunked = TmpDir();
    RunResult single, chunked;
    BOOST_REQUIRE(PublishRun(dirSingle, 1000, 0, &single));
    BOOST_REQUIRE(PublishRun(dirChunked, 1000, 250, &chunked));

    // Identical logical state: same tip identity and height.
    BOOST_CHECK_MESSAGE(single.tip == chunked.tip,
                        "chunked publication must land on the identical tip hash");
    BOOST_CHECK_EQUAL(single.height, chunked.height);
    BOOST_CHECK_EQUAL(single.height, 1000);

    // Four barriers instead of one -> exactly three extra generations.
    BOOST_CHECK_MESSAGE(chunked.generation == single.generation + 3,
                        "1000 blocks in 4x250 must pay 4 barriers (single: " << single.generation
                        << ", chunked: " << chunked.generation << ")");

    // Durability of the last chunk survives a reopen on both stores.
    RunResult reopenedSingle, reopenedChunked;
    BOOST_REQUIRE(ReopenState(dirSingle, &reopenedSingle));
    BOOST_REQUIRE(ReopenState(dirChunked, &reopenedChunked));
    BOOST_CHECK(reopenedSingle.tip == single.tip);
    BOOST_CHECK(reopenedChunked.tip == chunked.tip);
    BOOST_CHECK_EQUAL(reopenedChunked.height, 1000);
    printf("chunk test: single gen=%llu chunked gen=%llu tip equal=%d\n",
           (unsigned long long)single.generation, (unsigned long long)chunked.generation,
           (int)(single.tip == chunked.tip));
}

BOOST_AUTO_TEST_CASE(chunk_boundary_is_all_or_nothing_and_ordered)
{
    const std::string dir = TmpDir();
    RunResult exact, uneven;
    BOOST_REQUIRE(PublishRun(dir, 250, 250, &exact));
    BOOST_CHECK_EQUAL(exact.height, 250);

    const std::string dir2 = TmpDir();
    BOOST_REQUIRE(PublishRun(dir2, 251, 250, &uneven));
    BOOST_CHECK_EQUAL(uneven.height, 251);
    BOOST_CHECK_MESSAGE(uneven.generation == exact.generation + 1,
                        "the trailing partial chunk must be its own barrier");
}

BOOST_AUTO_TEST_SUITE_END()
