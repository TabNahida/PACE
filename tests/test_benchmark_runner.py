"""Regression checks for accepting benchmark evidence, using synthetic records."""

import importlib.util
import copy
import contextlib
import io
import json
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


RUNNER_PATH = Path(__file__).resolve().parents[1] / "scripts" / "run_benchmarks.py"
SPEC = importlib.util.spec_from_file_location("pace_benchmark_runner", RUNNER_PATH)
RUNNER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(RUNNER)
PROFILE_SPEC = importlib.util.spec_from_file_location("pace_profile_runner", RUNNER_PATH.with_name("run_profile.py"))
PROFILE = importlib.util.module_from_spec(PROFILE_SPEC)
with mock.patch.dict(sys.modules, {"run_benchmarks": RUNNER}):
    PROFILE_SPEC.loader.exec_module(PROFILE)


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

    def diagnostic_record(self):
        return {**self.record, "instrumented": True, "pace_profile": {
            "window": RUNNER.PROFILE_WINDOW,
            "stages": [
                {"name": "reactor_run", "calls": 1, "inclusive_ns": 1000, "exclusive_ns": 100},
                {"name": "recv", "calls": 20, "inclusive_ns": 900, "exclusive_ns": 900},
            ],
        }}

    def test_accepts_explicit_instrumented_diagnostic_record(self):
        RUNNER.validate_record(self.diagnostic_record(), "pace", 1, self.config, instrumented=True)

    def test_rejects_profile_data_in_normal_comparison(self):
        with self.assertRaises(ValueError):
            self.validate(self.diagnostic_record())
        with self.assertRaises(ValueError):
            self.validate({**self.record, "pace_profile": self.diagnostic_record()["pace_profile"]})

    def test_requires_instrumented_flag_and_counters_together(self):
        for record in (self.record, {**self.record, "instrumented": True}):
            with self.subTest(record=record), self.assertRaises(ValueError):
                RUNNER.validate_record(record, "pace", 1, self.config, instrumented=True)
        for flag in (1, "true", None):
            with self.subTest(flag=flag), self.assertRaises(ValueError):
                RUNNER.validate_record({**self.diagnostic_record(), "instrumented": flag},
                                       "pace", 1, self.config, instrumented=True)

    def test_rejects_invalid_profile_window_and_stage_counters(self):
        invalid_profiles = [None, {}, {"window": "measured workload only", "stages": []}]
        profile = self.diagnostic_record()["pace_profile"]
        invalid_profiles += [{**profile, "stages": []}, {**profile, "stages": profile["stages"] * 2}]
        for change in ({"calls": -1}, {"calls": True}, {"inclusive_ns": 0.5},
                       {"exclusive_ns": 1001}, {"calls": 0}, {"name": ""},
                       {"calls": 0, "inclusive_ns": 0, "exclusive_ns": 0}, {"calls": 2}):
            altered = copy.deepcopy(profile)
            altered["stages"][0].update(change)
            invalid_profiles.append(altered)
        for invalid in invalid_profiles:
            with self.subTest(profile=invalid), self.assertRaises(ValueError):
                RUNNER.validate_record({**self.diagnostic_record(), "pace_profile": invalid},
                                       "pace", 1, self.config, instrumented=True)


