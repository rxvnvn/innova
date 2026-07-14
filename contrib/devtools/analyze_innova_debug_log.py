#!/usr/bin/env python3
"""Analyze Innova P2P sync diagnostics by peer connection session.

Older logs only contain periodic PEERSTATE counters.  Newer diagnostic logs
may also contain GETHEADERS_TRACE/HEADERS_TRACE records, which make exact
response-range and interval-overlap analysis possible.
"""

from __future__ import annotations

import argparse
import collections
import datetime as dt
import os
import re
import sys
from dataclasses import dataclass, field
from typing import Counter, Dict, Iterable, List, Optional, TextIO, Tuple


HEADER_SERIALIZED_SIZE = 82

TS_PATTERNS = (
    re.compile(r'^(\d{4}-\d{2}-\d{2})[ T](\d{2}:\d{2}:\d{2})'),
    re.compile(r'^\[(\d{4}-\d{2}-\d{2})[ T](\d{2}:\d{2}:\d{2})\]'),
    re.compile(r'^(\d{2}\.\d{2}\.\d{4})[ T](\d{2}:\d{2}:\d{2})'),
    re.compile(r'^\[(\d{2}\.\d{2}\.\d{4})[ T](\d{2}:\d{2}:\d{2})\]'),
)
KEYVAL_RE = re.compile(r'([A-Za-z0-9_]+)=([^ \t\r\n]+)')
COMMAND_STAT_RE = re.compile(r'([A-Za-z0-9_:-]+)=(\d+)/(\d+)')
PEERSTATE_RE = re.compile(r'^PEERSTATE:\s+(.*?)\s+in\{(.*?)\}\s+out\{(.*?)\}\s*$')
PEER_ADDRESS_RE = re.compile(r'(?:(?:addr|peer)=)([^\s,}]+)')
DISCONNECT_RE = re.compile(
    r'(?:DEBUG-DISCONNECT|Disconnecting node|socket.*closed.*peer|recv-error)',
    re.I,
)


def has_marker(line: str, marker: str) -> bool:
    return re.search(r'(?:^|\s)' + re.escape(marker) + r':', line) is not None


def periodic_diagnostic_line(line: str) -> str:
    """Remove an optional log timestamp before periodic diagnostic records."""
    for marker in ('SYNCSTATE', 'GLOBAL_P2PMSG', 'PEERSTATE'):
        match = re.search(
            r'(?:^|\s)(' + re.escape(marker) + r':.*)$',
            line.rstrip('\n'),
        )
        if match:
            return match.group(1)
    return line


def parse_timestamp(line: str) -> Optional[dt.datetime]:
    for pattern in TS_PATTERNS:
        match = pattern.search(line)
        if not match:
            continue
        for fmt in ('%Y-%m-%d %H:%M:%S', '%d.%m.%Y %H:%M:%S'):
            try:
                return dt.datetime.strptime(f'{match.group(1)} {match.group(2)}', fmt)
            except ValueError:
                pass
    return None


def parse_keyvals(line: str) -> Dict[str, str]:
    return {match.group(1): match.group(2) for match in KEYVAL_RE.finditer(line)}


def parse_command_stats(blob: str) -> Dict[str, Tuple[int, int]]:
    return {
        command: (int(count), int(bytes_))
        for command, count, bytes_ in COMMAND_STAT_RE.findall(blob)
    }


def parse_int(value: Optional[str]) -> Optional[int]:
    if value is None:
        return None
    try:
        return int(value.rstrip(',}'))
    except ValueError:
        return None


def first_int(values: Dict[str, str], *names: str) -> Optional[int]:
    for name in names:
        value = parse_int(values.get(name))
        if value is not None:
            return value
    return None


def first_value(values: Dict[str, str], *names: str) -> Optional[str]:
    for name in names:
        value = values.get(name)
        if value is not None:
            return value.rstrip(',}')
    return None


def update_command_stats(
    destination: Dict[str, Tuple[int, int]],
    source: Dict[str, Tuple[int, int]],
) -> None:
    for command, (count, bytes_) in source.items():
        old_count, old_bytes = destination.get(command, (0, 0))
        destination[command] = (max(old_count, count), max(old_bytes, bytes_))


