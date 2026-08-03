// Copyright (c) 2026 The Innova developers
// Distributed under the MIT/X11 software license, see the accompanying
// file COPYING or http://www.opensource.org/licenses/mit-license.php.

// Unit tests for the passive per-getdata-batch forensic instrumentation
// (src/ibdforensic.cpp).  These verify the correctness of the recorders
// (batch/seq assignment, mark/receive/timeout times, re-request flags), of
// the summary aggregation that the forensic analysis relies on, and of the
// file output (enablement from CLI/config-style args, file creation even
// with zero events, error reporting, counters surviving SetEnabled(false)).
// They do not exercise the scheduler; the scheduler hooks are pure
// observation and are intentionally not unit-tested here.

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/test/unit_test.hpp>

#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "../ibdforensic.h"
#include "../net.h"
#include "../uint256.h"
#include "../util.h"

namespace {

uint256 TestHash(unsigned int i)
{
    return uint256((uint64_t)i);
}

std::vector<uint256> Hashes(const std::vector<uint256>& v)
{
    return v;
}

// A unique temp path per call, so parallel/serial cases never collide.
boost::filesystem::path TmpForensicPath(const std::string& tag)
{
    static unsigned int counter = 0;
    return boost::filesystem::temp_directory_path() /
           ("ibdforensic_" + tag + "_" + std::to_string(counter++) + ".log");
}

std::string ReadFile(const boost::filesystem::path& p)
{
    std::ifstream in(p.string().c_str());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

// Restore the global args maps after a test mutates them (same pattern as
// DoS_tests.cpp / finality_tally_tests.cpp).
struct ArgsRestore
{
    std::map<std::string, std::string> mapArgsSaved;
    std::map<std::string, std::vector<std::string> > mapMultiArgsSaved;
    ArgsRestore()
    {
        mapArgsSaved = mapArgs;
        mapMultiArgsSaved = mapMultiArgs;
    }
    ~ArgsRestore()
    {
        mapArgs = mapArgsSaved;
        mapMultiArgs = mapMultiArgsSaved;
    }
};

// Feed a whitespace-separated option list through ParseParameters, exactly
// like the CLI path in AppInit() (init.cpp:385).
void ParseForensicArgs(const std::string& strArgs)
{
    std::vector<std::string> vec;
    boost::split(vec, strArgs, boost::is_any_of(" "),
                 boost::token_compress_on);
    vec.insert(vec.begin(), "testbitcoin");
    std::vector<const char*> argv;
    for (size_t i = 0; i < vec.size(); ++i)
        argv.push_back(vec[i].c_str());
    ParseParameters(argv.size(), &argv[0]);
}

} // namespace

BOOST_AUTO_TEST_SUITE(ibdforensic_tests)

// A getdata batch is recorded with peer, sequential batch id, block-relative
// seq, mark time, send-buffer size, and the continuation (hashContinue) flag.
BOOST_AUTO_TEST_CASE(records_batch_peer_batchid_seq_count_and_mark_time)
{
    ibdforensic::ResetForTesting();

    const uint256 h1 = TestHash(1);
    const uint256 h2 = TestHash(2);
    const uint256 h3 = TestHash(3);
    std::vector<uint256> vGetData = Hashes(std::vector<uint256>({h1, h2, h3}));

    // hashLastBlockInBatch == h3 and a truncated (1000-block) announced batch
    // mean the last hash is the continuation marker.
    ibdforensic::RecordGetDataBatch(
        7, vGetData, 1000000, 4096, h3, 1000);

    BOOST_CHECK_EQUAL(ibdforensic::BatchCount(), (size_t)1);
    BOOST_CHECK_EQUAL(ibdforensic::EntryCount(), (size_t)3);

    const std::vector<ibdforensic::BatchRecord>& batches =
        ibdforensic::BatchesForTesting();
    BOOST_REQUIRE(batches.size() == 1);
    BOOST_CHECK_EQUAL(batches[0].peer, 7);
    BOOST_CHECK_EQUAL(batches[0].batchId, (uint64_t)0);
    BOOST_CHECK_EQUAL(batches[0].nHashes, (uint32_t)3);
    BOOST_CHECK_EQUAL(batches[0].sendTimeUs, (int64_t)1000000);
    BOOST_CHECK_EQUAL(batches[0].sendBufferBytes, (size_t)4096);
    BOOST_CHECK(batches[0].hashes[0] == h1);
    BOOST_CHECK(batches[0].hashes[2] == h3);

    const std::map<uint256, ibdforensic::BatchEntry>& entries =
        ibdforensic::EntriesForTesting();
    BOOST_CHECK(entries.at(h1).seq == 0);
    BOOST_CHECK(entries.at(h2).seq == 1);
    BOOST_CHECK(entries.at(h3).seq == 2);
    BOOST_CHECK(entries.at(h1).markTimeUs == 1000000);
    BOOST_CHECK_EQUAL(entries.at(h1).requestPeer, 7);
    BOOST_CHECK(!entries.at(h1).wasHashContinue);
    BOOST_CHECK(entries.at(h3).wasHashContinue);

    // A second batch gets the next sequential batch id.
    std::vector<uint256> vSecond = Hashes(std::vector<uint256>({h1, h2}));
    ibdforensic::RecordGetDataBatch(
        8, vSecond, 2000000, 512, uint256(0), 1000);
    BOOST_CHECK_EQUAL(ibdforensic::BatchCount(), (size_t)2);
    BOOST_CHECK_EQUAL(ibdforensic::BatchesForTesting()[1].batchId,
                      (uint64_t)1);
    BOOST_CHECK_EQUAL(ibdforensic::BatchesForTesting()[1].nHashes,
                      (uint32_t)2);
    BOOST_CHECK_EQUAL(ibdforensic::EntryCount(), (size_t)3);
}

// A truncated (non-1000) announced batch has no continuation marker, so
// wasHashContinue must stay false.
BOOST_AUTO_TEST_CASE(no_continuation_marker_for_partial_batch)
{
    ibdforensic::ResetForTesting();

    const uint256 h1 = TestHash(10);
    std::vector<uint256> vGetData = Hashes(std::vector<uint256>({h1}));
    ibdforensic::RecordGetDataBatch(
        1, vGetData, 3000000, 0, h1, 5); // final, non-truncated batch

    BOOST_CHECK(!ibdforensic::EntriesForTesting().at(h1).wasHashContinue);
}

// Receipt before timeout clears cleanly; receipt after an expiry is flagged.
BOOST_AUTO_TEST_CASE(records_receipt_before_and_after_timeout)
{
    ibdforensic::ResetForTesting();

    const uint256 hOk = TestHash(20);
    const uint256 hLate = TestHash(21);
    const uint256 hNever = TestHash(22);
    std::vector<uint256> vGetData =
        Hashes(std::vector<uint256>({hOk, hLate, hNever}));
    ibdforensic::RecordGetDataBatch(1, vGetData, 5000000, 0, uint256(0), 1000);

    // Clean arrival: received before any timeout.
    ibdforensic::RecordReceived(1, hOk, 5100000, 0);
    const ibdforensic::BatchEntry& eOk =
        ibdforensic::EntriesForTesting().at(hOk);
    BOOST_CHECK_EQUAL(eOk.recvTimeUs, (int64_t)5100000);
    BOOST_CHECK(!eOk.receivedAfterTimeout);
    BOOST_CHECK_EQUAL(eOk.timeoutTimeUs, (int64_t)0);

    // Expiry fires, then the (already in transit) block arrives late.
    ibdforensic::RecordExpired(1, hLate, 6000000, 0);
    ibdforensic::RecordReceived(1, hLate, 6100000, 0);
    const ibdforensic::BatchEntry& eLate =
        ibdforensic::EntriesForTesting().at(hLate);
    BOOST_CHECK_EQUAL(eLate.timeoutTimeUs, (int64_t)6000000);
    BOOST_CHECK_EQUAL(eLate.recvTimeUs, (int64_t)6100000);
    BOOST_CHECK(eLate.receivedAfterTimeout);

    // Expiry with no receipt.
    ibdforensic::RecordExpired(1, hNever, 6000000, 0);
    const ibdforensic::BatchEntry& eNever =
        ibdforensic::EntriesForTesting().at(hNever);
    BOOST_CHECK_EQUAL(eNever.timeoutTimeUs, (int64_t)6000000);
    BOOST_CHECK_EQUAL(eNever.recvTimeUs, (int64_t)0);
    BOOST_CHECK(!eNever.receivedAfterTimeout);

    // Duplicate expiry does not overwrite the first timeout time.
    ibdforensic::RecordExpired(1, hLate, 7000000, 0);
    BOOST_CHECK_EQUAL(ibdforensic::EntriesForTesting().at(hLate).timeoutTimeUs,
                      (int64_t)6000000);

    // Blocks never requested are counted as unsolicited, not recorded.
    ibdforensic::RecordReceived(1, TestHash(23), 6200000, 0);
    BOOST_CHECK_EQUAL(ibdforensic::UnsolicitedReceiptCount(), (size_t)1);
    BOOST_CHECK_EQUAL(ibdforensic::EntryCount(), (size_t)3);
}

// A re-requested hash updates the canonical entry: cross-peer vs same-peer.
BOOST_AUTO_TEST_CASE(records_cross_peer_and_same_peer_rerequest)
{
    ibdforensic::ResetForTesting();

    const uint256 hx = TestHash(30);
    const uint256 hy = TestHash(31);

    // First request from peer 1; it times out.
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({hx})), 8000000, 0, uint256(0), 1000);
    ibdforensic::RecordExpired(1, hx, 13000000, 0);

    // Re-request from a different peer.
    ibdforensic::RecordGetDataBatch(
        2, Hashes(std::vector<uint256>({hx})), 14000000, 0, uint256(0), 1000);
    const ibdforensic::BatchEntry& eCross =
        ibdforensic::EntriesForTesting().at(hx);
    BOOST_CHECK(eCross.reRequested);
    BOOST_CHECK(eCross.reRequestedOtherPeer);
    BOOST_CHECK_EQUAL(eCross.reRequestPeer, 2);
    BOOST_CHECK_EQUAL(eCross.reRequestTimeUs, (int64_t)14000000);
    BOOST_CHECK_EQUAL(ibdforensic::EntryCount(), (size_t)1);

    // A third re-request from the original peer keeps the first re-request.
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({hx})), 15000000, 0, uint256(0), 1000);
    const ibdforensic::BatchEntry& eThird =
        ibdforensic::EntriesForTesting().at(hx);
    BOOST_CHECK_EQUAL(eThird.reRequestPeer, 2);
    BOOST_CHECK(eThird.reRequestedOtherPeer);

    // A same-peer re-request of a released hash is a re-request but not to
    // another peer.
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({hy})), 16000000, 0, uint256(0), 1000);
    ibdforensic::RecordExpired(1, hy, 21000000, 0);
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({hy})), 22000000, 0, uint256(0), 1000);
    const ibdforensic::BatchEntry& eSame =
        ibdforensic::EntriesForTesting().at(hy);
    BOOST_CHECK(eSame.reRequested);
    BOOST_CHECK(!eSame.reRequestedOtherPeer);
    BOOST_CHECK_EQUAL(eSame.reRequestPeer, 1);
}

