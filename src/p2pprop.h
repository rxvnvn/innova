#ifndef INNOVA_P2PPROP_H
#define INNOVA_P2PPROP_H

#include <stdint.h>
#include <string>
#include "uint256.h"

// Opt-in block-propagation latency telemetry. DISABLED BY DEFAULT.
//
// When enabled (-p2pprop=1) it emits ONE LogPrintf stream, prefix "[P2PPROP]",
// one line per event, keyed by block hash with epoch UTC microseconds
// (GetTimeMicros()). It reconstructs the propagation pipeline on the receiving
// and announcing nodes:
//     inv_received  -> getdata_sent -> block_received
//                   -> block_accepted -> announce_sent
// so that, with two nodes observed together, per-stage propagation latency
// (inv->getdata, getdata->block, block->accept, accept->announce) can be
// measured for locally-mined, network-received and side-branch blocks.
//
// Purely additive and protocol-neutral: no consensus/nBits/trust/serialization
// change, no new wire message, no persistent state beyond a bool. When disabled
// every Record* is an immediate return (no allocation, no clock read).
//
// Compose-able with the existing opt-in IBD telemetry (-ibdblocklatency,
// -showtimers, ibdactivepath); this focuses specifically on the propagation
// hops that the others do not carry (inv receipt and block re-announce fan-out).

namespace p2pprop {

void SetEnabled(bool fEnabled);
bool Enabled();

// Deterministic isolated-verification access to the recent-emission ring
// (only populated while enabled; empty otherwise). Not needed by the node.
size_t EmissionCount();
std::string EmissionAt(size_t i);
void ClearEmissionsForTest();

// stage markers (all keyed by the block hash; peer = pfrom->GetId(), -1 if none)
void RecordInvReceived(const uint256& hash, int peer);
// fRetry distinguishes a first-time request from a re-request of an in-flight
// block (0 = first dispatch, 1 = re-request after no timely answer).
// Marker fires at the point the block-hash getdata is placed into the
// outbound send path in the AskFor dispatch loop (net send), i.e. enqueue of
// the request, NOT the socket write and NOT the peer's receipt.
void RecordGetDataSent(const uint256& hash, int peer, bool fRetry);
void RecordBlockReceived(const uint256& hash, int peer, uint64_t bytes);
void RecordBlockAccepted(const uint256& hash, int64_t height,
                         int64_t writeDiskMs, int64_t addIndexMs);
void RecordAnnounce(const uint256& hash, int peer);

// Wire-stage marker: the getdata message carrying `hash` was ACTUALLY written
// to the peer's socket — fired in the socket-send path when the frame's first
// byte returns from ::send() (>0 bytes), not merely enqueued into vSendMsg.
// Correlates with getdata_sent (enqueue) to measure the per-request
// enqueue->wire queue delay on the SAME node (skew-free). Cross-node
// correlation by hash + per-node peer id + connection id; absolute deltas
// between nodes carry the node clock-skew (~634 ms measured).
void RecordGetDataWire(const uint256& hash, int peer);
// (receiving node) an incoming getdata message carrying `hash` was parsed.
void RecordGetDataReceived(const uint256& hash, int peer);
// (serving node) the block `hash` was placed on the outgoing queue (PushMessage
// "block") in response to a block getdata, then sent.
void RecordBlockServed(const uint256& hash, int peer);

} // namespace p2pprop

#endif // INNOVA_P2PPROP_H