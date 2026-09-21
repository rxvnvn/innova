#include <boost/test/unit_test.hpp>
#include "../dag_tips_delta.h"
#include <vector>
extern bool CorruptDagTipDeltaSpillForTest(int);
namespace { struct C { std::vector<DagTipCommittedDeltaEvent> e; static void F(const DagTipCommittedDeltaEvent& x, void* p) { static_cast<C*>(p)->e.push_back(x); } }; }
BOOST_AUTO_TEST_SUITE(dag_tips_delta_tests)
BOOST_AUTO_TEST_CASE(disabled_no_events) { SetDagTipCommittedDeltaObserver(NULL,NULL); BOOST_CHECK(!BeginDagTipDeltaTransaction(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX)); }
BOOST_AUTO_TEST_CASE(commit_envelope_and_order) { C c; SetDagTipCommittedDeltaObserver(&C::F,&c); BOOST_REQUIRE(BeginDagTipDeltaTransaction(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX)); AppendDagTipDelta(DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD,uint256(1))); AppendDagTipDelta(DagTipDeltaRecord(DagTipDeltaRecord::TIP_REMOVE,uint256(2))); CommitDagTipDeltaTransaction(); BOOST_REQUIRE_EQUAL(c.e.size(),4u); BOOST_CHECK_EQUAL(c.e[0].kind,DagTipCommittedDeltaEvent::BEGIN); BOOST_CHECK_EQUAL(c.e[0].origin,DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX); BOOST_CHECK_EQUAL(c.e[1].kind,DagTipCommittedDeltaEvent::RECORD); BOOST_CHECK(c.e[1].record.hash==uint256(1)); BOOST_CHECK_EQUAL(c.e[2].record.op,DagTipDeltaRecord::TIP_REMOVE); BOOST_CHECK_EQUAL(c.e[3].kind,DagTipCommittedDeltaEvent::END); SetDagTipCommittedDeltaObserver(NULL,NULL); }
BOOST_AUTO_TEST_CASE(discard_has_no_envelope) { C c; SetDagTipCommittedDeltaObserver(&C::F,&c); BOOST_REQUIRE(BeginDagTipDeltaTransaction(DAG_TIP_DELTA_REORGANIZE)); AppendDagTipDelta(DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD,uint256(3))); DiscardDagTipDeltaTransaction(); BOOST_CHECK(c.e.empty()); SetDagTipCommittedDeltaObserver(NULL,NULL); }
BOOST_AUTO_TEST_CASE(spill_streams_bounded_ordered_envelope) { C c; SetDagTipCommittedDeltaObserver(&C::F,&c); SetDagTipDeltaRamCapacityForTest(2); BOOST_REQUIRE(BeginDagTipDeltaTransaction(DAG_TIP_DELTA_REORGANIZE)); for(int i=0;i<300;++i) AppendDagTipDelta(DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD,uint256(i+10))); DagTipDeltaState s=GetDagTipDeltaState(); BOOST_CHECK(s.spilled); BOOST_CHECK_LE(s.ramRecords,s.ramCapacity); CommitDagTipDeltaTransaction(); BOOST_REQUIRE_EQUAL(c.e.size(),302u); BOOST_CHECK_EQUAL(c.e.front().kind,DagTipCommittedDeltaEvent::BEGIN); BOOST_CHECK_EQUAL(c.e.back().kind,DagTipCommittedDeltaEvent::END); for(int i=0;i<300;++i) BOOST_CHECK(c.e[i+1].record.hash==uint256(i+10)); SetDagTipCommittedDeltaObserver(NULL,NULL); SetDagTipDeltaRamCapacityForTest(256); }
BOOST_AUTO_TEST_CASE(nested_join_keeps_root_origin_and_single_envelope) { C c; SetDagTipCommittedDeltaObserver(&C::F,&c); BOOST_REQUIRE(BeginDagTipDeltaTransaction(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX)); BOOST_CHECK(!BeginDagTipDeltaTransaction(DAG_TIP_DELTA_REORGANIZE)); AppendDagTipDelta(DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD,uint256(9))); LeaveDagTipDeltaTransaction(); CommitDagTipDeltaTransaction(); BOOST_REQUIRE_EQUAL(c.e.size(),3u); BOOST_CHECK_EQUAL(c.e[0].origin,DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX); SetDagTipCommittedDeltaObserver(NULL,NULL); }
BOOST_AUTO_TEST_CASE(final_source_token_is_root_scoped_and_nested_latest_wins) { C c; SetDagTipCommittedDeltaObserver(&C::F,&c); BOOST_REQUIRE(BeginDagTipDeltaTransaction(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX)); BOOST_CHECK(!GetDagTipDeltaFinalSourceStateId(NULL)); SetDagTipDeltaFinalSourceStateId(uint256(11)); uint256 got; BOOST_REQUIRE(GetDagTipDeltaFinalSourceStateId(&got)); BOOST_CHECK(got == uint256(11)); BOOST_CHECK(!BeginDagTipDeltaTransaction(DAG_TIP_DELTA_REORGANIZE)); SetDagTipDeltaFinalSourceStateId(uint256(12)); LeaveDagTipDeltaTransaction(); BOOST_REQUIRE(GetDagTipDeltaFinalSourceStateId(&got)); BOOST_CHECK(got == uint256(12)); CommitDagTipDeltaTransaction(); BOOST_CHECK(!GetDagTipDeltaFinalSourceStateId(&got)); SetDagTipCommittedDeltaObserver(NULL,NULL); }
BOOST_AUTO_TEST_CASE(corrupt_spill_cannot_publish_end)
{
    for (int mode=0; mode<3; ++mode) {
        C capture; SetDagTipCommittedDeltaObserver(&C::F,&capture);
        SetDagTipDeltaRamCapacityForTest(1);
        BOOST_REQUIRE(BeginDagTipDeltaTransaction(DAG_TIP_DELTA_ADD_TO_BLOCK_INDEX));
        SetDagTipDeltaInitialSourceStateId(uint256(100)); SetDagTipDeltaFinalSourceStateId(uint256(101));
        for (int i=0;i<4;++i) AppendDagTipDelta(DagTipDeltaRecord(DagTipDeltaRecord::TIP_ADD,uint256(i+1)));
        BOOST_REQUIRE(CorruptDagTipDeltaSpillForTest(mode)); CommitDagTipDeltaTransaction();
        BOOST_REQUIRE(!capture.e.empty());
        BOOST_CHECK_EQUAL(capture.e.front().kind,DagTipCommittedDeltaEvent::BEGIN);
        for (const auto& e:capture.e) BOOST_CHECK(e.kind!=DagTipCommittedDeltaEvent::END);
        SetDagTipCommittedDeltaObserver(NULL,NULL);
    }
    SetDagTipDeltaRamCapacityForTest(256);
}
BOOST_AUTO_TEST_SUITE_END()