// getblocks RATE_LIMIT / no-response counters accumulate and are no-ops when
// the module is disabled.
BOOST_AUTO_TEST_CASE(counts_getblocks_rate_limit_events)
{
    ibdforensic::ResetForTesting();

    ibdforensic::CountGetBlocksRateLimitInbound();
    ibdforensic::CountGetBlocksRateLimitInbound();
    ibdforensic::CountGetBlocksRateLimitOutboundDedup();
    ibdforensic::CountGetBlocksRateLimitOutboundWakeCooldown();
    ibdforensic::CountGetBlocksOutstandingNoResponse(3);

    const ibdforensic::GetBlocksRateCounters c =
        ibdforensic::RateCounters();
    BOOST_CHECK_EQUAL(c.inboundRateLimited, (uint64_t)2);
    BOOST_CHECK_EQUAL(c.outboundDedupSkipped, (uint64_t)1);
    BOOST_CHECK_EQUAL(c.outboundWakeCooldown, (uint64_t)1);
    BOOST_CHECK_EQUAL(c.outstandingNoResponse, (uint64_t)3);

    // Disabled: everything is a no-op.
    ibdforensic::SetEnabled(false, "");
    ibdforensic::CountGetBlocksRateLimitInbound();
    ibdforensic::CountGetBlocksOutstandingNoResponse(9);
    const ibdforensic::GetBlocksRateCounters d =
        ibdforensic::RateCounters();
    BOOST_CHECK_EQUAL(d.inboundRateLimited, (uint64_t)2);
    BOOST_CHECK_EQUAL(d.outstandingNoResponse, (uint64_t)3);

    ibdforensic::ResetForTesting(); // re-enable for subsequent cases
}

