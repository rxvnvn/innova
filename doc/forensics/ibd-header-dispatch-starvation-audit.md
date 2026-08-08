# IBD header dispatch starvation audit

## Status and verdict

**STAGE 1b IMPLEMENTED / RUNTIME GATE FAILED**

Reference tree: `10f11ba47e6d3156784e6c73667b3b00958adde6`.
Capture: `runtime-artifacts/ibd-headers-observe-20260808T130500Z`.

# PRIORITY DISPATCH

Expected IBD-observer/scheduler `headers` responses require bounded priority at
both shared FIFO boundaries: the responder's outbound `vSendMsg` and the
requester's inbound `vRecvMsg`. This is narrower than a dedicated queue/thread
and keeps graph mutation on the existing message-handler thread under
`cs_main`. Stage 2 remains blocked until an isolated implementation passes the
runtime gate below. This audit changes no production dispatch or selection.

## Evidence correction

The source line `net: to ... headers` is not a wire timestamp.
`CNode::BeginMessage()` prints its prefix and `EndMessage()` prints its size
before appending the serialization to `vSendMsg` (`src/net.h`). Nonblocking
writes occur later in `SocketSendData()` (`src/net.cpp`). The capture had no
per-header source socket stamp.

Thus six responses were serialized and queued; only two are proven dispatched
at the receiver. The remaining delay cannot retrospectively be split between
the source send FIFO and receiver receive FIFO. Both are strict FIFO shared
with blocks, so either can violate the required control-plane bound.

## Reconstructed path

1. `ProcessMessage(getheaders)` calls `PushMessage("headers", vHeaders)`.
2. `EndMessage()` appends under `cs_vSend`; optimistic write occurs only when
   the new entry is the queue head.
3. `SocketSendData()` writes from the head and never skips bulk data.
4. The requester calls `recv()`/`ReceiveMsgBytes()` under `cs_vRecvMsg`.
   Completion sets `CNetMessage::nTime` and leaves the message in `vRecvMsg`.
5. The single `ThreadMessageHandler` visits each peer under `cs_vRecvMsg`.
6. `ProcessMessages()` services `vRecvGetData` first, selects only
   `vRecvMsg.begin()`, processes one complete message, and returns.
7. `ProcessMessage(headers)` deserializes, acquires `cs_main`, updates the
   anchor/graph/window, and queues continuation `getheaders`.

## Capture measurements

The source queued 2,000-header responses at 10:27:56, 10:27:57, 10:28:39,
10:29:39, 10:30:39, and 10:31:39 UTC. The requester dispatched responses at
10:27:57 and 10:28:39, advancing 0 -> 2,000 -> 4,000. No later response reached
the handler before shutdown at 10:31:47. The first continuation's aggregate
queue-to-handler delay was about 42 seconds.

Requester block telemetry localizes its large component. `CNetMessage::nTime`
is the complete-frame time; `RecordBlockDispatchDelay()` measures from there to
selection. During starvation:

- maximum complete-frame-to-block-dispatch delay reached 146.420243 seconds
  (the earlier 104 seconds was an interim maximum);
- average delay reached 50.877753 seconds;
- average `ProcessBlock` time reached 0.913718 seconds;
- `cs_main` wait averaged 0 microseconds and peaked at 15 microseconds;
- block in-flight stayed around 118--128.

Complete messages therefore wait before `ProcessMessage(block)` attempts
`cs_main`. Nearly one second of validation/connection per FIFO head limits the
one-message drain. `cs_main` contention is excluded. The two selected header
batches completed graph logging within their dispatch second; graph work is not
implicated.

| Stage | Evidence | Classification |
|---|---|---|
| source serialization/queue | exact enqueue log | observed |
| source socket first/last byte | no header stamp | unmeasured split |
| requester framing | no header frame event | unmeasured split |
| complete-message receive queue | block frames wait up to 146.4 s | causal |
| message selection | one FIFO head per peer visit | causal |
| `cs_main` wait | 0 us average, 15 us maximum | excluded |
| block validation | ~0.35 s rising to ~0.91 s each | drain limiter |
| graph insertion | same-second completion | not causal |

## FIFO causality

