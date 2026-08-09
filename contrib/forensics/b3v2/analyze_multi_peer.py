#!/usr/bin/env python3
"""Analyze the reproducible multi-peer B3-v2 failover smoke trace."""

import argparse
import json


def parse_fields(line):
    fields = {}
    for token in line.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value.rstrip(",")
    return fields


def analyze(client_text, relay_texts):
    owners = {}
    transitions = []
    frontier_hash = None
    frontier_height = None
    opportunities = 0
    attempts = 0
    successes = 0
    excluded = []
    late_foreign = 0
    duplicate_high_watermark = 0
    pending_failover = False
    requester_events = []

    for line in client_text.splitlines():
        data = parse_fields(line)
        if "frontier_hash=" in line:
            frontier_hash = data.get("frontier_hash")
            frontier_height = int(data.get("frontier_height", "-1"))
        if data.get("event") == "OWNER_ASSIGN" and data.get("hash"):
            hash_value = data["hash"]
            peer = data.get("peer", "unknown")
            if hash_value in owners:
                duplicate_high_watermark = max(duplicate_high_watermark, 2)
            owners[hash_value] = peer
            duplicate_high_watermark = max(duplicate_high_watermark, 1)
            if hash_value == frontier_hash:
                attempts += 1
                reason = "failover" if transitions and transitions[-1]["to"] == "none" and peer != transitions[-1]["from"] else "initial"
                if reason == "failover":
                    pending_failover = True
                transitions.append({"from": "none", "to": peer, "reason": reason,
                                    "time_us": data.get("time_us")})
        elif data.get("event") == "OWNER_RELEASE" and data.get("hash") == frontier_hash:
            previous = owners.pop(frontier_hash, data.get("peer", "unknown"))
            reason = data.get("reason", "unknown")
            if reason == "frontier_retry":
                opportunities += 1
                excluded.append(previous)
            transitions.append({"from": previous, "to": "none", "reason": reason,
                                "time_us": data.get("time_us")})
        elif data.get("event") in ("RECEIVE", "PROCESS_BLOCK") and data.get("hash") == frontier_hash:
            requester_events.append({"event": data["event"], "peer": data.get("peer"),
                                     "outcome": data.get("outcome"), "time_us": data.get("time_us")})
            if data.get("peer") and owners.get(frontier_hash) not in (None, data["peer"]):
                late_foreign += 1
            if pending_failover and data.get("event") == "PROCESS_BLOCK" and data.get("outcome") == "accepted-active":
                successes += 1
                pending_failover = False

    relay_records = []
    for supplier, text in relay_texts.items():
        for line in text.splitlines():
            data = parse_fields(line)
            if data.get("hash"):
                relay_records.append({"supplier": supplier, "event": line.split()[1],
                                      "command": data.get("command"), "hash": data["hash"],
                                      "time_us": line.split()[0]})
    return {"frontier_hash": frontier_hash, "frontier_height": frontier_height,
            "frontier_owner_transitions": transitions,
            "frontier_failover_opportunities": opportunities,
            "frontier_failover_attempts": attempts,
            "frontier_failover_successes": successes,
            "frontier_retry_excluded_peer": excluded,
            "frontier_late_foreign_response": late_foreign,
            "duplicate_owner_high_watermark": duplicate_high_watermark,
            "frontier_requester_events": requester_events,
            "relay_records": relay_records}


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("client_log")
    parser.add_argument("--relay-log", action="append", default=[])
    parser.add_argument("--supplier", action="append", default=[])
    args = parser.parse_args()
    client = open(args.client_log, encoding="utf-8", errors="replace").read()
    relays = {}
    for index, path in enumerate(args.relay_log):
        supplier = args.supplier[index] if index < len(args.supplier) else str(index)
        relays[supplier] = open(path, encoding="utf-8", errors="replace").read()
    print(json.dumps(analyze(client, relays), indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
