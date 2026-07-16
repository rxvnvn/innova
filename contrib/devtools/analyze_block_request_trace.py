#!/usr/bin/env python3
"""Summarize session-scoped BLOCKREQTRACE block-hash lifecycles.

The parser deliberately accepts arbitrary text before the BLOCKREQTRACE marker,
so it works with both timestamped debug.log files and extracted trace files.
Only the Python standard library is required.
"""

from __future__ import annotations

import argparse
import datetime as dt
import re
import shlex
import sys
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Set, TextIO, Tuple


TRACE_MARKER = "BLOCKREQTRACE"
PREFIX_TIME_RE = re.compile(
    r"(\d{4}-\d{2}-\d{2})[ T](\d{2}:\d{2}:\d{2}(?:\.\d{1,6})?)"
)
TRIGGER_EVENTS = {
    "BATCH75",
    "GETBLOCKS_QUEUE",
    "GETBLOCKS_TRIGGER",
    "PREFETCH",
    "PREFETCH_TRIGGER",
    "PIPELINE_DRAINED",
    "PIPELINE_GETBLOCKS",
    "STALL_RECOVERY",
    "STALL_TOUCH",
}
STATE_FIELD_PARTS = (
    "known",
    "indexed",
    "active_chain",
    "activechain",
    "setbestchain",
    "height",
    "result",
)


def parse_int(value: Optional[str]) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(value.rstrip(",;"), 10)
    except ValueError:
        return None


def normalize_hash(value: Optional[str]) -> Optional[str]:
    if value is None:
        return None
    normalized = value.strip().rstrip(",;").lower()
    if normalized.startswith("0x"):
        normalized = normalized[2:]
    if normalized in {"", "-", "none", "null", "unknown", "0"}:
        return None
    return normalized


def parse_prefix_time_us(prefix: str) -> Optional[int]:
    match = PREFIX_TIME_RE.search(prefix)
    if not match:
        return None
    value = f"{match.group(1)} {match.group(2)}"
    fmt = "%Y-%m-%d %H:%M:%S.%f" if "." in value else "%Y-%m-%d %H:%M:%S"
    try:
        parsed = dt.datetime.strptime(value, fmt).replace(tzinfo=dt.timezone.utc)
    except ValueError:
        return None
    return int(parsed.timestamp() * 1_000_000)


def parse_event_time_us(fields: Dict[str, str], prefix: str) -> Optional[int]:
    for name in ("ts_us", "time_us", "timestamp_us", "event_time_us"):
        value = parse_int(fields.get(name))
        if value is not None:
            return value
    for name in ("ts_ms", "time_ms", "timestamp_ms", "event_time_ms"):
        value = parse_int(fields.get(name))
        if value is not None:
            return value * 1_000
    for name in ("ts", "time", "timestamp"):
        value = parse_int(fields.get(name))
        if value is None:
            continue
        if value >= 100_000_000_000_000:
            return value
        if value >= 100_000_000_000:
            return value * 1_000
        return value * 1_000_000
    return parse_prefix_time_us(prefix)


@dataclass(frozen=True)
class TraceEvent:
    line_number: int
    event: str
    fields: Dict[str, str]
    trace_text: str
    time_us: Optional[int]
    session_index: int = 0

    @property
    def block_hash(self) -> Optional[str]:
        return normalize_hash(
            self.fields.get("hash")
            or self.fields.get("block_hash")
            or self.fields.get("blockHash")
        )

    @property
    def peer(self) -> str:
        peer_id = (
            self.fields.get("peer")
            or self.fields.get("peer_id")
            or self.fields.get("peerId")
            or self.fields.get("node_id")
            or "?"
        )
        address = (
            self.fields.get("addr")
            or self.fields.get("peer_addr")
            or self.fields.get("peer_address")
            or self.fields.get("peerAddress")
            or self.fields.get("address")
        )
        return f"{peer_id}@{address}" if address else peer_id

    def related_hashes(self) -> List[str]:
        hashes = []
        for key, value in self.fields.items():
            lower_key = key.lower()
            if lower_key == "hash" or lower_key.endswith("hash"):
                normalized = normalize_hash(value)
                if normalized is not None:
                    hashes.append(normalized)
        return hashes


