#!/usr/bin/env python3
"""Offline analyzer for the IBD forensic CSV dump (src/ibdforensic.cpp).

Usage:
    python3 analyze_forensic_csv.py <forensic.csv> [--json FILE]
    python3 analyze_forensic_csv.py --baseline A.csv --compare B.csv [--json FILE]

Reads the per-hash/per-batch CSV written by ibdforensic::Dump() and reports,
by exact declared batch size and by peer:

    first-block delay p50/p90/p95/p99
    stream duration   p50/p90/p95/p99
    total receive duration p50/p90/p95/p99
    timeout-before-first rate
    fully-late rate
    rerequest / cross-peer rates

Singleton batches (n_hashes == 1) are excluded from the multi-block
stream-duration quantiles and reported in a dedicated section.  Every
percentage states its denominator.  Row-weighted and batch-weighted metrics
are kept separate.  Rows are classified into no-rerequest / same-peer
rerequest / cross-peer rerequest buckets.  A cap=384 comparison is refused
when the input contains no n_hashes=384 batches.

Input format (column names parsed from the '# ...' header line):
    peer,batch_id,seq,n_hashes,hash,was_hashcontinue,mark_time_us,
    recv_time_us,timeout_time_us,received_after_timeout,rerequested,
    rerequested_other_peer,rerequest_peer,rerequest_time_us,send_buffer_bytes

Timestamps are wall-clock microseconds (GetTimeMicros()).  recv_time_us == 0
means never received; timeout_time_us == 0 means never expired.

The reader is column-name driven so it also tolerates the additive
generation columns introduced by later instrumentation.
"""

import argparse
import json
import sys


# ---------------------------------------------------------------------------
#  Row / batch model
# ---------------------------------------------------------------------------

# Required legacy columns (names from the CSV header).
REQUIRED = [
    "peer", "batch_id", "seq", "n_hashes", "hash", "was_hashcontinue",
    "mark_time_us", "recv_time_us", "timeout_time_us",
    "received_after_timeout", "rerequested", "rerequested_other_peer",
    "rerequest_peer", "rerequest_time_us", "send_buffer_bytes",
]

# Columns that would indicate a generation-aware dump (Phase 1).  When
# present they are carried into the JSON but the legacy aggregation below is
# unchanged (a generation-aware aggregation is added with the instrumented
# dumps).
OPTIONAL = [
    "generation_id", "generation_mark_us", "generation_release_us",
    "generation_release_reason", "enqueue_time_us", "first_socket_send_us",
    "nsend_first_send", "progress_last_us", "head_age_at_expiry_us",
]


def pct(values, p):
    """Nearest-rank percentile matching the C++ summary
    (ibdforensic.cpp FormatSummary: lat[int(p*size)], clamped)."""
    if not values:
        return None
    n = len(values)
    a = sorted(values)
    idx = int(p * n)
    if idx >= n:
        idx = n - 1
    return a[idx]


def quantiles(values, min_samples=1):
    if values is None:
        return None
    if len(values) < min_samples:
        return None
    return {
        "n": len(values),
        "p50": pct(values, 0.50),
        "p90": pct(values, 0.90),
        "p95": pct(values, 0.95),
        "p99": pct(values, 0.99),
        "min": min(values),
        "max": max(values),
    }


class Row(object):
    def __init__(self, fields, col):
        self.peer = int(fields[col["peer"]])
        self.batch_id = int(fields[col["batch_id"]])
        self.seq = int(fields[col["seq"]])
        self.n_hashes = int(fields[col["n_hashes"]])
        self.hash = fields[col["hash"]]
        self.was_hashcontinue = int(fields[col["was_hashcontinue"]])
        self.mark = int(fields[col["mark_time_us"]])
        self.recv = int(fields[col["recv_time_us"]])
        self.timeout = int(fields[col["timeout_time_us"]])
        self.received_after_timeout = int(fields[col["received_after_timeout"]])
        self.rerequested = int(fields[col["rerequested"]])
        self.rerequested_other_peer = int(fields[col["rerequested_other_peer"]])
        self.rerequest_peer = int(fields[col["rerequest_peer"]])
        self.rerequest_time = int(fields[col["rerequest_time_us"]])
        self.send_buffer_bytes = int(fields[col["send_buffer_bytes"]])

    def bucket(self):
        if self.rerequested_other_peer:
            return "cross_peer"
        if self.rerequested:
            return "same_peer"
        return "no_rerequest"

    def delivering_latency_us(self):
        """Best latency basis available from the legacy dump: the delivering
        generation.  For a re-requested hash the canonical mark belongs to the
        first generation; use the recorded re-request time instead when it is
        consistent with the receipt."""
        if self.recv > 0:
            if (self.rerequested and self.rerequest_time > 0 and
                    self.recv >= self.rerequest_time):
                return self.recv - self.rerequest_time
            if self.mark > 0:
                return self.recv - self.mark
        return None


