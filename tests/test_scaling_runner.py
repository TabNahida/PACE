"""Deterministic scaling-runner validation; no performance measurements."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
SPEC = importlib.util.spec_from_file_location("scaling_runner", ROOT / "scripts/run_scaling.py")
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class ScalingValidationTests(unittest.TestCase):
    def setUp(self):
        self.args = SimpleNamespace(clients=8, requests=10, warmup=2, bytes=64, timeout_ms=1000)
        self.record = {
            "benchmark": "tcp_worker_scaling", "mode": "runtime", "trial": 1,
            "worker_count": 2, "server_threads": 2, "spawned_server_threads": 2,
            "caller_runs_reactor": False, "client_threads": 8, "connections": 8,
            "connections_per_worker": 4, "listener_count": 2, "pipeline": 1,
            "payload_bytes": 64, "requests_per_client": 10, "warmup_per_client": 2,
            "socket_timeout_ms": 1000, "completed_requests": 80, "checksum": 1980,
            "per_worker_completed_requests": [40, 40], "validated": True, "instrumented": False,
            "setup_ns": 1000000, "launch_ns": 2000000, "startup_ns": 3000000,
            "client_setup_warmup_ns": 4000000, "steady_ns": 10000000, "shutdown_ns": 1000000,
            "seconds": 0.01, "requests_per_second": 8000.0, "roundtrip_mib_per_second": 0.9765625,
            "p50_us": 10.0, "p99_us": 20.0, "max_us": 30.0,
        }

    def validate(self, record):
        RUNNER.validate_record(record, "runtime", 2, 1, self.args)

    def test_accepts_valid_records_and_caller_thread_topology(self):
        self.validate(self.record)
        caller = {**self.record, "mode": "caller", "spawned_server_threads": 1, "caller_runs_reactor": True}
        RUNNER.validate_record(caller, "caller", 2, 1, self.args)
        with self.assertRaises(ValueError):
            RUNNER.validate_record({**caller, "spawned_server_threads": 2}, "caller", 2, 1, self.args)

    def test_rejects_missing_fields(self):
        for field in self.record:
            altered = self.record.copy()
            del altered[field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate(altered)

    def test_rejects_workload_drift_and_unbalanced_workers(self):
        for change in ({"worker_count": 4}, {"trial": 2}, {"mode": "external"},
                       {"checksum": 1979}, {"completed_requests": 79}, {"connections_per_worker": 8},
                       {"per_worker_completed_requests": [39, 41]}, {"per_worker_completed_requests": [80]},
                       {"per_worker_completed_requests": [40.0, 40]}, {"pipeline": 2}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.validate({**self.record, **change})

    def test_rejects_instrumented_or_unvalidated_data_and_boolean_integers(self):
        for change in ({"instrumented": True}, {"instrumented": 0}, {"validated": False},
                       {"validated": 1}, {"caller_runs_reactor": 0}, {"trial": True}, {"worker_count": 2.0}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.validate({**self.record, **change})

    def test_rejects_invalid_timing_metrics(self):
        for field in RUNNER.TIME_FIELDS:
            for value in (0, -1, True, 1.5):
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    self.validate({**self.record, field: value})
        for field in ("seconds", "requests_per_second", "roundtrip_mib_per_second", "p50_us", "p99_us", "max_us"):
            for value in (0, -1, True, "10", float("nan"), float("inf")):
                with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                    self.validate({**self.record, field: value})

    def test_rejects_inconsistent_windows_rates_and_percentiles(self):
        for change in ({"startup_ns": 3000001}, {"seconds": 0.02}, {"requests_per_second": 7999},
                       {"roundtrip_mib_per_second": 0.5}, {"p50_us": 21}, {"p99_us": 31}, {"max_us": 10001}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.validate({**self.record, **change})

    def test_requires_every_trial_exactly_once_for_summary(self):
        second = {**self.record, "trial": 2}
        summary = RUNNER.summarize([self.record, second], [("runtime", 2)], 2)
        self.assertEqual(summary[0]["requests_per_second"]["median"], 8000.0)
        for records in ([self.record], [self.record, self.record]):
            with self.subTest(records=records), self.assertRaises(ValueError):
                RUNNER.summarize(records, [("runtime", 2)], 2)


class ScalingRunnerFailureTests(unittest.TestCase):
    def setUp(self):
        self.build = {"modes": list(RUNNER.MODES), "compiler": "test compiler", "cpp_standard": 202002,
                      "ndebug": True, "profiling_enabled": False}

    def arguments(self, output):
        return ["run_scaling.py", "--executable", str(ROOT / "scripts/run_scaling.py"),
                "--output", str(output), "--modes", "runtime", "--workers", "1", "--repeats", "1"]

    def test_rejects_profiling_build_before_any_trial(self):
        build = {**self.build, "profiling_enabled": True}
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(sys, "argv", self.arguments(Path(directory) / "report.json")), \
             mock.patch.object(RUNNER.subprocess, "check_output", return_value=json.dumps(build)), \
             mock.patch.object(RUNNER.subprocess, "run") as run, \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            RUNNER.main()
        run.assert_not_called()

    def test_rejects_unbalanced_fixed_clients_before_inspecting_binary(self):
        with tempfile.TemporaryDirectory() as directory, \
             mock.patch.object(sys, "argv", self.arguments(Path(directory) / "report.json") + ["--workers", "3", "--clients", "8"]), \
             mock.patch.object(RUNNER.subprocess, "check_output") as inspect, \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            RUNNER.main()
        inspect.assert_not_called()

    def test_saves_failure_report_and_removes_stale_markdown(self):
        failures = [subprocess.CompletedProcess([], 1, "", "synthetic process failure"),
                    subprocess.CompletedProcess([], 0, "{}", ""),
                    subprocess.TimeoutExpired(["synthetic"], 1)]
        for failure in failures:
            with self.subTest(failure=failure), tempfile.TemporaryDirectory() as directory:
                output = Path(directory) / "report.json"
                output.with_suffix(".md").write_text("stale success", encoding="utf-8")
                with mock.patch.object(sys, "argv", self.arguments(output)), \
                     mock.patch.object(RUNNER.subprocess, "check_output", return_value=json.dumps(self.build)), \
                     mock.patch.object(RUNNER, "command_output", return_value="synthetic metadata"), \
                     mock.patch.object(RUNNER, "cpu_model", return_value="synthetic CPU"), \
                     mock.patch.object(RUNNER.platform, "platform", return_value="synthetic OS"), \
                     mock.patch.object(RUNNER.platform, "machine", return_value="synthetic architecture"), \
                     mock.patch.object(RUNNER.subprocess, "run", **({"side_effect": failure} if isinstance(failure, Exception) else {"return_value": failure})), \
                     contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(RUNNER.main(), 1)
                report = json.loads(output.read_text())
                self.assertEqual(report["status"], "failed")
                # Platform discovery may itself invoke subprocess on BSD/macOS;
                # keep it outside the synthetic benchmark-process failure path.
                self.assertEqual(report["environment"]["platform"], "synthetic OS")
                self.assertEqual(report["environment"]["machine"], "synthetic architecture")
                self.assertIsNotNone(report["failure"])
                self.assertEqual(report["runs"], [])
                self.assertIsNone(report["summary"])
                self.assertFalse(output.with_suffix(".md").exists())


if __name__ == "__main__":
    unittest.main()
