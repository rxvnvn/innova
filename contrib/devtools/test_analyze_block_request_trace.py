#!/usr/bin/env python3
"""Regression tests for analyze_block_request_trace.py."""

import contextlib
import io
import os
import tempfile
import unittest

import analyze_block_request_trace as analyzer


HASH_A = "a" * 64
HASH_B = "b" * 64


def trace_event(time_us, event, **fields):
    payload = " ".join(f"{name}={value}" for name, value in fields.items())
    suffix = f" {payload}" if payload else ""
    return f"BLOCKREQTRACE time_us={time_us} event={event}{suffix}\n"


def summary(time_us, total_schedules):
    return trace_event(
        time_us,
        "SUMMARY",
        total_schedules=total_schedules,
        total_getdata_sends=total_schedules,
        stall_recoveries=total_schedules,
    )


class AnalyzerRegressionTests(unittest.TestCase):
    def run_cli(self, trace, *arguments):
        path = None
        try:
            with tempfile.NamedTemporaryFile(
                "w", encoding="utf-8", delete=False
            ) as stream:
                stream.write(trace)
                path = stream.name
            stdout = io.StringIO()
            stderr = io.StringIO()
            with contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(
                stderr
            ):
                status = analyzer.main([path, *arguments])
            return status, stdout.getvalue(), stderr.getvalue()
        finally:
            if path is not None:
                os.unlink(path)

    def test_hash_uses_summary_from_its_session(self):
        trace = "".join(
            [
                trace_event(100, "START"),
                trace_event(
                    101,
                    "ASK_SCHEDULE",
                    hash=HASH_A,
                    peer=1,
                    source="inv",
                    request_count=2,
                ),
                summary(102, 9),
                trace_event(200, "START"),
                summary(201, 0),
            ]
        )
        sessions = analyzer.parse_trace_sessions(io.StringIO(trace))
        self.assertEqual([session.index for session in sessions], [1, 2])
        self.assertTrue(
            all(event.session_index == 1 for event in sessions[0].events)
        )
        self.assertTrue(
            all(event.session_index == 2 for event in sessions[1].events)
        )
        self.assertTrue(sessions[0].contains_hash(HASH_A))
        self.assertFalse(sessions[1].contains_hash(HASH_A))

        status, stdout, stderr = self.run_cli(trace, "--hash", HASH_A)
        self.assertEqual(status, 0, stderr)
        self.assertIn("Session 1 last SUMMARY", stdout)
        self.assertIn("total_schedules=9", stdout)
        self.assertNotIn("Session 2 last SUMMARY", stdout)
        self.assertNotIn("total_schedules=0", stdout)

    def test_hash_in_two_sessions_is_reported_without_mixing(self):
        trace = "".join(
            [
                trace_event(100, "START"),
                trace_event(
                    101,
                    "ASK_SCHEDULE",
                    hash=HASH_A,
                    peer=1,
                    source="inv",
                    request_count=2,
                ),
                summary(102, 2),
                trace_event(200, "START"),
                trace_event(
                    201,
                    "ASK_SCHEDULE",
                    hash=HASH_A,
                    peer=2,
                    source="orphan",
                    request_count=3,
                ),
                summary(202, 3),
            ]
        )
        status, stdout, stderr = self.run_cli(trace, "--hash", HASH_A)
        self.assertEqual(status, 0, stderr)
        self.assertIn("appears in multiple sessions: 1, 2", stdout)
        self.assertEqual(stdout.count("Aggregate: requests="), 2)
        self.assertIn("Aggregate: requests=2", stdout)
        self.assertIn("Aggregate: requests=3", stdout)
        self.assertNotIn("Aggregate: requests=5", stdout)

        status, stdout, stderr = self.run_cli(
            trace, "--hash", HASH_A, "--session", "2"
        )
        self.assertEqual(status, 0, stderr)
        self.assertIn("Selected session 2 only", stdout)
        self.assertNotIn("Session 1 last SUMMARY", stdout)
        self.assertIn("Session 2 last SUMMARY", stdout)

    def test_top_defaults_to_last_nonempty_session(self):
        trace = "".join(
            [
                trace_event(100, "START"),
                trace_event(
                    101,
                    "ASK_SCHEDULE",
                    hash=HASH_A,
                    request_count=2,
                ),
                summary(102, 2),
                trace_event(200, "START"),
                trace_event(
                    201,
                    "ASK_SCHEDULE",
                    hash=HASH_B,
                    request_count=3,
                ),
                summary(202, 3),
                trace_event(300, "START"),
                summary(301, 0),
            ]
        )
        status, stdout, stderr = self.run_cli(trace, "--top", "5")
        self.assertEqual(status, 0, stderr)
        self.assertIn("Defaulting to last nonempty session: 2", stdout)
        self.assertIn(HASH_B, stdout)
        self.assertNotIn(HASH_A, stdout)

        status, stdout, stderr = self.run_cli(
            trace, "--top", "5", "--session", "1"
        )
        self.assertEqual(status, 0, stderr)
        self.assertIn(HASH_A, stdout)
        self.assertNotIn(HASH_B, stdout)

    def test_legacy_source_is_displayed_as_last_observed_source(self):
        trace = trace_event(
            100,
            "GETDATA_SEND",
            hash=HASH_A,
            peer=1,
            path="askfor",
            source="orphan",
            request_count=1,
            send_count=1,
            first_request_us=90,
            first_send_us=100,
            first_send_source="orphan",
        )
        sessions = analyzer.parse_trace_sessions(io.StringIO(trace))
        self.assertEqual(sessions[0].events[0].fields["source"], "orphan")

        status, stdout, stderr = self.run_cli(trace, "--hash", HASH_A)
        self.assertEqual(status, 0, stderr)
        self.assertIn("last_observed_source=orphan", stdout)
        self.assertIn("do not identify the exact due mapAskFor entry", stdout)
        self.assertNotIn(" path=askfor source=orphan", stdout)

    def test_suppressed_baselines_preserve_snapshot_counts(self):
        trace = "".join(
            [
                trace_event(100, "START"),
                trace_event(
                    200,
                    "ASK_SCHEDULE",
                    hash=HASH_A,
                    peer=12,
                    source="orphan",
                    request_count=247,
                    first_request_us=100,
                    first_peer=12,
                    first_source="inv",
                ),
                trace_event(
                    300,
                    "GETDATA_SEND",
                    hash=HASH_A,
                    peer=12,
                    source="orphan",
                    request_count=247,
                    send_count=1,
                    first_request_us=100,
                    first_send_us=300,
                ),
                trace_event(
                    400,
                    "BLOCK_RECEIVE",
                    hash=HASH_A,
                    peer=12,
                    receive_count=1,
                    first_request_us=100,
                    first_send_us=300,
                    first_receive_us=400,
                ),
            ]
        )
        session = analyzer.parse_trace_sessions(io.StringIO(trace))[0]
        analysis = analyzer.analyze(session.events, HASH_A, session.index)
        stats = analysis.hashes[HASH_A]
        self.assertEqual(stats.request_count, 247)
        self.assertEqual(stats.send_count, 1)
        self.assertEqual(stats.receive_count, 1)


if __name__ == "__main__":
    unittest.main()
