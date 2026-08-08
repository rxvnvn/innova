#!/usr/bin/env python3
"""Reconstruct per-hash block-request lifecycles from a raw daemon debug.log.

The node's per-hash instrumentation (ibdforensic CSV, ibdblocklatency CSV) was
NOT enabled in the run under audit, so this tool replays the streams that were
captured and re-derives the exact state transitions that produced:

    DUPLICATE_INDEXED        -- a block received after it is already in the
                                block index (re-announcement / re-request /
                                reissue of a hash already processed)
    funnel_unsolicited       -- ibdblocklatency receives with no active
                                request->receive record (no askfor that
                                survived to a getdata; after a timeout evicts
                                the request, a late arrival counts here)
    global cap exceedance    -- per-second pipeline saturation: the 512
                                admission cap vs the raw queued+inflight gauge
    getblocks_outstanding    -- getblocks requests queued/outstanding but not
                                yet consumed by a response

Streams parsed (line-keyed, timestamp-ordered):
    sync: ProcessBlock <20-hex> from <ip:port> (height N)   -- block arrivals
    PBREJECT ... reason=... hash=<64-hex> peer=...          -- reject outcomes
    ERROR: ProcessBlock() : already have block ...          -- duplicate index
    IBD_BLOCKLAT_1S ... funnel_* outcome_*                  -- receive/result
    IBD_ACTIVE_1S  ... per-second pipeline gauges           -- cap/getblocks

Usage:
    python3 trace_hash_lifecycle.py <debug.log.gz|debug.log> [--json OUT]
"""

import argparse
import collections
import gzip
import json
import re
import sys

# ---------------------------------------------------------------------------
# Line parsers
# ---------------------------------------------------------------------------

SYNC_RE = re.compile(r"^(\d\d/\d\d/\d\d) (\d\d:\d\d:\d\d) sync: ProcessBlock ([0-9a-f]{20}) from ([0-9a-f.:]+) \(height (-?\d+)\)")
PBREJECT_RE = re.compile(r"^(\d\d/\d\d/\d\d) (\d\d:\d\d:\d\d) PBREJECT (?:time_us=\d+ )?.*? hash=([0-9a-f]{64}) prev=([0-9a-f]{64}) peer=(-?\d+) reason=(\w+)( .*)?$")
ALREADY_RE = re.compile(r"already have block(?: \(orphan\))?(?: \d+)? ([0-9a-f]{20})")
BLOCKLAT_RE = re.compile(r"^(\d\d/\d\d/\d\d) (\d\d:\d\d:\d\d) IBD_BLOCKLAT_1S ")
ACTIVE_RE = re.compile(r"^(\d\d/\d\d/\d\d) (\d\d:\d\d:\d\d) IBD_ACTIVE_1S ")

def kv(s):
    out = {}
    for m in re.finditer(r"([a-zA-Z0-9_]+)=([^ \n]+)", s):
        out[m.group(1)] = m.group(2)
    return out

def wallclock_to_seconds(d, t):
    """Monotonic-ish index: seconds since log start for (dd/mm/yy HH:MM:SS)."""
    hh, mm, ss = (int(x) for x in t.split(":"))
    return hh * 3600 + mm * 60 + ss


class LifecycleLog(object):
    def __init__(self):
        self.t0 = -1
        self.arrivals = []          # (t, peer_str, short, height)
        self.rejects = []           # dict per PBREJECT line
        self.already = []           # (t, short)
        self.funnel = []            # (t, kv dict)
        self.active = []            # (t, kv dict)


