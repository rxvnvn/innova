#!/usr/bin/env python3
"""Deterministic, framed, two-plane impairment relay for B3-v2."""

import argparse
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


def relay_loop(source, target, args, rng, stats, direction, log):
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
                if may_drop and rng.random() < args.drop_probability:
                    stats.add("%s.%s.dropped" % (direction, command))
                    stats.add("plane.%s.dropped" % plane)
                    log("DROP direction=%s command=%s plane=%s" %
                        (direction, command, plane))
                    continue
                if delay_ms:
                    time.sleep(delay_ms / 1000.0)
                    stats.add("%s.%s.delayed" % (direction, command))
                target.sendall(frame)
                stats.add("%s.%s.sent" % (direction, command))
                log("SEND direction=%s command=%s plane=%s bytes=%d" %
                    (direction, command, plane, size - HEADER_SIZE))
    except (ConnectionError, OSError) as exc:
        log("CLOSE direction=%s type=%s detail=%s" %
            (direction, type(exc).__name__, exc))


def run_relay(args):
    host, port_text = args.target.rsplit(":", 1)
    stats = RelayStats()
    output_lock = threading.Lock()
    output = open(args.log, "a", buffering=1)

    def log(line):
        with output_lock:
            output.write("%d %s\n" % (time.time_ns() // 1000, line))

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.listen_host, args.listen_port))
    listener.listen(8)
    log("READY listen=%s:%d target=%s delay_ms=%g drop_pct=%g seed=%d "
        "header_drop=0 exempt=%s" %
        (args.listen_host, args.listen_port, args.target, args.delay_ms,
         args.drop_probability * 100.0, args.seed,
         ",".join(sorted(LIVENESS_COMMANDS))))
    try:
        while True:
            client, address = listener.accept()
            upstream = socket.create_connection((host, int(port_text)), timeout=10)
            client.settimeout(None)
            upstream.settimeout(None)
            log("ACCEPT peer=%s" % (address,))
            threading.Thread(
                target=relay_loop,
                args=(client, upstream, args, random.Random(args.seed), stats,
                      "c2s", log), daemon=True).start()
            threading.Thread(
                target=relay_loop,
                args=(upstream, client, args,
                      random.Random(args.seed ^ 0x9E3779B9), stats,
                      "s2c", log), daemon=True).start()
    except KeyboardInterrupt:
        pass
    finally:
        log("STATS %s" % json.dumps(stats.snapshot(), sort_keys=True))
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
    parser.add_argument("--log", required=True)
    args = parser.parse_args()
    if not 0 <= args.drop_pct <= 100:
        parser.error("--drop-pct must be between 0 and 100")
    args.drop_probability = args.drop_pct / 100.0
    run_relay(args)


if __name__ == "__main__":
    main()
