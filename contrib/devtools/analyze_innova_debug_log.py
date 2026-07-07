#!/usr/bin/env python3
from __future__ import annotations

import argparse
import collections
import datetime as dt
import os
import re
import sys
from typing import Counter, Dict, List, Optional, Tuple

EVENT_PATTERNS = {
    'SYNCSTATE': re.compile(r'SYNCSTATE'),
    'PEERSTATE': re.compile(r'PEERSTATE'),
    'GLOBAL_P2PMSG': re.compile(r'GLOBAL_P2PMSG'),
    'GETHEADERS_SEND': re.compile(r'GETHEADERS_SEND'),
    'GETHEADERS_SUPPRESSED': re.compile(r'GETHEADERS_SUPPRESSED'),
    'GETHEADERS_RECV': re.compile(r'GETHEADERS_RECV'),
    'HEADERS_RECV': re.compile(r'HEADERS_RECV'),
    'inv': re.compile(r'inv', re.I),
    'getdata': re.compile(r'getdata', re.I),
    'block': re.compile(r'(received block|block)', re.I),
    'headers': re.compile(r'headers', re.I),
    'getheaders': re.compile(r'getheaders', re.I),
    'getblocks': re.compile(r'getblocks', re.I),
    'addr': re.compile(r'addr', re.I),
    'tx': re.compile(r'tx', re.I),
    'version': re.compile(r'version', re.I),
    'verack': re.compile(r'verack', re.I),
    'disconnect': re.compile(r'disconnect', re.I),
    'recv-error 104': re.compile(r'recv-error\s*104', re.I),
    'socket-closed-by-peer': re.compile(r'socket.*closed.*peer', re.I),
    'connect failed': re.compile(r'connect.*failed|connection refused|no route to host', re.I),
    'SetBestChain': re.compile(r'SetBestChain|UpdateTip', re.I),
    'orphan': re.compile(r'orphan', re.I),
    'duplicate': re.compile(r'duplicate', re.I),
    'invalid': re.compile(r'invalid', re.I),
    'checkpoint': re.compile(r'checkpoint', re.I),
    'fork': re.compile(r'fork', re.I),
    'timeout': re.compile(r'timeout|stall', re.I),
}

TS_PATTERNS = [
    re.compile(r'^(\d{4}-\d{2}-\d{2})[ T](\d{2}:\d{2}:\d{2})'),
    re.compile(r'^\[(\d{4}-\d{2}-\d{2})[ T](\d{2}:\d{2}:\d{2})\]'),
    re.compile(r'^(\d{2}\.\d{2}\.\d{4})[ T](\d{2}:\d{2}:\d{2})'),
    re.compile(r'^\[(\d{2}\.\d{2}\.\d{4})[ T](\d{2}:\d{2}:\d{2})\]'),
]
PEER_RE = re.compile(r'peer=([^\s,]+)|from ([^\s,]+)|to ([^\s,]+)', re.I)
HASH_RE = re.compile(r'\b([0-9a-fA-F]{12,64})\b')
KEYVAL_RE = re.compile(r'([A-Za-z0-9_]+)=([^ \t\r\n]+)')
COMMAND_STAT_RE = re.compile(r'([A-Za-z0-9_:-]+)=(\d+)/(\d+)')


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description='Analyze Innova debug.log for sync churn and traffic patterns.')
    p.add_argument('logfile', help='Path to debug.log')
    return p.parse_args()


def parse_timestamp(line: str) -> Optional[dt.datetime]:
    for pat in TS_PATTERNS:
        m = pat.search(line)
        if m:
            for fmt in ('%Y-%m-%d %H:%M:%S', '%d.%m.%Y %H:%M:%S'):
                try:
                    return dt.datetime.strptime(f'{m.group(1)} {m.group(2)}', fmt)
                except ValueError:
                    pass
    return None


def peer_from_line(line: str) -> str:
    m = PEER_RE.search(line)
    if not m:
        return 'unknown'
    for i in range(1, 4):
        if m.group(i):
            return m.group(i)
    return 'unknown'


def parse_keyvals(line: str) -> Dict[str, str]:
    return {m.group(1): m.group(2) for m in KEYVAL_RE.finditer(line)}


def parse_command_stats(blob: str) -> Dict[str, Tuple[int, int]]:
    return {key: (int(count), int(bytes_)) for key, count, bytes_ in COMMAND_STAT_RE.findall(blob)}


def update_command_stats(dest: Dict[str, Tuple[int, int]], stats: Dict[str, Tuple[int, int]]) -> None:
    for key, (count, bytes_) in stats.items():
        prev_count, prev_bytes = dest.get(key, (0, 0))
        dest[key] = (max(prev_count, count), max(prev_bytes, bytes_))