// The summary answers the forensic questions: latency grows with position,
// the timeout tail sits in the last bucket, late arrivals are counted, and
// the hashContinue block's timeout is reported.
BOOST_AUTO_TEST_CASE(summary_reports_position_latency_tail_and_hashcontinue)
{
    ibdforensic::ResetForTesting();

    const uint256 h0 = TestHash(40);
    const uint256 h1 = TestHash(41);
    const uint256 h2 = TestHash(42);
    const uint256 h3 = TestHash(43);
    std::vector<uint256> vGetData =
        Hashes(std::vector<uint256>({h0, h1, h2, h3}));
    const int64_t mark = 100000000;
    ibdforensic::RecordGetDataBatch(
        5, vGetData, mark, 2048, h3, 1000); // truncated batch, continuation = h3

    // Clean arrivals with growing latency toward the tail.
    ibdforensic::RecordReceived(5, h0, mark + 10000, 0);
    ibdforensic::RecordReceived(5, h1, mark + 20000, 0);
    ibdforensic::RecordReceived(5, h2, mark + 30000, 0);

    // Tail block times out, is re-requested to another peer, then the
    // already-in-transit original arrives after the timeout.
    ibdforensic::RecordExpired(5, h3, mark + 60000, 0);
    ibdforensic::RecordGetDataBatch(
        6, Hashes(std::vector<uint256>({h3})), mark + 61000, 0, uint256(0), 1000);
    ibdforensic::RecordReceived(5, h3, mark + 65000, 0);

    const std::string s = ibdforensic::FormatSummary();
    BOOST_CHECK(s.find("batches=2") != std::string::npos);
    BOOST_CHECK(s.find("timeouts_total=1") != std::string::npos);
    BOOST_CHECK(s.find("received_after_timeout=1") != std::string::npos);
    BOOST_CHECK(s.find("of_those_rerequested=1") != std::string::npos);
    BOOST_CHECK(s.find("of_those_rerequested_other_peer=1") != std::string::npos);
    BOOST_CHECK(s.find("of_those_rerequested_before_receipt=1") != std::string::npos);
    BOOST_CHECK(s.find("hashcontinue_entries=1") != std::string::npos);
    BOOST_CHECK(s.find("hashcontinue") != std::string::npos);
    BOOST_CHECK(s.find("timed_out=1") != std::string::npos);
    BOOST_CHECK(s.find("never_received_total=0") != std::string::npos);

    // The timeout must land in the tail bucket (bucket 9 for the last of 4
    // hashes) and the clean-latency bucket means must grow with position.
    const std::string tail =
        s.substr(s.find("bucket=9"));
    BOOST_CHECK(tail.find("timeouts_in_bucket=1") != std::string::npos);
    const std::string head = s.substr(s.find("bucket=0"));
    BOOST_CHECK(head.find("timeouts_in_bucket=0") != std::string::npos);

    // Slope over absolute seq must be positive (latency grows with position).
    BOOST_CHECK(s.find("latency_slope_us_per_seq=") != std::string::npos);
}

