"""Regression checks for accepting benchmark evidence, using synthetic records."""

import importlib.util
import json
from pathlib import Path
from types import SimpleNamespace
import unittest


RUNNER_PATH = Path(__file__).resolve().parents[1] / "scripts" / "run_benchmarks.py"
SPEC = importlib.util.spec_from_file_location("pace_benchmark_runner", RUNNER_PATH)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)


class BenchmarkRecordValidationTests(unittest.TestCase):
    def setUp(self):
        self.config = SimpleNamespace(clients=4, requests=2000, warmup=200, bytes=64)
        # These values exercise validation arithmetic; they are not measured results.
        self.record = {
            "benchmark": "tcp_echo", "backend": "pace", "trial": 1,
            "server_threads": 1, "client_threads": 4, "connections": 4,
            "pipeline": 1, "payload_bytes": 64, "requests_per_client": 2000,
            "warmup_per_client": 200, "completed_requests": 8000,
            "seconds": 1.0, "requests_per_second": 8000.0,
            "roundtrip_mib_per_second": 0.9765625,
            "p50_us": 10.0, "p99_us": 20.0, "max_us": 30.0,
            "payload_verified": True,
        }

    def validate(self, record):
        RUNNER.validate_record(record, "pace", 1, self.config)

    def test_accepts_complete_records_for_each_backend(self):
        for backend in ("pace", "asio", "libuv"):
            with self.subTest(backend=backend):
                record = {**self.record, "backend": backend, "trial": 3}
                RUNNER.validate_record(json.loads(json.dumps(record)), backend, 3, self.config)

    def test_accepts_printed_timing_precision(self):
        elapsed = 0.1234567894
        throughput = self.record["completed_requests"] / elapsed
        self.validate({**self.record, "seconds": round(elapsed, 9),
                       "requests_per_second": round(throughput, 9),
                       "roundtrip_mib_per_second": round(throughput * 128 / 1048576, 9)})

    def test_rejects_non_object_json(self):
        for value in (None, [], "result", 1, True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.validate(value)

    def test_rejects_wrong_trial_backend_and_workload(self):
        incorrect = {
            "benchmark": "other", "backend": "asio", "trial": 2,
            "server_threads": 2, "client_threads": 8, "connections": 8,
            "pipeline": 2, "payload_bytes": 128, "requests_per_client": 1999,
            "warmup_per_client": 0, "completed_requests": 7999,
        }
        for field, value in incorrect.items():
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate({**self.record, field: value})

    def test_requires_true_boolean_payload_verification(self):
        for value in (False, 1, "true", None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.validate({**self.record, "payload_verified": value})
        record = self.record.copy()
        del record["payload_verified"]
        with self.assertRaises(ValueError):
            self.validate(record)

    def test_rejects_wrong_types_for_workload_counts(self):
        for field, value in (("trial", True), ("completed_requests", 8000.0),
                             ("connections", "4")):
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate({**self.record, field: value})

    def test_rejects_missing_nonfinite_nonpositive_and_nonnumeric_metrics(self):
        metrics = ("seconds", "requests_per_second", "roundtrip_mib_per_second",
                   "p50_us", "p99_us", "max_us")
        for metric in metrics:
            for value in (float("nan"), float("inf"), -float("inf"), 0, -1, True, "10", None):
                with self.subTest(metric=metric, value=value), self.assertRaises(ValueError):
                    self.validate({**self.record, metric: value})
            record = self.record.copy()
            del record[metric]
            with self.subTest(metric=metric, value="missing"), self.assertRaises(ValueError):
                self.validate(record)

    def test_rejects_reversed_percentiles(self):
        for change in ({"p50_us": 21.0}, {"p99_us": 31.0}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.validate({**self.record, **change})

    def test_rejects_inconsistent_throughput_and_bandwidth(self):
        for change in ({"requests_per_second": 16000.0}, {"seconds": 2.0},
                       {"roundtrip_mib_per_second": 1.953125}):
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.validate({**self.record, **change})


if __name__ == "__main__":
    unittest.main()