def parse(path):
    lg = LifecycleLog()
    open_ = gzip.open if path.endswith(".gz") else open
    with open_(path, "rt", errors="replace") as f:
        for line in f:
            m = SYNC_RE.match(line)
            if m:
                t = wallclock_to_seconds(m.group(1), m.group(2))
                if lg.t0 < 0:
                    lg.t0 = t
                lg.arrivals.append((t - lg.t0, m.group(4), m.group(3), int(m.group(5))))
                continue
            m = PBREJECT_RE.match(line)
            if m:
                t = wallclock_to_seconds(m.group(1), m.group(2))
                if lg.t0 < 0:
                    lg.t0 = t
                d = kv(m.group(4) or "")
                d["time"] = t - lg.t0
                d["hash"] = m.group(3)
                d["peer"] = int(m.group(5))
                d["reason"] = m.group(6)
                lg.rejects.append(d)
                continue
            m = BLOCKLAT_RE.match(line)
            if m:
                t = wallclock_to_seconds(m.group(1), m.group(2))
                if lg.t0 < 0:
                    lg.t0 = t
                lg.funnel.append((t - lg.t0, kv(line)))
                continue
            m = ACTIVE_RE.match(line)
            if m:
                t = wallclock_to_seconds(m.group(1), m.group(2))
                if lg.t0 < 0:
                    lg.t0 = t
                lg.active.append((t - lg.t0, kv(line)))
                continue
            m = ALREADY_RE.search(line)
            if m:
                tm = re.search(r"(\d\d:\d\d:\d\d) ERROR", line)
                t = wallclock_to_seconds("01/01/01", tm.group(1)) if tm else 0
                lg.already.append((t - lg.t0, m.group(1)))
    return lg


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--json", default=None)
    args = ap.parse_args()

    lg = parse(args.log)
    print("log_start_t0=%d arrivals=%d pbreject=%d already_have=%d funnel=%d active=%d"
          % (lg.t0, len(lg.arrivals), len(lg.rejects), len(lg.already),
             len(lg.funnel), len(lg.active)))

    # ---------------- 1. DUPLICATE_INDEXED origin -------------------------
    reasons = collections.Counter(r["reason"] for r in lg.rejects)
    print("\n== PBREJECT by reason ==")
    for k, v in reasons.most_common():
        print("  %-28s %d" % (k, v))

    dup = [r for r in lg.rejects if r["reason"] == "DUPLICATE_INDEXED"]
    print("\nDUPLICATE_INDEXED total:", len(dup))
    dup_by_peer = collections.Counter(r["peer"] for r in dup)
    print("DUPLICATE_INDEXED by peer (top):")
    for k, v in dup_by_peer.most_common(8):
        print("  peer %-4s %d" % (k, v))

    # repeated DUPLICATE_INDEXED per hash
    dup_hash = collections.Counter(r["hash"] for r in dup)
    repeat = {h: c for h, c in dup_hash.items() if c > 1}
    print("hashes DUPLICATE_INDEXED more than once:", len(repeat),
          "of", len(dup_hash), "distinct; total events:", sum(repeat.values()))
    top_repeat = sorted(repeat.items(), key=lambda x: -x[1])[:5]
    print("top repeated:")
    for h, c in top_repeat:
        evs = [r for r in dup if r["hash"] == h]
        t0 = min(r["time"] for r in evs)
        t1 = max(r["time"] for r in evs)
        print("  %s x%d  t=%d..%d  peers=%s" % (h, c, t0, t1,
              ",".join(str(r["peer"]) for r in evs)))

    # For a sample of DUPLICATE_INDEXED hashes, count arrivals (by 20-hex prefix)
    sample_hashes = [h for h, c in repeat.items()] or [h for h, _ in dup_hash.most_common(20)]
    print("\nDUPLICATE_INDEXED hash -> arrival reissue chain (20-hex prefix match):")
    for h in sample_hashes[:10]:
        short = h[:20]
        arr = [(t, p) for (t, p, s, ht) in lg.arrivals if s == short]
        first = dup_hash.get(h, 0)
        print("  %s  dup_idx_events=%d  sync_arrivals=%d  first_arr=%s last_arr=%s"
              % (h, first, len(arr),
                 arr[0] if arr else None, arr[-1] if arr else None))

    # ---------------- 2. funnel_unsolicited --------------------------------
    print("\n== funnel_unsolicited growth ==")
    prev = None
    for t, d in lg.funnel:
        if t % 300 == 0 or t == lg.funnel[-1][0]:
            if prev is not None:
                dt = t - prev[0]
                du = int(d["funnel_unsolicited"]) - int(prev[1]["funnel_unsolicited"])
                dr = int(d["funnel_received"]) - int(prev[1]["funnel_received"])
                dc = int(d["funnel_connected"]) - int(prev[1]["funnel_connected"])
                dah = int(d["outcome_already_have"]) - int(prev[1]["outcome_already_have"])
                print("  t=%-5d unsolicited=%-6d already_have=%-6d received=%-6d connected=%-6d unsolicited_pct=%.1f%%"
                      % (t, du, dah, dr, dc, 100.0 * du / dr if dr else 0))
            prev = (t, d)

    last_f = lg.funnel[-1][1]
    print("\nend-of-run funnel identity check:")
    print("  unsolicited=%s  already_have=%s  dup_indexed+dup_orphan=%d+%d=%d"
          % (last_f["funnel_unsolicited"], last_f["outcome_already_have"],
             reasons.get("DUPLICATE_INDEXED", 0), reasons.get("DUPLICATE_ORPHAN", 0),
             reasons.get("DUPLICATE_INDEXED", 0) + reasons.get("DUPLICATE_ORPHAN", 0)))

    # peer id -> addr map from SYNCPEER lines (log may not contain them; best-effort)
    peer_map = {}
    open2 = gzip.open if args.log.endswith(".gz") else open
    with open2(args.log, "rt", errors="replace") as f:
        for line in f:
            m = re.search(r"#(\d+) ([0-9a-f.:]+)", line)
            if m and line.find("SYNCPEER") >= 0:
                peer_map[int(m.group(1))] = m.group(2)
    if peer_map:
        print("\nDUPLICATE_INDEXED by peer (id -> addr):")
        for k, v in dup_by_peer.most_common(10):
            print("  peer %-4s %-22s %d" % (k, peer_map.get(k, "?"), v))

    # ---------------- 3. global cap saturation ------------------------------
    print("\n== pipeline saturation (free slots / inflight) ==")
    sat_seconds = 0
    last_t = None
    stretches = []
    cur_start = None
    for t, d in lg.active:
        free = int(d.get("global_free_active_slots", "0"))
        if free == 0:
            if cur_start is None:
                cur_start = t
            sat_seconds += 1
        else:
            if cur_start is not None:
                stretches.append((cur_start, t))
                cur_start = None
    if cur_start is not None:
        stretches.append((cur_start, lg.active[-1][0]))
    print("seconds with free slots == 0 (cap pinned):", sat_seconds, "of", len(lg.active))
    print("longest saturation stretches:")
    for a, b in sorted(stretches, key=lambda x: -(x[1] - x[0]))[:6]:
        print("  t=%d..%d len=%d" % (a, b, b - a))

    # transition build-up 2385..2420
    print("transition build-up (t=2385..2420):")
    for t, d in lg.active:
        if 2385 <= t <= 2420:
            print("  t=%-4d deferred=%-6s inflight=%-5s depth=%-5s free=%-4s blocks=%-4s getblocks_1s=%s"
                  % (t, d.get("deferred_peer"), d.get("inflight_peer"),
                     d.get("askfor_depth"), d.get("global_free_active_slots"),
                     d.get("blocks_1s"), d.get("getblocks_sent_1s")))

    # ---------------- 4. getblocks_outstanding ------------------------------
    print("\n== getblocks_outstanding growth ==")
    prev = None
    for t, d in lg.active:
        if t % 300 == 0 or t == lg.active[-1][0]:
            gb = int(d.get("getblocks_outstanding_peer", "0"))
            gq = int(d.get("getblocks_queued_peer", "0"))
            gs = int(d.get("getblocks_sent_1s", "0"))
            mark = " <== grows" if prev is not None and gb > prev[1] + 100 else ""
            print("  t=%-5d outstanding=%-5d queued=%-3d sent_1s=%d%s"
                  % (t, gb, gq, gs, mark))
            prev = (t, gb)

    if args.json:
        out = {
            "arrivals": len(lg.arrivals),
            "rejects": len(lg.rejects),
            "reasons": dict(reasons),
            "dup_total": len(dup),
            "dup_by_peer": dict(dup_by_peer),
            "dup_repeat_hashes": len(repeat),
            "dup_top": [{"hash": h, "count": c} for h, c in top_repeat],
            "sat_seconds": sat_seconds,
        }
        with open(args.json, "w") as f:
            json.dump(out, f, indent=1)


if __name__ == "__main__":
    main()
