// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Unit tests for the passive per-block GETDATA->CONNECT latency recorder
// (src/ibdblocklatency.cpp).  These verify the lifecycle accounting
// (T0..T7 stamping, interval deltas, total == sum of sub-intervals), the
// funnel counters, the unsolicited-receive path, the disabled no-op gate, and
// the CSV dump.  They do not exercise the scheduler; the scheduler hooks are
// pure observation and are intentionally not unit-tested here.

#include <boost/filesystem.hpp>
#include <boost/test/unit_test.hpp>

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "../ibdblocklatency.h"
#include "../uint256.h"
#include "../util.h"

namespace {

uint256 TestHash(unsigned int i)
{
    return uint256((uint64_t)i);
}

boost::filesystem::path TmpCsvPath(const std::string& tag)
{
    static unsigned int counter = 0;
    return boost::filesystem::temp_directory_path() /
           ("ibdblocklatency_" + tag + "_" + std::to_string(counter++) + ".csv");
}

std::string ReadFile(const boost::filesystem::path& p)
{
    std::ifstream in(p.string().c_str());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

void RunFullLifecycle(const uint256& hash, int requestPeer, int receivePeer,
                      int64_t nPingUsec)
{
    ibdblocklatency::RecordAskForEnqueue(hash, requestPeer, 7);
    ibdblocklatency::RecordGetDataSent(hash, requestPeer);
    ibdblocklatency::RecordBlockReceived(hash, receivePeer, 4096,
                                         /*nTimeReceivedUs=*/GetTimeMicros(),
                                         nPingUsec);
    ibdblocklatency::RecordProcessBlockBegin(hash);
    ibdblocklatency::RecordAcceptBlockBegin(hash);
    ibdblocklatency::RecordAddToBlockIndexBegin(hash);
    ibdblocklatency::RecordSetBestChainBegin(hash);
    ibdblocklatency::RecordBlockConnected(hash, 100);
}

} // namespace

BOOST_AUTO_TEST_SUITE(ibdblocklatency_tests)

// Disabled: every hook is a no-op and no sample is ever produced.
BOOST_AUTO_TEST_CASE(disabled_is_noop)
{
    ibdblocklatency::ResetForTesting();
    ibdblocklatency::SetEnabled(false, "");

    const uint256 hash = TestHash(1);
    RunFullLifecycle(hash, 2, 3, 5000);

    BOOST_CHECK(!ibdblocklatency::Enabled());
    BOOST_CHECK_EQUAL(ibdblocklatency::SampleCountForTesting(), (size_t)0);
    BOOST_CHECK_EQUAL(ibdblocklatency::ReceivedForTesting(), 0);
    BOOST_CHECK_EQUAL(ibdblocklatency::ConnectedForTesting(), 0);

    ibdblocklatency::SetEnabled(false, "");
}

// A complete request lifecycle yields exactly one completed sample whose
// intervals are well formed: each later stage is not before the earlier one
// and TOTAL equals the sum of the sub-intervals.
BOOST_AUTO_TEST_CASE(full_lifecycle_produces_consistent_sample)
{
    ibdblocklatency::ResetForTesting();
    ibdblocklatency::SetEnabled(true, "");

    const uint256 hash = TestHash(2);
    RunFullLifecycle(hash, 11, 12, 3000);

    BOOST_CHECK(ibdblocklatency::Enabled());
    BOOST_CHECK_EQUAL(ibdblocklatency::SampleCountForTesting(), (size_t)1);
    BOOST_CHECK_EQUAL(ibdblocklatency::ReceivedForTesting(), 1);
    BOOST_CHECK_EQUAL(ibdblocklatency::UnsolicitedForTesting(), 0);
    BOOST_CHECK_EQUAL(ibdblocklatency::ProcessedForTesting(), 1);
    BOOST_CHECK_EQUAL(ibdblocklatency::ConnectedForTesting(), 1);

    const std::vector<ibdblocklatency::BlockLatencySample>& samples =
        ibdblocklatency::SamplesForTesting();
    BOOST_REQUIRE(samples.size() == 1);
    const ibdblocklatency::BlockLatencySample& s = samples[0];
    BOOST_CHECK(s.hash == hash);
    BOOST_CHECK_EQUAL(s.requestPeer, 11);
    BOOST_CHECK_EQUAL(s.receivePeer, 12);
    BOOST_CHECK_EQUAL(s.blockSize, (int64_t)4096);
    BOOST_CHECK_EQUAL(s.height, (int64_t)100);
    BOOST_CHECK_EQUAL(s.pingMs, (int64_t)3);
    BOOST_CHECK_EQUAL(s.requestPeerPressure, (int64_t)7);
    BOOST_CHECK_EQUAL(s.outcome, ibdblocklatency::OUTCOME_CONNECTED_ACTIVE);
    BOOST_CHECK_EQUAL(s.fOrphaned, 0);
    BOOST_CHECK_EQUAL(ibdblocklatency::OutcomeCountForTesting(ibdblocklatency::OUTCOME_CONNECTED_ACTIVE), 1);

    // All sub-intervals plus total must be present and consistent.
    for (int i = 0; i < ibdblocklatency::BLOCKLAT_NUM_INTERVALS; ++i)
        BOOST_CHECK(s.intervalUs[i] >= 0);

    int64_t nSum = 0;
    for (int i = 0; i < ibdblocklatency::BLOCKLAT_INTERVAL_TOTAL; ++i)
        nSum += s.intervalUs[i];
    BOOST_CHECK_EQUAL(nSum, s.intervalUs[ibdblocklatency::BLOCKLAT_INTERVAL_TOTAL]);

    ibdblocklatency::SetEnabled(false, "");
}

// An unsolicited block (received without an AskFor record) is still sampled
// but is marked with requestPeer == -1 and no askfor/getdata intervals.
BOOST_AUTO_TEST_CASE(unsolicited_receive_is_marked)
{
    ibdblocklatency::ResetForTesting();
    ibdblocklatency::SetEnabled(true, "");

    const uint256 hash = TestHash(3);
    ibdblocklatency::RecordBlockReceived(hash, 21, 512,
                                         /*nTimeReceivedUs=*/GetTimeMicros(), 0);
    ibdblocklatency::RecordProcessBlockBegin(hash);
    ibdblocklatency::RecordAcceptBlockBegin(hash);
    ibdblocklatency::RecordAddToBlockIndexBegin(hash);
    ibdblocklatency::RecordSetBestChainBegin(hash);
    ibdblocklatency::RecordBlockConnected(hash, 101);

    BOOST_CHECK_EQUAL(ibdblocklatency::UnsolicitedForTesting(), 1);
    BOOST_CHECK_EQUAL(ibdblocklatency::SampleCountForTesting(), (size_t)1);
    const std::vector<ibdblocklatency::BlockLatencySample>& samples =
        ibdblocklatency::SamplesForTesting();
    BOOST_REQUIRE(samples.size() == 1);
    BOOST_CHECK_EQUAL(samples[0].requestPeer, -1);
    BOOST_CHECK_EQUAL(samples[0].pingMs, (int64_t)-1);
    BOOST_CHECK_EQUAL(samples[0].intervalUs[ibdblocklatency::BLOCKLAT_INTERVAL_ASKFOR_TO_GETDATA], (int64_t)-1);
    BOOST_CHECK_EQUAL(samples[0].intervalUs[ibdblocklatency::BLOCKLAT_INTERVAL_GETDATA_TO_RECEIVE], (int64_t)-1);
    // recv_to_process is still present.
    BOOST_CHECK(samples[0].intervalUs[ibdblocklatency::BLOCKLAT_INTERVAL_RECEIVE_TO_PROCESS] >= 0);

    ibdblocklatency::SetEnabled(false, "");
}

// A record that never connects is dropped by the stale purge, leaving no
// sample and no leak in the in-flight record map.
BOOST_AUTO_TEST_CASE(incomplete_record_never_emits)
{
    ibdblocklatency::ResetForTesting();
    ibdblocklatency::SetEnabled(true, "");

    const uint256 hash = TestHash(4);
    ibdblocklatency::RecordAskForEnqueue(hash, 5, 3);
    ibdblocklatency::RecordGetDataSent(hash, 5);
    ibdblocklatency::RecordBlockReceived(hash, 6, 1024, /*nTimeReceivedUs=*/0, 0);

    // No connect -> no sample, no outcome, no leak in the in-flight map.
    BOOST_CHECK_EQUAL(ibdblocklatency::SampleCountForTesting(), (size_t)0);
    BOOST_CHECK_EQUAL(ibdblocklatency::ConnectedForTesting(), 0);
    BOOST_CHECK_EQUAL(ibdblocklatency::OutcomeCountForTesting(ibdblocklatency::OUTCOME_INCOMPLETE_EVICTED), 0);

    ibdblocklatency::SetEnabled(false, "");
}

// A rejected request (T0..T3 reached, no connect) streams a fate row with
// outcome=rejected and a TOTAL interval built from the terminal time.
BOOST_AUTO_TEST_CASE(rejected_terminal_streams_fate_row)
{
    ibdblocklatency::ResetForTesting();
    const boost::filesystem::path p = TmpCsvPath("reject");
    ibdblocklatency::SetEnabled(true, p.string());

    const uint256 hash = TestHash(6);
    ibdblocklatency::RecordAskForEnqueue(hash, 9, 2);
    ibdblocklatency::RecordGetDataSent(hash, 9);
    ibdblocklatency::RecordBlockReceived(hash, 9, 2048, GetTimeMicros(), 3000);
    ibdblocklatency::RecordProcessBlockBegin(hash);
    ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_REJECTED);