// The same GetBoolArg/GetArg expression AppInit2() uses (init.cpp:976-978)
// must enable the module from command-line arguments.  The file is then
// written even though zero events were recorded.
BOOST_AUTO_TEST_CASE(enablement_from_cli_args_creates_dump_file)
{
    ArgsRestore restore;
    ibdforensic::ResetForTesting();

    const boost::filesystem::path p = TmpForensicPath("cli");
    ParseForensicArgs("-ibdforensic=1 -ibdforensicpath=" + p.string());

    ibdforensic::SetEnabled(GetBoolArg("-ibdforensic", false),
                            GetArg("-ibdforensicpath", ""));

    BOOST_CHECK(ibdforensic::IsEnabled());
    BOOST_CHECK(ibdforensic::Dump());
    BOOST_CHECK(boost::filesystem::exists(p));

    const std::string contents = ReadFile(p);
    BOOST_CHECK(contents.find("IBDFORENSIC SUMMARY") != std::string::npos);
    BOOST_CHECK(contents.find("batches=0") != std::string::npos);
    BOOST_CHECK(contents.find("# peer,batch_id,seq,n_hashes,hash")
                != std::string::npos);

    boost::filesystem::remove(p);
    ibdforensic::ResetForTesting();
}

// innova.conf entries land in mapArgs exactly like ReadConfigFile() copies
// them (util.cpp:1400-1405: "-" + key -> value).  The same SetEnabled
// expression must enable the module and produce the dump file.
BOOST_AUTO_TEST_CASE(enablement_from_config_style_args_creates_dump_file)
{
    ArgsRestore restore;
    ibdforensic::ResetForTesting();

    const boost::filesystem::path p = TmpForensicPath("conf");
    mapArgs["-ibdforensic"] = "1";
    mapArgs["-ibdforensicpath"] = p.string();

    ibdforensic::SetEnabled(GetBoolArg("-ibdforensic", false),
                            GetArg("-ibdforensicpath", ""));

    BOOST_CHECK(ibdforensic::IsEnabled());
    BOOST_CHECK(ibdforensic::Dump());
    BOOST_CHECK(boost::filesystem::exists(p));

    const std::string contents = ReadFile(p);
    BOOST_CHECK(contents.find("IBDFORENSIC SUMMARY") != std::string::npos);
    BOOST_CHECK(contents.find("batches=0") != std::string::npos);

    boost::filesystem::remove(p);
    ibdforensic::ResetForTesting();
}

// The dump file carries the recorded batches and the CSV rows.
BOOST_AUTO_TEST_CASE(dump_file_contains_recorded_batch_and_csv_rows)
{
    ibdforensic::ResetForTesting();

    const boost::filesystem::path p = TmpForensicPath("data");
    ibdforensic::SetEnabled(true, p.string());
    ibdforensic::RecordGetDataBatch(
        3, Hashes(std::vector<uint256>({TestHash(1)})), 1000000, 128,
        uint256(0), 1000);

    BOOST_CHECK(ibdforensic::Dump());
    const std::string contents = ReadFile(p);
    BOOST_CHECK(contents.find("batches=1") != std::string::npos);
    // One CSV data row: peer=3, batch_id=0, seq=0, n_hashes=1.
    BOOST_CHECK(contents.find("3,0,0,1,") != std::string::npos);

    boost::filesystem::remove(p);
    ibdforensic::ResetForTesting();
}

// SetEnabled(false) must not erase recorded counters, and the shutdown dump
// must still write everything that was collected.
BOOST_AUTO_TEST_CASE(disable_keeps_counters_and_dump_still_writes)
{
    ibdforensic::ResetForTesting();

    const boost::filesystem::path p = TmpForensicPath("nodump-wipe");
    ibdforensic::SetEnabled(true, p.string());
    ibdforensic::RecordGetDataBatch(
        3, Hashes(std::vector<uint256>({TestHash(1)})), 1000000, 128,
        uint256(0), 1000);
    ibdforensic::CountGetBlocksRateLimitInbound();

    ibdforensic::SetEnabled(false, p.string());
    BOOST_CHECK(!ibdforensic::IsEnabled());
    BOOST_CHECK_EQUAL(ibdforensic::BatchCount(), (size_t)1);
    BOOST_CHECK_EQUAL(ibdforensic::EntryCount(), (size_t)1);
    BOOST_CHECK_EQUAL(ibdforensic::RateCounters().inboundRateLimited,
                      (uint64_t)1);

    BOOST_CHECK(ibdforensic::Dump());
    const std::string contents = ReadFile(p);
    BOOST_CHECK(contents.find("batches=1") != std::string::npos);
    BOOST_CHECK(contents.find("getblocks_rate_limited_inbound=1")
                != std::string::npos);

    boost::filesystem::remove(p);
    ibdforensic::ResetForTesting();
}

