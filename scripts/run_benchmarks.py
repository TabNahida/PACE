#!/usr/bin/env python3
"""Run bounded, rotated backend comparisons and preserve every raw result."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys

PROFILE_WINDOW = "server event-loop lifetime including startup, warmup and shutdown"


def validate_profile(profile):
    """Validate diagnostic counters without interpreting them as CPU samples."""
    if not isinstance(profile, dict) or profile.get("window") != PROFILE_WINDOW:
        raise ValueError("profile must identify the server lifetime measurement window")
    stages = profile.get("stages")
    if not isinstance(stages, list) or not stages:
        raise ValueError("profile stages must be a nonempty array")
    names = set()
    for stage in stages:
        if not isinstance(stage, dict) or not isinstance(stage.get("name"), str) or not stage["name"]:
            raise ValueError("each profile stage must have a name")
        if stage["name"] in names:
            raise ValueError("profile stage names must be unique")
        names.add(stage["name"])
        for field in ("calls", "inclusive_ns", "exclusive_ns"):
            if type(stage.get(field)) is not int or stage[field] < 0:
                raise ValueError(f"profile {field} must be a nonnegative integer")
        if stage["exclusive_ns"] > stage["inclusive_ns"]:
            raise ValueError("profile exclusive time exceeds inclusive time")
        if stage["calls"] == 0 and (stage["inclusive_ns"] or stage["exclusive_ns"]):
            raise ValueError("profile stage without calls cannot contain elapsed time")
    if "reactor_run" not in names:
        raise ValueError("profile lacks the reactor_run stage")
    reactor = next(stage for stage in stages if stage["name"] == "reactor_run")
    if reactor["calls"] != 1 or reactor["inclusive_ns"] == 0:
        raise ValueError("profile must contain one completed, timed reactor run")


def validate_profiling_mode(available, requested_backends, require_all):
    if not isinstance(available, dict):
        raise ValueError("executable capabilities must be a JSON object")
    enabled = available.get("pace_profiling", False)
    if type(enabled) is not bool:
        raise ValueError("pace_profiling capability must be a boolean")
    if enabled and (requested_backends != "pace" or require_all):
        raise ValueError("profiling builds are diagnostic only: pass --backends pace without --require-all; "
                         "use an uninstrumented build for backend comparisons")
    return enabled


def command_output(command, cwd=None):
    try:
        result = subprocess.run(command, cwd=cwd, text=True, capture_output=True, timeout=10)
        return result.stdout.strip() if result.returncode == 0 else None
    except (OSError, subprocess.TimeoutExpired):
        return None


def cpu_model():
    cpuinfo = Path("/proc/cpuinfo")
    if cpuinfo.is_file():
        for line in cpuinfo.read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    if sys.platform == "darwin":
        return command_output(["sysctl", "-n", "machdep.cpu.brand_string"])
    return platform.processor() or os.environ.get("PROCESSOR_IDENTIFIER")


def validate_record(record, backend, trial, config, instrumented=False):
    """Reject mismatched workloads and unusable measurements before aggregation."""
    if not isinstance(record, dict):
        raise ValueError("benchmark output must be a JSON object")
    expected = {
        "benchmark": "tcp_echo", "backend": backend, "trial": trial,
        "server_threads": 1, "client_threads": config.clients,
        "connections": config.clients, "pipeline": 1, "payload_bytes": config.bytes,
        "requests_per_client": config.requests, "warmup_per_client": config.warmup,
        "completed_requests": config.clients * config.requests, "payload_verified": True,
    }
    for key, value in expected.items():
        if type(record.get(key)) is not type(value) or record[key] != value:
            raise ValueError(f"unexpected {key}: expected {value!r}, received {record.get(key)!r}")
    for key in ("seconds", "requests_per_second", "roundtrip_mib_per_second", "p50_us", "p99_us", "max_us"):
        value = record.get(key)
        if type(value) not in (int, float) or not math.isfinite(value) or value <= 0:
            raise ValueError(f"{key} must be finite and positive")
    if not record["p50_us"] <= record["p99_us"] <= record["max_us"]:
        raise ValueError("invalid latency percentile ordering")
    expected_throughput = record["completed_requests"] / record["seconds"]
    if not math.isclose(record["requests_per_second"], expected_throughput, rel_tol=0.01):
        raise ValueError("throughput does not match completed requests / elapsed time")
    expected_bandwidth = record["requests_per_second"] * config.bytes * 2 / (1024 * 1024)
    if not math.isclose(record["roundtrip_mib_per_second"], expected_bandwidth, rel_tol=0.01, abs_tol=0.000001):
        raise ValueError("bandwidth does not match throughput / payload size")
    if type(record.get("instrumented", False)) is not bool or record.get("instrumented", False) != instrumented:
        raise ValueError("instrumentation flag does not match the executable capability")
    if instrumented:
        if backend != "pace":
            raise ValueError("profiling output is only supported for PACE")
        validate_profile(record.get("pace_profile"))
    elif "pace_profile" in record:
        raise ValueError("uninstrumented comparisons must not contain profiling counters")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, help="Default: newest build/**/pace_bench(.exe)")
    parser.add_argument("--build-dir", type=Path, help="Restrict automatic executable discovery to this directory")
    parser.add_argument("--output", type=Path, default=Path("benchmarks/results/local"))
    parser.add_argument("--backends", help="Comma-separated backends; default: all compiled backends")
    parser.add_argument("--require-all", action="store_true", help="Fail unless pace, asio, and libuv were built")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--requests", type=int, default=5000)
    parser.add_argument("--warmup", type=int, default=500)
    parser.add_argument("--bytes", "--message-size", dest="bytes", type=int, default=64)
    parser.add_argument("--timeout", type=float, default=60, help="Hard wall-clock limit per subprocess, seconds")
    parser.add_argument("--label", default="local", help="Hardware / runner label, without performance claims")
    args = parser.parse_args()
    if args.repeats < 1 or args.repeats > 100 or not math.isfinite(args.timeout) or args.timeout <= 0:
        parser.error("repeats must be 1..100 and timeout must be positive")
    root = Path(__file__).resolve().parent.parent
    if args.executable is None:
        build_dir = args.build_dir.resolve() if args.build_dir else root / "build"
        candidates = [path for name in ("pace_bench", "pace_bench.exe")
                      for path in build_dir.rglob(name) if path.is_file()]
        if not candidates:
            parser.error(f"no benchmark executable found under {build_dir}; build pace_bench or pass --executable")
        args.executable = max(candidates, key=lambda path: path.stat().st_mtime)
    executable = args.executable.resolve(strict=True)
    available = json.loads(subprocess.check_output([str(executable), "--list"], text=True, timeout=10))
    try:
        instrumented = validate_profiling_mode(available, args.backends, args.require_all)
    except ValueError as error:
        parser.error(str(error))
    backends = args.backends.split(",") if args.backends else available["backends"]
    if not backends or len(set(backends)) != len(backends):
        parser.error("backend list must be nonempty and contain no duplicates")
    missing = set(backends) - set(available["backends"])
    if args.require_all:
        missing |= {"pace", "asio", "libuv"} - set(backends)
    if missing:
        parser.error("required backends unavailable: " + ", ".join(sorted(missing)))
    destination = args.output.resolve()
    report_path = destination if destination.suffix == ".json" else destination / "report.json"
    output = destination.with_name(destination.stem + "-evidence") if destination.suffix == ".json" else destination
    output.mkdir(parents=True, exist_ok=True)
    report_path.parent.mkdir(parents=True, exist_ok=True)
    metadata = {
        "schema_version": 1,
        "measurement_kind": "instrumented_diagnostic" if instrumented else "uninstrumented_benchmark",
        "fair_backend_comparison": not instrumented and len(backends) > 1,
        "status": "running",
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "label": args.label,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpu_model": cpu_model(),
        "logical_cpus": os.cpu_count(),
        "python": platform.python_version(),
        # VM source synchronization can preserve the host owner's UID. Trust
        # this explicit build root for these read-only metadata commands only.
        "git_commit": command_output(["git", "-c", f"safe.directory={root}", "rev-parse", "HEAD"], cwd=root),
        "git_status": command_output(["git", "-c", f"safe.directory={root}", "status", "--porcelain"], cwd=root),
        "executable": str(executable),
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "backend_versions": available,
        "configuration": {key: str(value) if isinstance(value, Path) else value
                          for key, value in vars(args).items() if key not in {"executable", "output"}},
        "server_threads": 1,
        "pipeline": 1,
        "timing": "TCP round trip including send, recv and complete payload validation; connection setup and warmup excluded",
        "aggregation": "Median throughput and median of per-run p50/p99; min/max throughput show run spread",
        "acceptance_scope": "Complete, payload-correct, internally consistent measurements; does not certify faster performance",
        "limitations": [
            "Loopback closed-loop workload; client threads, kernel and CPU scheduler contribute to results",
            "One reactor thread per backend; does not establish multicore scaling, HTTP speed or peak capacity",
            "Library defaults and public async APIs; PACE/Asio use coroutines, libuv uses callbacks",
            "CPU affinity and frequency are not controlled by this script; shared CI runners are noisy",
            "Per-run latency percentiles are closed-loop observed latency, without coordinated-omission correction",
        ],
        "commands": [],
    }
    if instrumented:
        metadata["profile_window"] = PROFILE_WINDOW
        metadata["limitations"].insert(0, "Profiling adds timing overhead: throughput and latency are instrumented diagnostics, not fair backend comparisons")
    manifest_path = output / "environment.json"
    for old_summary in (output / "summary.json", output / "summary.md"):
        if old_summary.is_file():
            old_summary.unlink()
    manifest_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    report_path.write_text(json.dumps({"environment": metadata, "runs": [], "summary": None}, indent=2) + "\n", encoding="utf-8")
    records = []
    failure = None
    with (output / "raw.jsonl").open("w", encoding="utf-8") as raw:
        for trial in range(1, args.repeats + 1):
            offset = (trial - 1) % len(backends)
            order = backends[offset:] + backends[:offset]
            for backend in order:
                command = [str(executable), "--backend", backend, "--clients", str(args.clients),
                           "--requests", str(args.requests), "--warmup", str(args.warmup),
                           "--bytes", str(args.bytes), "--trial", str(trial)]
                metadata["commands"].append(command)
                print(f"Trial {trial}/{args.repeats}: {backend}", flush=True)
                try:
                    result = subprocess.run(command, text=True, capture_output=True, timeout=args.timeout)
                    if result.returncode:
                        raise RuntimeError(f"exit {result.returncode}: {result.stderr.strip()}")
                    record = json.loads(result.stdout)
                    validate_record(record, backend, trial, args, instrumented=instrumented)
                    records.append(record)
                    raw.write(json.dumps(record, separators=(",", ":")) + "\n")
                    raw.flush()
                except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
                    failure = {"backend": backend, "trial": trial, "error": str(error)}
                    break
            if failure:
                break
    metadata["finished_utc"] = datetime.now(timezone.utc).isoformat()
    metadata["failure"] = failure
    metadata["status"] = "failed" if failure else "completed"
    manifest_path.write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    if failure:
        report_path.write_text(json.dumps({"environment": metadata, "runs": records, "summary": None}, indent=2) + "\n", encoding="utf-8")
        print("Benchmark failed: " + json.dumps(failure), file=sys.stderr)
        return 1
    summary = []
    for backend in backends:
        samples = [record for record in records if record["backend"] == backend]
        summary.append({
            "backend": backend,
            "instrumented": instrumented,
            "trials": len(samples),
            "median_requests_per_second": statistics.median(sample["requests_per_second"] for sample in samples),
            "min_requests_per_second": min(sample["requests_per_second"] for sample in samples),
            "max_requests_per_second": max(sample["requests_per_second"] for sample in samples),
            "median_p50_us": statistics.median(sample["p50_us"] for sample in samples),
            "median_p99_us": statistics.median(sample["p99_us"] for sample in samples),
        })
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    report_path.write_text(json.dumps({"environment": metadata, "runs": records, "summary": summary}, indent=2) + "\n", encoding="utf-8")
    rows = [
        "| Backend | " + ("Instrumented requests/s (median)" if instrumented else "Requests/s (median)") + " | Min–max requests/s | p50 µs¹ | p99 µs¹ |",
        "|---|---:|---:|---:|---:|",
    ]
    if instrumented:
        rows = ["**Instrumented diagnostic run. Timing overhead is included; these results are not a fair backend comparison.**", ""] + rows
    for item in summary:
        rows.append(f"| {item['backend']} | {item['median_requests_per_second']:,.0f} | "
                    f"{item['min_requests_per_second']:,.0f}–{item['max_requests_per_second']:,.0f} | "
                    f"{item['median_p50_us']:.2f} | {item['median_p99_us']:.2f} |")
    rows += ["", "¹ Median of each run's percentile; not pooled percentiles.", "",
             f"{args.repeats} trials per backend; one server thread; {args.clients} persistent connections; "
             f"{args.bytes}-byte payload; one outstanding request per connection; "
             f"{args.requests} measured + {args.warmup} warmup requests per connection per trial.", "",
             "Loopback results describe this workload on this machine, not general superiority or multicore/HTTP performance.", "",
             "Environment and commands: `environment.json`. Every run: `raw.jsonl`.", ""]
    report = "\n".join(rows)
    (output / "summary.md").write_text(report, encoding="utf-8")
    github_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if github_summary:
        with Path(github_summary).open("a", encoding="utf-8") as destination:
            title = "TCP echo profiling diagnostic" if instrumented else "TCP echo"
            destination.write(f"\n### {title}: {args.label}\n\n{report}\n")
    print(report)
    print(f"Saved benchmark evidence to {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