def parse_trace_line(line: str, line_number: int) -> Optional[TraceEvent]:
    marker_position = line.find(TRACE_MARKER)
    if marker_position < 0:
        return None
    prefix = line[:marker_position]
    trace_text = line[marker_position:].strip()
    payload = trace_text[len(TRACE_MARKER) :].strip()
    try:
        tokens = shlex.split(payload)
    except ValueError:
        tokens = payload.split()
    fields: Dict[str, str] = {}
    for token in tokens:
        name, separator, value = token.partition("=")
        if separator and name:
            fields[name] = value
    event_name = fields.get("event", "UNKNOWN").upper()
    return TraceEvent(
        line_number=line_number,
        event=event_name,
        fields=fields,
        trace_text=trace_text,
        time_us=parse_event_time_us(fields, prefix),
    )


@dataclass
class TraceSession:
    index: int
    events: List[TraceEvent] = field(default_factory=list)
    start_event: Optional[TraceEvent] = None

    @property
    def final_summary(self) -> Optional[TraceEvent]:
        return next(
            (event for event in reversed(self.events) if event.event == "SUMMARY"),
            None,
        )

    @property
    def tracked_hashes(self) -> Set[str]:
        return {
            block_hash
            for event in self.events
            for block_hash in [event.block_hash]
            if block_hash is not None
        }

    @property
    def is_nonempty(self) -> bool:
        return any(event.event not in {"START", "SUMMARY"} for event in self.events)

    def contains_hash(self, block_hash: str) -> bool:
        return any(block_hash in event.related_hashes() for event in self.events)


def event_in_session(event: TraceEvent, session_index: int) -> TraceEvent:
    return TraceEvent(
        line_number=event.line_number,
        event=event.event,
        fields=event.fields,
        trace_text=event.trace_text,
        time_us=event.time_us,
        session_index=session_index,
    )


def parse_trace_sessions(lines: Iterable[str]) -> List[TraceSession]:
    sessions: List[TraceSession] = []
    current: Optional[TraceSession] = None
    for line_number, line in enumerate(lines, 1):
        event = parse_trace_line(line, line_number)
        if event is None:
            continue
        if event.event == "START":
            current = TraceSession(index=len(sessions) + 1)
            sessions.append(current)
        elif current is None:
            # Traces written before START was introduced remain one implicit
            # session and retain full backward compatibility.
            current = TraceSession(index=1)
            sessions.append(current)
        event = event_in_session(event, current.index)
        if event.event == "START":
            current.start_event = event
        current.events.append(event)
    return sessions


def peer_label(peer: Optional[str], address: Optional[str]) -> str:
    peer_value = peer or "?"
    return f"{peer_value}@{address}" if address else peer_value


def baseline_event_candidates(event: TraceEvent) -> List[Tuple[str, int, str]]:
    candidates: List[Tuple[str, int, str]] = []
    fields = event.fields
    request_time = parse_int(fields.get("first_request_us"))
    if request_time is not None and request_time > 0:
        candidates.append(
            (
                "ASK_SCHEDULE",
                request_time,
                peer_label(fields.get("first_peer"), fields.get("first_addr")),
            )
        )
    send_time = parse_int(fields.get("first_send_us"))
    if send_time is not None and send_time > 0:
        candidates.append(
            (
                "GETDATA_SEND",
                send_time,
                peer_label(
                    fields.get("first_send_peer"), fields.get("first_send_addr")
                ),
            )
        )
    receive_time = parse_int(fields.get("first_receive_us"))
    if receive_time is not None and receive_time > 0:
        receive_peer = peer_label(
            fields.get("first_receive_peer"), fields.get("first_receive_addr")
        )
        candidates.append(("BLOCK_RECEIVE", receive_time, receive_peer))
        if fields.get("previous_result", "unknown") != "unknown":
            candidates.append(("BLOCK_RESULT", receive_time, receive_peer))
    return candidates


@dataclass
class EventCounts:
    schedules: int = 0
    sends: int = 0
    receives: int = 0
    results: int = 0
    stall: int = 0
    getblocks: int = 0
    other: int = 0

    def observe(self, event: str) -> None:
        if event == "ASK_SCHEDULE":
            self.schedules += 1
        elif event == "GETDATA_SEND":
            self.sends += 1
        elif event == "BLOCK_RECEIVE":
            self.receives += 1
        elif event == "BLOCK_RESULT":
            self.results += 1
        elif event in {"STALL_RECOVERY", "STALL_TOUCH"}:
            self.stall += 1
        elif event in TRIGGER_EVENTS:
            self.getblocks += 1
        else:
            self.other += 1


