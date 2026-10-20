#!/usr/bin/env python3
"""Run bounded, rotated backend comparisons and preserve every raw result."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess
import sys


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


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, help="Default: newest build/**/pace_bench(.exe)")
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
    if args.repeats < 1 or args.repeats > 100 or args.timeout <= 0:
        parser.error("repeats must be 1..100 and timeout must be positive")
    root = Path(__file__).resolve().parent.parent
    if args.executable is None:
        candidates = [path for name in ("pace_bench", "pace_bench.exe")
                      for path in (root / "build").rglob(name) if path.is_file()]
        if not candidates:
            parser.error("no benchmark executable found under build/; build pace_bench or pass --executable")
        args.executable = max(candidates, key=lambda path: path.stat().st_mtime)
    executable = args.executable.resolve(strict=True)
    available = json.loads(subprocess.check_output([str(executable), "--list"], text=True, timeout=10))
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
        "status": "running",
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "label": args.label,
        "platform": platform.platform(),
        "machine": platform.machine(),
        "cpu_model": cpu_model(),
        "logical_cpus": os.cpu_count(),
        "python": platform.python_version(),
        "git_commit": command_output(["git", "rev-parse", "HEAD"], cwd=root),
        "git_status": command_output(["git", "status", "--porcelain"], cwd=root),
        "executable": str(executable),
        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(),
        "backend_versions": available,
        "configuration": {key: value for key, value in vars(args).items() if key not in {"executable", "output"}},
        "server_threads": 1,
        "pipeline": 1,
        "timing": "TCP round trip including send, recv and complete payload validation; connection setup and warmup excluded",
        "aggregation": "Median throughput and median of per-run p50/p99; min/max throughput show run spread",
        "limitations": [
            "Loopback closed-loop workload; client threads, kernel and CPU scheduler contribute to results",
            "One reactor thread per backend; does not establish multicore scaling, HTTP speed or peak capacity",
            "Library defaults and public async APIs; PACE/Asio use coroutines, libuv uses callbacks",
            "CPU affinity and frequency are not controlled by this script; shared CI runners are noisy",
            "Per-run latency percentiles are closed-loop observed latency, without coordinated-omission correction",
        ],
        "commands": [],
    }
    manifest_path = output / "environment.json"
    for old_summary in (output / "summary.json", output / "summary.md"):
        if old_summary.is_file():
            old_summary.unlink()
    manifest_path.write_text(json.dumps(metadata, indent=2) + "\n")
    report_path.write_text(json.dumps({"environment": metadata, "runs": [], "summary": None}, indent=2) + "\n")
    records = []
    failure = None
    with (output / "raw.jsonl").open("w") as raw:
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
                    if record.get("backend") != backend or record.get("payload_verified") is not True:
                        raise RuntimeError("backend output failed validation")
                    if record.get("completed_requests") != args.clients * args.requests:
                        raise RuntimeError("incomplete workload")
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
    manifest_path.write_text(json.dumps(metadata, indent=2) + "\n")
    if failure:
        report_path.write_text(json.dumps({"environment": metadata, "runs": records, "summary": None}, indent=2) + "\n")
        print("Benchmark failed: " + json.dumps(failure), file=sys.stderr)
        return 1
    summary = []
    for backend in backends:
        samples = [record for record in records if record["backend"] == backend]
        summary.append({
            "backend": backend,
            "trials": len(samples),
            "median_requests_per_second": statistics.median(sample["requests_per_second"] for sample in samples),
            "min_requests_per_second": min(sample["requests_per_second"] for sample in samples),
            "max_requests_per_second": max(sample["requests_per_second"] for sample in samples),
            "median_p50_us": statistics.median(sample["p50_us"] for sample in samples),
            "median_p99_us": statistics.median(sample["p99_us"] for sample in samples),
        })
    (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    report_path.write_text(json.dumps({"environment": metadata, "runs": records, "summary": summary}, indent=2) + "\n")
    rows = [
        "| Backend | Requests/s (median) | Min–max requests/s | p50 µs¹ | p99 µs¹ |",
        "|---|---:|---:|---:|---:|",
    ]
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
    (output / "summary.md").write_text(report)
    github_summary = os.environ.get("GITHUB_STEP_SUMMARY")
    if github_summary:
        with Path(github_summary).open("a") as destination:
            destination.write(f"\n### TCP echo: {args.label}\n\n{report}\n")
    print(report)
    print(f"Saved benchmark evidence to {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