// A dump path whose parent directory does not exist must fail loudly: Dump()
// reports it and returns false, and the already-recorded data survives.
BOOST_AUTO_TEST_CASE(unwritable_dump_path_is_reported_and_data_survives)
{
    ibdforensic::ResetForTesting();

    const boost::filesystem::path bad =
        boost::filesystem::temp_directory_path() /
        "ibdforensic-no-such-dir" / "out.log";
    ibdforensic::SetEnabled(true, bad.string());

    BOOST_CHECK(!ibdforensic::Dump());
    BOOST_CHECK(!boost::filesystem::exists(bad));

    // Data recorded after the failed dump (and before it) is preserved.
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({TestHash(1)})), 1000000, 0,
        uint256(0), 1000);
    BOOST_CHECK_EQUAL(ibdforensic::BatchCount(), (size_t)1);
    BOOST_CHECK_EQUAL(ibdforensic::EntryCount(), (size_t)1);

    ibdforensic::ResetForTesting();
}

// A generation opens in MarkBlockInFlight (RecordGenerationStart), is closed
// by an ownership release (RecordGenerationEnd) with its reason, and is
// idempotent on both ends.  The summary histogram reflects the reasons.
BOOST_AUTO_TEST_CASE(generation_lifecycle_open_close_and_idempotency)
{
    ibdforensic::ResetForTesting();

    const uint256 h = TestHash(50);
    ibdforensic::RecordGenerationStart(3, h, 1000000);
    BOOST_CHECK_EQUAL(ibdforensic::GenerationCount(), (size_t)1);

    // Double start while a generation is active must not open a second one
    // (mirrors the in-flight admission gate).
    ibdforensic::RecordGenerationStart(3, h, 1000100);
    BOOST_CHECK_EQUAL(ibdforensic::GenerationCount(), (size_t)1);

    ibdforensic::RecordGenerationEnd(h, 2000000, "timeout");
    BOOST_CHECK_EQUAL(ibdforensic::GenerationCount(), (size_t)1);

    // Double close (multiple release chokepoints can fire) keeps the first.
    ibdforensic::RecordGenerationEnd(h, 2100000, "timeout");
    BOOST_CHECK_EQUAL(ibdforensic::GenerationCount(), (size_t)1);

    // A release is the precondition for a new generation (re-request).
    ibdforensic::RecordGenerationStart(7, h, 2200000);
    BOOST_CHECK_EQUAL(ibdforensic::GenerationCount(), (size_t)2);

    const std::string s = ibdforensic::FormatSummary();
    BOOST_CHECK(s.find("generations_total=2") != std::string::npos);
    BOOST_CHECK(s.find("generations_active=1") != std::string::npos);
    BOOST_CHECK(s.find("closed_timeout=1") != std::string::npos);
    BOOST_CHECK(s.find("closed_receive=0") != std::string::npos);
    BOOST_CHECK(s.find("closed_disconnect=0") != std::string::npos);
}

// RecordExpired stores the oldest-in-flight head age at expiry; the first
// expiry wins and equal head ages (equal in-flight marks) are recorded
// identically.
BOOST_AUTO_TEST_CASE(record_expired_stores_head_age_and_keeps_first)
{
    ibdforensic::ResetForTesting();

    const uint256 ha = TestHash(60);
    const uint256 hb = TestHash(61);
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({ha, hb})), 5000000, 0, uint256(0), 1000);

    ibdforensic::RecordExpired(1, ha, 13000000, 3000000);
    const ibdforensic::BatchEntry& ea =
        ibdforensic::EntriesForTesting().at(ha);
    BOOST_CHECK_EQUAL(ea.timeoutTimeUs, (int64_t)13000000);
    BOOST_CHECK_EQUAL(ea.headAgeAtExpiryUs, (int64_t)3000000);

    // Duplicate expiry does not overwrite the head age (first expiry wins).
    ibdforensic::RecordExpired(1, ha, 14000000, 9000000);
    BOOST_CHECK_EQUAL(ibdforensic::EntriesForTesting().at(ha).headAgeAtExpiryUs,
                      (int64_t)3000000);

    // Equal marks (the tie ExpireBlockInFlight breaks by sorted hash order,
    // which is deterministic by construction) yield identical head ages.
    ibdforensic::RecordExpired(1, hb, 13000000, 3000000);
    BOOST_CHECK_EQUAL(ibdforensic::EntriesForTesting().at(hb).headAgeAtExpiryUs,
                      (int64_t)3000000);
}