@dataclass
class HashStats:
    schedule_events: int = 0
    send_events: int = 0
    receive_events: int = 0
    result_events: int = 0
    max_request_count: int = 0
    max_send_count: int = 0
    max_receive_count: int = 0
    peers: Dict[str, EventCounts] = field(default_factory=dict)

    def observe(self, trace_event: TraceEvent) -> None:
        counts = self.peers.setdefault(trace_event.peer, EventCounts())
        counts.observe(trace_event.event)
        for _, _, peer in baseline_event_candidates(trace_event):
            self.peers.setdefault(peer, EventCounts())
        if trace_event.event == "ASK_SCHEDULE":
            self.schedule_events += 1
        elif trace_event.event == "GETDATA_SEND":
            self.send_events += 1
        elif trace_event.event == "BLOCK_RECEIVE":
            self.receive_events += 1
        elif trace_event.event == "BLOCK_RESULT":
            self.result_events += 1

        self.max_request_count = max(
            self.max_request_count,
            parse_int(
                trace_event.fields.get("request_count")
                or trace_event.fields.get("requestCount")
            )
            or 0,
        )
        self.max_send_count = max(
            self.max_send_count,
            parse_int(
                trace_event.fields.get("send_count")
                or trace_event.fields.get("sendCount")
            )
            or 0,
        )
        self.max_receive_count = max(
            self.max_receive_count,
            parse_int(
                trace_event.fields.get("receive_count")
                or trace_event.fields.get("receiveCount")
            )
            or 0,
        )

    @property
    def request_count(self) -> int:
        return max(self.schedule_events, self.max_request_count)

    @property
    def send_count(self) -> int:
        return max(self.send_events, self.max_send_count)

    @property
    def receive_count(self) -> int:
        return max(self.receive_events, self.max_receive_count)

    @property
    def repeat_score(self) -> int:
        return max(self.request_count, self.send_count)


@dataclass
class Analysis:
    session_index: int = 0
    trace_events: int = 0
    hashes: Dict[str, HashStats] = field(default_factory=dict)
    peer_counts: Dict[str, EventCounts] = field(default_factory=dict)
    selected_events: List[TraceEvent] = field(default_factory=list)
    latest_summary: Optional[TraceEvent] = None


@dataclass(frozen=True)
class BaselineObservation:
    time_us: int
    event: str
    peer: str
    label: str


def analyze(
    events: Iterable[TraceEvent],
    selected_hash: Optional[str],
    session_index: int = 0,
) -> Analysis:
    result = Analysis(session_index=session_index)
    actual_event_keys = set()
    actual_result_hashes = set()
    inferred_event_keys = set()
    for event in events:
        result.trace_events += 1
        if event.event == "SUMMARY":
            result.latest_summary = event
        else:
            peer_counts = result.peer_counts.setdefault(event.peer, EventCounts())
            peer_counts.observe(event.event)

        if selected_hash is not None and selected_hash in event.related_hashes():
            result.selected_events.append(event)

        block_hash = event.block_hash
        if block_hash is None or (
            selected_hash is not None and block_hash != selected_hash
        ):
            continue
        if event.time_us is not None:
            actual_event_keys.add((block_hash, event.event, event.time_us))
        if event.event == "BLOCK_RESULT":
            actual_result_hashes.add(block_hash)
        for event_name, event_time, peer in baseline_event_candidates(event):
            key = (block_hash, event_name, event_time)
            if key in actual_event_keys or key in inferred_event_keys:
                continue
            if event_name == "BLOCK_RESULT" and block_hash in actual_result_hashes:
                continue
            result.peer_counts.setdefault(peer, EventCounts()).observe(event_name)
            inferred_event_keys.add(key)
        stats = result.hashes.setdefault(block_hash, HashStats())
        stats.observe(event)
    return result


def open_input(path: str) -> Tuple[TextIO, bool]:
    if path == "-":
        return sys.stdin, False
    return open(path, "r", encoding="utf-8", errors="replace"), True


