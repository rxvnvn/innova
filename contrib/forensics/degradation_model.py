#!/usr/bin/env python3
"""Quantitative degradation model for the IBD block pipeline.

Implements the *existing* architecture (single slow serving peer) as a
discrete-event simulation and attributes every in-flight slot to an origin:

    BULK            block from a getblocks response batch (>=2 items)
    CONTINUATION    block from a 1-item getblocks response (continuation tip)
    RELAY           block from an unsolicited 1-item relay INV
    ORPHAN_PARENT   block asked for as the missing parent of an orphan
                    (AskFor(BLOCKREQ_SOURCE_ORPHAN), main.cpp:7612)
    CROSS_PEER      re-ask after ownership was released to another peer
    OTHER           anything else (initial/empty/continuation responses)

Deliberately NOT an origin here: TIMEOUT_REISSUE.  In the tree there is no
automatic re-ask of a timed-out in-flight hash -- ExpireBlockInFlight frees
the slot and releases ownership (net.h:2165-2236); the block returns only
when a *new* announcement re-lists it (a continuation getblocks batch, a
relay INV, or an orphan-parent walk-back).  A timed-out-then-delivered block
is therefore counted under the source of the request that announced it
again, matching the forensic source tags (BLOCKREQ_SOURCE_INV/ORPHAN/...).

Constants are taken from the tree:
    per-peer window       128   net.h:564 MAX_DEFERRED_INV_ACTIVE_PER_PEER
    global window         512   net.h:566
    in-flight timeout       5s  net.h:2167 BLOCK_IN_FLIGHT_TIMEOUT
    getblocks timeout      15s  net.h:1057 GETBLOCKS_RESPONSE_TIMEOUT
    ask retry delay         1s  net.h:1804 BLOCK_ASK_RETRY_US
    orphan cap/peer       750   main.h:102 MAX_ORPHAN_BLOCKS_PER_PEER
    batch limit          1000   main.cpp:9222
    continuation cooldown  10s  main.cpp:10036 (nLastGetBlocksTime >= 10)

Behavioural rules reproduced:
  * relay INV for every new tip block (AcceptBlock, main.cpp:7092-7103);
  * budget-gated admission for bulk + relay (main.cpp:314-341), ungated
    AskFor for orphan parents (main.cpp:7608-7614);
  * queue drained in due-time order at the in-flight cap (main.cpp:10946);
  * block connects only when its parent is connected; else orphan, and the
    same peer is asked for the missing parent (main.cpp:7545, 7608-7614);
  * orphan-limit rejection at 750/peer: further orphans rejected, parent not
    asked (main.cpp:7554-7570);
  * no automatic re-ask of timed-out blocks (net.h:2165-2236); a pipeline
    drained for >=10s triggers a continuation getblocks that re-lists the
    missing range from the client's best connected tip (main.cpp:10031-10079);
  * server-side suppression of repeated identical getblocks (Evaluate,
    net.cpp:716-745) - simplified to a cooldown that grows with repeats;
  * flaky peer: each getdata response is dropped with probability p_drop
    (the runtime records ~10% of timed-out requests never arriving).
"""

import heapq
import argparse


ORIGINS = ["BULK", "CONTINUATION", "RELAY", "ORPHAN_PARENT",
           "CROSS_PEER", "OTHER"]