def main() -> int:
    args = parse_args()
    if not os.path.exists(args.logfile):
        print(f'error: file not found: {args.logfile}', file=sys.stderr)
        return 1

    counts: Counter[str] = collections.Counter()
    peer_disconnects: Counter[str] = collections.Counter()
    peer_errors: Counter[str] = collections.Counter()
    repeated_hashes: Counter[str] = collections.Counter()

    first_ts = None
    last_ts = None
    total_lines = 0

    syncstate_last: Dict[str, str] = {}
    peerstate_last: Dict[str, str] = {}
    global_incoming: Dict[str, Tuple[int, int]] = {}
    global_outgoing: Dict[str, Tuple[int, int]] = {}

    getheaders_send_lines = 0
    getheaders_suppressed_lines = 0
    getheaders_inflight_max = 0
    queued_getblocks_max = 0
    blocks_in_flight_max = 0

    headers_recv_batches = 0
    headers_recv_headers = 0
    headers_recv_bytes = 0
    headers_recv_new = 0
    headers_recv_duplicate = 0
    headers_recv_unknown_parent = 0

    global_in_headers_count = 0
    global_in_headers_bytes = 0
    global_in_block_count = 0
    global_in_block_bytes = 0
    global_out_getblocks_count = 0
    global_out_getblocks_bytes = 0
    global_out_getheaders_count = 0
    global_out_getheaders_bytes = 0

    with open(args.logfile, 'r', errors='replace') as f:
        for line in f:
            total_lines += 1

            ts = parse_timestamp(line)
            if ts is not None:
                if first_ts is None:
                    first_ts = ts
                last_ts = ts

            for name, pat in EVENT_PATTERNS.items():
                if not pat.search(line):
                    continue

                counts[name] += 1

                if name == 'SYNCSTATE':
                    kv = parse_keyvals(line)
                    syncstate_last = kv
                    if 'queued_getblocks' in kv:
                        queued_getblocks_max = max(queued_getblocks_max, int(kv['queued_getblocks']))
                    if 'blocks_in_flight' in kv:
                        blocks_in_flight_max = max(blocks_in_flight_max, int(kv['blocks_in_flight']))
                elif name == 'PEERSTATE':
                    peerstate_last = parse_keyvals(line)
                    kv = peerstate_last
                    if 'getheaders_inflight' in kv:
                        getheaders_inflight_max = max(getheaders_inflight_max, int(kv['getheaders_inflight']))
                elif name == 'GLOBAL_P2PMSG':
                    payload = line.split(':', 1)[1] if ':' in line else line
                    side = 'incoming' if 'incoming' in payload else 'outgoing' if 'outgoing' in payload else 'unknown'
                    stats = parse_command_stats(payload)
                    if side == 'incoming':
                        update_command_stats(global_incoming, stats)
                        if 'headers' in stats:
                            global_in_headers_count, global_in_headers_bytes = stats['headers']
                        if 'block' in stats:
                            global_in_block_count, global_in_block_bytes = stats['block']
                    elif side == 'outgoing':
                        update_command_stats(global_outgoing, stats)
                        if 'getblocks' in stats:
                            global_out_getblocks_count, global_out_getblocks_bytes = stats['getblocks']
                        if 'getheaders' in stats:
                            global_out_getheaders_count, global_out_getheaders_bytes = stats['getheaders']
                elif name == 'GETHEADERS_SEND':
                    getheaders_send_lines += 1
                    kv = parse_keyvals(line)
                    if 'inflight' in kv:
                        getheaders_inflight_max = max(getheaders_inflight_max, int(kv['inflight']))
                elif name == 'GETHEADERS_SUPPRESSED':
                    getheaders_suppressed_lines += 1
                    kv = parse_keyvals(line)
                    if 'inflight' in kv:
                        getheaders_inflight_max = max(getheaders_inflight_max, int(kv['inflight']))
                elif name == 'HEADERS_RECV':
                    headers_recv_batches += 1
                    kv = parse_keyvals(line)
                    headers_recv_headers += int(kv.get('headers', '0'))
                    headers_recv_bytes += int(kv.get('bytes', '0'))
                    headers_recv_new += int(kv.get('new', '0'))
                    headers_recv_duplicate += int(kv.get('duplicate', '0'))
                    headers_recv_unknown_parent += int(kv.get('unknown_parent', '0'))

                if name in ('disconnect', 'recv-error 104', 'socket-closed-by-peer', 'connect failed'):
                    peer = peer_from_line(line)
                    if name == 'disconnect':
                        peer_disconnects[peer] += 1
                    else:
                        peer_errors[peer] += 1
                break

            lower = line.lower()
            if any(key in lower for key in ('inv', 'headers', 'getheaders', 'getblocks', 'block', 'getdata')):
                for h in HASH_RE.findall(line):
                    if len(h) >= 16:
                        repeated_hashes[h.lower()] += 1

    print(f'total lines: {total_lines}')
    if first_ts and last_ts:
        print(f'time range: {first_ts.isoformat(sep=" ")} -> {last_ts.isoformat(sep=" ")}')
    else:
        print('time range: unavailable')

    print('
counts by event:')
    for key in sorted(EVENT_PATTERNS.keys()):
        if counts.get(key):
            print(f'  {key}: {counts[key]}')

    print('
parsed GLOBAL_P2PMSG:')
    if global_incoming:
        print('  incoming:')
        for key in sorted(global_incoming):
            count, bytes_ = global_incoming[key]
            print(f'    {key}: {count}/{bytes_}')
    else:
        print('  incoming: none')
    if global_outgoing:
        print('  outgoing:')
        for key in sorted(global_outgoing):
            count, bytes_ = global_outgoing[key]
            print(f'    {key}: {count}/{bytes_}')
    else:
        print('  outgoing: none')

    print('
last SYNCSTATE:')
    if syncstate_last:
        for key in ('local_height', 'best_header', 'initialblockdownload', 'connections', 'sync_peer', 'blocks_in_flight', 'queued_getblocks', 'datareceived', 'delta_recv', 'delta_sent'):
            if key in syncstate_last:
                print(f'  {key}: {syncstate_last[key]}')
    else:
        print('  none')

    print('
last PEERSTATE sample:')
    if peerstate_last:
        for key in ('id', 'addr', 'subver', 'h', 'best', 'adv', 'inflight', 'askfor', 'getheaders_sent', 'getheaders_suppressed', 'getheaders_inflight', 'recv_age', 'send_age', 'inbound', 'whitelisted', 'connected', 'disconnect'):
            if key in peerstate_last:
                print(f'  {key}: {peerstate_last[key]}')
    else:
        print('  none')

    print('
key diagnostic maxima and totals:')
    print(f'  getheaders_send_lines: {getheaders_send_lines}')
    print(f'  getheaders_suppressed_lines: {getheaders_suppressed_lines}')
    print(f'  getheaders_inflight_max: {getheaders_inflight_max}')
    print(f'  queued_getblocks_max: {queued_getblocks_max}')
    print(f'  blocks_in_flight_max: {blocks_in_flight_max}')
    print(f'  headers_recv_batches: {headers_recv_batches}')
    print(f'  headers_recv_diag: {headers_recv_headers}/{headers_recv_bytes}')
    print(f'  headers_recv_new: {headers_recv_new}')
    print(f'  headers_recv_duplicate: {headers_recv_duplicate}')
    print(f'  headers_recv_unknown_parent: {headers_recv_unknown_parent}')
    print(f'  global_in_headers: {global_in_headers_count}/{global_in_headers_bytes}')
    print(f'  global_in_block: {global_in_block_count}/{global_in_block_bytes}')
    print(f'  global_out_getblocks: {global_out_getblocks_count}/{global_out_getblocks_bytes}')
    print(f'  global_out_getheaders: {global_out_getheaders_count}/{global_out_getheaders_bytes}')

    print('
top peers by disconnect:')
    for peer, count in peer_disconnects.most_common(10):
        print(f'  {peer}: {count}')
    if not peer_disconnects:
        print('  none')

    print('
top peers by recv-error/connect-fail:')
    for peer, count in peer_errors.most_common(10):
        print(f'  {peer}: {count}')
    if not peer_errors:
        print('  none')

    print('
top repeated hashes:')
    for h, count in repeated_hashes.most_common(20):
        print(f'  {h}: {count}')
    if not repeated_hashes:
        print('  none')

    churn = counts['disconnect'] + counts['recv-error 104'] + counts['socket-closed-by-peer'] + counts['connect failed']
    loopish = counts['getheaders'] + counts['getblocks'] + counts['inv'] + counts['getdata']
    incoming_headers_bytes = global_in_headers_bytes or headers_recv_bytes
    incoming_block_bytes = global_in_block_bytes or block_recv_bytes
    headers_vs_blocks = incoming_headers_bytes - incoming_block_bytes
    if churn > loopish and churn > 0:
        pattern = 'reconnect churn / transport instability'
    elif incoming_headers_bytes > 0 and incoming_headers_bytes > incoming_block_bytes * 10 and global_out_getheaders_count >= global_out_getblocks_count:
        pattern = 'headers/getheaders amplification likely'
    elif counts['headers'] > 0 and counts['getheaders'] > counts['block']:
        pattern = 'headers-first loop likely'
    elif repeated_hashes:
        pattern = 'possible repeated download loop'
    else:
        pattern = 'normal sync / insufficient signal'

    print('
estimated pattern:')
    print(f'  {pattern}')
    if incoming_headers_bytes or incoming_block_bytes:
        print(f'  incoming_headers_minus_blocks_bytes: {headers_vs_blocks}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