def print_hash_table(rows: List[Tuple[str, HashStats]], limit: int) -> None:
    if not rows:
        print("  (none)")
        return
    print("  hash requests sends receives peers")
    for block_hash, stats in rows[:limit]:
        print(
            f"  {block_hash} {stats.request_count} {stats.send_count} "
            f"{stats.receive_count} {len(stats.peers)}"
        )


def print_peer_counts(peer_counts: Dict[str, EventCounts]) -> None:
    if not peer_counts:
        print("  (none)")
        return
    print("  peer schedules sends receives results stall getblocks other")
    for peer, counts in sorted(peer_counts.items()):
        print(
            f"  {peer} {counts.schedules} {counts.sends} {counts.receives} "
            f"{counts.results} {counts.stall} {counts.getblocks} {counts.other}"
        )


def print_session_summary(
    summary: Optional[TraceEvent], session_index: int
) -> None:
    print(f"\nSession {session_index} last SUMMARY:")
    if summary is None:
        print("  (none)")
    else:
        print(f"  line={summary.line_number} {summary.trace_text}")


def print_overview(analysis: Analysis, top: int) -> None:
    print(f"Selected session: {analysis.session_index}")
    print(f"BLOCKREQTRACE events: {analysis.trace_events}")
    print(f"Tracked hashes: {len(analysis.hashes)}")

    by_send = sorted(
        (
            (block_hash, stats)
            for block_hash, stats in analysis.hashes.items()
            if stats.send_count > 0
        ),
        key=lambda item: (
            -item[1].send_count,
            -item[1].request_count,
            -item[1].receive_count,
            item[0],
        ),
    )
    by_receive = sorted(
        (
            (block_hash, stats)
            for block_hash, stats in analysis.hashes.items()
            if stats.receive_count > 0
        ),
        key=lambda item: (
            -item[1].receive_count,
            -item[1].send_count,
            -item[1].request_count,
            item[0],
        ),
    )
    repeated = sorted(
        (
            (block_hash, stats)
            for block_hash, stats in analysis.hashes.items()
            if stats.repeat_score >= 2
        ),
        key=lambda item: (
            -item[1].repeat_score,
            -item[1].send_count,
            -item[1].receive_count,
            item[0],
        ),
    )

    print("\nHighest GETDATA_SEND send_count:")
    print_hash_table(by_send, top)
    print("\nHighest BLOCK_RECEIVE receive_count:")
    print_hash_table(by_receive, top)
    print("\nTop repeated-request hashes:")
    print_hash_table(repeated, top)
    print("\nPeer grouping:")
    print_peer_counts(analysis.peer_counts)
    print_session_summary(analysis.latest_summary, analysis.session_index)


def event_state_fields(event: TraceEvent) -> Dict[str, str]:
    return {
        name: value
        for name, value in event.fields.items()
        if any(part in name.lower() for part in STATE_FIELD_PARTS)
    }


def chronological(events: List[TraceEvent]) -> List[TraceEvent]:
    if events and all(event.time_us is not None for event in events):
        return sorted(events, key=lambda event: (event.time_us, event.line_number))
    # debug.log is append-only and therefore line order is authoritative when
    # older records do not carry a sub-second timestamp.
    return sorted(events, key=lambda event: event.line_number)


def format_timeline_event(event: TraceEvent, first_time_us: Optional[int]) -> str:
    elapsed = "?"
    if first_time_us is not None and event.time_us is not None:
        elapsed = f"{(event.time_us - first_time_us) / 1000.0:.3f}"
    timestamp = str(event.time_us) if event.time_us is not None else "?"
    trace_text = event.trace_text
    if event.event == "GETDATA_SEND" and event.fields.get("source") is not None:
        trace_text = re.sub(
            r"(?<!\S)source=\S+",
            f"last_observed_source={event.fields['source']}",
            trace_text,
            count=1,
        )
    return (
        f"  line={event.line_number} time_us={timestamp} elapsed_ms={elapsed} "
        f"{trace_text}"
    )


def first_positive_field(
    events: List[TraceEvent], field_name: str
) -> Tuple[Optional[int], Optional[TraceEvent]]:
    for event in events:
        value = parse_int(event.fields.get(field_name))
        if value is not None and value > 0:
            return value, event
    return None, None