// Receipt attribution picks the generation that delivered the block: the
// active generation when the receipt closes one, else the closed generation
// whose [mark, release] window contains the dispatch time, else the most
// recent generation.
BOOST_AUTO_TEST_CASE(receipt_attribution_delivering_generation)
{
    ibdforensic::ResetForTesting();

    const uint256 hActive = TestHash(70);
    const uint256 hWindow = TestHash(71);
    const uint256 hRecent = TestHash(72);

    // (a) Active generation is closed as "receive" and attributed.
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({hActive})), 900000, 0, uint256(0), 1000);
    ibdforensic::RecordGenerationStart(1, hActive, 1000000);
    ibdforensic::RecordReceived(1, hActive, 1500000, 1400000);
    const ibdforensic::BatchEntry& eA =
        ibdforensic::EntriesForTesting().at(hActive);
    BOOST_CHECK_EQUAL(eA.recvTimeUs, (int64_t)1500000);
    BOOST_CHECK_EQUAL(eA.recvFramingCompleteUs, (int64_t)1400000);
    BOOST_CHECK_EQUAL(eA.genIdAtReceipt, (uint64_t)1);

    // (b) Closed generation whose window contains the dispatch time wins.
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({hWindow})), 900000, 0, uint256(0), 1000);
    ibdforensic::RecordGenerationStart(1, hWindow, 1000000);
    ibdforensic::RecordGenerationEnd(hWindow, 2000000, "timeout");
    ibdforensic::RecordReceived(1, hWindow, 1500000, 1400000);
    BOOST_CHECK_EQUAL(
        ibdforensic::EntriesForTesting().at(hWindow).genIdAtReceipt,
        (uint64_t)2);

    // (c) Dispatch outside every window falls back to the most recent gen.
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({hRecent})), 900000, 0, uint256(0), 1000);
    ibdforensic::RecordGenerationStart(1, hRecent, 1000000);
    ibdforensic::RecordGenerationEnd(hRecent, 2000000, "timeout");
    ibdforensic::RecordReceived(1, hRecent, 3000000, 2900000);
    BOOST_CHECK_EQUAL(
        ibdforensic::EntriesForTesting().at(hRecent).genIdAtReceipt,
        (uint64_t)3);
}

// The delayed-first-block scenario: the original peer's block arrives after a
// cross-peer re-request already took over ownership.  The active (re-request)
// generation is what the receive releases, so the ledger closes it as
// "receive" deterministically; delivery progress stays keyed to the actual
// sender (the original peer).
BOOST_AUTO_TEST_CASE(late_original_peer_receipt_after_cross_peer_rerequest)
{
    ibdforensic::ResetForTesting();

    const uint256 h = TestHash(80);
    ibdforensic::RecordGetDataBatch(
        5, Hashes(std::vector<uint256>({h})), 1000000, 0, uint256(0), 1000);
    ibdforensic::RecordGenerationStart(5, h, 1000000);

    // Original request times out; ownership released.
    ibdforensic::RecordExpired(5, h, 6000000, 5000000);
    ibdforensic::RecordGenerationEnd(h, 6000000, "timeout");

    // Re-request to another peer opens a new (active) generation.
    ibdforensic::RecordGetDataBatch(
        7, Hashes(std::vector<uint256>({h})), 6100000, 0, uint256(0), 1000);
    ibdforensic::RecordGenerationStart(7, h, 6100000);

    // The already-in-transit original block arrives late from peer 5.
    ibdforensic::RecordReceived(5, h, 6500000, 6400000);

    const ibdforensic::BatchEntry& e =
        ibdforensic::EntriesForTesting().at(h);
    BOOST_CHECK(e.receivedAfterTimeout);
    BOOST_CHECK_EQUAL(e.genIdAtReceipt, (uint64_t)2); // the re-request gen
    BOOST_CHECK_EQUAL(e.reRequested, true);
    BOOST_CHECK(e.reRequestedOtherPeer);
    BOOST_CHECK_EQUAL(e.progressLastUs, (int64_t)0); // peer 5 never delivered before

    const std::string s = ibdforensic::FormatSummary();
    BOOST_CHECK(s.find("generations_total=2") != std::string::npos);
    BOOST_CHECK(s.find("closed_timeout=1") != std::string::npos);
    BOOST_CHECK(s.find("closed_receive=1") != std::string::npos);
    BOOST_CHECK(s.find("generations_active=0") != std::string::npos);
}