class DegradationModel:
    def __init__(self, rtt=13.9, window=128, in_flight_timeout=5.0,
                 getblocks_timeout=15.0, batch=1000,
                 orphan_cap=750, net_block_interval=60.0, tip_height=2000,
                 sim_time=3600.0, bw=100.0, p_drop=0.1, seed=1):
        self.rtt = rtt
        self.window = window
        self.ift = in_flight_timeout
        self.gb_timeout = getblocks_timeout
        self.batch = batch
        self.orphan_cap = orphan_cap
        self.net_interval = net_block_interval
        self.tip = tip_height
        self.sim_time = sim_time
        self.bw = bw                        # server transmit rate, blk/s
        self.p_drop = p_drop                # per-response drop probability
        self.seed = seed

        import random
        self.rnd = random.Random(seed)

        self.t = 0.0
        self.h_conn = 0                     # client connected tip
        self.in_flight = {}                 # height -> (origin, t_send)
        self.req_origin = {}                # height -> last getdata origin
        self.queue = []                     # heap (due, height, origin)
        self.deferred = []                  # deferred bulk/relay
        self.orphans = {}                   # height -> True (not connected)
        self.have = set()                   # heights received
        self.orphan_count = 0
        self.orphan_limit_events = 0
        self.rejected = set()               # rejected while orphan cap full
        self.lost_blocks = set()            # never arrived
        self.timeouts = 0

        # server-side transmission (bandwidth bottleneck, optional drops)
        self.server_busy_until = 0.0
        self.wire = []                      # heap (arrival_t, height)

        # getblocks cycle
        self.gb_outstanding = False
        self.gb_sent_at = None
        self.gb_response_at = None
        self.gb_pending = None

        # server-side suppression state (simplified Evaluate)
        self.server_cooldown_until = 0.0
        self.server_repeats = 0
        self.server_last_resolved = -1

        # next relay event
        self.next_relay_at = self.net_interval
        self.last_getblocks = self.t

        # getblocks response accounting
        self.gb_sent_total = 0
        self.gb_resp_1item = 0
        self.gb_resp_bulk = 0
        self.gb_resp_empty = 0
        self.gb_suppressed = 0

        # origin accounting: slot-seconds and request counts
        self.slot_time = {o: 0.0 for o in ORIGINS}
        self.req_count = {o: 0 for o in ORIGINS}
        self.arrived = {o: 0 for o in ORIGINS}
        self.connects = {o: 0 for o in ORIGINS}
        self.orphaned = {o: 0 for o in ORIGINS}
        self.last_bulk_slot = None

        # timeline of key events
        self.events = []
        self.trace_interval = 300.0
        self.trace = []                      # (t, h_conn, gap, n_orphans)

    # ---- helpers -----------------------------------------------------

    def log(self, what):
        self.events.append((self.t, what))

    def locator(self):
        return self.h_conn

    def budget(self):
        return max(0, self.window - (len(self.queue) + len(self.in_flight)))

    def ask(self, height, origin):
        """AskFor a block.  Origin-tagged; due time depends on origin."""
        if height <= self.h_conn or height in self.have:
            return
        if height in self.rejected:
            return
        if height in self.in_flight:
            return
        for (_, h, o) in self.queue:
            if h == height:
                return
        if origin in ("ORPHAN_PARENT", "CROSS_PEER", "OTHER"):
            # ungated AskFor
            due = self.t
        else:
            # bulk/relay: budget-gated admission (defer if no budget)
            if self.budget() <= 0:
                self.deferred.append((height, origin))
                return
            due = self.t
        heapq.heappush(self.queue, (due, height, origin))
        self.req_count[origin] += 1

    def send_pass(self):
        """getdata pass: drain due queue up to in-flight cap."""
        while (self.in_flight.__len__() < self.window and self.queue):
            due, height, origin = self.queue[0]
            if due > self.t:
                break
            heapq.heappop(self.queue)
            if height in self.in_flight:
                continue
            if height <= self.h_conn or height in self.have:
                continue
            self.in_flight[height] = (origin, self.t)
            self.req_origin[height] = origin
            if origin == "BULK":
                self.last_bulk_slot = self.t
            self._server_getdata(height)
        # re-admit deferred bulk/relay as budget allows
        while self.budget() > 0 and self.deferred:
            h, o = self.deferred.pop(0)
            if h in self.in_flight or h <= self.h_conn or h in self.have:
                continue
            heapq.heappush(self.queue, (self.t, h, o))
            self.req_count[o] += 1

    def _server_getdata(self, height):
        """Client sent a getdata; server queues a transmission."""
        t_srv = max(self.server_busy_until, self.t) + 1.0 / self.bw
        self.server_busy_until = t_srv
        if self.rnd.random() < self.p_drop:
            # response dropped: block never arrives; client has a gap
            if height > self.h_conn:
                self.lost_blocks.add(height)
            return
        heapq.heappush(self.wire, (t_srv + self.rtt, height))

    # ---- server side ------------------------------------------------

    def process_getblocks(self, source):
        """Client sends getblocks(locator).  Server decides a response."""
        L = self.locator()
        gap = self.tip - L
        # suppression: identical locator, no progress, cooldown active
        if L == self.server_last_resolved:
            self.server_repeats += 1
        else:
            self.server_repeats = 0
        self.server_last_resolved = L
        cooldown = 2.0 * (2 ** min(self.server_repeats // 16, 4))
        if self.server_repeats >= 2 and self.t < self.server_cooldown_until:
            self.server_cooldown_until = max(self.server_cooldown_until,
                                             self.t + cooldown)
            self.gb_suppressed += 1
            return [], "OTHER"                       # suppressed: no INV
        self.server_cooldown_until = self.t + 0.0
        if gap <= 0:
            return [], "OTHER"                       # empty (silent)
        n = min(self.batch, gap)
        # one item -> continuation/tip response; >=2 -> bulk
        return list(range(L + 1, L + 1 + n)), ("CONTINUATION" if n == 1
                                               else "BULK")

    # ---- slot accounting ---------------------------------------------

    def _free_slot(self, height):
        """Account slot-seconds for an in-flight request when it frees."""
        entry = self.in_flight.pop(height, None)
        if entry is not None:
            self.slot_time[entry[0]] += self.t - entry[1]

    # ---- receive ----------------------------------------------------

    def receive(self, height, origin):
        """A requested block arrived.  Try to connect, else orphan."""
        self.arrived[origin] += 1
        if height <= self.h_conn or height in self.have:
            return                                 # duplicate
        self.have.add(height)
        self.lost_blocks.discard(height)
        # connectable if parent is connected and it is the next block
        if height == self.h_conn + 1:
            self.h_conn += 1
            self.connects[origin] += 1
            self._try_flush()
            return
        # else orphan.  Cap is on *current* orphan storage; while the cap is
        # saturated further orphans are rejected and their parent is not
        # asked (main.cpp:7554-7570).  Rejected heights are re-listed later
        # by the next announcement once storage drops below the cap.
        if len(self.orphans) >= self.orphan_cap:
            self.orphan_limit_events += 1
            self.log("ORPHAN_LIMIT_IBD")
            # rejected: block is forgotten, not stored as an orphan, and its
            # parent is not asked.  A rejected height may be re-listed later
            # by the next announcement once storage drops below the cap; it
            # must NOT stay in `have` or the re-ask is suppressed.
            self.rejected.add(height)
            self.have.discard(height)
            self.lost_blocks.add(height)
            return
        self.orphans[height] = True
        self.orphan_count += 1
        self.orphaned[origin] += 1
        parent = height - 1
        if parent > self.h_conn and parent not in self.have and \
           parent not in self.in_flight:
            self.ask(parent, "ORPHAN_PARENT")
            self.last_orphan_ask = self.t

    def _try_flush(self):
        """Connect any orphans that are now the next block."""
        while self.h_conn + 1 in self.orphans:
            self.h_conn += 1
            del self.orphans[self.h_conn]
            self.orphan_count -= 1
        # orphan storage below cap again: rejected blocks may be re-listed
        if self.rejected and len(self.orphans) < self.orphan_cap:
            self.rejected = {h for h in self.rejected if h > self.h_conn}
            for h in list(self.rejected):
                if h in self.have or h in self.in_flight:
                    self.rejected.discard(h)
                    continue
                self.ask(h, "OTHER")
                self.rejected.discard(h)

    # ---- main loop --------------------------------------------------

    def run(self):
        self._send_getblocks("INITIAL")
        while self.t < self.sim_time:
            candidates = []
            for height, (origin, t0) in self.in_flight.items():
                candidates.append((t0 + self.ift, "TIMEOUT", height, origin))
            if self.wire:
                t_arr, h = self.wire[0]
                candidates.append((t_arr, "WIRE_ARRIVE", h, None))
            if self.gb_outstanding and self.gb_sent_at is not None:
                candidates.append((self.gb_sent_at + self.gb_timeout,
                                   "GB_TIMEOUT", None, None))
            if self.gb_response_at is not None:
                candidates.append((self.gb_response_at, "GB_RESPONSE",
                                   None, None))
            candidates.append((self.next_relay_at, "RELAY", None, None))
            # pipeline-drained continuation (>=10s idle)
            if not self.gb_outstanding and len(self.in_flight) <= 1 \
                    and not self.queue:
                candidates.append((self.t + max(0.0, 10.0 - (self.t - self.last_getblocks)),
                                   "CONTINUE", None, None))
            candidates.sort(key=lambda c: c[0])
            t_next, kind, height, origin = candidates[0]
            dt = max(0.0, t_next - self.t)

            # record gap trajectory at fixed intervals
            while (not self.trace and self.t + dt >= self.trace_interval) or \
                  (self.trace and self.t + dt >= self.trace[-1][0] + self.trace_interval):
                t_samp = (self.trace[-1][0] + self.trace_interval) if self.trace \
                    else self.trace_interval
                self.trace.append((t_samp, self.h_conn, self.tip - self.h_conn,
                                   self.orphan_count))

            self.t = t_next

            if kind == "WIRE_ARRIVE":
                t_arr, h = heapq.heappop(self.wire)
                if h <= self.h_conn or h in self.have:
                    continue
                o = self.req_origin.pop(h, "OTHER")
                self._free_slot(h)
                self.receive(h, o)
            elif kind == "TIMEOUT":
                self._free_slot(height)
                self.timeouts += 1
                # NOT re-asked: the hash must be re-listed by a new
                # announcement (continuation batch, relay, orphan parent).
            elif kind == "GB_RESPONSE":
                self.gb_outstanding = False
                self.gb_response_at = None
                resp_blocks, resp_kind = self.gb_pending or ([], "OTHER")
                self.gb_pending = None
                if not resp_blocks:
                    self.gb_resp_empty += 1
                elif len(resp_blocks) == 1:
                    self.gb_resp_1item += 1
                else:
                    self.gb_resp_bulk += 1
                self._inject_response(resp_blocks, resp_kind)
            elif kind == "GB_TIMEOUT":
                self.gb_outstanding = False
                self.gb_response_at = None
                self.gb_pending = None
                self.log("GETBLOCKS_TIMEOUT")
                self._send_getblocks("CONTINUATION")
            elif kind == "RELAY":
                self.tip += 1
                self.next_relay_at += self.net_interval
                self.ask(self.tip, "RELAY")
            elif kind == "CONTINUE":
                self._send_getblocks("CONTINUATION")
            self.send_pass()

        for h, (o, t0) in self.in_flight.items():
            self.slot_time[o] += max(0.0, self.sim_time - t0)
        self.in_flight.clear()
        return self

    def _send_getblocks(self, source):
        if self.gb_outstanding:
            return
        self.gb_outstanding = True
        self.gb_sent_at = self.t
        self.last_getblocks = self.t
        self.gb_sent_total += 1
        # response computed at send time (server processes on receipt) but
        # arrives at the client after one RTT
        self.gb_pending = self.process_getblocks(source)
        self.gb_response_at = self.t + self.rtt

    def _inject_response(self, blocks, kind):
        if not blocks:
            return
        for h in blocks:
            if h in self.have or h in self.in_flight:
                continue
            self.ask(h, kind)

    def results(self):
        total = sum(self.slot_time.values()) or 1.0
        frac = {o: self.slot_time[o] / total for o in ORIGINS}
        return {
            "h_conn": self.h_conn,
            "h_peer": self.tip,
            "gap": self.tip - self.h_conn,
            "slot_fraction": frac,
            "req_count": dict(self.req_count),
            "arrivals": dict(self.arrived),
            "connects": dict(self.connects),
            "orphaned": dict(self.orphaned),
            "orphan_limit_events": self.orphan_limit_events,
            "timeouts": self.timeouts,
            "lost": len(self.lost_blocks),
            "last_bulk_slot": self.last_bulk_slot,
            "gb": {"total": self.gb_sent_total, "1item": self.gb_resp_1item,
                   "bulk": self.gb_resp_bulk, "empty": self.gb_resp_empty,
                   "suppressed": self.gb_suppressed},
            "events": self.events,
            "trace": self.trace,
        }


def main():
    p = argparse.ArgumentParser()
    p.add_argument("--rtt", type=float, default=13.9)
    p.add_argument("--window", type=int, default=128)
    p.add_argument("--ift", type=float, default=5.0)
    p.add_argument("--net-interval", type=float, default=60.0)
    p.add_argument("--tip", type=int, default=2000)
    p.add_argument("--sim-time", type=float, default=3600.0)
    p.add_argument("--bw", type=float, default=100.0)
    p.add_argument("--p-drop", type=float, default=0.10)
    p.add_argument("--trace", action="store_true",
                   help="print gap trajectory every 300s")
    args = p.parse_args()

    m = DegradationModel(rtt=args.rtt, window=args.window,
                         in_flight_timeout=args.ift,
                         net_block_interval=args.net_interval,
                         tip_height=args.tip, sim_time=args.sim_time,
                         bw=args.bw, p_drop=args.p_drop)
    m.run()
    r = m.results()

    print("== pipeline occupancy by origin (slot-seconds) ==")
    for o in ORIGINS:
        print(f"  {o:14s} {r['slot_fraction'][o]*100:6.1f}%  "
              f"req={r['req_count'][o]:6d} arr={r['arrivals'][o]:6d} "
              f"conn={r['connects'][o]:6d} orph={r['orphaned'][o]:6d}")
    print(f"connected tip={r['h_conn']} peer tip={r['h_peer']} gap={r['gap']}")
    print(f"orphan_limit_events={r['orphan_limit_events']} "
          f"timeouts={r['timeouts']} lost={r['lost']}")
    gb = r["gb"]
    print(f"getblocks: total={gb['total']} 1item={gb['1item']} "
          f"bulk={gb['bulk']} empty={gb['empty']} suppressed={gb['suppressed']}")
    print(f"last bulk slot admitted at t={r['last_bulk_slot']}")
    if args.trace:
        print("== gap trajectory (t, h_conn, gap, orphans) ==")
        for t_samp, hc, gap, no in r["trace"]:
            print(f"  t={t_samp:7.0f}  h_conn={hc:6d}  gap={gap:6d}  orphans={no:5d}")


if __name__ == "__main__":
    main()