Inbound dispatch is strict per-peer FIFO, stops at the first incomplete frame,
and processes one complete message per call. Outbound is also strict FIFO:
`EndMessage()` appends and `SocketSendData()` writes from the head. There is no
command priority.

**Yes: a sustained stream of block messages can indefinitely delay solicited
headers control-plane progress.** Continuous production faster than the single
handler drains it gives no finite bound for a later header.

Complete P2P messages generally have no application-level transport-order
dependency. Handshake ordering and `vRecvGetData` response ordering are not
made priority-eligible.

## Alternatives

- **Message priority — selected.** It addresses both proven FIFO boundaries.
  Only bounded solicited IBD header responses qualify.
- **Dedicated control queue — rejected.** It adds ownership, locking, cleanup,
  and lifetime state and still does not solve outbound FIFO by itself.
- **Budgeted dispatch — insufficient alone.** A budget cannot reach through
  hundreds of FIFO blocks. Priority includes a budget against reverse
  starvation.
- **Separate thread — rejected.** It adds lock/lifetime hazards although graph
  mutation must still serialize under `cs_main`; no header CPU bottleneck exists.

## Exact mechanism

Inbound, before `ProcessGetData()` and ordinary FIFO selection, scan a bounded
complete part of `vRecvMsg` for the first eligible `headers`. Eligibility needs:

1. observer/later-select mode, full-node IBD, not SPV;
2. exactly one outstanding header request for this peer;
3. complete valid frame, valid checksum, and existing size/count limits;
4. no priority response already processed for that peer this handler pass.

Dispatch one through the existing headers branch and erase that exact element.
If none is eligible, preserve FIFO. Unsolicited and SPV headers remain on the
existing path.

Outbound, mark a `headers` response created by `getheaders` in
`SendMessageMeta`. Insert it after any partially written head and earlier
control response, but before not-yet-started bulk blocks. Never interleave bytes
of framed messages; keep `vSendMsg`/`vSendMeta` aligned.

### Bounded-work/DoS rule

- one prioritized inbound response per peer/message-handler pass;
- one outstanding header request per peer;
- existing maximum 2,000 headers and serialized-message bound;
- scan at most 256 complete messages or 16 MiB per pass, retaining a bounded
  cursor for a later pass;
- one outbound control response may bypass bulk per peer/pass;
- malformed/unexpected messages gain no additional priority opportunity;
- continuation follows consumption of the expected generation, preventing
  simultaneous stale-retry generations.

The initial guarantee is in handler opportunities: an eligible response inside
the scan bound is selected on the next peer visit before another block. Runtime
measurement must establish wall-clock p99/max before claiming a time threshold.

## Locking contract

- Queue inspection/reordering stays under `cs_vRecvMsg`/`cs_vSend`.
- Never acquire `cs_main` in `SocketHandler`; add no queue mutex.
- Eligibility uses bounded peer request-generation metadata readable without
  taking `cs_main` while `cs_vRecvMsg` is held; the headers branch verifies and
  consumes it under `cs_main`.
- Headers handling stays on `ThreadMessageHandler`; graph/anchor mutation stays
  under `cs_main`.
- No `mapBlockIndex`, `ProcessBlock`, AskFor, ownership, admission, or block
  request state is touched.
- Prefer extracting the chosen complete message under `cs_vRecvMsg`, releasing
  that lock, then dispatching if all callers can adopt that contract. Otherwise
  retain current scope with lock-free eligibility metadata and no new nesting.

## Source-level implementation map

- `src/net.h`: priority/generation `SendMessageMeta` and bounded scan state.
- `src/net.cpp`: stable outbound insertion, metadata alignment, source socket
  telemetry, cleanup.
- `src/main.cpp`: bounded expected-header selection/erase in `ProcessMessages`,
  mark `getheaders` responses, retain existing headers body/`cs_main`.
- `src/main.h`: narrow helpers for tests if needed.
- `src/test/p2p_sync_tests.cpp`: queue, bounds, SPV, malformed, cleanup,
  ordering, and no-request-mutation tests.
- `src/ibdheaderscheduler.*`: no graph change expected; at most a request
  generation accessor.

Scheduler selection, ownership, expiry, `ProcessBlock`, consensus, orphan
handling, wallet/staking, and wire serialization stay untouched.