def baseline_observations(events: List[TraceEvent]) -> List[BaselineObservation]:
    observations: List[BaselineObservation] = []

    request_time, request_event = first_positive_field(events, "first_request_us")
    if request_time is not None and request_event is not None:
        first_logged_request = next(
            (event for event in events if event.event == "ASK_SCHEDULE"), None
        )
        if first_logged_request is None or first_logged_request.time_us != request_time:
            peer = request_event.fields.get("first_peer", "?")
            address = request_event.fields.get("first_addr", "?")
            observations.append(
                BaselineObservation(
                    request_time,
                    "ASK_SCHEDULE",
                    f"{peer}@{address}" if address != "?" else peer,
                    "event=FIRST_REQUEST "
                    f"peer={peer} addr={address} "
                    f"source={request_event.fields.get('first_source', '?')} "
                    f"scheduled_us={request_event.fields.get('first_scheduled_us', '?')}",
                )
            )

    send_time, send_event = first_positive_field(events, "first_send_us")
    if send_time is not None and send_event is not None:
        first_logged_send = next(
            (event for event in events if event.event == "GETDATA_SEND"), None
        )
        if first_logged_send is None or first_logged_send.time_us != send_time:
            peer = send_event.fields.get("first_send_peer", "?")
            address = send_event.fields.get("first_send_addr", "?")
            observations.append(
                BaselineObservation(
                    send_time,
                    "GETDATA_SEND",
                    f"{peer}@{address}" if address != "?" else peer,
                    "event=FIRST_GETDATA_SEND "
                    f"peer={peer} addr={address} "
                    "last_observed_source="
                    f"{send_event.fields.get('first_send_source', '?')}",
                )
            )

    receive_time, receive_event = first_positive_field(events, "first_receive_us")
    if receive_time is not None and receive_event is not None:
        first_logged_receive = next(
            (event for event in events if event.event == "BLOCK_RECEIVE"), None
        )
        if first_logged_receive is None or first_logged_receive.time_us != receive_time:
            peer = receive_event.fields.get("first_receive_peer", "?")
            address = receive_event.fields.get("first_receive_addr", "?")
            peer_label = f"{peer}@{address}" if address != "?" else peer
            observations.append(
                BaselineObservation(
                    receive_time,
                    "BLOCK_RECEIVE",
                    peer_label,
                    "event=FIRST_BLOCK_RECEIVE "
                    f"peer={peer} addr={address}",
                )
            )
            previous_result = receive_event.fields.get("previous_result")
            if previous_result and previous_result != "unknown":
                observations.append(
                    BaselineObservation(
                        receive_time,
                        "BLOCK_RESULT",
                        peer_label,
                        "event=FIRST_BLOCK_RESULT "
                        f"result={previous_result} "
                        f"indexed={receive_event.fields.get('previous_indexed', '?')} "
                        f"active={receive_event.fields.get('previous_active', '?')} "
                        f"height={receive_event.fields.get('previous_height', '?')}",
                    )
                )

    return sorted(observations, key=lambda item: (item.time_us, item.label))


def format_baseline(
    observation: BaselineObservation, first_time_us: Optional[int]
) -> str:
    elapsed = "?"
    if first_time_us is not None:
        elapsed = f"{(observation.time_us - first_time_us) / 1000.0:.3f}"
    return (
        f"  time_us={observation.time_us} elapsed_ms={elapsed} "
        f"synthetic=1 {observation.label}"
    )