def unique_header_payload_bound(height: int) -> int:
    """Conservative payload bound for one canonical header through height."""
    if height < 0:
        return 0
    # Each non-empty headers vector has at most one CompactSize byte per
    # header (the worst case is one header per message).  Include genesis to
    # keep this fallback estimate conservative when only a peer height is
    # available and exact ranges were not logged.
    return (height + 1) * (HEADER_SERIALIZED_SIZE + 1)


def interval_overlap(intervals: List[Tuple[int, int]], start: int, end: int) -> int:
    overlap = 0
    for old_start, old_end in intervals:
        left = max(start, old_start)
        right = min(end, old_end)
        if left <= right:
            overlap += right - left + 1
    return overlap


def add_interval(intervals: List[Tuple[int, int]], start: int, end: int) -> None:
    merged: List[Tuple[int, int]] = []
    inserted = False
    for old_start, old_end in intervals:
        if old_end + 1 < start:
            merged.append((old_start, old_end))
        elif end + 1 < old_start:
            if not inserted:
                merged.append((start, end))
                inserted = True
            merged.append((old_start, old_end))
        else:
            start = min(start, old_start)
            end = max(end, old_end)
    if not inserted:
        merged.append((start, end))
    intervals[:] = merged


@dataclass(frozen=True)
class ResponseRange:
    first_hash: str
    last_hash: str
    first_height: Optional[int]
    last_height: Optional[int]

    def key(self) -> Tuple[str, str, Optional[int], Optional[int]]:
        if self.first_hash != 'none' or self.last_hash != 'none':
            return (self.first_hash, self.last_hash, None, None)
        return (self.first_hash, self.last_hash, self.first_height, self.last_height)

    def describe(self) -> str:
        heights = 'unknown'
        if self.first_height is not None and self.last_height is not None:
            heights = f'{self.first_height}-{self.last_height}'
        return f'heights={heights} first={self.first_hash} last={self.last_hash}'