## Required tests

1. expected headers behind 1, 256, and more-than-scan-bound blocks;
2. bounded cursor eventually reaches the response;
3. at most one priority response per peer/pass;
4. incomplete/bad-checksum headers never qualify;
5. unsolicited full-node and SPV headers retain current behavior;
6. outbound priority never interleaves a partially written message;
7. `vSendMsg`/`vSendMeta` alignment through partial send/erase/disconnect;
8. `vRecvGetData` and ordinary block response ordering stay correct;
9. duplicate continuation generations are impossible;
10. zero AskFor/owner/in-flight/admission/selection mutation;
11. mixed peers cannot consume another peer's expectation;
12. malformed/flooding headers cannot starve blocks.

## Runtime telemetry and gate

Gated `IBD_HEADER_DISPATCH` events carry peer, request generation, response
sequence, bytes, queue messages/bytes ahead, and timestamps:

- `response_queued` (`EndMessage`);
- `socket_first_byte`/`socket_last_byte` (`SocketSendData`);
- `frame_complete` (`ReceiveMsgBytes`);
- `priority_selected` and `dispatch_begin`;
- `cs_main_wait_begin/acquired` in headers handling;
- `graph_insert_complete`.

Report source queue, complete-frame receive queue, lock wait, and graph work
separately. Cross-host wire time needs synchronized clocks or packet capture.
Count scan-bound hits, bypassed messages/bytes, malformed candidates, and block
throughput/dispatch impact.

Repeat the saturated observation capture. Pass requires multiple processed
continuations, no FIFO-induced same-generation retries, bounded eligible
frame-to-dispatch latency, no material healthy throughput loss, and zero
observer-originated block request decisions. Stage 2 remains blocked until then.

## Control/data-plane invariant

**CONTROL PLANE:** getheaders lifecycle, expected headers ingestion, provisional
graph/branch, anchor, continuation locator, and frontier metadata.

**DATA PLANE:** block receive, ownership, `ProcessBlock`, orphan handling,
database work, and active-chain connection.

> During IBD, saturation of either outbound or inbound block-data queues must
> not indefinitely delay a bounded, solicited ordered-chain control-plane
> response. Control priority is bounded in turn and cannot indefinitely delay
> block-data progress.

## Stage 1b implementation and repeated runtime gate (2026-08-08)

The bounded outbound/inbound priority queues and extraction-before-dispatch lock
scope were implemented and tested. `cs_vRecvMsg` now protects complete-frame
selection, move, and erase only; the single message-handler thread releases it
before checksum/command dispatch and `ProcessBlock`. No `cs_main`, ownership,
AskFor, admission, timeout, orphan, or request-selection behavior moved.

The repeated saturated capture is preserved at
`runtime-artifacts/ibd-header-priority-refactor-20260808T123415Z`. It proved the
receive-lock starvation diagnosis and advanced through eight dispatched 2,000
header rounds to graph height 16,000 (connected anchor height 7), with zero
duplicates, disconnected headers, or quarantined headers. All eight receives
and connects known to the graph were `IN_PREDICTED_WINDOW`. The initial 128
block requests were `UNKNOWN_TO_GRAPH`; after graph population, 133 requests
were `IN_PREDICTED_WINDOW`.

Outbound enqueue-to-first-write latency was bounded (median 336 us, maximum
356 us). Inbound complete-frame-to-priority-selection was not: its eight values
grew from 33 ms through 0.321, 2.054, 5.024, 9.402, 15.219, 22.475, to 31.616
seconds (median about 7.21 s). Graph insertion for a 2,000-header batch also
grew to about 10.4 s, while sampled `ProcessBlock` work grew to about 12.4 s.
Extraction frees SocketHandler to frame during data-plane work, but a single
message handler still cannot dispatch a framed control response until the
already-running command completes.

Therefore the runtime gate remains **FAIL** under the task rule that tens-of-
seconds header stalls block Stage 2. Priority dispatch is necessary but is not
sufficient to guarantee the required wall-clock control-plane bound. No Stage
2 request selection was implemented. Mixed-peer graph contribution was not
proven in this short capture: additional peers connected, but peer 2 supplied
all observed header rounds.