class ExtendedMetricTests(unittest.TestCase):
    def setUp(self):
        fixture = BenchmarkRecordValidationTests()
        fixture.setUp()
        self.config, self.legacy = fixture.config, fixture.record
        self.record = {**self.legacy, "extended_metrics_version": 1, "p95_us": 15.0,
                       "latency_samples": 8000, "peak_process_memory": {
                           "bytes": 8 * 1048576, "source": "getrusage(RUSAGE_SELF).ru_maxrss_KiB",
                           "scope": RUNNER.MEMORY_SCOPE, "window": RUNNER.LEGACY_MEMORY_WINDOW}}

    def validate(self, record, **kwargs):
        RUNNER.validate_record(record, "pace", 1, self.config, **kwargs)

    def test_accepts_legacy_and_each_portable_memory_source(self):
        self.validate(self.legacy)
        for source in RUNNER.LEGACY_MEMORY_SOURCES:
            record = copy.deepcopy(self.record)
            record["peak_process_memory"]["source"] = source
            with self.subTest(source=source):
                self.validate(record, require_extended=True, expected_memory_source=source)

    def test_advertised_extension_cannot_silently_omit_all_new_fields(self):
        with self.assertRaises(ValueError):
            self.validate(self.legacy, require_extended=True)

    def test_rejects_partial_extension_and_wrong_versions(self):
        for field in RUNNER.EXTENDED_FIELDS:
            record = copy.deepcopy(self.record)
            del record[field]
            with self.subTest(field=field), self.assertRaises(ValueError):
                self.validate(record)
        for version in (True, 0, 2, "1", None):
            with self.subTest(version=version), self.assertRaises(ValueError):
                self.validate({**self.record, "extended_metrics_version": version})

    def test_rejects_invalid_p95_and_sample_counts(self):
        for value in (True, None, "15", float("nan"), float("inf"), 0, 9.9, 20.1):
            with self.subTest(p95=value), self.assertRaises(ValueError):
                self.validate({**self.record, "p95_us": value})
        for value in (True, 8000.0, 7999, 8001, None):
            with self.subTest(samples=value), self.assertRaises(ValueError):
                self.validate({**self.record, "latency_samples": value})

    def test_rejects_invalid_peak_bytes_and_unidentified_measurement(self):
        for value in (True, None, "8", 1.5, 0, -1, 2**64, 1025):
            record = copy.deepcopy(self.record)
            record["peak_process_memory"]["bytes"] = value
            with self.subTest(bytes=value), self.assertRaises(ValueError):
                self.validate(record)
        for field in ("bytes", "source", "scope", "window"):
            record = copy.deepcopy(self.record)
            del record["peak_process_memory"][field]
            with self.subTest(missing=field), self.assertRaises(ValueError):
                self.validate(record)
        for field, value in (("source", "RSS"), ("source", []), ("scope", "server only"), ("window", "steady state")):
            record = copy.deepcopy(self.record)
            record["peak_process_memory"][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.validate(record)
        with self.assertRaises(ValueError):
            self.validate(self.record, expected_memory_source="GetProcessMemoryInfo.PeakWorkingSetSize")

    def test_summary_preserves_spread_for_all_latency_and_memory_metrics(self):
        second = copy.deepcopy(self.record)
        second.update(trial=2, p50_us=12, p95_us=17, p99_us=22, max_us=100)
        second["peak_process_memory"]["bytes"] = 12 * 1048576
        summary = RUNNER.summarize([self.record, second], ["pace"], 2)[0]
        for metric, median, low, high in (("p50_us", 11, 10, 12), ("p95_us", 16, 15, 17),
                                         ("p99_us", 21, 20, 22), ("max_us", 65, 30, 100),
                                         ("peak_process_memory_bytes", 10*1048576, 8*1048576, 12*1048576)):
            self.assertEqual(summary[f"median_{metric}"], median)
            self.assertEqual(summary[f"min_{metric}"], low)
            self.assertEqual(summary[f"max_{metric}"], high)
        legacy = RUNNER.summarize([self.legacy], ["pace"], 1)[0]
        self.assertNotIn("median_p95_us", legacy)
        self.assertNotIn("median_peak_process_memory_bytes", legacy)
        self.assertEqual(legacy["median_p99_us"], 20)

    def test_rejects_mixed_sources_schemas_and_incomplete_aggregate(self):
        second = copy.deepcopy(self.record)
        second["trial"] = 2
        second["peak_process_memory"]["source"] = "GetProcessMemoryInfo.PeakWorkingSetSize"
        for records in ([self.record, second], [self.legacy, {**self.record, "trial": 2}],
                        [self.record], [self.record, self.record]):
            with self.subTest(records=records), self.assertRaises(ValueError):
                RUNNER.summarize(records, ["pace"], 2)


class CurrentImageMemoryTests(unittest.TestCase):
    def setUp(self):
        fixture = ExtendedMetricTests()
        fixture.setUp()
        self.config, self.legacy = fixture.config, fixture.record
        self.record = copy.deepcopy(self.legacy)
        self.record["extended_metrics_version"] = 2
        self.record["peak_process_memory"].update(
            source="/proc/self/status.VmHWM_KiB", window=RUNNER.MEMORY_WINDOW, available=True)

    def validate(self, record, **kwargs):
        RUNNER.validate_record(record, "pace", 1, self.config, require_extended=True, **kwargs)

    def unavailable(self):
        record = copy.deepcopy(self.record)
        record["peak_process_memory"].update(bytes=None, source="unavailable", available=False,
                                             unavailable_reason=RUNNER.MEMORY_UNAVAILABLE_REASON)
        return record

    def test_accepts_current_image_providers_with_declared_version(self):
        for source in RUNNER.IMAGE_MEMORY_SOURCES:
            record = copy.deepcopy(self.record)
            record["peak_process_memory"]["source"] = source
            self.validate(record, expected_memory_source=source, expected_metrics_version=2)

    def test_legacy_records_remain_readable_but_cannot_masquerade_as_new(self):
        self.validate(self.legacy, expected_metrics_version=1)
        with self.assertRaises(ValueError):
            self.validate(self.legacy, expected_metrics_version=2)
        for source in RUNNER.LEGACY_MEMORY_SOURCES - RUNNER.IMAGE_MEMORY_SOURCES:
            record = copy.deepcopy(self.record)
            record["peak_process_memory"]["source"] = source
            with self.subTest(source=source), self.assertRaises(ValueError):
                self.validate(record)
        changed = copy.deepcopy(self.record)
        changed["peak_process_memory"]["window"] = RUNNER.LEGACY_MEMORY_WINDOW
        with self.assertRaises(ValueError):
            self.validate(changed)

    def test_unavailable_memory_preserves_latencies_without_fake_aggregate(self):
        record = self.unavailable()
        self.validate(record, expected_memory_source="unavailable", expected_metrics_version=2)
        summary = RUNNER.summarize([record], ["pace"], 1)[0]
        self.assertFalse(summary["peak_process_memory_available"])
        self.assertEqual(summary["peak_process_memory_unavailable_reason"], RUNNER.MEMORY_UNAVAILABLE_REASON)
        self.assertEqual(summary["median_p95_us"], 15)
        self.assertNotIn("median_peak_process_memory_bytes", summary)
        self.assertNotIn("min_peak_process_memory_bytes", summary)
        self.assertNotIn("max_peak_process_memory_bytes", summary)

    def test_available_memory_retains_all_aggregates(self):
        summary = RUNNER.summarize([self.record], ["pace"], 1)[0]
        self.assertTrue(summary["peak_process_memory_available"])
        self.assertEqual(summary["extended_metrics_version"], 2)
        self.assertEqual(summary["peak_process_memory_window"], RUNNER.MEMORY_WINDOW)
        self.assertEqual(summary["median_peak_process_memory_bytes"], 8 * 1048576)

    def test_rejects_ambiguous_or_fabricated_availability(self):
        for field, value in (("bytes", 0), ("bytes", 1234), ("available", True),
                             ("available", 0), ("unavailable_reason", ""),
                             ("unavailable_reason", "estimated")):
            record = self.unavailable()
            record["peak_process_memory"][field] = value
            with self.subTest(field=field, value=value), self.assertRaises(ValueError):
                self.validate(record)
        for record in (self.record, self.unavailable()):
            for field in ("bytes", "available"):
                changed = copy.deepcopy(record)
                del changed["peak_process_memory"][field]
                with self.subTest(field=field), self.assertRaises(ValueError):
                    self.validate(changed)
        for change in ({"available": False}, {"bytes": None}, {"unavailable_reason": "not measured"}):
            changed = copy.deepcopy(self.record)
            changed["peak_process_memory"].update(change)
            with self.subTest(change=change), self.assertRaises(ValueError):
                self.validate(changed)

    def test_rejects_mixing_versions_or_missing_and_measured_memory(self):
        for other in (self.legacy, self.unavailable()):
            with self.subTest(other=other), self.assertRaises(ValueError):
                RUNNER.summarize([self.record, {**other, "trial": 2}], ["pace"], 2)

    def probe(self, before=4, after=36, unavailable=False):
        memory = self.unavailable()["peak_process_memory"] if unavailable else self.record["peak_process_memory"]
        initial, final = copy.deepcopy(memory), copy.deepcopy(memory)
        if not unavailable:
            initial["bytes"], final["bytes"] = before * 1048576, after * 1048576
        return {"probe": "peak_process_memory", "allocated_bytes": 32 * 1048576,
                "before": initial, "after_release": final}

    def check_probe(self, probes, source="/proc/self/status.VmHWM_KiB"):
        with mock.patch.object(RUNNER.subprocess, "check_output", side_effect=[json.dumps(p) for p in probes]), \
             mock.patch.object(RUNNER, "bytearray", return_value=bytearray(1), create=True):
            return RUNNER.check_memory_provider("pace_bench", {
                "extended_metrics_version": 2, "peak_process_memory_source": source})

    def test_probe_checks_retained_peak_and_rejects_launcher_contamination(self):
        result = self.check_probe([self.probe(), self.probe(before=5, after=37)])
        self.assertEqual(result["status"], "passed")
        self.assertEqual(result["launcher_padding_bytes"], 128 * 1048576)
        for probes in ([self.probe(before=4, after=5)],
                       [self.probe(), self.probe(before=140, after=172)],
                       [self.probe(), self.probe(before=140, after=140)]):
            with self.subTest(probes=probes), self.assertRaises(ValueError):
                self.check_probe(probes)

    def test_probe_preserves_explicit_unavailable_and_rejects_v1(self):
        result = self.check_probe([self.probe(unavailable=True)], source="unavailable")
        self.assertEqual(result["status"], "unavailable")
        self.assertNotIn("large_launcher", result)
        with self.assertRaises(ValueError):
            RUNNER.check_memory_provider("pace_bench", {"extended_metrics_version": 1})

    def test_cli_reports_unavailable_memory_without_losing_timing_results(self):
        record = self.unavailable()
        capabilities = {"backends": ["pace"], "pace_profiling": False,
                        "extended_metrics_version": 2, "peak_process_memory_source": "unavailable"}
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "report.json"
            argv = ["run_benchmarks.py", "--executable", str(RUNNER_PATH), "--output", str(output),
                    "--backends", "pace", "--repeats", "1", "--clients", "4", "--requests", "2000",
                    "--warmup", "200", "--bytes", "64"]
            with mock.patch("sys.argv", argv), \
                 mock.patch.object(RUNNER.subprocess, "check_output", return_value=json.dumps(capabilities)), \
                 mock.patch.object(RUNNER, "command_output", return_value=None), \
                 mock.patch.object(RUNNER, "cpu_model", return_value="synthetic test"), \
                 mock.patch.dict(RUNNER.os.environ, {"GITHUB_STEP_SUMMARY": ""}), \
                 mock.patch.object(RUNNER.subprocess, "run", return_value=SimpleNamespace(
                     returncode=0, stdout=json.dumps(record), stderr="")), \
                 contextlib.redirect_stdout(io.StringIO()) as printed:
                self.assertEqual(RUNNER.main(), 0)
            self.assertIn("| pace | n/a | n/a |", printed.getvalue())
            report = json.loads(output.read_text())
            self.assertEqual(report["environment"]["status"], "completed")
            self.assertEqual(report["summary"][0]["median_p95_us"], 15)
            self.assertNotIn("median_peak_process_memory_bytes", report["summary"][0])


class ProfilingComparisonGuardTests(unittest.TestCase):
    def test_profiling_requires_explicit_pace_only_selection(self):
        available = {"backends": ["pace", "asio", "libuv"], "pace_profiling": True}
        self.assertTrue(RUNNER.validate_profiling_mode(available, "pace", False))
        for selection, required in ((None, False), ("pace,asio", False), ("asio", False),
                                    ("pace", True), ("pace,asio,libuv", True)):
            with self.subTest(selection=selection, required=required), self.assertRaises(ValueError):
                RUNNER.validate_profiling_mode(available, selection, required)

    def test_normal_and_legacy_binaries_remain_eligible_for_comparison(self):
        for available in ({"pace_profiling": False}, {}):
            self.assertFalse(RUNNER.validate_profiling_mode(available, None, True))

    def test_rejects_non_boolean_capability(self):
        for value in (1, "true", None):
            with self.subTest(value=value), self.assertRaises(ValueError):
                RUNNER.validate_profiling_mode({"pace_profiling": value}, "pace", False)

    def test_cli_rejects_instrumented_comparison_before_running_trials(self):
        capabilities = json.dumps({"backends": ["pace", "asio", "libuv"], "pace_profiling": True})
        with mock.patch("sys.argv", ["run_benchmarks.py", "--executable", str(RUNNER_PATH), "--require-all"]), \
             mock.patch.object(RUNNER.subprocess, "check_output", return_value=capabilities), \
             mock.patch.object(RUNNER.subprocess, "run") as run, \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as stopped:
            RUNNER.main()
        self.assertEqual(stopped.exception.code, 2)
        run.assert_not_called()


class ProfileSummaryTests(unittest.TestCase):
    def setUp(self):
        record_tests = BenchmarkRecordValidationTests()
        record_tests.setUp()
        self.report = {
            "environment": {
                "status": "completed", "measurement_kind": "instrumented_diagnostic",
                "fair_backend_comparison": False, "profile_window": RUNNER.PROFILE_WINDOW,
                "configuration": {**vars(record_tests.config), "repeats": 2, "backends": "pace"},
            },
            "runs": [record_tests.diagnostic_record(), record_tests.diagnostic_record()],
        }
        self.report["runs"][1]["trial"] = 2
        stages = self.report["runs"][1]["pace_profile"]["stages"]
        stages[0].update(inclusive_ns=2000, exclusive_ns=400)
        stages[1].update(calls=40, inclusive_ns=1600, exclusive_ns=1600)
        for record in self.report["runs"]:
            record["pace_profile"]["stages"].append(
                {"name": "send", "calls": 0, "inclusive_ns": 0, "exclusive_ns": 0})

    def test_aggregates_per_trial_values_and_preserves_stage_spread(self):
        summary = PROFILE.summarize(self.report)
        rows = {row["stage"]: row for row in summary["stages"]}
        self.assertEqual(summary["trials"], 2)
        self.assertEqual(rows["recv"]["median_calls"], 30)
        self.assertEqual(rows["recv"]["median_exclusive_ns"], 1250)
        self.assertEqual(rows["recv"]["min_exclusive_ns"], 900)
        self.assertEqual(rows["recv"]["max_exclusive_ns"], 1600)
        self.assertEqual(rows["recv"]["median_exclusive_share_percent"], 85)
        self.assertEqual(rows["recv"]["median_inclusive_ns_per_call"], 42.5)
        self.assertIsNone(rows["send"]["median_inclusive_ns_per_call"])
        self.assertIn("not CPU sampling", PROFILE.markdown(summary))

    def test_rejects_failed_normal_or_mixed_reports(self):
        for changes in ({"status": "failed"}, {"measurement_kind": "uninstrumented_benchmark"},
                        {"fair_backend_comparison": True}, {"profile_window": "steady state"}):
            report = copy.deepcopy(self.report)
            report["environment"].update(changes)
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                PROFILE.summarize(report)
        for value in (None, [], {}, {"environment": None}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                PROFILE.summarize(value)
        self.report["environment"]["configuration"]["backends"] = "pace,asio"
        with self.assertRaises(ValueError):
            PROFILE.summarize(self.report)

    def test_requires_complete_consistent_trial_set(self):
        bad_reports = []
        for count in (0, 1, 3):
            report = copy.deepcopy(self.report)
            report["environment"]["configuration"]["repeats"] = count
            bad_reports.append(report)
        report = copy.deepcopy(self.report)
        report["runs"][1]["trial"] = 1
        bad_reports.append(report)
        report = copy.deepcopy(self.report)
        report["runs"][1]["pace_profile"]["stages"].pop()
        bad_reports.append(report)
        report = copy.deepcopy(self.report)
        report["runs"][1]["pace_profile"]["stages"][1]["exclusive_ns"] -= 1
        bad_reports.append(report)
        for report in bad_reports:
            with self.subTest(report=report), self.assertRaises(ValueError):
                PROFILE.summarize(report)

    def test_cli_refuses_to_overwrite_raw_evidence(self):
        with mock.patch("sys.argv", ["run_profile.py", "--input", "profile.json", "--output", "profile.md"]), \
             contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as stopped:
            PROFILE.main()
        self.assertEqual(stopped.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
