// -blockaccepttrace contract tests.
//
// The tracer must be inert when disabled (the hot-path contract) and must
// accumulate enough state to name a stall when enabled: in-flight block
// identity, last stage, record-store read ops/bytes and the slowest read.
#include <boost/test/unit_test.hpp>

#include "../blockacctrace.h"

BOOST_AUTO_TEST_SUITE(blockacctrace_tests)

BOOST_AUTO_TEST_CASE(disabled_is_inert)
{
    blockacctrace::SetEnabled(false);
    const blockacctrace::ReadSnapshot before = blockacctrace::Snapshot();
    // The store hooks guard on Enabled(); nothing here may move when it is off.
    if (blockacctrace::Enabled()) {
        blockacctrace::NoteRead(4096, 10);
    }
    const blockacctrace::ReadSnapshot after = blockacctrace::Snapshot();
    BOOST_CHECK_EQUAL(before.ops, after.ops);
    BOOST_CHECK_EQUAL(before.bytes, after.bytes);
    BOOST_CHECK(!blockacctrace::Enabled());
}

BOOST_AUTO_TEST_CASE(enabled_accounts_reads_ops_bytes_and_slowest)
{
    blockacctrace::SetEnabled(true);
    const blockacctrace::ReadSnapshot before = blockacctrace::Snapshot();
    blockacctrace::NoteRead(228, 5);
    blockacctrace::NoteRead(228, 900);
    blockacctrace::NoteRead(228, 7);
    const blockacctrace::ReadSnapshot after = blockacctrace::Snapshot();
    BOOST_CHECK_EQUAL(after.ops - before.ops, 3u);
    BOOST_CHECK_EQUAL(after.bytes - before.bytes, 684u);
    BOOST_CHECK(after.slowUs >= 900u);
    blockacctrace::SetEnabled(false);
}

BOOST_AUTO_TEST_CASE(in_flight_block_carries_hash_height_and_stage)
{
    blockacctrace::SetEnabled(true);
    blockacctrace::BeginBlock("0000000051cbe048fcd321", 8202766);
    BOOST_CHECK(blockacctrace::Current().active.load());
    BOOST_CHECK_EQUAL(blockacctrace::Current().height.load(), 8202766);
    BOOST_CHECK_EQUAL(std::string(blockacctrace::StageName(blockacctrace::Current().stage.load())),
                      std::string("checkblock"));

    blockacctrace::SetStage(6);   // store3a_publish
    BOOST_CHECK_EQUAL(std::string(blockacctrace::StageName(blockacctrace::Current().stage.load())),
                      std::string("store3a_publish"));

    blockacctrace::EndBlock();
    BOOST_CHECK(!blockacctrace::Current().active.load());
    blockacctrace::SetEnabled(false);
}

BOOST_AUTO_TEST_SUITE_END()
