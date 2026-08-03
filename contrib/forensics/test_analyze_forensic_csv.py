#!/usr/bin/env python3
"""Tests for analyze_forensic_csv.py over synthetic dumps.

Covers the legacy (phase-0) and the generation-aware (phase-1) dump schemas:

  - legacy-only dump: quantiles, singleton exclusion, bucket counts,
    error paths (missing column, malformed row, empty file)
  - new-schema dump: additive legacy columns parsed, #generations section
    aggregated (reasons, lifetimes, hashes with >1 generation), malformed
    generation row rejected, legacy-only input renders no generation section
  - compare mode over a legacy baseline vs a generation-aware target

Run:  python3 test_analyze_forensic_csv.py
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ANALYZER = os.path.join(HERE, "analyze_forensic_csv.py")

H_A = "1111111111111111111111111111111111111111111111111111111111111111"
H_B = "2222222222222222222222222222222222222222222222222222222222222222"

LEGACY_HEADER = (
    "# peer,batch_id,seq,n_hashes,hash,was_hashcontinue,mark_time_us,"
    "recv_time_us,timeout_time_us,received_after_timeout,rerequested,"
    "rerequested_other_peer,rerequest_peer,rerequest_time_us,send_buffer_bytes"
)

GEN_HEADER = (
    "# peer,batch_id,seq,n_hashes,hash,was_hashcontinue,mark_time_us,"
    "recv_time_us,timeout_time_us,received_after_timeout,rerequested,"
    "rerequested_other_peer,rerequest_peer,rerequest_time_us,send_buffer_bytes,"
    "generation_id,generation_mark_us,generation_release_us,"
    "generation_release_reason,enqueue_time_us,first_socket_send_us,"
    "nsend_first_send,progress_last_us,head_age_at_expiry_us,"
    "recv_framing_complete_us"
)


def write(path, lines):
    with open(path, "w") as f:
        f.write("\n".join(lines) + "\n")


def run_analyzer(args):
    return subprocess.run([sys.executable, ANALYZER] + args,
                          capture_output=True, text=True)


def legacy_dump():
    # One 384-batch: first hash clean, second hash times out, is re-requested
    # cross-peer, and the in-transit original arrives late.
    return [
        "IBDFORENSIC SUMMARY",
        "batches=1 canonical_entries=2 unsolicited_receipts=0 clean_arrivals=1",
        LEGACY_HEADER,
        "6,0,0,384,%s,0,1000000,1200000,0,0,0,0,-1,0,1048576" % H_A,
        "6,0,1,384,%s,1,1000000,0,6000000,0,1,1,7,3000000,1048576" % H_B,
    ]


def gen_dump():
    rows = legacy_dump()
    # Same rows plus the additive generation columns (gen 1 = clean receive,
    # gen 2 = timeout on the original peer, gen 3 = the re-request).
    rows[2] = GEN_HEADER
    rows[3] = rows[3] + ",1,1000000,1200000,receive,1000010,1000100,1048576,0,0,1100000"
    rows[4] = rows[4] + ",2,1000000,6000000,timeout,1000010,1000100,1048576,1200000,5000000,0"
    rows += [
        "#generations",
        "# generation_id,batch_id,hash,peer,mark_us,release_us,reason",
        "1,0,%s,6,1000000,1200000,receive" % H_A,
        "2,0,%s,6,1000000,6000000,timeout" % H_B,
        "3,0,%s,7,3000000,4000000,receive" % H_B,
    ]
    return rows


def check(name, cond):
    if cond:
        print("PASS %s" % name)
    else:
        print("FAIL %s" % name)
        sys.exit(1)


def main():
    with tempfile.TemporaryDirectory() as tmp:
        legacy = os.path.join(tmp, "legacy.csv")
        gen = os.path.join(tmp, "gen.csv")
        write(legacy, legacy_dump())
        write(gen, gen_dump())

        # ---- legacy-only single mode ----
        r = run_analyzer([legacy, "--min-samples", "1", "--cap-sizes", "384"])
        check("legacy parses", r.returncode == 0)
        check("legacy global counts",
              "batches=1 rows=2 (received=1 never=0 with_timeout=1 "
              "with_late=0)" in r.stdout)
        check("legacy first_block_delay p50",
              "first_block_delay_us : n=1 p50=200000" in r.stdout)
        check("legacy cross-peer bucket",
              "bucket cross_peer" in r.stdout and
              "timeout=1" in r.stdout.split("bucket cross_peer")[1][:200])
        check("legacy no generations section",
              "=== GENERATIONS ===" not in r.stdout)

        # ---- new-schema single mode ----
        r = run_analyzer([gen, "--min-samples", "1", "--cap-sizes", "384"])
        check("new-schema parses", r.returncode == 0)
        check("new-schema generations section",
              "=== GENERATIONS ===" in r.stdout)
        check("new-schema reason counts",
              "reason receive" in r.stdout and
              "count=2" in r.stdout.split("reason receive")[1][:80])
        check("new-schema timeout lifetime",
              "reason timeout" in r.stdout and
              "count=1" in r.stdout.split("reason timeout")[1][:120])
        check("new-schema multi-generation hash",
              "hashes with >1 generation: 1 (max 2 per hash)" in r.stdout)

        # ---- JSON carries the generations aggregation ----
        jpath = os.path.join(tmp, "out.json")
        r = run_analyzer([gen, "--min-samples", "1", "--json", jpath])
        check("json written", r.returncode == 0 and os.path.exists(jpath))
        with open(jpath) as f:
            data = f.read()
        check("json has generations",
              '"generations"' in data and '"by_reason"' in data and
              '"receive"' in data)

        # ---- compare mode: legacy baseline vs generation-aware target ----
        r = run_analyzer(["--baseline", legacy, "--compare", gen,
                          "--min-samples", "1"])
        check("compare parses", r.returncode == 0)
        check("compare deltas",
              "first_block_delay_us  : p50 +0" in r.stdout)

        # ---- error paths ----
        missing = os.path.join(tmp, "missing.csv")
        write(missing, [
            "IBDFORENSIC SUMMARY",
            "# peer,batch_id,seq,n_hashes,hash,was_hashcontinue",
            "6,0,0,384,%s,0" % H_A,
        ])
        r = run_analyzer([missing])
        check("missing column rejected", r.returncode != 0 and
              "missing columns" in r.stderr)

        malformed = os.path.join(tmp, "malformed.csv")
        write(malformed, legacy_dump()[:-1] + ["6,0,1,384,%s,1,1,0,0,0" % H_B])
        r = run_analyzer([malformed])
        check("malformed row rejected", r.returncode != 0 and
              "malformed CSV row" in r.stderr)

        badgen = os.path.join(tmp, "badgen.csv")
        write(badgen, gen_dump() + ["9,0,notahash,6,notanumber,0,receive"])
        r = run_analyzer([badgen])
        check("malformed generation row rejected", r.returncode != 0 and
              "malformed generation row" in r.stderr)

        empty = os.path.join(tmp, "empty.csv")
        write(empty, ["IBDFORENSIC SUMMARY", LEGACY_HEADER])
        r = run_analyzer([empty])
        check("empty file rejected", r.returncode != 0 and
              "no data rows" in r.stderr)

    print("all analyzer tests passed")


if __name__ == "__main__":
    main()
