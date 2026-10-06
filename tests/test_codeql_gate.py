#!/usr/bin/env python3
"""Fixture-only security-gate regressions; never contact a live account."""
import importlib.util
import json
import os
from pathlib import Path
import subprocess
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location(
    "codeql_gate", Path(__file__).resolve().parents[1] / "scripts/ci/codeql-gate.py"
)
gate = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(gate)
SHA = "a" * 40
REF = "refs/heads/fixture/branch"
RUN = {"id": 42, "head_sha": SHA, "status": "completed", "conclusion": "success",
       "event": "workflow_dispatch", "head_branch": "fixture/branch"}
ANALYSIS = {"ref": REF, "commit_sha": SHA, "category": "/language:c-cpp",
            "error": "", "tool": {"name": "CodeQL"}}


class CodeqlGateTests(unittest.TestCase):
    def test_denied_api_is_visible_and_fails(self):
        response = subprocess.CompletedProcess([], 1, "", "HTTP 403 fixture denial")
        with patch.object(gate.subprocess, "run", return_value=response):
            with self.assertRaisesRegex(gate.GateError, "HTTP 403 fixture denial"):
                gate.api("fixture/repo", "code-scanning/alerts", ref=REF)

    def test_invalid_api_json_fails(self):
        response = subprocess.CompletedProcess([], 0, "not JSON", "")
        with patch.object(gate.subprocess, "run", return_value=response):
            with self.assertRaisesRegex(gate.GateError, "Invalid GitHub API JSON"):
                gate.api("fixture/repo", "code-scanning/alerts")

    def test_api_uses_get_with_encoded_ref_field(self):
        response = subprocess.CompletedProcess([], 0, "[]", "")
        with patch.object(gate.subprocess, "run", return_value=response) as run:
            self.assertEqual(gate.api("fixture/repo", "code-scanning/alerts", ref=REF), [])
        command = run.call_args.args[0]
        self.assertIn(f"ref={REF}", command)
        self.assertEqual(command[3:5], ["--method", "GET"])

    def test_wait_denied_api_does_not_sleep_or_report_missing_run(self):
        with patch.object(gate, "api", side_effect=gate.GateError("HTTP 403")), \
                patch.object(gate.time, "sleep") as sleep:
            with self.assertRaisesRegex(gate.GateError, "HTTP 403"):
                gate.successful_run("fixture/repo", SHA)
            sleep.assert_not_called()

    def test_wait_pending_then_exact_head_success(self):
        pending = dict(RUN, status="in_progress", conclusion=None)
        with patch.object(gate, "api", side_effect=[{"workflow_runs": [pending]},
                                                   {"workflow_runs": [RUN]}]), \
                patch.object(gate.time, "sleep") as sleep:
            self.assertEqual(gate.successful_run("fixture/repo", SHA), RUN)
            sleep.assert_called_once_with(30)

    def test_wrong_head_failed_and_unknown_run_fail(self):
        for run in (dict(RUN, head_sha="b" * 40), dict(RUN, conclusion="cancelled"),
                    dict(RUN, status="unrecognized")):
            with self.subTest(run=run), patch.object(gate, "api", return_value={"workflow_runs": [run]}):
                with self.assertRaises(gate.GateError):
                    gate.successful_run("fixture/repo", SHA)

    def test_missing_run_preserves_ninety_attempt_budget(self):
        with patch.object(gate, "api", return_value={"workflow_runs": []}) as api, \
                patch.object(gate.time, "sleep") as sleep, patch("builtins.print"):
            with self.assertRaisesRegex(gate.GateError, "CodeQL timeout"):
                gate.successful_run("fixture/repo", SHA)
            self.assertEqual(api.call_count, 90)
            self.assertEqual(sleep.call_count, 90)
            self.assertTrue(all(call.args == (30,) for call in sleep.call_args_list))

    def test_pr_and_branch_refs_follow_selected_run(self):
        self.assertEqual(gate.run_ref(RUN), REF)
        pr = dict(RUN, event="pull_request", pull_requests=[{"number": 1}])
        self.assertEqual(gate.run_ref(pr), "refs/pull/1/merge")
        for pulls in ([], [{"number": 0}], [{"number": 1}, {"number": 2}]):
            with self.subTest(pulls=pulls), self.assertRaises(gate.GateError):
                gate.run_ref(dict(pr, pull_requests=pulls))

    def test_exact_uploaded_analysis_required(self):
        with patch.object(gate, "pages", return_value=iter([ANALYSIS])):
            gate.check_analysis("fixture/repo", SHA, REF)
        for analysis in (dict(ANALYSIS, commit_sha="b" * 40), dict(ANALYSIS, ref="refs/heads/main"),
                         dict(ANALYSIS, error="analysis failed"), dict(ANALYSIS, category="other")):
            with self.subTest(analysis=analysis), patch.object(gate, "pages", return_value=iter([analysis])):
                with self.assertRaises(gate.GateError):
                    gate.check_analysis("fixture/repo", SHA, REF)

    def test_old_matching_upload_cannot_override_newer_wrong_head(self):
        newer = dict(ANALYSIS, commit_sha="b" * 40)
        with patch.object(gate, "pages", return_value=iter([newer, ANALYSIS])):
            with self.assertRaisesRegex(gate.GateError, "Newest"):
                gate.check_analysis("fixture/repo", SHA, REF)

    def test_missing_analysis_cannot_pass_as_zero_alerts(self):
        with patch.object(gate, "pages", return_value=iter([])):
            with self.assertRaisesRegex(gate.GateError, "No uploaded"):
                gate.check_analysis("fixture/repo", SHA, REF)

    def test_alert_api_denial_cannot_pass_as_zero(self):
        with patch.object(gate, "api", side_effect=gate.GateError("HTTP 403")):
            with self.assertRaisesRegex(gate.GateError, "HTTP 403"):
                gate.check_alerts("fixture/repo", REF)

    def test_pagination_counts_alert_on_second_page(self):
        with patch.object(gate, "api", side_effect=[[{}] * 100, [{}]]) as api:
            with self.assertRaisesRegex(gate.GateError, "101 open"):
                gate.check_alerts("fixture/repo", REF)
            self.assertEqual(api.call_count, 2)
            self.assertEqual(api.call_args.kwargs["ref"], REF)
            self.assertEqual(api.call_args.kwargs["state"], "open")

    def test_malformed_alert_records_fail(self):
        for records in (None, {}, [None], ["alert"]):
            with self.subTest(records=records), patch.object(gate, "api", return_value=records):
                with self.assertRaises(gate.GateError):
                    gate.check_alerts("fixture/repo", REF)

    def test_main_checks_exact_upload_and_ref_twice(self):
        with patch.dict(os.environ, {"GITHUB_REPOSITORY": "fixture/repo", "CODEQL_HEAD_SHA": SHA}), \
                patch.object(gate, "successful_run", return_value=RUN), \
                patch.object(gate, "check_analysis") as analysis, \
                patch.object(gate, "check_alerts") as alerts, patch.object(gate.time, "sleep") as sleep:
            gate.main()
            self.assertEqual(analysis.call_count, 2)
            analysis.assert_called_with("fixture/repo", SHA, REF)
            self.assertEqual(alerts.call_count, 2)
            alerts.assert_called_with("fixture/repo", REF)
            self.assertEqual([call.args for call in sleep.call_args_list], [(60,), (15,)])


if __name__ == "__main__":
    unittest.main()