    BOOST_CHECK_EQUAL(ibdblocklatency::SampleCountForTesting(), (size_t)0);
    BOOST_CHECK_EQUAL(ibdblocklatency::OutcomeCountForTesting(ibdblocklatency::OUTCOME_REJECTED), 1);

    // Row is streamed immediately: after flushing the stdio buffer the file
    // contains the rejected fate row (requestPeer=9, receivePeer=9, height=-1,
    // size 2048, 3000us -> 3ms ping).
    ibdblocklatency::FlushCsvForTesting();
    const std::string csv = ReadFile(p);
    BOOST_CHECK(csv.find("9,9,-1,2048,3,") != std::string::npos);
    BOOST_CHECK(csv.find(",rejected,0,") != std::string::npos);
    BOOST_CHECK(csv.find("connected_active,") == std::string::npos);

    ibdblocklatency::SetEnabled(false, "");
    boost::filesystem::remove(p);
}

// An orphaned block is not terminal: the record survives, is flagged, and a
// later connect is reported as connected_active with fOrphaned == 1.
BOOST_AUTO_TEST_CASE(orphaned_then_connected_is_flagged)
{
    ibdblocklatency::ResetForTesting();
    ibdblocklatency::SetEnabled(true, "");

    const uint256 hash = TestHash(7);
    ibdblocklatency::RecordAskForEnqueue(hash, 13, 2);
    ibdblocklatency::RecordGetDataSent(hash, 13);
    ibdblocklatency::RecordBlockReceived(hash, 13, 1024, GetTimeMicros(), 2000);
    ibdblocklatency::RecordProcessBlockBegin(hash);
    ibdblocklatency::RecordAcceptBlockBegin(hash);
    ibdblocklatency::RecordBlockOrphaned(hash);

    // No terminal yet; the orphan stage is reflected as a counter.
    BOOST_CHECK_EQUAL(ibdblocklatency::OrphanedForTesting(), 1);

    // Parent arrives; the orphan is accepted and becomes the active tip.
    ibdblocklatency::RecordAcceptBlockBegin(hash);
    ibdblocklatency::RecordAddToBlockIndexBegin(hash);
    ibdblocklatency::RecordSetBestChainBegin(hash);
    ibdblocklatency::RecordBlockConnected(hash, 200);

    BOOST_CHECK_EQUAL(ibdblocklatency::SampleCountForTesting(), (size_t)1);
    const std::vector<ibdblocklatency::BlockLatencySample>& samples =
        ibdblocklatency::SamplesForTesting();
    BOOST_REQUIRE(samples.size() == 1);
    BOOST_CHECK_EQUAL(samples[0].outcome, ibdblocklatency::OUTCOME_CONNECTED_ACTIVE);
    BOOST_CHECK_EQUAL(samples[0].fOrphaned, 1);
    // process_to_accept spans the orphan wait (original T3 -> eventual T4).
    BOOST_CHECK(samples[0].intervalUs[ibdblocklatency::BLOCKLAT_INTERVAL_PROCESS_TO_ACCEPT] >= 0);

    ibdblocklatency::SetEnabled(false, "");
}