@dataclass
class PeerSession:
    address: str
    peer_id: str
    first_line: int
    last_line: int
    subver: str = ''
    protocol_version: Optional[int] = None
    max_peer_height: int = -1
    peerstate_getheaders_requests: int = 0
    peerstate_headers_responses: int = 0
    peerstate_headers_bytes: int = 0
    peerstate_block_responses: int = 0
    peerstate_block_bytes: int = 0
    trace_getheaders_lines: int = 0
    trace_getheaders_sequence: int = 0
    trace_headers_lines: int = 0
    trace_headers_sequence: int = 0
    trace_headers_bytes: int = 0
    trace_headers_count: int = 0
    trace_headers_count_max: int = 0
    trace_headers_bytes_max: int = 0
    trace_new_headers: int = 0
    trace_known_headers: int = 0
    trace_known_headers_bytes: int = 0
    trace_connected_headers: int = 0
    trace_orphan_headers: int = 0
    suppressed_getheaders: int = 0
    range_counts: Counter[Tuple[str, str, Optional[int], Optional[int]]] = field(
        default_factory=collections.Counter
    )
    ranges: Dict[Tuple[str, str, Optional[int], Optional[int]], ResponseRange] = field(
        default_factory=dict
    )
    seen_intervals: List[Tuple[int, int]] = field(default_factory=list)
    overlap_duplicate_headers: int = 0
    overlap_duplicate_bytes: int = 0
    disconnected: bool = False

    @property
    def label(self) -> str:
        return f'{self.address}#{self.peer_id}'

    @property
    def getheaders_requests(self) -> int:
        trace_requests = max(self.trace_getheaders_lines, self.trace_getheaders_sequence)
        return max(self.peerstate_getheaders_requests, trace_requests)

    @property
    def headers_responses(self) -> int:
        trace_responses = max(self.trace_headers_lines, self.trace_headers_sequence)
        return max(self.peerstate_headers_responses, trace_responses)

    @property
    def headers_bytes(self) -> int:
        return max(self.peerstate_headers_bytes, self.trace_headers_bytes)

    @property
    def unique_response_ranges(self) -> int:
        return len(self.range_counts)

    @property
    def repeated_response_ranges(self) -> int:
        return sum(max(0, count - 1) for count in self.range_counts.values())

    @property
    def repeated_range_keys(self) -> int:
        return sum(1 for count in self.range_counts.values() if count > 1)

    @property
    def fallback_unique_bytes_bound(self) -> int:
        return unique_header_payload_bound(self.max_peer_height)

    @property
    def fallback_duplicate_bytes(self) -> int:
        return max(0, self.headers_bytes - self.fallback_unique_bytes_bound)

    def observe_response(
        self,
        response_range: ResponseRange,
        headers_count: int,
        bytes_: int,
        range_contiguous: bool,
    ) -> None:
        key = response_range.key()
        already_seen_exact = self.range_counts[key]
        self.range_counts[key] += 1
        previous_range = self.ranges.get(key)
        if previous_range is None or (
            (previous_range.first_height is None or previous_range.last_height is None)
            and response_range.first_height is not None
            and response_range.last_height is not None
        ):
            self.ranges[key] = response_range

        first_height = response_range.first_height
        last_height = response_range.last_height
        if already_seen_exact:
            self.overlap_duplicate_headers += headers_count
            self.overlap_duplicate_bytes += bytes_
        elif range_contiguous and first_height is not None and last_height is not None:
            start, end = sorted((first_height, last_height))
            span = end - start + 1
            overlap_heights = interval_overlap(self.seen_intervals, start, end)
            if headers_count > 0 and span > 0:
                duplicate_headers = min(
                    headers_count,
                    (headers_count * overlap_heights) // span,
                )
            else:
                duplicate_headers = overlap_heights
            duplicate_bytes = (
                (bytes_ * duplicate_headers) // headers_count
                if bytes_ > 0 and headers_count > 0
                else duplicate_headers * HEADER_SERIALIZED_SIZE
            )
            self.overlap_duplicate_headers += duplicate_headers
            self.overlap_duplicate_bytes += duplicate_bytes
        if range_contiguous and first_height is not None and last_height is not None:
            start, end = sorted((first_height, last_height))
            add_interval(self.seen_intervals, start, end)


@dataclass
class Analysis:
    total_lines: int = 0
    first_timestamp: Optional[dt.datetime] = None
    last_timestamp: Optional[dt.datetime] = None
    syncstate_last: Dict[str, str] = field(default_factory=dict)
    global_incoming: Dict[str, Tuple[int, int]] = field(default_factory=dict)
    global_outgoing: Dict[str, Tuple[int, int]] = field(default_factory=dict)
    sessions: Dict[str, PeerSession] = field(default_factory=dict)
    current_session_by_address: Dict[str, str] = field(default_factory=dict)

    def get_session(self, address: str, peer_id: str, line_number: int) -> PeerSession:
        label = f'{address}#{peer_id}'
        session = self.sessions.get(label)
        if session is None:
            trace_label = f'{address}#trace'
            if peer_id != 'trace' and self.current_session_by_address.get(address) == trace_label:
                session = self.sessions.pop(trace_label)
                session.peer_id = peer_id
                self.sessions[label] = session
            else:
                session = PeerSession(address, peer_id, line_number, line_number)
                self.sessions[label] = session
        session.last_line = line_number
        self.current_session_by_address[address] = label
        return session

    def session_for_trace(self, values: Dict[str, str], line_number: int) -> PeerSession:
        address = first_value(values, 'addr', 'peer') or 'unknown'
        if address.startswith('id='):
            address = first_value(values, 'addr') or 'unknown'
        peer_id = first_value(values, 'peer_id', 'id')
        if peer_id is not None:
            return self.get_session(address, peer_id, line_number)
        current_label = self.current_session_by_address.get(address)
        if current_label is not None:
            session = self.sessions[current_label]
            session.last_line = line_number
            return session
        return self.get_session(address, 'trace', line_number)


