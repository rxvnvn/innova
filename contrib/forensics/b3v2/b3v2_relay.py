#!/usr/bin/env python3
"""Deterministic, framed, two-plane impairment relay for B3-v2."""

import argparse
import hashlib
import json
import random
import socket
import struct
import threading
import time

HEADER_SIZE = 24
LIVENESS_COMMANDS = frozenset(("version", "verack", "ping", "pong", "addr"))
HEADER_COMMANDS = frozenset(("getheaders", "headers"))


def parse_frame(buffer):
    """Return (frame, size) when one complete P2P frame is buffered."""
    if len(buffer) < HEADER_SIZE:
        return None
    payload_size = struct.unpack_from("<I", buffer, 16)[0]
    if payload_size > 4_000_000:
        raise ValueError("payload exceeds 4 MiB")
    total = HEADER_SIZE + payload_size
    if len(buffer) < total:
        return None
    return bytes(buffer[:total]), total


def command_of(frame):
    return frame[4:16].split(b"\0", 1)[0].decode("ascii", "replace")


def policy_for(command, delay_ms, drop_probability):
    """Return (delay_ms, may_drop, plane) for the frozen B3-v2 profile."""
    if command in LIVENESS_COMMANDS:
        return 0.0, False, "liveness"
    if command in HEADER_COMMANDS:
        return float(delay_ms), False, "headers"
    return float(delay_ms), True, "data"


def parse_compact_size(payload, offset):
    first = payload[offset]
    if first < 253:
        return first, offset + 1
    if first == 253:
        return struct.unpack_from("<H", payload, offset + 1)[0], offset + 3
    if first == 254:
        return struct.unpack_from("<I", payload, offset + 1)[0], offset + 5
    return struct.unpack_from("<Q", payload, offset + 1)[0], offset + 9


def describe_inv_payload(frame):
    payload = frame[HEADER_SIZE:]
    try:
        count, offset = parse_compact_size(payload, 0)
    except Exception:
        return "inv_parse=error"
    hashes = []
    for _ in range(count):
        if offset + 36 > len(payload):
            return "inv_parse=truncated"
        _inv_type = struct.unpack_from("<I", payload, offset)[0]
        offset += 4
        inv_hash = payload[offset:offset + 32][::-1].hex()
        offset += 32
        hashes.append(inv_hash)
    return "count=%d hashes=%s" % (count, ",".join(hashes))


def describe_block_payload(frame):
    payload = frame[HEADER_SIZE:]
    if len(payload) < 80:
        return "block_parse=truncated"
    header = payload[:80]
    digest = hashlib.sha256(hashlib.sha256(header).digest()).digest()[::-1].hex()
    return "hash=%s" % digest


def describe_frame(command, frame):
    if command in ("getdata", "inv"):
        return describe_inv_payload(frame)
    if command == "block":
        return describe_block_payload(frame)
    return ""


class RelayStats:
    def __init__(self):
        self.lock = threading.Lock()
        self.values = {}

    def add(self, key, amount=1):
        with self.lock:
            self.values[key] = self.values.get(key, 0) + amount

    def snapshot(self):
        with self.lock:
            return dict(self.values)


class TargetedImpairment:
    def __init__(self, drop_first_block_response):
        self.lock = threading.Lock()
        self.drop_first_block_response = drop_first_block_response
        self.triggered = False

    def should_drop(self, direction, command):
        if not self.drop_first_block_response or direction != "s2c" or command != "block":
            return False
        with self.lock:
            if self.triggered:
                return False
            self.triggered = True
            return True