// An accepted-but-side-chain block streams an accepted_side row with its index
// height, never reaching the connected ring.
BOOST_AUTO_TEST_CASE(accepted_side_terminal)
{
    ibdblocklatency::ResetForTesting();
    const boost::filesystem::path p = TmpCsvPath("side");
    ibdblocklatency::SetEnabled(true, p.string());

    const uint256 hash = TestHash(8);
    ibdblocklatency::RecordAskForEnqueue(hash, 17, 1);
    ibdblocklatency::RecordGetDataSent(hash, 17);
    ibdblocklatency::RecordBlockReceived(hash, 17, 256, GetTimeMicros(), 1000);
    ibdblocklatency::RecordProcessBlockBegin(hash);
    ibdblocklatency::RecordAcceptBlockBegin(hash);
    ibdblocklatency::RecordAddToBlockIndexBegin(hash);
    ibdblocklatency::RecordBlockAcceptedSide(hash, 333);

    BOOST_CHECK_EQUAL(ibdblocklatency::SampleCountForTesting(), (size_t)0);
    BOOST_CHECK_EQUAL(ibdblocklatency::OutcomeCountForTesting(ibdblocklatency::OUTCOME_ACCEPTED_SIDE), 1);

    ibdblocklatency::FlushCsvForTesting();
    const std::string csv = ReadFile(p);
    BOOST_CHECK(csv.find("17,17,333,256,1,") != std::string::npos);
    BOOST_CHECK(csv.find(",accepted_side,0,") != std::string::npos);

    ibdblocklatency::SetEnabled(false, "");
    boost::filesystem::remove(p);
}

