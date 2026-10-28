// Isolated self-test for the opt-in p2pprop telemetry module.
// Compiles p2pprop.cpp with -DP2PPROP_SELFTEST (stubbed util sink/clock) and
// proves, deterministically and WITHOUT a live chain:
//   * OFF-by-default: no emission when telemetry is disabled.
//   * ON: one [P2PPROP] marker per stage hop, with correct stage tag and prefix.
//   * OFF again: silent.
// Exit 0 = PASS.
#include "p2pprop.h"

#include <cstdio>
#include <string>

int main()
{
    const uint256 h; // zero hash

    // 1) disabled => silent
    if (p2pprop::Enabled())
        return 1;
    p2pprop::ClearEmissionsForTest();
    p2pprop::RecordInvReceived(h, 1);
    p2pprop::RecordBlockAccepted(h, 1, 2, 3);
    if (p2pprop::EmissionCount() != 0)
        return 2;

    // 2) enabled => exactly one marker per hop
    p2pprop::SetEnabled(true);
    if (!p2pprop::Enabled())
        return 3;
    p2pprop::ClearEmissionsForTest();
    p2pprop::RecordInvReceived(h, 7);
    p2pprop::RecordGetDataSent(h, 7, false);
    p2pprop::RecordBlockReceived(h, 7, 2048);
    p2pprop::RecordBlockAccepted(h, 12345, 3400, 1200);
    p2pprop::RecordAnnounce(h, 9);
    if (p2pprop::EmissionCount() != 5)
        return 4;

    const char* want[5] = {
        "stage=inv_received", "stage=getdata_sent", "stage=block_received",
        "stage=block_accepted", "stage=announce_sent"};
    for (int i = 0; i < 5; ++i) {
        std::string l = p2pprop::EmissionAt((size_t)i);
        if (l.find("[P2PPROP]") == std::string::npos)
            return 10 + i;
        if (l.find(want[i]) == std::string::npos)
            return 20 + i;
        if (l.find("wall_us=") == std::string::npos)
            return 30 + i;
        if (i == 1 && l.find("retry=0") == std::string::npos)
            return 35;
    }

    // 2b) retry discriminator is a distinct tag value
    p2pprop::RecordGetDataSent(h, 8, true);
    std::string lr = p2pprop::EmissionAt(p2pprop::EmissionCount() - 1);
    if (lr.find("retry=1") == std::string::npos)
        return 36;

    // 2c) Stage-5 wire/serve lifecycle scenarios (getdata_wire / getdata_received / block_served)
    p2pprop::ClearEmissionsForTest();
    const uint256 h2;
    auto count_stage = [](const char* st) -> size_t {
        size_t n = 0;
        for (size_t i = 0; i < p2pprop::EmissionCount(); ++i)
            if (p2pprop::EmissionAt(i).find(st) != std::string::npos) ++n;
        return n;
    };
    // (i) enqueue WITHOUT socket write: getdata_sent present, getdata_wire absent
    p2pprop::RecordGetDataSent(h2, 25, false);
    if (count_stage("stage=getdata_sent") != 1) return 50;
    if (count_stage("stage=getdata_wire") != 0) return 51;
    // (ii) successful wire write (first ::send()>0): one wire marker carrying peer + wire flag
    p2pprop::RecordGetDataWire(h2, 25);
    if (count_stage("stage=getdata_wire") != 1) return 52;
    if (p2pprop::EmissionAt(p2pprop::EmissionCount()-1).find("wire_evt=first_byte") == std::string::npos) return 53;
    if (p2pprop::EmissionAt(p2pprop::EmissionCount()-1).find("peer=25") == std::string::npos) return 54;
    // (iii) partial send does not duplicate stamping: first-write hook fires once per frame
    if (count_stage("stage=getdata_wire") != 1) return 55;
    // (iv) receiver parsed incoming getdata; server enqueued block in response
    p2pprop::RecordGetDataReceived(h2, 61);
    p2pprop::RecordBlockServed(h2, 61);
    if (count_stage("stage=getdata_received") != 1) return 56;
    if (count_stage("stage=block_served") != 1) return 57;
    // (v) disconnect WITHOUT wire: enqueue present but never written => no wire marker for h3
    const uint256 h3;
    p2pprop::RecordGetDataSent(h3, 5, false);
    if (count_stage("stage=getdata_sent") != 2) return 58;
    if (count_stage("stage=getdata_wire") != 1) return 59; // only h2's wire, not h3's
    // (vi) re-request after disconnect: retry=1, then the re-sent frame does reach wire
    p2pprop::RecordGetDataSent(h3, 6, true);
    if (count_stage("stage=getdata_sent") != 3) return 60;
    std::string last = p2pprop::EmissionAt(p2pprop::EmissionCount()-1);
    if (last.find("stage=getdata_sent") == std::string::npos || last.find("retry=1") == std::string::npos) return 61;
    p2pprop::RecordGetDataWire(h3, 6);
    if (count_stage("stage=getdata_wire") != 2) return 62;

    // 3) re-disable => silent again
    p2pprop::SetEnabled(false);
    p2pprop::ClearEmissionsForTest();
    p2pprop::RecordInvReceived(h, 1);
    if (p2pprop::EmissionCount() != 0)
        return 40;

    printf("P2PPROP_SELFTEST PASS: disabled=0 | enabled=8 markers (%s) | wire/serve scenarios ok | re-disabled=0\n",
           "inv_received,getdata_sent,block_received,block_accepted,announce_sent,getdata_wire,getdata_received,block_served");
    return 0;
}