def relay_loop(source, target, args, rng, stats, direction, log, targeted):
    buffer = bytearray()
    try:
        while True:
            chunk = source.recv(65536)
            if not chunk:
                return
            buffer.extend(chunk)
            while True:
                parsed = parse_frame(buffer)
                if parsed is None:
                    break
                frame, size = parsed
                del buffer[:size]
                command = command_of(frame)
                delay_ms, may_drop, plane = policy_for(
                    command, args.delay_ms, args.drop_probability
                )
                stats.add("%s.%s.received" % (direction, command))
                stats.add("plane.%s.received" % plane)
                detail = describe_frame(command, frame)
                targeted_drop = targeted.should_drop(direction, command)
                if targeted_drop or (may_drop and rng.random() < args.drop_probability):
                    stats.add("%s.%s.dropped" % (direction, command))
                    stats.add("plane.%s.dropped" % plane)
                    reason = "targeted_first_block" if targeted_drop else "profile"
                    log("DROP supplier=%s direction=%s command=%s plane=%s reason=%s %s" %
                        (args.supplier_id, direction, command, plane, reason, detail))
                    continue
                if delay_ms:
                    time.sleep(delay_ms / 1000.0)
                    stats.add("%s.%s.delayed" % (direction, command))
                target.sendall(frame)
                stats.add("%s.%s.sent" % (direction, command))
                log("SEND supplier=%s direction=%s command=%s plane=%s bytes=%d %s" %
                    (args.supplier_id, direction, command, plane, size - HEADER_SIZE, detail))
    except (ConnectionError, OSError) as exc:
        log("CLOSE supplier=%s direction=%s type=%s detail=%s" %
            (args.supplier_id, direction, type(exc).__name__, exc))


def run_relay(args):
    host, port_text = args.target.rsplit(":", 1)
    stats = RelayStats()
    output_lock = threading.Lock()
    output = open(args.log, "a", buffering=1)
    targeted = TargetedImpairment(args.drop_first_block_response)

    def log(line):
        with output_lock:
            output.write("%d %s\n" % (time.time_ns() // 1000, line))

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.listen_host, args.listen_port))
    listener.listen(8)
    log("READY supplier=%s listen=%s:%d target=%s delay_ms=%g drop_pct=%g seed=%d "
        "targeted_drop_first_block=%d header_drop=0 exempt=%s" %
        (args.supplier_id, args.listen_host, args.listen_port, args.target, args.delay_ms,
         args.drop_probability * 100.0, args.seed,
         int(args.drop_first_block_response),
         ",".join(sorted(LIVENESS_COMMANDS))))
    try:
        while True:
            client, address = listener.accept()
            upstream = socket.create_connection((host, int(port_text)), timeout=10)
            client.settimeout(None)
            upstream.settimeout(None)
            log("ACCEPT supplier=%s peer=%s" % (args.supplier_id, address))
            threading.Thread(
                target=relay_loop,
                args=(client, upstream, args, random.Random(args.seed), stats,
                      "c2s", log, targeted), daemon=True).start()
            threading.Thread(
                target=relay_loop,
                args=(upstream, client, args,
                      random.Random(args.seed ^ 0x9E3779B9), stats,
                      "s2c", log, targeted), daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        log("STATS supplier=%s targeted_drop_triggered=%d %s" %
            (args.supplier_id, int(targeted.triggered),
             json.dumps(stats.snapshot(), sort_keys=True)))
        listener.close()
        output.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--listen-host", default="127.0.0.1")
    parser.add_argument("--listen-port", type=int, required=True)
    parser.add_argument("--target", required=True)
    parser.add_argument("--delay-ms", type=float, default=1000.0)
    parser.add_argument("--drop-pct", type=float, default=10.0)
    parser.add_argument("--seed", type=int, default=1337)
    parser.add_argument("--supplier-id", default="single")
    parser.add_argument("--drop-first-block-response", action="store_true")
    parser.add_argument("--log", required=True)
    args = parser.parse_args()
    if not 0 <= args.drop_pct <= 100:
        parser.error("--drop-pct must be between 0 and 100")
    args.drop_probability = args.drop_pct / 100.0
    run_relay(args)


if __name__ == "__main__":
    main()