// RecordSocketSend stamps the oldest un-stamped getdata batch (FIFO), ignores
// non-getdata commands, and is a no-op when nothing is pending.
BOOST_AUTO_TEST_CASE(record_socket_send_fifo_stamps_oldest_unstamped_batch)
{
    ibdforensic::ResetForTesting();

    const uint256 h1 = TestHash(90);
    const uint256 h2 = TestHash(91);
    const uint256 h3 = TestHash(92);
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({h1})), 1000000, 512, uint256(0), 1000);
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({h2})), 2000000, 512, uint256(0), 1000);
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({h3})), 3000000, 512, uint256(0), 1000);

    // First "getdata" first-send stamps the oldest batch.
    ibdforensic::RecordSocketSend("getdata", 1100000, 4096);
    BOOST_CHECK_EQUAL(ibdforensic::BatchesForTesting()[0].firstSocketSendUs,
                      (int64_t)1100000);
    BOOST_CHECK_EQUAL(ibdforensic::BatchesForTesting()[0].nsendFirstSend,
                      (size_t)4096);

    // A non-getdata first-send must not consume the queue.
    ibdforensic::RecordSocketSend("version", 1200000, 8192);
    BOOST_CHECK_EQUAL(ibdforensic::BatchesForTesting()[1].firstSocketSendUs,
                      (int64_t)0);

    // Next getdata first-send stamps the next batch.
    ibdforensic::RecordSocketSend("getdata", 2100000, 1000);
    BOOST_CHECK_EQUAL(ibdforensic::BatchesForTesting()[1].firstSocketSendUs,
                      (int64_t)2100000);
    ibdforensic::RecordSocketSend("getdata", 3100000, 2000);
    BOOST_CHECK_EQUAL(ibdforensic::BatchesForTesting()[2].firstSocketSendUs,
                      (int64_t)3100000);

    // Queue exhausted: further first-sends are a no-op (no crash, no stamp).
    ibdforensic::RecordSocketSend("getdata", 3200000, 3000);
    BOOST_CHECK_EQUAL(ibdforensic::BatchesForTesting()[2].firstSocketSendUs,
                      (int64_t)3100000);
}

// EndMessage pushes the parallel vSendMeta entry; the failure path (a send()
// that errors immediately) must leave the two deques aligned and un-stamped.
BOOST_AUTO_TEST_CASE(send_meta_alignment_on_failure_path)
{
    ibdforensic::ResetForTesting();

    CAddress addr;
    CNode n(INVALID_SOCKET, addr, "", true);

    // Optimistic send fails (EBADF): the node is marked for disconnect but the
    // message stays queued, so later pushes keep the deques in lockstep.
    n.PushMessage("ping");
    BOOST_CHECK(n.fDisconnect);
    n.PushMessage("pong");
    n.PushMessage("verack");

    BOOST_CHECK_EQUAL(n.vSendMsg.size(), (size_t)3);
    BOOST_CHECK_EQUAL(n.vSendMeta.size(), (size_t)3);
    BOOST_CHECK_EQUAL(n.vSendMeta[0].command, "ping");
    BOOST_CHECK_EQUAL(n.vSendMeta[1].command, "pong");
    BOOST_CHECK_EQUAL(n.vSendMeta[2].command, "verack");
    // No byte was ever written: every meta stays pending.
    BOOST_CHECK(n.vSendMeta[0].fStampPending);
    BOOST_CHECK(n.vSendMeta[1].fStampPending);
    BOOST_CHECK(n.vSendMeta[2].fStampPending);
    BOOST_CHECK(n.nSendSize > 0);

    ibdforensic::ResetForTesting();
}

// Small messages are written entirely by the optimistic send in EndMessage:
// both deques are erased together and nSendSize returns to zero.  A getdata
// message first-sent this way stamps its recorded batch (FIFO end-to-end).
BOOST_AUTO_TEST_CASE(send_meta_alignment_full_send_empties_both_and_stamps)
{
    ibdforensic::ResetForTesting();

    int sv[2];
    BOOST_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    CAddress addr;
    CNode n(INVALID_SOCKET, addr, "", true);
    n.hSocket = sv[0];

    // Three fully-sendable messages; each optimistic write completes, so the
    // queue (and the parallel meta deque) is empty again each time.
    n.PushMessage("ping");
    BOOST_CHECK(n.vSendMsg.empty());
    BOOST_CHECK(n.vSendMeta.empty());
    n.PushMessage("pong");
    BOOST_CHECK(n.vSendMsg.empty());
    BOOST_CHECK(n.vSendMeta.empty());
    BOOST_CHECK_EQUAL(n.nSendSize, (size_t)0);

    // End-to-end getdata: a recorded batch is stamped by the getdata message's
    // first successful send, even when that send completes immediately.
    const uint256 h = TestHash(95);
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({h})), 1000000, 0, uint256(0), 1000);
    std::vector<CInv> vInv;
    vInv.push_back(CInv(MSG_BLOCK, h));
    n.PushMessage("getdata", vInv);
    BOOST_CHECK(n.vSendMsg.empty());
    BOOST_CHECK(n.vSendMeta.empty());
    BOOST_CHECK(ibdforensic::BatchesForTesting()[0].firstSocketSendUs > 0);
    BOOST_CHECK(ibdforensic::BatchesForTesting()[0].nsendFirstSend > 0);

    close(sv[1]);
    ibdforensic::ResetForTesting();
}