def parse_peerstate(analysis: Analysis, line: str, line_number: int) -> None:
    match = PEERSTATE_RE.match(line.rstrip('\n'))
    if not match:
        return
    peer_values = parse_keyvals(match.group(1))
    incoming = parse_command_stats(match.group(2))
    outgoing = parse_command_stats(match.group(3))
    address = peer_values.get('addr', 'unknown')
    peer_id = peer_values.get('id', 'unknown')
    session = analysis.get_session(address, peer_id, line_number)
    session.subver = peer_values.get('subver', session.subver)
    version = parse_int(peer_values.get('ver'))
    if version is not None:
        session.protocol_version = version
    for name in ('h', 'best', 'adv'):
        height = parse_int(peer_values.get(name))
        if height is not None:
            session.max_peer_height = max(session.max_peer_height, height)
    if 'getheaders' in outgoing:
        session.peerstate_getheaders_requests = max(
            session.peerstate_getheaders_requests, outgoing['getheaders'][0]
        )
    if 'headers' in incoming:
        session.peerstate_headers_responses = max(
            session.peerstate_headers_responses, incoming['headers'][0]
        )
        session.peerstate_headers_bytes = max(
            session.peerstate_headers_bytes, incoming['headers'][1]
        )
    if 'block' in incoming:
        session.peerstate_block_responses = max(
            session.peerstate_block_responses, incoming['block'][0]
        )
        session.peerstate_block_bytes = max(session.peerstate_block_bytes, incoming['block'][1])


def parse_getheaders_trace(analysis: Analysis, line: str, line_number: int) -> None:
    values = parse_keyvals(line)
    # A server-side GETHEADERS_SEND has response fields but no local_height.
    if 'GETHEADERS_SEND' in line and 'headers' in values and 'local_height' not in values:
        return
    session = analysis.session_for_trace(values, line_number)
    action = first_value(values, 'action')
    if 'GETHEADERS_SUPPRESSED' in line or action == 'suppress':
        session.suppressed_getheaders += 1
        return
    session.trace_getheaders_lines += 1
    sequence = first_int(values, 'request_sequence', 'peer_sent', 'sequence', 'seq')
    if sequence is not None:
        session.trace_getheaders_sequence = max(session.trace_getheaders_sequence, sequence)


def parse_headers_trace(analysis: Analysis, line: str, line_number: int) -> None:
    values = parse_keyvals(line)
    session = analysis.session_for_trace(values, line_number)
    headers_count = first_int(values, 'headers_count', 'headers') or 0
    bytes_ = first_int(values, 'headers_bytes', 'bytes') or 0
    known_headers = first_int(
        values, 'already_known_count', 'already_known', 'duplicate'
    ) or 0
    sequence = first_int(values, 'response_sequence', 'diag', 'sequence', 'seq')
    session.trace_headers_lines += 1
    if sequence is not None:
        session.trace_headers_sequence = max(session.trace_headers_sequence, sequence)
    session.trace_headers_bytes += bytes_
    session.trace_headers_count += headers_count
    session.trace_headers_count_max = max(session.trace_headers_count_max, headers_count)
    session.trace_headers_bytes_max = max(session.trace_headers_bytes_max, bytes_)
    session.trace_new_headers += first_int(values, 'new_headers_count', 'new') or 0
    session.trace_known_headers += known_headers
    if headers_count > 0:
        session.trace_known_headers_bytes += (bytes_ * known_headers) // headers_count
    session.trace_connected_headers += first_int(values, 'connected_count', 'connected') or 0
    session.trace_orphan_headers += first_int(
        values, 'orphan_count', 'orphan', 'unknown_parent'
    ) or 0

    first_hash = first_value(values, 'first_hash') or 'none'
    last_hash = first_value(values, 'last_hash') or 'none'
    first_height = first_int(values, 'first_height', 'first_height_known')
    last_height = first_int(values, 'last_height', 'last_height_known')
    if first_height is not None and first_height < 0:
        first_height = None
    if last_height is not None and last_height < 0:
        last_height = None
    range_contiguous_value = first_int(values, 'range_contiguous')
    range_contiguous = (
        range_contiguous_value != 0 if range_contiguous_value is not None else True
    )
    if first_hash != 'none' or last_hash != 'none' or (
        first_height is not None and last_height is not None
    ):
        session.observe_response(
            ResponseRange(first_hash, last_hash, first_height, last_height),
            headers_count,
            bytes_,
            range_contiguous,
        )


