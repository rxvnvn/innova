#!/usr/bin/env python3

import importlib.util
import io
import pathlib
import sys
import unittest


MODULE_PATH = pathlib.Path(__file__).with_name('analyze_innova_debug_log.py')
SPEC = importlib.util.spec_from_file_location('analyze_innova_debug_log', MODULE_PATH)
ANALYZER = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = ANALYZER
SPEC.loader.exec_module(ANALYZER)


def peerstate(peer_id, address, height, headers, header_bytes, getheaders):
    return (
        f'PEERSTATE: id={peer_id} addr={address} subver=/innova:5.0.1/ '
        f'ver=50000 h={height} best={height} adv={height} inflight=0 '
        f'in{{headers={headers}/{header_bytes} block=1/100}} '
        f'out{{getheaders={getheaders}/{getheaders * 741}}}\n'
    )


class AnalyzeInnovaDebugLogTest(unittest.TestCase):
    def test_exact_ranges_and_interval_overlap(self):
        address = '203.0.113.5:14530'
        lines = [
            peerstate(7, address, 130, 3, 2469, 3),
            (
                f'GETHEADERS_TRACE: peer={address} peer_id=7 reason=start action=send '
                'local_height=100 '
                'locator_tip_hash=aa locator_tip_height=100 locator_size=10 '
                'hash_stop=0 previous_request_age=-1 request_sequence=1\n'
            ),
            (
                f'HEADERS_TRACE: peer={address} peer_id=7 response_sequence=1 '
                'headers_count=10 headers_bytes=823 '
                'first_hash=aaaa first_height_known=101 '
                'last_hash=bbbb last_height_known=110 new_headers_count=10 '
                'already_known_count=0 connected_count=10 orphan_count=0 '
                'range_contiguous=1\n'
            ),
            (
                f'GETHEADERS_TRACE: peer={address} peer_id=7 reason=continue action=send '
                'local_height=105 '
                'locator_tip_hash=cc locator_tip_height=105 locator_size=10 '
                'hash_stop=0 previous_request_age=1 request_sequence=2\n'
            ),
            (
                f'HEADERS_TRACE: peer={address} peer_id=7 response_sequence=2 '
                'headers_count=10 headers_bytes=823 '
                'first_hash=cccc first_height_known=106 '
                'last_hash=dddd last_height_known=115 new_headers_count=5 '
                'already_known_count=5 connected_count=5 orphan_count=0 '
                'range_contiguous=1\n'
            ),
            (
                f'GETHEADERS_TRACE: peer={address} peer_id=7 reason=repeat action=send '
                'local_height=105 '
                'locator_tip_hash=cc locator_tip_height=105 locator_size=10 '
                'hash_stop=0 previous_request_age=1 request_sequence=3\n'
            ),
            (
                f'HEADERS_TRACE: peer={address} peer_id=7 response_sequence=3 '
                'headers_count=10 headers_bytes=823 '
                'first_hash=cccc first_height_known=106 '
                'last_hash=dddd last_height_known=115 new_headers_count=0 '
                'already_known_count=10 connected_count=0 orphan_count=0 '
                'range_contiguous=1\n'
            ),
        ]

        analysis = ANALYZER.analyze_stream(lines)
        session = analysis.sessions[f'{address}#7']

        self.assertEqual(session.getheaders_requests, 3)
        self.assertEqual(session.headers_responses, 3)
        self.assertEqual(session.headers_bytes, 2469)
        self.assertEqual(session.unique_response_ranges, 2)
        self.assertEqual(session.repeated_response_ranges, 1)
        self.assertEqual(session.repeated_range_keys, 1)
        self.assertEqual(session.trace_new_headers, 15)
        self.assertEqual(session.trace_known_headers, 15)
        self.assertEqual(session.trace_known_headers_bytes, 1234)
        self.assertEqual(session.trace_headers_count, 30)
        self.assertEqual(session.trace_headers_count_max, 10)
        self.assertEqual(session.trace_headers_bytes_max, 823)
        self.assertEqual(session.overlap_duplicate_headers, 15)
        self.assertEqual(session.overlap_duplicate_bytes, 1234)
        self.assertEqual(session.seen_intervals, [(101, 115)])

        output = io.StringIO()
        ANALYZER.render_analysis(analysis, '#7', output)
        rendered = output.getvalue()
        self.assertIn('peer sessions: 1', rendered)
        self.assertIn('unique_response_ranges: 2', rendered)
        self.assertIn('repeated_response_ranges: 1', rendered)
        self.assertIn('overlap_duplicate_headers_estimate: 15', rendered)
        self.assertIn('trace_headers_per_response_average: 10.00', rendered)
        self.assertIn('trace_headers_bytes_per_response_max: 823', rendered)
        self.assertIn('repeated_range: occurrences=2 heights=106-115', rendered)

    def test_current_trace_suppression_and_unknown_ranges(self):
        address = '203.0.113.12:14530'
        lines = [
            (
                f'GETHEADERS_TRACE: peer={address} peer_id=12 reason=handshake '
                'action=send request_sequence=1\n'
            ),
            (
                f'GETHEADERS_TRACE: peer={address} peer_id=12 reason=continue '
                'action=suppress request_sequence=1\n'
            ),
            (
                f'HEADERS_TRACE: peer={address} peer_id=12 response_sequence=1 '
                'headers_count=4 headers_bytes=329 first_hash=aaaa '
                'first_height_known=-1 last_hash=bbbb last_height_known=-1 '
                'new_headers_count=4 already_known_count=0 connected_count=0 '
                'orphan_count=4 range_contiguous=1\n'
            ),
            (
                f'HEADERS_TRACE: peer={address} peer_id=12 response_sequence=2 '
                'headers_count=4 headers_bytes=329 first_hash=cccc '
                'first_height_known=-1 last_hash=dddd last_height_known=-1 '
                'new_headers_count=4 already_known_count=0 connected_count=0 '
                'orphan_count=4 range_contiguous=1\n'
            ),
            (
                f'HEADERS_TRACE: peer={address} peer_id=12 response_sequence=3 '
                'headers_count=10 headers_bytes=823 first_hash=eeee '
                'first_height_known=1 last_hash=ffff last_height_known=10 '
                'new_headers_count=10 already_known_count=0 connected_count=0 '
                'orphan_count=10 range_contiguous=0\n'
            ),
        ]

        analysis = ANALYZER.analyze_stream(lines)
        session = analysis.sessions[f'{address}#12']

        self.assertEqual(session.getheaders_requests, 1)
        self.assertEqual(session.suppressed_getheaders, 1)
        self.assertEqual(session.headers_responses, 3)
        self.assertEqual(session.trace_headers_count, 18)
        self.assertEqual(session.overlap_duplicate_headers, 0)
        self.assertEqual(session.overlap_duplicate_bytes, 0)
        self.assertEqual(session.seen_intervals, [])

    def test_exact_range_identity_survives_height_discovery(self):
        address = '203.0.113.13:14530'
        lines = [
            (
                f'HEADERS_TRACE: peer={address} peer_id=13 response_sequence=1 '
                'headers_count=10 headers_bytes=823 first_hash=aaaa '
                'first_height_known=-1 last_hash=bbbb last_height_known=-1 '
                'range_contiguous=1\n'
            ),
            (
                f'HEADERS_TRACE: peer={address} peer_id=13 response_sequence=2 '
                'headers_count=10 headers_bytes=823 first_hash=aaaa '
                'first_height_known=1 last_hash=bbbb last_height_known=10 '
                'range_contiguous=1\n'
            ),
        ]

        analysis = ANALYZER.analyze_stream(lines)
        session = analysis.sessions[f'{address}#13']

        self.assertEqual(session.unique_response_ranges, 1)
        self.assertEqual(session.repeated_response_ranges, 1)
        self.assertEqual(session.overlap_duplicate_headers, 10)
        self.assertEqual(session.overlap_duplicate_bytes, 823)
        self.assertEqual(session.seen_intervals, [(1, 10)])

    def test_timestamp_prefixed_periodic_diagnostics(self):
        address = '192.0.2.22:14530'
        analysis = ANALYZER.analyze_stream(
            [
                '2026-07-15 01:02:03 SYNCSTATE: local_height=42 connections=1\n',
                '[2026-07-15 01:02:04] GLOBAL_P2PMSG: incoming headers=2/1646\n',
                '15.07.2026 01:02:05 ' + peerstate(22, address, 42, 2, 1646, 1),
            ]
        )

        self.assertEqual(analysis.first_timestamp, ANALYZER.dt.datetime(2026, 7, 15, 1, 2, 3))
        self.assertEqual(analysis.last_timestamp, ANALYZER.dt.datetime(2026, 7, 15, 1, 2, 5))
        self.assertEqual(analysis.syncstate_last['local_height'], '42')
        self.assertEqual(analysis.global_incoming['headers'], (2, 1646))
        self.assertIn(f'{address}#22', analysis.sessions)

    def test_peerstate_fallback_duplicate_estimate(self):
        address = '192.0.2.23:14530'
        headers_bytes = 14542047152
        height = 109936
        analysis = ANALYZER.analyze_stream(
            [peerstate(6, address, height, 188522, headers_bytes, 83904)]
        )
        session = analysis.sessions[f'{address}#6']
        bound = ANALYZER.unique_header_payload_bound(height)

        self.assertEqual(session.getheaders_requests, 83904)
        self.assertEqual(session.headers_responses, 188522)
        self.assertEqual(session.headers_bytes, headers_bytes)
        self.assertEqual(session.unique_response_ranges, 0)
        self.assertEqual(session.fallback_unique_bytes_bound, bound)
        self.assertEqual(session.fallback_duplicate_bytes, headers_bytes - bound)
        self.assertGreater(session.fallback_duplicate_bytes, 14_500_000_000)

    def test_reconnect_creates_distinct_sessions_and_peer_filter(self):
        address = '198.51.100.9:14530'
        analysis = ANALYZER.analyze_stream(
            [
                peerstate(1, address, 10, 1, 823, 1),
                f'DEBUG-DISCONNECT recv-error 104 peer={address}\n',
                peerstate(2, address, 20, 2, 1646, 2),
            ]
        )

        self.assertEqual(len(analysis.sessions), 2)
        self.assertTrue(analysis.sessions[f'{address}#1'].disconnected)
        self.assertFalse(analysis.sessions[f'{address}#2'].disconnected)
        self.assertEqual(len(ANALYZER.selected_sessions(analysis, address)), 2)
        self.assertEqual(
            [session.label for session in ANALYZER.selected_sessions(analysis, '#2')],
            [f'{address}#2'],
        )

    def test_trace_before_first_peerstate_is_attached_to_session(self):
        address = '203.0.113.19:14530'
        analysis = ANALYZER.analyze_stream(
            [
                (
                    f'GETHEADERS_TRACE: peer={address} peer_id=19 reason=handshake '
                    'action=send '
                    'request_sequence=1\n'
                ),
                peerstate(19, address, 40, 1, 823, 1),
            ]
        )

        self.assertEqual(list(analysis.sessions), [f'{address}#19'])
        self.assertEqual(analysis.sessions[f'{address}#19'].getheaders_requests, 1)


if __name__ == '__main__':
    unittest.main()
