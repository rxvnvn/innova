#!/usr/bin/env python3
"""Standalone streaming analyzer for FIRSTORDER + BLOCKREQTRACE events.

Usage:
    python3 analyze_first_order_break.py <debug.log> [--output-dir <dir>]

Reads the debug log line-by-line (safe for multi-GB files), identifies the
earliest child-before-parent order break, reconstructs the ancestry chain,
cross-correlates all trace events, and writes human/machine output.
"""

import argparse
import json
import os
import re
import sys


# ---------------------------------------------------------------------------
#  Trace line classification
# ---------------------------------------------------------------------------
# A leading timestamp (e.g. "2024-01-01 12:00:00") is optional; the actual
# structured data begins with BLOCKREQTRACE or FIRSTORDER.

_RE_PREFIX = re.compile(
    r'^.*?\b(BLOCKREQTRACE|FIRSTORDER)\s+'
)

# Once the prefix is extracted, the remainder is space-separated key=value
# tokens.  Values may be bare words, integers, hex hashes, or quoted strings.
_RE_KV = re.compile(
    r'(?P<key>[a-zA-Z_][a-zA-Z_0-9]*)=(?P<value>\S+)'
)

# Result-code mapping from the C++ enum
RESULT_NAMES = {
    0: "UNKNOWN",
    1: "ACCEPTED_ACTIVE",
    2: "ACCEPTED_INDEXED",
    3: "ORPHAN_NEW",
    4: "ALREADY_KNOWN",
    5: "ORPHAN_DUPLICATE",
    6: "REJECTED",
    7: "ORPHAN_LIMIT_IBD",
    8: "ACCEPT_FAILED",
    9: "TRUE_UNINDEXED",
}


def parse_kv_pairs(text):
    """Return dict of key→value for all key=value tokens in *text*."""
    d = {}
    for m in _RE_KV.finditer(text):
        d[m.group("key")] = m.group("value")
    return d


def classify_line(line):
    """If *line* contains a trace event, return (prefix, event_type, kv_dict).

    Otherwise return None.
    """
    m = _RE_PREFIX.match(line)
    if not m:
        return None
    prefix = m.group(1)                # "BLOCKREQTRACE" or "FIRSTORDER"
    after = line[m.end():]
    kv = parse_kv_pairs(after)
    event = kv.get("event", "UNKNOWN")
    return prefix, event, kv


# ---------------------------------------------------------------------------
#  Result-code helpers
# ---------------------------------------------------------------------------

def is_orphan_result(code_str):
    """Return True when the result_code indicates an orphan event."""
    try:
        code = int(code_str)
    except (ValueError, TypeError):
        return False
    return code in (3, 7)  # ORPHAN_NEW, ORPHAN_LIMIT_IBD


def result_name(code_str):
    try:
        return RESULT_NAMES.get(int(code_str), f"UNKNOWN_{code_str}")
    except (ValueError, TypeError):
        return str(code_str)


# ---------------------------------------------------------------------------
#  Event store
# ---------------------------------------------------------------------------

class EventStore:
    """Holds all parsed events organised by block hash and by time."""

    def __init__(self):
        # hash (lowercase 64-char hex) → list of event-dicts (sorted by time_us)
        self.by_hash = {}
        # all events in chronological order (time_us ascending)
        self.timeline = []
        # unique hashes seen in any BLOCK_RECEIVE event
        self.received_hashes = set()

    def add(self, prefix, event_type, kv):
        t_us = int(kv.get("time_us", 0))
        entry = {
            "prefix": prefix,
            "event": event_type,
            "time_us": t_us,
            "kv": kv,
        }
        self.timeline.append(entry)

        # Index by block hash when the event carries one.
        h = kv.get("hash", "").strip()
        if h and re.match(r'^[0-9a-f]{64}$', h):
            self.by_hash.setdefault(h, []).append(entry)
            if event_type == "BLOCK_RECEIVE":
                self.received_hashes.add(h)

    def sort(self):
        self.timeline.sort(key=lambda e: e["time_us"])
        for lst in self.by_hash.values():
            lst.sort(key=lambda e: e["time_us"])

    def events_for_hash(self, h):
        return self.by_hash.get(h, [])

    def first_block_receive(self, h):
        """Return earliest FIRSTORDER BLOCK_RECEIVE for *h*, or None."""
        for e in self.events_for_hash(h):
            if e["prefix"] == "FIRSTORDER" and e["event"] == "BLOCK_RECEIVE":
                return e
        return None