def print_selected(analysis: Analysis, selected_hash: str) -> None:
    stats = analysis.hashes.get(selected_hash, HashStats())
    events = chronological(analysis.selected_events)
    baselines = baseline_observations(events)
    first_request = next(
        (observation for observation in baselines
         if "event=FIRST_REQUEST " in observation.label),
        None,
    )
    first_logged_request = next(
        (event for event in events if event.event == "ASK_SCHEDULE"),
        next((event for event in events if event.event == "GETDATA_SEND"), None),
    )
    first_time_us = (
        first_request.time_us
        if first_request is not None
        else first_logged_request.time_us
        if first_logged_request is not None
        else None
    )
    selected_peer_counts: Dict[str, EventCounts] = {}
    for event in events:
        counts = selected_peer_counts.setdefault(event.peer, EventCounts())
        counts.observe(event.event)
    for observation in baselines:
        counts = selected_peer_counts.setdefault(observation.peer, EventCounts())
        counts.observe(observation.event)

    result_count = stats.result_events + sum(
        observation.event == "BLOCK_RESULT" for observation in baselines
    )

    print(f"Selected session: {analysis.session_index}")
    print(f"Hash: {selected_hash}")
    print(f"Related events: {len(events)}")
    print(
        "Aggregate: "
        f"requests={stats.request_count} sends={stats.send_count} "
        f"receives={stats.receive_count} results={result_count} "
        f"duplicate_requests={max(0, stats.request_count - 1)} "
        f"duplicate_sends={max(0, stats.send_count - 1)} "
        f"duplicate_receives={max(0, stats.receive_count - 1)}"
    )
    print(
        "Source attribution: GETDATA_SEND last_observed_source (stored as "
        "source in the trace) and first_send_source are hash-state observations "
        "captured at send time; they do not identify the exact due mapAskFor "
        "entry that was sent."
    )

    print("\nFirst request:")
    if first_request is not None:
        print(format_baseline(first_request, first_time_us))
    elif first_logged_request is None:
        print("  (none)")
    else:
        print(format_timeline_event(first_logged_request, first_time_us))

    print("\nAll GETDATA sends:")
    send_baselines = [
        item for item in baselines if "GETDATA_SEND" in item.label
    ]
    send_events = [event for event in events if event.event == "GETDATA_SEND"]
    if not send_baselines and not send_events:
        print("  (none)")
    else:
        for item in send_baselines:
            print(format_baseline(item, first_time_us))
        for event in send_events:
            print(format_timeline_event(event, first_time_us))

    print("\nAll block receives:")
    receive_baselines = [
        item for item in baselines if "BLOCK_RECEIVE" in item.label
    ]
    receive_events = [
        event for event in events if event.event == "BLOCK_RECEIVE"
    ]
    if not receive_baselines and not receive_events:
        print("  (none)")
    else:
        for item in receive_baselines:
            print(format_baseline(item, first_time_us))
        for event in receive_events:
            print(format_timeline_event(event, first_time_us))

    print("\nPeer grouping:")
    print_peer_counts(selected_peer_counts)

    print("\nKnown/indexed transitions:")
    transitions = [event for event in events if event_state_fields(event)]
    if not transitions:
        print("  (none)")
    else:
        for event in transitions:
            state = " ".join(
                f"{name}={value}"
                for name, value in sorted(event_state_fields(event).items())
            )
            print(f"  line={event.line_number} event={event.event} {state}")

    print("\nStall/prefetch/getblocks links:")
    linked_triggers = [event for event in events if event.event in TRIGGER_EVENTS]
    if not linked_triggers:
        print("  (none)")
    else:
        for event in linked_triggers:
            print(format_timeline_event(event, first_time_us))

    print("\nChronology (includes every GETDATA_SEND and BLOCK_RECEIVE):")
    if not events and not baselines:
        print("  (none)")
    else:
        timeline: List[Tuple[int, int, object]] = []
        for item in baselines:
            timeline.append((item.time_us, 0, item))
        for event in events:
            sort_time = event.time_us if event.time_us is not None else sys.maxsize
            timeline.append((sort_time, event.line_number, event))
        for _, _, item in sorted(timeline, key=lambda row: (row[0], row[1])):
            if isinstance(item, BaselineObservation):
                print(format_baseline(item, first_time_us))
            else:
                print(format_timeline_event(item, first_time_us))

    print_session_summary(analysis.latest_summary, analysis.session_index)


def print_session_list(
    sessions: List[TraceSession], selected_indices: Set[int]
) -> None:
    print("Trace sessions:")
    if not sessions:
        print("  (none)")
        return
    print(
        "  session start_line start_time_us events tracked_hashes "
        "final_summary selected"
    )
    for session in sessions:
        start_line = (
            str(session.start_event.line_number)
            if session.start_event is not None
            else "-"
        )
        start_time = (
            str(session.start_event.time_us)
            if session.start_event is not None
            and session.start_event.time_us is not None
            else "-"
        )
        summary = session.final_summary
        summary_status = (
            f"yes(line={summary.line_number})" if summary is not None else "no"
        )
        print(
            f"  {session.index} {start_line} {start_time} "
            f"{len(session.events)} {len(session.tracked_hashes)} "
            f"{summary_status} "
            f"{'yes' if session.index in selected_indices else 'no'}"
        )