class Batch(object):
    def __init__(self, peer, batch_id, n_hashes, send_time_us):
        self.peer = peer
        self.batch_id = batch_id
        self.n_hashes = n_hashes
        self.send_time_us = send_time_us
        self.rows = []
        self.received = []      # rows with recv > 0
        self.never = []         # rows with recv == 0
        self.timeout_rows = []  # rows with timeout > 0
        self.late_rows = []     # rows received_after_timeout == 1
        self.rerequest_rows = []
        self.cross_rows = []

    def add(self, row):
        self.rows.append(row)
        if row.recv > 0:
            self.received.append(row)
        else:
            self.never.append(row)
        if row.timeout > 0:
            self.timeout_rows.append(row)
        if row.received_after_timeout:
            self.late_rows.append(row)
        if row.rerequested:
            self.rerequest_rows.append(row)
        if row.rerequested_other_peer:
            self.cross_rows.append(row)

    @property
    def first_recv_us(self):
        if not self.received:
            return None
        return min(r.recv for r in self.received)

    @property
    def last_recv_us(self):
        if not self.received:
            return None
        return max(r.recv for r in self.received)

    @property
    def first_timeout_us(self):
        if not self.timeout_rows:
            return None
        return min(r.timeout for r in self.timeout_rows)

    @property
    def is_singleton(self):
        return self.n_hashes <= 1


# ---------------------------------------------------------------------------
#  Parsing
# ---------------------------------------------------------------------------