# ---------------------------------------------------------------------------
#  First-order-break detection
# ---------------------------------------------------------------------------

def find_first_break(store):
    """Return the FIRSTORDER BLOCK_RECEIVE event that constitutes the earliest
    first-order break, or None.

    Criteria (in chronological order):
      • event is FIRSTORDER BLOCK_RECEIVE
      • prev_in_index is "0"
      • prev_in_orphans is "0"
      • result_code is an orphan code
    """
    candidate = None
    for e in store.timeline:
        if e["prefix"] != "FIRSTORDER" or e["event"] != "BLOCK_RECEIVE":
            continue
        kv = e["kv"]
        if kv.get("prev_in_index") != "0":
            continue
        if kv.get("prev_in_orphans") != "0":
            continue
        rc = kv.get("result_code", "")
        if not is_orphan_result(rc):
            continue
        candidate = e
        break  # earliest match
    return candidate


# ---------------------------------------------------------------------------
#  Ancestry reconstruction
# ---------------------------------------------------------------------------

def reconstruct_chain(store, break_event):
    """Walk the ``prev`` field backward from the break block, using
    FIRSTORDER BLOCK_RECEIVE records to build the ancestry list.

    Returns [(hash, receive_event_or_None), …] from break-block (index 0) up
    to the earliest reachable ancestor.
    """
    chain = []
    seen = set()
    cur_hash = break_event["kv"].get("hash", "").strip()
    while cur_hash and cur_hash not in seen:
        seen.add(cur_hash)
        ev = store.first_block_receive(cur_hash)
        chain.append((cur_hash, ev))
        if ev is None:
            break
        nxt = ev["kv"].get("prev", "").strip()
        if nxt == "0000000000000000000000000000000000000000000000000000000000000000":
            break
        cur_hash = nxt
    return chain


# ---------------------------------------------------------------------------
#  Classification
# ---------------------------------------------------------------------------

def classify_break(store, break_event, chain):
    """Classify the root cause of the first-order break."""

    child_hash = break_event["kv"]["hash"]
    prev_hash = break_event["kv"].get("prev", "").strip()

    # Gather all events for the parent hash
    parent_events = store.events_for_hash(prev_hash)
    child_events = store.events_for_hash(child_hash)

    # Check if any BLOCK_RECEIVE for the parent exists at all
    parent_receives = [e for e in parent_events
                       if e["event"] == "BLOCK_RECEIVE"]
    child_receives = [e for e in child_events
                      if e["event"] == "BLOCK_RECEIVE"]

    # 1) Parent never received → check if it was requested
    if not parent_receives:
        parent_ask = [e for e in parent_events if e["event"] == "ASK_SCHEDULE"]
        parent_send = [e for e in parent_events if e["event"] == "GETDATA_SEND"]
        parent_getdata_item = [e for e in parent_events
                               if e["event"] == "GETDATA_ITEM"]

        if not parent_ask and not parent_send and not parent_getdata_item:
            # No record of requesting the parent at all
            child_unsolicited = any(
                e["kv"].get("sender_inflight_before") == "0"
                for e in child_receives
            )
            if child_unsolicited:
                return "UNSOLICITED_CHILD"
            return "PARENT_NOT_REQUESTED"

        parent_queued_not_sent = False
        for e in parent_events:
            if e["event"] == "ASK_SCHEDULE":
                # A schedule exists without a subsequent GETDATA_SEND
                break
        else:
            parent_queued_not_sent = True

        if parent_ask and not parent_send and not parent_getdata_item:
            return "PARENT_QUEUED_NOT_SENT"
        return "PARENT_SENT_NOT_RECEIVED"

    # 2) Parent was received – check its result
    #    (look at BLOCKREQTRACE BLOCK_RESULT or FIRSTORDER BLOCK_RECEIVE)
    parent_last = parent_receives[-1]
    parent_result = parent_last["kv"].get("result_code",
                    parent_last["kv"].get("result", ""))
    if parent_result:
        if is_orphan_result(str(parent_result)):
            return "PARENT_ALREADY_ORPHAN"
        if int(parent_result) in (6, 8):  # REJECTED, ACCEPT_FAILED
            return "PARENT_RECEIVED_REJECTED"

    # 3) Check for unsolicited child (child not in any inflight for sender)
    child_unsolicited = any(
        e["kv"].get("sender_inflight_before") == "0"
        for e in child_receives
    )
    if child_unsolicited:
        return "UNSOLICITED_CHILD"

    return "UNKNOWN"


