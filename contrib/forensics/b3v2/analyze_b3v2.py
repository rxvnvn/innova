#!/usr/bin/env python3
"""Extract the B3-v2 validity metrics from a captured debug log."""

import argparse
import json
import re


def last_match(pattern, text):
    matches = re.findall(pattern, text, re.MULTILINE)
    return matches[-1] if matches else None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("debug_log")
    args = parser.parse_args()
    text = open(args.debug_log, encoding="utf-8", errors="replace").read()
    result = {
        "block_latency": last_match(r"IBD_BLOCKLAT_SUMMARY (.*)", text),
        "efficiency": last_match(r"IBDEFFICIENCY (.*)", text),
        "origins": re.findall(r"EXPTRACE ORIGIN .*", text),
        "orphans": last_match(r"EXPTRACE ORPHAN (.*)", text),
        "active_samples": len(re.findall(r"IBD_ACTIVE_1S ", text)),
        "header_refills": len(re.findall(r"IBD_HEADERS_SCHED event=refill ", text)),
        "outside_window": len(re.findall(r"IBD_HEADERS_SCHED event=inv ", text)),
        "stall_recovery": len(re.findall(r"STALL_RECOVERY", text)),
    }
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