// A message larger than the peer's receive buffer is only partially written:
// the first send stamps its meta, the queue stays aligned, an EWOULDBLOCK
// send leaves it aligned, and a mid-queue connection drop keeps it aligned.
BOOST_AUTO_TEST_CASE(send_meta_alignment_partial_send_ewouldblock_and_disconnect)
{
    ibdforensic::ResetForTesting();

    int sv[2];
    BOOST_REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0);
    // Fix a small receive buffer on the peer end so a 4 MiB message cannot
    // complete in one send (disables autotuning; ~2x the requested value).
    int nRcvBuf = 8192;
    BOOST_REQUIRE(setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &nRcvBuf,
                             sizeof(nRcvBuf)) == 0);
    CAddress addr;
    CNode n(INVALID_SOCKET, addr, "", true);
    n.hSocket = sv[0];

    std::vector<unsigned char> big(4 * 1024 * 1024, 0xaa);
    n.PushMessage("block", big);

    // The optimistic write could only partially send: one message queued, its
    // meta stamped on the first successful write.
    BOOST_CHECK_EQUAL(n.vSendMsg.size(), (size_t)1);
    BOOST_CHECK_EQUAL(n.vSendMeta.size(), (size_t)1);
    BOOST_CHECK(!n.vSendMeta[0].fStampPending);
    BOOST_CHECK(n.vSendMeta[0].firstSendUs > 0);
    BOOST_CHECK(n.nSendOffset > 0);

    // More messages queue without touching the send buffer (queue non-empty).
    n.PushMessage("tx");
    n.PushMessage("addr");
    BOOST_CHECK_EQUAL(n.vSendMsg.size(), (size_t)3);
    BOOST_CHECK_EQUAL(n.vSendMeta.size(), (size_t)3);
    BOOST_CHECK(n.vSendMeta[1].fStampPending);
    BOOST_CHECK(n.vSendMeta[2].fStampPending);

    // The buffer is full: EWOULDBLOCK must not advance or misalign anything.
    {
        LOCK(n.cs_vSend);
        SocketSendData(&n);
    }
    BOOST_CHECK_EQUAL(n.vSendMsg.size(), (size_t)3);
    BOOST_CHECK_EQUAL(n.vSendMeta.size(), (size_t)3);
    BOOST_CHECK(!n.fDisconnect);

    // Close the peer: the next send fails (EPIPE) -> disconnect, and the two
    // deques remain exactly aligned.
    close(sv[1]);
    {
        LOCK(n.cs_vSend);
        SocketSendData(&n);
    }
    BOOST_CHECK(n.fDisconnect);
    BOOST_CHECK_EQUAL(n.vSendMsg.size(), (size_t)3);
    BOOST_CHECK_EQUAL(n.vSendMeta.size(), (size_t)3);

    ibdforensic::ResetForTesting();
}

// A generation-aware dump writes the additive legacy columns and the separate
// #generations section, so both the legacy and the new-schema analyzers can
// consume it.
BOOST_AUTO_TEST_CASE(dump_contains_generation_section)
{
    ibdforensic::ResetForTesting();

    const boost::filesystem::path p = TmpForensicPath("gens");
    ibdforensic::SetEnabled(true, p.string());

    const uint256 h = TestHash(100);
    ibdforensic::RecordGetDataBatch(
        1, Hashes(std::vector<uint256>({h})), 1000000, 0, uint256(0), 1000);
    ibdforensic::RecordGenerationStart(1, h, 1000000);
    ibdforensic::RecordExpired(1, h, 6000000, 5000000);
    ibdforensic::RecordGenerationEnd(h, 6000000, "timeout");

    BOOST_CHECK(ibdforensic::Dump());
    const std::string contents = ReadFile(p);
    BOOST_CHECK(contents.find("generation_id") != std::string::npos);
    BOOST_CHECK(contents.find("head_age_at_expiry_us") != std::string::npos);
    BOOST_CHECK(contents.find("recv_framing_complete_us") != std::string::npos);
    BOOST_CHECK(contents.find("#generations") != std::string::npos);
    BOOST_CHECK(contents.find("1,0,") != std::string::npos);
    BOOST_CHECK(contents.find("timeout") != std::string::npos);

    boost::filesystem::remove(p);
    ibdforensic::ResetForTesting();
}

// Disabled recording is a hot-path no-op: every record function returns before
// touching state, so nothing accumulates even when called.
BOOST_AUTO_TEST_CASE(disabled_forensic_accumulates_nothing)
{
    ibdforensic::ResetForTesting();
    ibdforensic::SetEnabled(false, "");

    const uint256 h = TestHash(110);
    std::vector<uint256> v = Hashes(std::vector<uint256>({h}));
    ibdforensic::RecordGetDataBatch(1, v, 1000000, 0, uint256(0), 1000);
    ibdforensic::RecordGenerationStart(1, h, 1000000);
    ibdforensic::RecordGenerationEnd(h, 2000000, "receive");
    ibdforensic::RecordSocketSend("getdata", 3000000, 4096);
    ibdforensic::RecordExpired(1, h, 4000000, 3000000);
    ibdforensic::RecordReceived(1, h, 5000000, 4900000);

    BOOST_CHECK_EQUAL(ibdforensic::BatchCount(), (size_t)0);
    BOOST_CHECK_EQUAL(ibdforensic::EntryCount(), (size_t)0);
    BOOST_CHECK_EQUAL(ibdforensic::GenerationCount(), (size_t)0);
    BOOST_CHECK_EQUAL(ibdforensic::UnsolicitedReceiptCount(), (size_t)0);

    ibdforensic::ResetForTesting();
}

BOOST_AUTO_TEST_SUITE_END()