# ---------------------------------------------------------------------------
#  Output helpers
# ---------------------------------------------------------------------------

def shorten(h, n=12):
    if not h:
        return "0" * n
    return h[:n]


def fmt_time(t_us):
    return f"{t_us // 1000000}.{t_us % 1000000:06d}"


def event_sort_key(e):
    return e["time_us"]


# ---------------------------------------------------------------------------
#  Human-readable timeline
# ---------------------------------------------------------------------------

def write_timeline(fh, store, chain, break_event, classification):
    fh.write("=" * 78 + "\n")
    fh.write("FIRST-ORDER BREAK ANALYSIS\n")
    fh.write("=" * 78 + "\n\n")

    # Break-event detail
    bk = break_event["kv"]
    fh.write("BREAK EVENT\n")
    fh.write(f"  hash             : {bk['hash']}\n")
    fh.write(f"  prev             : {bk.get('prev', '?' )}\n")
    fh.write(f"  time_us          : {break_event['time_us']}\n")
    fh.write(f"  peer             : {bk.get('peer', '?')}\n")
    fh.write(f"  result_code      : {bk.get('result_code', '?')} "
             f"({result_name(bk.get('result_code', ''))})\n")
    fh.write(f"  prev_in_index    : {bk.get('prev_in_index', '?')}\n")
    fh.write(f"  prev_in_orphans  : {bk.get('prev_in_orphans', '?')}\n")
    fh.write(f"  orphan_before    : {bk.get('orphan_before', '?')}\n")
    fh.write(f"  orphan_after     : {bk.get('orphan_after', '?')}\n")
    fh.write(f"  classification   : {classification}\n\n")

    # Chain summary
    fh.write("ANCESTRY CHAIN\n")
    fh.write(f"{'#':>4}  {'hash':<20}  {'received':>10}  "
             f"{'result':<25}  notes\n")
    fh.write("-" * 78 + "\n")
    for idx, (h, ev) in enumerate(chain):
        if ev:
            t = fmt_time(ev["time_us"])
            rc = result_name(ev["kv"].get("result_code", ""))
            notes = []
            if ev["kv"].get("prev_in_index") == "0":
                notes.append("parent-missing")
            if ev["kv"].get("prev_in_orphans") == "1":
                notes.append("parent-orphan")
            notes_str = ", ".join(notes) if notes else ""
        else:
            t = "  NEVER   "
            rc = "—"
            notes_str = "no receive record"
        fh.write(f"{idx:>4}  {shorten(h, 20):<20}  {t:>10}  "
                 f"{rc:<25}  {notes_str}\n")

    fh.write("\n")

    # Collect all hashes in the chain for timeline filtering
    chain_hashes = {h for h, _ in chain}

    # Timeline – collect relevant events for the chain
    relevant = []
    for e in store.timeline:
        h = e["kv"].get("hash", "").strip()
        if h in chain_hashes:
            relevant.append(e)
        # Also include parent-related events where the hash field may name
        # a different but relevant block (e.g. orphan_child_hash)
        for extra_key in ("parent_hash", "orphan_child_hash",
                          "last_batch_hash", "begin_hash", "stop_hash"):
            v = e["kv"].get(extra_key, "").strip()
            if v in chain_hashes and v != h:
                relevant.append(e)

    relevant.sort(key=event_sort_key)

    fh.write("CHRONOLOGICAL TIMELINE (chain blocks)\n")
    fh.write(f"{'time_us':>12}  {'event':<20}  {'hash':<20}  "
             f"{'peer':>5}  details\n")
    fh.write("-" * 78 + "\n")
    for e in relevant:
        kv = e["kv"]
        h = shorten(kv.get("hash", ""), 20)
        evt = f"{e['prefix']}/{e['event']}"
        peer = kv.get("peer", "?")
        # Build a compact detail string
        detail_parts = []
        if e["event"] == "BLOCK_RECEIVE":
            r = result_name(kv.get("result_code", kv.get("result", "")))
            detail_parts.append(f"result={r}")
            detail_parts.append(f"prev={shorten(kv.get('prev',''),12)}")
        elif e["event"] == "GETDATA_ITEM":
            detail_parts.append(
                f"batch={kv.get('batch_id','?')} "
                f"pos={kv.get('batch_pos','?')}")
            detail_parts.append(f"source={kv.get('source','?')}")
        elif e["event"] == "GETDATA_SEND":
            detail_parts.append(f"path={kv.get('path','?')}")
            detail_parts.append(f"known={kv.get('known_index','?')}")
        elif e["event"] == "OWNER_ASSIGN":
            detail_parts.append(f"state={kv.get('state','?')}")
            detail_parts.append(f"source={kv.get('source','?')}")
        elif e["event"] == "OWNER_RELEASE":
            detail_parts.append(f"reason={kv.get('reason','?')}")
        elif e["event"] == "ASK_SCHEDULE":
            detail_parts.append(f"source={kv.get('source','?')}")
        elif e["event"] == "SERVE_GETDATA":
            detail_parts.append(
                f"found={kv.get('found','?')} "
                f"sent={kv.get('sent','?')}")
        elif e["event"] == "BLOCK_RESULT":
            detail_parts.append(
                f"result={kv.get('result','?')} "
                f"height={kv.get('height','?')}")
        elif e["event"] == "INFLIGHT_CLEAR":
            detail_parts.append(f"reason={kv.get('reason','?')}")
        elif e["event"] == "INFLIGHT_EXPIRE":
            detail_parts.append(f"age={kv.get('age_s','?')}s")
        elif e["event"] == "GETDATA_SKIP":
            detail_parts.append(
                f"owner={kv.get('owner_peer','?')} "
                f"state={kv.get('owner_state','?')}")
        elif e["event"] == "ASK_SKIP":
            detail_parts.append(f"reason={kv.get('reason','?')}")
        elif e["event"] == "ASK_REMOVE":
            detail_parts.append(f"reason={kv.get('reason','?')}")
        detail_str = "  ".join(detail_parts) if detail_parts else ""
        fh.write(f"{e['time_us']:>12}  {evt:<20}  {h:<20}  "
                 f"{peer:>5}  {detail_str}\n")

    fh.write("\n")