// A timeout terminal for a request that was never delivered (only T0/T1 set)
// streams a row with a TOTAL interval and -1 for everything after T1.
BOOST_AUTO_TEST_CASE(timeout_never_delivered)
{
    ibdblocklatency::ResetForTesting();
    const boost::filesystem::path p = TmpCsvPath("timeout");
    ibdblocklatency::SetEnabled(true, p.string());

    const uint256 hash = TestHash(9);
    ibdblocklatency::RecordAskForEnqueue(hash, 23, 4);
    ibdblocklatency::RecordGetDataSent(hash, 23);
    ibdblocklatency::RecordBlockTerminal(hash, ibdblocklatency::OUTCOME_TIMEOUT);

    BOOST_CHECK_EQUAL(ibdblocklatency::OutcomeCountForTesting(ibdblocklatency::OUTCOME_TIMEOUT), 1);

    ibdblocklatency::FlushCsvForTesting();
    const std::string csv = ReadFile(p);
    BOOST_CHECK(csv.find("23,-1,-1,0,-1,") != std::string::npos);
    BOOST_CHECK(csv.find(",timeout,0,") != std::string::npos);
    ibdblocklatency::SetEnabled(false, "");
    boost::filesystem::remove(p);
}

// The CSV streams rows as outcomes are recorded (header on open, connected
// row on terminal) and the summary always prints even with an empty CSV path.
BOOST_AUTO_TEST_CASE(csv_dump_contains_samples)
{
    ibdblocklatency::ResetForTesting();
    const boost::filesystem::path p = TmpCsvPath("dump");
    ibdblocklatency::SetEnabled(true, p.string());

    RunFullLifecycle(TestHash(5), 31, 32, 4000);

    ibdblocklatency::Dump();
    BOOST_CHECK(boost::filesystem::exists(p));
    const std::string csv = ReadFile(p);
    BOOST_CHECK(csv.find("# request_peer,receive_peer,height,size,ping_ms") != std::string::npos);
    BOOST_CHECK(csv.find("connected_active,0,") != std::string::npos);
    BOOST_CHECK(csv.find("31,32,100,4096,4,") != std::string::npos);

    ibdblocklatency::SetEnabled(false, "");
    boost::filesystem::remove(p);
}

BOOST_AUTO_TEST_SUITE_END()