def mark_disconnect(analysis: Analysis, line: str) -> None:
    if not DISCONNECT_RE.search(line):
        return
    match = PEER_ADDRESS_RE.search(line)
    if not match:
        return
    address = match.group(1)
    label = analysis.current_session_by_address.get(address)
    if label is not None:
        analysis.sessions[label].disconnected = True
        del analysis.current_session_by_address[address]


def analyze_stream(lines: Iterable[str]) -> Analysis:
    analysis = Analysis()
    for line_number, line in enumerate(lines, 1):
        analysis.total_lines = line_number
        timestamp = parse_timestamp(line)
        if timestamp is not None:
            if analysis.first_timestamp is None:
                analysis.first_timestamp = timestamp
            analysis.last_timestamp = timestamp

        diagnostic_line = periodic_diagnostic_line(line)
        if diagnostic_line.startswith('SYNCSTATE:'):
            analysis.syncstate_last = parse_keyvals(diagnostic_line)
        elif diagnostic_line.startswith('GLOBAL_P2PMSG:'):
            payload = diagnostic_line.split(':', 1)[1]
            stats = parse_command_stats(payload)
            if 'incoming' in payload:
                update_command_stats(analysis.global_incoming, stats)
            elif 'outgoing' in payload:
                update_command_stats(analysis.global_outgoing, stats)
        elif diagnostic_line.startswith('PEERSTATE:'):
            parse_peerstate(analysis, diagnostic_line, line_number)

        if any(
            has_marker(line, marker)
            for marker in ('GETHEADERS_TRACE', 'GETHEADERS_SEND', 'GETHEADERS_SUPPRESSED')
        ):
            parse_getheaders_trace(analysis, line, line_number)
        if has_marker(line, 'HEADERS_TRACE') or has_marker(line, 'HEADERS_RECV'):
            parse_headers_trace(analysis, line, line_number)
        mark_disconnect(analysis, line)
    return analysis


def analyze_file(path: str) -> Analysis:
    with open(path, 'r', errors='replace') as handle:
        return analyze_stream(handle)


def selected_sessions(analysis: Analysis, peer_filter: Optional[str]) -> List[PeerSession]:
    sessions = sorted(
        analysis.sessions.values(),
        key=lambda session: (session.address, session.first_line, session.peer_id),
    )
    if not peer_filter:
        return sessions
    needle = peer_filter.lower()
    return [
        session
        for session in sessions
        if needle in session.label.lower()
        or needle in session.address.lower()
        or needle in session.subver.lower()
    ]


def format_command_stats(stats: Dict[str, Tuple[int, int]]) -> str:
    if not stats:
        return 'none'
    return ' '.join(
        f'{command}={count}/{bytes_}'
        for command, (count, bytes_) in sorted(stats.items())
    )