# ---------------------------------------------------------------------------
#  Compact table
# ---------------------------------------------------------------------------

def write_table(fh, store, chain, break_event, classification):
    bk = break_event["kv"]
    chain_hashes = {h for h, _ in chain}

    fh.write("=" * 78 + "\n")
    fh.write("COMPACT TABLE\n")
    fh.write("=" * 78 + "\n\n")

    header = (f"{'seq':>6}  {'time_us':>14}  {'event':<22}  "
              f"{'hash':<18}  {'key fields'}")
    fh.write(header + "\n")
    fh.write("-" * 78 + "\n")

    idx = 0
    for e in store.timeline:
        h = e["kv"].get("hash", "").strip()
        if h not in chain_hashes:
            continue
        kv = e["kv"]
        evt = f"{e['prefix']}/{e['event']}"
        short_h = shorten(kv.get("hash", ""), 18)

        # Pick the most informative key(s) for this event type
        sig = ""
        if e["event"] == "BLOCK_RECEIVE":
            r = result_name(kv.get("result_code", kv.get("result", "")))
            sig = f"result={r}  prev={shorten(kv.get('prev',''),12)}"
        elif e["event"] == "GETDATA_ITEM":
            sig = (f"batch={kv.get('batch_id','?')}  "
                   f"pos={kv.get('batch_pos','?')}  "
                   f"source={kv.get('source','?')}")
        elif e["event"] == "GETDATA_SEND":
            sig = (f"path={kv.get('path','?')}  "
                   f"known={kv.get('known_index','?')}")
        elif e["event"] == "OWNER_ASSIGN":
            sig = (f"state={kv.get('state','?')}  "
                   f"source={kv.get('source','?')}")
        elif e["event"] == "OWNER_RELEASE":
            sig = f"reason={kv.get('reason','?')}"
        elif e["event"] == "ASK_SCHEDULE":
            sig = f"source={kv.get('source','?')}"
        elif e["event"] == "SERVE_GETDATA":
            sig = (f"found={kv.get('found','?')}  "
                   f"sent={kv.get('sent','?')}")
        elif e["event"] == "BLOCK_RESULT":
            sig = (f"result={kv.get('result','?')}  "
                   f"height={kv.get('height','?')}")
        elif e["event"] == "INFLIGHT_CLEAR":
            sig = f"reason={kv.get('reason','?')}"
        elif e["event"] == "INFLIGHT_EXPIRE":
            sig = f"age={kv.get('age_s','?')}s"
        elif e["event"] == "GETDATA_SKIP":
            sig = (f"owner={kv.get('owner_peer','?')}  "
                   f"state={kv.get('owner_state','?')}")
        elif e["event"] == "ASK_SKIP":
            sig = f"reason={kv.get('reason','?')}"
        elif e["event"] == "ASK_REMOVE":
            sig = f"reason={kv.get('reason','?')}"
        elif e["event"] == "OWNER_TRANSITION":
            sig = f"from={kv.get('from','?')}  to={kv.get('to','?')}"

        fh.write(f"{idx:>6}  {e['time_us']:>14}  {evt:<22}  "
                 f"{short_h:<18}  {sig}\n")
        idx += 1

    fh.write("\n")


