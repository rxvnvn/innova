#!/usr/bin/env python3
"""No-daemon framing and policy smoke test for the B3-v2 relay."""

import struct

from b3v2_relay import HEADER_COMMANDS, LIVENESS_COMMANDS, command_of, parse_frame, policy_for
from analyze_multi_peer import analyze


def frame(command, payload=b""):
    header = b"\x00" * 4 + command.encode("ascii").ljust(12, b"\0")
    return header + struct.pack("<I", len(payload)) + b"\0" * 4 + payload


def main():
    for command in ("getheaders", "headers", "block", "inv", "version"):
        data = bytearray(frame(command, b"payload"))
        parsed = parse_frame(data)
        assert parsed is not None
        parsed_frame, size = parsed
        assert size == len(data)
        assert command_of(parsed_frame) == command

    for command in LIVENESS_COMMANDS:
        delay, may_drop, plane = policy_for(command, 6950, 0.10)
        assert (delay, may_drop, plane) == (0.0, False, "liveness")
    for command in HEADER_COMMANDS:
        delay, may_drop, plane = policy_for(command, 6950, 0.10)
        assert (delay, may_drop, plane) == (6950.0, False, "headers")
    delay, may_drop, plane = policy_for("block", 6950, 0.10)
    assert (delay, may_drop, plane) == (6950.0, True, "data")
    root = __import__("pathlib").Path(__file__).parent / "fixtures"
    client = (root / "multi_peer_failover_smoke_client.log").read_text()
    relays = {"A": (root / "multi_peer_failover_smoke_relay_a.log").read_text(), "B": (root / "multi_peer_failover_smoke_relay_b.log").read_text()}
    report = analyze(client, relays)
    assert report["frontier_failover_opportunities"] == 1
    assert report["frontier_failover_attempts"] == 2
    assert report["frontier_failover_successes"] == 1
    assert report["frontier_retry_excluded_peer"] == ["A"]
    assert report["frontier_late_foreign_response"] == 1
    assert report["duplicate_owner_high_watermark"] <= 1
    assert report["relay_records"]
    print("B3-V2 multi-peer failover harness smoke: PASS")
    print("B3-V2 framing/policy smoke: PASS")


if __name__ == "__main__":
    main()