def default_session(sessions: List[TraceSession]) -> Optional[TraceSession]:
    nonempty = [session for session in sessions if session.is_nonempty]
    if nonempty:
        return nonempty[-1]
    return sessions[-1] if sessions else None


def find_session(
    sessions: List[TraceSession], session_index: int
) -> Optional[TraceSession]:
    return next(
        (session for session in sessions if session.index == session_index),
        None,
    )


def positive_int(value: str) -> int:
    parsed = int(value)
    if parsed <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return parsed


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Analyze Innova BLOCKREQTRACE lifecycle events."
    )
    parser.add_argument("logfile", help="debug.log path, or - for standard input")
    parser.add_argument(
        "--top",
        type=positive_int,
        default=20,
        metavar="N",
        help="number of hashes shown in overview tables (default: 20)",
    )
    parser.add_argument(
        "--hash",
        dest="selected_hash",
        help="print the complete timeline related to this block hash",
    )
    parser.add_argument(
        "--session",
        type=positive_int,
        metavar="N",
        help="analyze only trace session N (sessions are numbered from 1)",
    )
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    selected_hash = normalize_hash(args.selected_hash)
    if args.selected_hash is not None and (
        selected_hash is None
        or re.fullmatch(r"[0-9a-f]{64}", selected_hash) is None
    ):
        print("error: --hash must be 64 hexadecimal characters", file=sys.stderr)
        return 2

    try:
        stream, should_close = open_input(args.logfile)
    except OSError as error:
        print(f"error: cannot read {args.logfile}: {error}", file=sys.stderr)
        return 1
    try:
        sessions = parse_trace_sessions(stream)
    finally:
        if should_close:
            stream.close()

    requested_session = (
        find_session(sessions, args.session) if args.session is not None else None
    )
    matching_sessions = (
        [
            session
            for session in sessions
            if selected_hash is not None and session.contains_hash(selected_hash)
        ]
        if selected_hash is not None
        else []
    )

    if args.session is not None:
        selected_sessions = [requested_session] if requested_session else []
    elif selected_hash is not None:
        selected_sessions = matching_sessions
    else:
        selected = default_session(sessions)
        selected_sessions = [selected] if selected is not None else []

    print_session_list(
        sessions,
        {session.index for session in selected_sessions},
    )

    if not sessions:
        print("error: no BLOCKREQTRACE events found", file=sys.stderr)
        return 1
    if args.session is not None and requested_session is None:
        print(f"error: trace session {args.session} does not exist", file=sys.stderr)
        return 2

    if selected_hash is not None:
        if len(matching_sessions) > 1:
            indices = ", ".join(str(session.index) for session in matching_sessions)
            suffix = (
                f" Selected session {args.session} only."
                if args.session is not None
                else " Reports are shown separately; use --session N to select one."
            )
            print(
                f"\nNotice: hash {selected_hash} appears in multiple sessions: "
                f"{indices}.{suffix}"
            )
        if not matching_sessions:
            print(
                f"error: hash {selected_hash} is not present in any session",
                file=sys.stderr,
            )
            return 1
        if requested_session is not None and not requested_session.contains_hash(
            selected_hash
        ):
            print(
                f"error: hash {selected_hash} is not present in session "
                f"{requested_session.index}",
                file=sys.stderr,
            )
            return 1

        for position, session in enumerate(selected_sessions):
            if position or len(sessions) > 1:
                print(f"\n=== Session {session.index} hash timeline ===")
            analysis = analyze(
                session.events,
                selected_hash,
                session_index=session.index,
            )
            print_selected(analysis, selected_hash)
    else:
        session = selected_sessions[0]
        if args.session is None:
            print(f"\nDefaulting to last nonempty session: {session.index}")
        print()
        analysis = analyze(session.events, None, session_index=session.index)
        print_overview(analysis, args.top)
    return 0


if __name__ == "__main__":
    sys.exit(main())