def render_analysis(
    analysis: Analysis,
    peer_filter: Optional[str] = None,
    output: TextIO = sys.stdout,
) -> None:
    print(f'total lines: {analysis.total_lines}', file=output)
    if analysis.first_timestamp is not None and analysis.last_timestamp is not None:
        print(
            'time range: '
            f'{analysis.first_timestamp.isoformat(sep=" ")} -> '
            f'{analysis.last_timestamp.isoformat(sep=" ")}',
            file=output,
        )
    else:
        print('time range: unavailable', file=output)
    print(f'global incoming: {format_command_stats(analysis.global_incoming)}', file=output)
    print(f'global outgoing: {format_command_stats(analysis.global_outgoing)}', file=output)
    if analysis.syncstate_last:
        print(
            'last syncstate: '
            + ' '.join(
                f'{name}={analysis.syncstate_last[name]}'
                for name in (
                    'local_height',
                    'best_header',
                    'initialblockdownload',
                    'connections',
                    'datareceived',
                )
                if name in analysis.syncstate_last
            ),
            file=output,
        )

    sessions = selected_sessions(analysis, peer_filter)
    print(f'peer sessions: {len(sessions)}', file=output)
    for session in sessions:
        average_response_bytes = (
            session.headers_bytes / session.headers_responses
            if session.headers_responses
            else 0.0
        )
        average_trace_headers = (
            session.trace_headers_count / session.trace_headers_lines
            if session.trace_headers_lines
            else 0.0
        )
        average_trace_bytes = (
            session.trace_headers_bytes / session.trace_headers_lines
            if session.trace_headers_lines
            else 0.0
        )
        print(
            f'peer session: {session.label} lines={session.first_line}-{session.last_line} '
            f'subver={session.subver or "unknown"} version={session.protocol_version} '
            f'height={session.max_peer_height} disconnected={int(session.disconnected)}',
            file=output,
        )
        print(f'  getheaders_requests: {session.getheaders_requests}', file=output)
        print(f'  getheaders_suppressed_trace: {session.suppressed_getheaders}', file=output)
        print(f'  headers_responses: {session.headers_responses}', file=output)
        print(f'  headers_bytes: {session.headers_bytes}', file=output)
        print(
            f'  headers_bytes_per_response_average: {average_response_bytes:.2f}',
            file=output,
        )
        print(
            f'  blocks_responses_bytes: {session.peerstate_block_responses}/'
            f'{session.peerstate_block_bytes}',
            file=output,
        )
        print(f'  unique_response_ranges: {session.unique_response_ranges}', file=output)
        print(f'  repeated_response_ranges: {session.repeated_response_ranges}', file=output)
        print(f'  repeated_range_keys: {session.repeated_range_keys}', file=output)
        print(f'  trace_headers_count: {session.trace_headers_count}', file=output)
        print(
            f'  trace_headers_per_response_average: {average_trace_headers:.2f}',
            file=output,
        )
        print(
            f'  trace_headers_per_response_max: {session.trace_headers_count_max}',
            file=output,
        )
        print(
            f'  trace_headers_bytes_per_response_average: {average_trace_bytes:.2f}',
            file=output,
        )
        print(
            f'  trace_headers_bytes_per_response_max: {session.trace_headers_bytes_max}',
            file=output,
        )
        print(f'  trace_new_headers: {session.trace_new_headers}', file=output)
        print(f'  trace_already_known_headers: {session.trace_known_headers}', file=output)
        print(
            f'  trace_already_known_bytes_estimate: '
            f'{session.trace_known_headers_bytes}',
            file=output,
        )
        print(f'  trace_connected_headers: {session.trace_connected_headers}', file=output)
        print(f'  trace_orphan_headers: {session.trace_orphan_headers}', file=output)
        print(
            f'  overlap_duplicate_headers_estimate: {session.overlap_duplicate_headers}',
            file=output,
        )
        print(
            f'  overlap_duplicate_bytes_estimate: {session.overlap_duplicate_bytes}',
            file=output,
        )
        print(
            f'  fallback_unique_headers_bytes_bound: {session.fallback_unique_bytes_bound}',
            file=output,
        )
        print(
            f'  fallback_duplicate_bytes_estimate: {session.fallback_duplicate_bytes}',
            file=output,
        )
        print(
            '  fallback_estimate_assumption: one canonical header per height',
            file=output,
        )
        repeated = sorted(
            (
                (count, session.ranges[key])
                for key, count in session.range_counts.items()
                if count > 1
            ),
            key=lambda item: (-item[0], item[1].describe()),
        )
        for count, response_range in repeated[:10]:
            print(
                f'  repeated_range: occurrences={count} {response_range.describe()}',
                file=output,
            )


def parse_args(argv: Optional[List[str]] = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description='Analyze Innova debug.log header-sync traffic by peer session.'
    )
    parser.add_argument('logfile', help='Path to debug.log')
    parser.add_argument(
        '--peer',
        help='Only show sessions whose address, id, or subver contains this value.',
    )
    return parser.parse_args(argv)


def main(argv: Optional[List[str]] = None) -> int:
    args = parse_args(argv)
    if not os.path.exists(args.logfile):
        print(f'error: file not found: {args.logfile}', file=sys.stderr)
        return 1
    analysis = analyze_file(args.logfile)
    render_analysis(analysis, args.peer)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