# ---------------------------------------------------------------------------
#  Machine-readable JSON
# ---------------------------------------------------------------------------

def write_json(path, store, chain, break_event, classification):
    bk = break_event["kv"]
    chain_hashes = {h for h, _ in chain}

    # Build timeline subset for chain hashes
    timeline = []
    for e in store.timeline:
        h = e["kv"].get("hash", "").strip()
        if h not in chain_hashes:
            continue
        timeline.append({
            "time_us": e["time_us"],
            "source": e["prefix"],
            "event": e["event"],
            "data": dict(e["kv"]),
        })

    ancestry = []
    for idx, (h, ev) in enumerate(chain):
        entry = {"chain_pos": idx, "hash": h}
        if ev:
            entry["receive_time_us"] = ev["time_us"]
            entry["receive_data"] = dict(ev["kv"])
        else:
            entry["receive_time_us"] = None
            entry["receive_data"] = None
        ancestry.append(entry)

    result = {
        "classification": classification,
        "break_event": {
            "time_us": break_event["time_us"],
            "data": dict(bk),
        },
        "ancestry": ancestry,
        "timeline": timeline,
    }

    with open(path, "w") as f:
        json.dump(result, f, indent=2, default=str)
    return result


# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Analyze first-order block-order breaks from trace logs.")
    parser.add_argument("log", help="Path to debug.log")
    parser.add_argument("--output-dir", default=".",
                        help="Directory for output files (default: .)")
    args = parser.parse_args()

    log_path = args.log
    out_dir = args.output_dir
    os.makedirs(out_dir, exist_ok=True)

    if not os.path.isfile(log_path):
        sys.exit(f"error: log file not found: {log_path}")

    store = EventStore()
    line_count = 0
    trace_count = 0

    print(f"Parsing {log_path} ...", flush=True)
    with open(log_path, "r", errors="replace") as f:
        for line in f:
            line_count += 1
            result = classify_line(line)
            if result is None:
                continue
            prefix, event_type, kv = result
            if event_type == "START":
                continue   # noise
            store.add(prefix, event_type, kv)
            trace_count += 1

    print(f"  Read {line_count} lines, found {trace_count} trace events, "
          f"{len(store.received_hashes)} unique received hashes.",
          flush=True)
    if trace_count == 0:
        sys.exit("error: no trace events found. Is -firstorderbreaktrace " \
                 "enabled in the node config?")

    store.sort()

    break_event = find_first_break(store)
    if break_event is None:
        print("No first-order break detected (no orphan with missing parent "
              "found).")
        # Still produce a report with the latest orphan events
        for e in reversed(store.timeline):
            if e["event"] == "BLOCK_RECEIVE" and \
               is_orphan_result(e["kv"].get("result_code", "")):
                break_event = e
                break
        if break_event is None:
            sys.exit("No orphan events found at all.")

    chain = reconstruct_chain(store, break_event)
    classification = classify_break(store, break_event, chain)
    h = break_event["kv"]["hash"]

    # Write outputs
    base_name = f"first_order_break_{shorten(h)}"

    # Human-readable
    txt_path = os.path.join(out_dir, f"{base_name}.txt")
    with open(txt_path, "w") as f:
        write_timeline(f, store, chain, break_event, classification)
        write_table(f, store, chain, break_event, classification)

    # Machine-readable JSON
    json_path = os.path.join(out_dir, f"{base_name}.json")
    write_json(json_path, store, chain, break_event, classification)

    print(f"Classification : {classification}")
    print(f"Break block    : {h}")
    print(f"Chain length   : {len(chain)}")
    print(f"Timeline       : {txt_path}")
    print(f"JSON           : {json_path}")


if __name__ == "__main__":
    main()
