#!/usr/bin/env python3
"""No-daemon framing and policy smoke test for the B3-v2 relay."""

import struct

from b3v2_relay import HEADER_COMMANDS, LIVENESS_COMMANDS, command_of, parse_frame, policy_for


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
    print("B3-V2 framing/policy smoke: PASS")


if __name__ == "__main__":
    main()