def parse_csv(path):
    """Return (summary, batches, generations).

    batches: {batch_id: Batch} from the legacy per-hash section.
    generations: list of dicts from the '#generations' section (empty when
    the dump predates the generation ledger; the legacy-only header line is
    '# peer,batch_id,...' while the generation section opens with a bare
    '#generations' marker).

    The dump file begins with the human-readable summary text (skipped), then
    a header line beginning with '#', then the legacy data rows, then (in
    generation-aware dumps) a '#generations' marker, a generation header and
    the generation rows.  Row order is not relied upon; batches are keyed by
    batch_id.
    """
    summary = []
    legacy_header = None
    gen_header = None
    col = {}
    gen_col = {}
    batches = {}
    generations = []
    seen_gens = False

    with open(path, "r", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line:
                continue
            stripped = line.strip()

            if stripped == "#generations":
                seen_gens = True
                continue

            if seen_gens:
                if gen_header is None:
                    if not stripped.startswith("#"):
                        sys.exit("error: expected generation header, got: %r"
                                 % line)
                    gen_header = stripped[1:].strip()
                    gen_col = {name: i for i, name in enumerate(
                        [c.strip() for c in gen_header.split(",")])}
                    for name in ("generation_id", "hash", "peer", "mark_us",
                                 "release_us", "reason"):
                        if name not in gen_col:
                            sys.exit("error: generation header missing "
                                     "column %s (seen: %s)"
                                     % (name, gen_header))
                else:
                    parts = line.split(",")
                    try:
                        generations.append({
                            "generation_id": int(parts[gen_col["generation_id"]]),
                            "hash": parts[gen_col["hash"]],
                            "peer": int(parts[gen_col["peer"]]),
                            "mark_us": int(parts[gen_col["mark_us"]]),
                            "release_us": int(parts[gen_col["release_us"]]),
                            "reason": parts[gen_col["reason"]],
                        })
                    except (ValueError, IndexError) as exc:
                        sys.exit("error: malformed generation row: %r (%s)"
                                 % (line, exc))
                continue

            if legacy_header is None:
                if stripped.startswith("#"):
                    legacy_header = stripped[1:].strip()
                    cols = [c.strip() for c in legacy_header.split(",")]
                    col = {name: i for i, name in enumerate(cols)}
                    missing = [c for c in REQUIRED if c not in col]
                    if missing:
                        sys.exit(
                            "error: CSV header missing columns: %s "
                            "(seen header: %s)" % (",".join(missing),
                                                   legacy_header))
                else:
                    summary.append(line)
                continue

            parts = line.split(",")
            try:
                row = Row(parts, col)
            except (ValueError, IndexError) as exc:
                sys.exit("error: malformed CSV row: %r (%s)" % (line, exc))

            key = row.batch_id
            if key not in batches:
                batches[key] = Batch(row.peer, row.batch_id, row.n_hashes,
                                     row.mark)
            batches[key].add(row)

    if not batches:
        sys.exit("error: no data rows parsed from %s" % path)
    return summary, batches, generations


# ---------------------------------------------------------------------------
#  Aggregation
# ---------------------------------------------------------------------------

def _first_block_delays(batches):
    return [b.first_recv_us - b.send_time_us
            for b in batches if b.first_recv_us is not None]


def _stream_durations(batches, only_multi_received=False):
    out = []
    for b in batches:
        if b.first_recv_us is None:
            continue
        if only_multi_received and len(b.received) < 2:
            continue
        out.append(b.last_recv_us - b.first_recv_us)
    return out


def _total_receive_durations(batches):
    return [b.last_recv_us - b.send_time_us
            for b in batches if b.last_recv_us is not None]


def aggregate_group(batches, min_samples):
    """Aggregate a list of Batch into the metric dict used everywhere."""
    n = len(batches)
    n_received = sum(1 for b in batches if b.first_recv_us is not None)
    n_never = n - n_received
    n_timeout = sum(1 for b in batches if b.timeout_rows)
    n_late = sum(1 for b in batches if b.late_rows)

    # timeout-before-first: min(timeout) < first recv (the peer=6 pattern).
    n_tbf = 0
    for b in batches:
        if b.first_recv_us is not None and b.first_timeout_us is not None:
            if b.first_timeout_us < b.first_recv_us:
                n_tbf += 1

    # fully-late: every row received AND every received row is late.
    n_fully_late = sum(
        1 for b in batches
        if not b.never and len(b.late_rows) == len(b.received)
        and len(b.received) > 0)

    rows = [r for b in batches for r in b.rows]
    n_rows = len(rows)
    n_rr = sum(1 for r in rows if r.rerequested)
    n_xp = sum(1 for r in rows if r.rerequested_other_peer)
    n_received_rows = sum(1 for r in rows if r.recv > 0)
    n_late_rows = sum(1 for r in rows if r.received_after_timeout)

    # per-bucket row latency (delivering generation basis)
    bucket_rows = {"no_rerequest": [], "same_peer": [], "cross_peer": []}
    for r in rows:
        bucket_rows[r.bucket()].append(r)

    buckets = {}
    for name, br in bucket_rows.items():
        lat = [v for r in br if (v := r.delivering_latency_us()) is not None]
        buckets[name] = {
            "rows": len(br),
            "received_rows": sum(1 for r in br if r.recv > 0),
            "never_rows": sum(1 for r in br if r.recv == 0),
            "timeout_rows": sum(1 for r in br if r.timeout > 0),
            "late_rows": sum(1 for r in br if r.received_after_timeout),
            "delivering_latency_us": quantiles(lat, min_samples),
        }

    return {
        "n_batches": n,
        "n_rows": n_rows,
        "batches_received": n_received,
        "batches_never_received": n_never,
        "batches_with_timeout": n_timeout,
        "batches_with_late_rows": n_late,
        "first_block_delay_us": quantiles(
            _first_block_delays(batches), min_samples),
        "stream_duration_us": quantiles(
            _stream_durations(batches), min_samples),
        "stream_duration_multi_received_us": quantiles(
            _stream_durations(batches, only_multi_received=True), min_samples),
        "total_receive_duration_us": quantiles(
            _total_receive_durations(batches), min_samples),
        # rates with explicit denominators
        "timeout_before_first": {
            "count": n_tbf,
            "over_all_batches": (n_tbf / n) if n else None,
            "denom_over_all": "all_batches",
            "over_batches_with_timeout":
                (n_tbf / n_timeout) if n_timeout else None,
            "denom_over_timeout": "batches_with_any_timeout",
        },
        "fully_late": {
            "count": n_fully_late,
            "over_all_batches": (n_fully_late / n) if n else None,
            "denom_over_all": "all_batches",
            "over_received_batches":
                (n_fully_late / n_received) if n_received else None,
            "denom_over_received": "batches_with_any_received_row",
        },
        "rerequest": {
            "rows": n_rr,
            "rate_over_rows": (n_rr / n_rows) if n_rows else None,
            "denom_over_rows": "all_rows",
        },
        "cross_peer": {
            "rows": n_xp,
            "rate_over_rows": (n_xp / n_rows) if n_rows else None,
            "denom_over_rows": "all_rows",
            "over_rerequested_rows":
                (n_xp / n_rr) if n_rr else None,
            "denom_over_rerequested": "rerequested_rows",
        },
        "late_rows": {
            "rows": n_late_rows,
            "received_rows": n_received_rows,
            "over_received_rows":
                (n_late_rows / n_received_rows) if n_received_rows else None,
            "denom_over_received": "received_rows",
        },
        "buckets": buckets,
    }


def group_batches(batches, singleton_only=False):
    if singleton_only:
        return [b for b in batches.values() if b.is_singleton]
    return [b for b in batches.values() if not b.is_singleton]


# ---------------------------------------------------------------------------
#  Reporting
# ---------------------------------------------------------------------------

def _fmt_quantiles(q):
    if q is None:
        return "insufficient samples"
    return ("n=%d p50=%d p90=%d p95=%d p99=%d min=%d max=%d us" %
            (q["n"], q["p50"], q["p90"], q["p95"], q["p99"], q["min"], q["max"]))


def _fmt_rate(d, key):
    v = d.get(key)
    return "n/a" if v is None else "%.1f%%" % (100.0 * v)


def render_group(title, g):
    lines = []
    lines.append(title)
    lines.append("  batches=%d rows=%d (received=%d never=%d "
                 "with_timeout=%d with_late=%d)" %
                 (g["n_batches"], g["n_rows"], g["batches_received"],
                  g["batches_never_received"], g["batches_with_timeout"],
                  g["batches_with_late_rows"]))
    lines.append("  first_block_delay_us : %s" % _fmt_quantiles(
        g["first_block_delay_us"]))
    lines.append("  stream_duration_us   : %s" % _fmt_quantiles(
        g["stream_duration_us"]))
    lines.append("  stream_duration_us (>=2 received rows): %s" %
                 _fmt_quantiles(g["stream_duration_multi_received_us"]))
    lines.append("  total_receive_us     : %s" % _fmt_quantiles(
        g["total_receive_duration_us"]))
    tbf = g["timeout_before_first"]
    fl = g["fully_late"]
    rr = g["rerequest"]
    xp = g["cross_peer"]
    lr = g["late_rows"]
    lines.append(
        "  timeout_before_first  : %d  (%.1f%% of all batches; %.1f%% of "
        "batches with any timeout)" %
        (tbf["count"],
         100.0 * (tbf["over_all_batches"] or 0.0),
         100.0 * (tbf["over_batches_with_timeout"] or 0.0)))
    lines.append(
        "  fully_late            : %d  (%.1f%% of all batches; %.1f%% of "
        "received batches)" %
        (fl["count"], 100.0 * (fl["over_all_batches"] or 0.0),
         100.0 * (fl["over_received_batches"] or 0.0)))
    lines.append(
        "  rerequest             : %d rows  (%.1f%% of %d rows)" %
        (rr["rows"], 100.0 * (rr["rate_over_rows"] or 0.0), g["n_rows"]))
    lines.append(
        "  cross_peer rerequest  : %d rows  (%.1f%% of %d rows; %.1f%% of "
        "rerequested rows)" %
        (xp["rows"], 100.0 * (xp["rate_over_rows"] or 0.0), g["n_rows"],
         100.0 * (xp["over_rerequested_rows"] or 0.0)))
    lines.append(
        "  late rows             : %d  (%.1f%% of %d received rows)" %
        (lr["rows"], 100.0 * (lr["over_received_rows"] or 0.0),
         lr["received_rows"]))
    for name in ("no_rerequest", "same_peer", "cross_peer"):
        b = g["buckets"][name]
        lines.append(
            "  bucket %-11s: rows=%d received=%d never=%d timeout=%d late=%d "
            "delivering_latency: %s" %
            (name, b["rows"], b["received_rows"], b["never_rows"],
             b["timeout_rows"], b["late_rows"],
             _fmt_quantiles(b["delivering_latency_us"])))
    return "\n".join(lines)


def aggregate_generations(gens):
    """Aggregate the '#generations' section: per-reason closure counts and
    lifetime (release - mark) quantiles, plus how often a hash went through
    more than one generation (the re-request lifecycle)."""
    n_total = len(gens)
    n_active = sum(1 for g in gens if g["release_us"] == 0)
    by_reason = {}
    by_hash = {}
    for g in gens:
        r = g["reason"]
        by_reason.setdefault(r, {"count": 0, "lifetimes": []})
        by_reason[r]["count"] += 1
        if g["release_us"] != 0:
            by_reason[r]["lifetimes"].append(g["release_us"] - g["mark_us"])
        by_hash.setdefault(g["hash"], []).append(g)
    by_hash_counts = [len(v) for v in by_hash.values()]
    return {
        "total": n_total,
        "active": n_active,
        "closed": n_total - n_active,
        "by_reason": {
            r: {"count": v["count"],
                "lifetime_us": quantiles(v["lifetimes"])}
            for r, v in by_reason.items()
        },
        "hashes_with_multiple_generations":
            sum(1 for c in by_hash_counts if c > 1),
        "max_generations_per_hash": max(by_hash_counts, default=0),
    }


def render_generations(g):
    lines = []
    lines.append("=== GENERATIONS ===")
    lines.append("  total=%d closed=%d active=%d" %
                 (g["total"], g["closed"], g["active"]))
    lines.append("  hashes with >1 generation: %d (max %d per hash)" %
                 (g["hashes_with_multiple_generations"],
                  g["max_generations_per_hash"]))
    for reason in sorted(g["by_reason"]):
        e = g["by_reason"][reason]
        name = reason if reason else "(active)"
        lines.append("  reason %-14s: count=%d lifetime_us: %s" %
                     (name, e["count"], _fmt_quantiles(e["lifetime_us"])))
    return "\n".join(lines)


def render_report(data):
    out = []
    out.append("=== GLOBAL (non-singleton batches) ===")
    out.append(render_group("global", data["global"]))
    out.append("")
    out.append("=== PER SIZE (declared n_hashes) ===")
    for size in sorted(data["by_size"], key=lambda s: (s is None, s)):
        g = data["by_size"][size]
        out.append(render_group("size=%s" % (size if size is not None else "?"),
                                g))
        out.append("")
    out.append("=== PER (size, peer) ===")
    for (size, peer) in sorted(data["by_size_peer"],
                               key=lambda k: (k[0] is None, k[0], k[1])):
        g = data["by_size_peer"][(size, peer)]
        out.append(render_group("size=%s peer=%s" % (size, peer), g))
        out.append("")
    out.append("=== SINGLETONS (n_hashes == 1) ===")
    s = data["singletons"]
    out.append(render_group("singletons", s))
    out.append("")
    out.append("=== CAP EVIDENCE (uncontaminated single-generation rows, "
               "rerequested=0 and rerequested_other_peer=0) ===")
    for size in sorted(data["cap"].get("sizes", []),
                       key=lambda s: (s is None, s)):
        entry = data["cap"]["sizes"][size]
        if entry is None:
            out.append("size=%s: absent from input (comparison refused)" % size)
            continue
        out.append(render_group("size=%s (uncontaminated)" % size, entry))
        out.append("")
    if data["generations"]["total"]:
        out.append(render_generations(data["generations"]))
        out.append("")
    out.append("=== CSV summary block (echoed) ===")
    out.extend("  " + line for line in data["summary_lines"])
    return "\n".join(out)


# ---------------------------------------------------------------------------
#  Analysis driver
# ---------------------------------------------------------------------------

def analyze(batches, min_samples, cap_sizes, generations=None):
    non_single = group_batches(batches, singleton_only=False)
    singles = group_batches(batches, singleton_only=True)

    by_size = {}
    for size in sorted({b.n_hashes for b in non_single}):
        by_size[size] = aggregate_group(
            [b for b in non_single if b.n_hashes == size], min_samples)

    by_size_peer = {}
    for b in non_single:
        key = (b.n_hashes, b.peer)
        by_size_peer.setdefault(key, []).append(b)
    by_size_peer = {k: aggregate_group(v, min_samples)
                    for k, v in by_size_peer.items()}

    # cap evidence: uncontaminated single-generation rows only.
    uncontam = []
    for b in non_single:
        if all(not r.rerequested and not r.rerequested_other_peer
               for r in b.rows):
            uncontam.append(b)
    cap = {"sizes": {}}
    for size in cap_sizes:
        sel = [b for b in uncontam if b.n_hashes == size]
        if not sel:
            cap["sizes"][size] = None
        else:
            cap["sizes"][size] = aggregate_group(sel, min_samples)

    return {
        "global": aggregate_group(non_single, min_samples),
        "singletons": aggregate_group(singles, min_samples),
        "by_size": by_size,
        "by_size_peer": by_size_peer,
        "cap": cap,
        "generations": aggregate_generations(generations or []),
    }


# ---------------------------------------------------------------------------
#  Comparison mode
# ---------------------------------------------------------------------------

def _cmp_quantiles(base, target):
    if base is None and target is None:
        return "insufficient"
    if base is None:
        return "n/a -> %s" % _fmt_quantiles(target)
    if target is None:
        return "%s -> n/a" % _fmt_quantiles(base)
    return ("p50 %+d  p90 %+d  p95 %+d  p99 %+d  (n %d -> %d)" %
            (target["p50"] - base["p50"], target["p90"] - base["p90"],
             target["p95"] - base["p95"], target["p99"] - base["p99"],
             base["n"], target["n"]))


def render_compare(base, target, cap_sizes):
    out = []
    out.append("=== COMPARISON baseline -> target (deltas) ===")
    for name in ("global",):
        bg, tg = base[name], target[name]
        out.append("GLOBAL")
        out.append("  first_block_delay_us  : %s" %
                   _cmp_quantiles(bg["first_block_delay_us"],
                                  tg["first_block_delay_us"]))
        out.append("  stream_duration_us    : %s" %
                   _cmp_quantiles(bg["stream_duration_us"],
                                  tg["stream_duration_us"]))
        out.append("  total_receive_us      : %s" %
                   _cmp_quantiles(bg["total_receive_duration_us"],
                                  tg["total_receive_duration_us"]))
        for key in ("timeout_before_first", "fully_late"):
            out.append("  %s : %.2f%% -> %.2f%% (count %d -> %d)" %
                       (key,
                        100.0 * (bg[key]["over_all_batches"] or 0.0),
                        100.0 * (tg[key]["over_all_batches"] or 0.0),
                        bg[key]["count"], tg[key]["count"]))
        out.append("  rerequest rate   : %.2f%% -> %.2f%% (rows %d -> %d)" %
                   (100.0 * (bg["rerequest"]["rate_over_rows"] or 0.0),
                    100.0 * (tg["rerequest"]["rate_over_rows"] or 0.0),
                    bg["rerequest"]["rows"], tg["rerequest"]["rows"]))
        out.append("  cross_peer rate  : %.2f%% -> %.2f%% (rows %d -> %d)" %
                   (100.0 * (bg["cross_peer"]["rate_over_rows"] or 0.0),
                    100.0 * (tg["cross_peer"]["rate_over_rows"] or 0.0),
                    bg["cross_peer"]["rows"], tg["cross_peer"]["rows"]))
    out.append("")
    out.append("=== COMPARISON per declared size (sizes present in BOTH) ===")
    bs, ts = base["by_size"], target["by_size"]
    common = sorted(set(bs) & set(ts))
    if not common:
        out.append("  no common sizes to compare")
    for size in common:
        bg, tg = bs[size], ts[size]
        out.append("size=%s" % size)
        out.append("  first_block_delay_us  : %s" %
                   _cmp_quantiles(bg["first_block_delay_us"],
                                  tg["first_block_delay_us"]))
        out.append("  stream_duration_us    : %s" %
                   _cmp_quantiles(bg["stream_duration_us"],
                                  tg["stream_duration_us"]))
        out.append("  total_receive_us      : %s" %
                   _cmp_quantiles(bg["total_receive_duration_us"],
                                  tg["total_receive_duration_us"]))
        out.append("  timeout_before_first  : %.2f%% -> %.2f%% (n %d -> %d)" %
                   (100.0 * (bg["timeout_before_first"]["over_all_batches"]
                             or 0.0),
                    100.0 * (tg["timeout_before_first"]["over_all_batches"]
                             or 0.0),
                    bg["n_batches"], tg["n_batches"]))
    for size in sorted(set(bs) ^ set(ts)):
        if size is None:
            continue
        out.append("size=%s: present in only one input (skipped)" % size)
    out.append("")
    out.append("=== CAP EVIDENCE comparison (uncontaminated rows) ===")
    for size in cap_sizes:
        bcap, tcap = base["cap"]["sizes"].get(size), target["cap"]["sizes"].get(size)
        if bcap is None and tcap is None:
            out.append("size=%s: absent from both (refused)" % size)
        elif bcap is None:
            out.append("size=%s: absent in baseline only" % size)
        elif tcap is None:
            out.append("size=%s: absent in target only" % size)
        else:
            out.append("size=%s: first_block_delay_us %s" %
                       (size, _cmp_quantiles(
                           bcap["first_block_delay_us"],
                           tcap["first_block_delay_us"])))
    return "\n".join(out)


# ---------------------------------------------------------------------------
#  Main
# ---------------------------------------------------------------------------

def _json_safe(obj):
    """Recursively convert non-string dict keys (e.g. (size, peer) tuples)
    into strings so the metric dict is JSON-serializable."""
    if isinstance(obj, dict):
        return {str(k): _json_safe(v) for k, v in obj.items()}
    if isinstance(obj, (list, tuple)):
        return [_json_safe(v) for v in obj]
    return obj


def main():
    parser = argparse.ArgumentParser(
        description="Analyze IBD forensic CSV dumps.")
    parser.add_argument("csv", nargs="?", help="Path to forensic CSV")
    parser.add_argument("--json", default=None,
                        help="Also write the full metric JSON to this file")
    parser.add_argument("--baseline", default=None,
                        help="Baseline CSV for comparison")
    parser.add_argument("--compare", default=None,
                        help="Target CSV to compare against --baseline")
    parser.add_argument("--min-samples", type=int, default=30,
                        help="Minimum samples to report quantiles (default 30)")
    parser.add_argument("--cap-sizes", default="255,256,384",
                        help="Declared sizes for cap evidence (default "
                             "255,256,384)")
    args = parser.parse_args()

    if args.baseline or args.compare:
        if not (args.baseline and args.compare):
            sys.exit("error: --baseline and --compare must be used together")
        if args.csv:
            sys.exit("error: positional CSV is not used with --baseline/--compare")

    cap_sizes = []
    for tok in args.cap_sizes.split(","):
        tok = tok.strip()
        if tok:
            try:
                cap_sizes.append(int(tok))
            except ValueError:
                sys.exit("error: --cap-sizes must be comma-separated ints")

    if args.baseline:
        print("Parsing baseline %s ..." % args.baseline, flush=True)
        bs, bb, bg = parse_csv(args.baseline)
        print("Parsing target   %s ..." % args.compare, flush=True)
        ts, tb, tg = parse_csv(args.compare)
        base = analyze(bb, args.min_samples, cap_sizes, bg)
        base["summary_lines"] = bs
        target = analyze(tb, args.min_samples, cap_sizes, tg)
        target["summary_lines"] = ts
        report = render_compare(base, target, cap_sizes)
        print(report)
        if args.json:
            with open(args.json, "w") as f:
                json.dump(_json_safe({"baseline": base, "target": target}),
                          f, indent=2, default=str)
        return

    if not args.csv:
        sys.exit("error: provide a CSV path or --baseline/--compare")

    print("Parsing %s ..." % args.csv, flush=True)
    summary, batches, generations = parse_csv(args.csv)
    data = analyze(batches, args.min_samples, cap_sizes, generations)
    data["summary_lines"] = summary

    n_batches = len(batches)
    n_single = sum(1 for b in batches.values() if b.is_singleton)
    print("  parsed %d batches (%d singletons, %d multi-block), %d rows" %
          (n_batches, n_single, n_batches - n_single,
           sum(len(b.rows) for b in batches.values())))
    print(render_report(data))
    if args.json:
        with open(args.json, "w") as f:
            json.dump(_json_safe(data), f, indent=2, default=str)
        print("wrote %s" % args.json)


if __name__ == "__main__":
    main()
