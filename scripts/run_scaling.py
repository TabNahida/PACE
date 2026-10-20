#!/usr/bin/env python3
"""Measure PACE worker/execution scaling with fixed, balanced TCP clients."""

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

from run_benchmarks import command_output, cpu_model


MODES = ("runtime", "caller", "external")
TIME_FIELDS = ("setup_ns", "launch_ns", "startup_ns", "client_setup_warmup_ns", "steady_ns", "shutdown_ns")


def validate_record(record, mode, workers, trial, args):
    if not isinstance(record, dict):
        raise ValueError("scaling output must be a JSON object")
    expected = {
        "benchmark": "tcp_worker_scaling", "mode": mode, "trial": trial,
        "worker_count": workers, "server_threads": workers,
        "spawned_server_threads": workers - 1 if mode == "caller" else workers,
        "caller_runs_reactor": mode == "caller", "client_threads": args.clients,
        "connections": args.clients, "connections_per_worker": args.clients // workers,
        "listener_count": workers, "pipeline": 1, "payload_bytes": args.bytes,
        "requests_per_client": args.requests, "warmup_per_client": args.warmup,
        "socket_timeout_ms": args.timeout_ms, "completed_requests": args.clients * args.requests,
        "checksum": args.clients * (args.clients + 1) // 2 * (args.requests * (args.requests + 1) // 2),
        "validated": True, "instrumented": False,
    }
    for field, value in expected.items():
        if type(record.get(field)) is not type(value) or record[field] != value:
            raise ValueError(f"unexpected {field}: {record.get(field)!r}, expected {value!r}")
    counts = record.get("per_worker_completed_requests")
    if (not isinstance(counts, list) or len(counts) != workers or
            any(type(value) is not int or value != args.clients // workers * args.requests for value in counts)):
        raise ValueError("worker completion counts are missing or unbalanced")
    for field in TIME_FIELDS:
        if type(record.get(field)) is not int or record[field] <= 0:
            raise ValueError(f"{field} must be a positive integer")
    if record["startup_ns"] != record["setup_ns"] + record["launch_ns"]:
        raise ValueError("startup_ns must equal setup_ns + launch_ns")
    for field in ("seconds", "requests_per_second", "roundtrip_mib_per_second", "p50_us", "p99_us", "max_us"):
        value = record.get(field)
        if type(value) not in (float, int) or not math.isfinite(value) or value <= 0:
            raise ValueError(f"{field} must be finite and positive")
    if not record["p50_us"] <= record["p99_us"] <= record["max_us"]:
        raise ValueError("latency percentiles are reversed")
    if record["max_us"] > record["seconds"] * 1e6 + 0.001:
        raise ValueError("request latency exceeds the whole measured phase")
    expected_metrics = {
        "seconds": record["steady_ns"] / 1e9,
        "requests_per_second": expected["completed_requests"] / record["seconds"],
        "roundtrip_mib_per_second": record["requests_per_second"] * args.bytes * 2 / 1048576,
    }
    for field, value in expected_metrics.items():
        if not math.isclose(record[field], value, rel_tol=1e-8, abs_tol=1e-9):
            raise ValueError(f"{field} is inconsistent with counts/elapsed time")


def summarize(records, cells, repeats):
    rows = []
    for mode, workers in cells:
        samples = [record for record in records if record["mode"] == mode and record["worker_count"] == workers]
        if len(samples) != repeats or {sample["trial"] for sample in samples} != set(range(1, repeats + 1)):
            raise ValueError("missing or duplicate scaling cell/trial")
        row = {"mode": mode, "workers": workers, "trials": len(samples)}
        for field in ("requests_per_second", "p50_us", "p99_us", "max_us", *TIME_FIELDS):
            values = [sample[field] for sample in samples]
            row[field] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
        rows.append(row)
    return rows


def markdown(report):
    config = report["configuration"]
    lines = [
        "# PACE worker/execution scaling", "",
        f"{config['repeats']} trials per cell, {config['clients']} fixed clients, "
        f"{config['bytes']}-byte messages, {config['requests']} measured + {config['warmup']} warmup requests/client.", "",
        "PACE-only loopback echo with one listening port per worker. These results are separate from the Asio/libuv comparison.", "",
        "| Mode | Workers | Requests/s median | Requests/s min-max | p50 us median | p99 us median |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for row in report["summary"]:
        rate = row["requests_per_second"]
        lines.append(f"| {row['mode']} | {row['workers']} | {rate['median']:,.0f} | "
                     f"{rate['min']:,.0f}-{rate['max']:,.0f} | {row['p50_us']['median']:.2f} | {row['p99_us']['median']:.2f} |")
    lines += ["", "Lifecycle times below are medians in milliseconds; raw JSON also preserves every range and trial.", "",
              "| Mode | Workers | Setup ms | Launch ms | Startup sum ms | Clients/warmup ms | Shutdown ms |",
              "|---|---:|---:|---:|---:|---:|---:|---:|"]
    for row in report["summary"]:
        values = [row[field]["median"] / 1e6 for field in ("setup_ns", "launch_ns", "startup_ns", "client_setup_warmup_ns", "shutdown_ns")]
        lines.append(f"| {row['mode']} | {row['workers']} | " + " | ".join(f"{value:.3f}" for value in values) + " |")
    lines += ["", *[f"- {limitation}" for limitation in report["limitations"]], ""]
    return "\n".join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--build-dir", type=Path, help="Limit automatic binary discovery (default: project build/)")
    parser.add_argument("--output", type=Path, default=Path("benchmark-results/scaling.json"))
    parser.add_argument("--modes", default=",".join(MODES))
    parser.add_argument("--workers", default="1,2,4,8")
    parser.add_argument("--clients", type=int, default=8)
    parser.add_argument("--requests", type=int, default=2000)
    parser.add_argument("--warmup", type=int, default=200)
    parser.add_argument("--bytes", type=int, default=64)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout-ms", type=int, default=5000, help="Socket send/receive timeout")
    parser.add_argument("--timeout", type=float, default=60, help="Whole-process timeout per cell/trial, seconds")
    parser.add_argument("--label", default="local-scaling")
    args = parser.parse_args()
    try:
        workers = [int(value) for value in args.workers.split(",")]
    except ValueError:
        parser.error("--workers must be comma-separated integers")
    modes = args.modes.split(",")
    if (not workers or len(set(workers)) != len(workers) or any(not 1 <= value <= 64 for value in workers) or
            not modes or len(set(modes)) != len(modes) or any(mode not in MODES for mode in modes)):
        parser.error("unknown, duplicate or out-of-range workers/modes")
    if (not 1 <= args.clients <= 256 or any(args.clients < value or args.clients % value for value in workers) or
            not 1 <= args.requests <= 10000000 or args.clients * args.requests > 20000000 or
            not 0 <= args.warmup <= 1000000 or not 8 <= args.bytes <= 1048576 or
            not 1 <= args.repeats <= 100 or not 1 <= args.timeout_ms <= 600000 or
            not math.isfinite(args.timeout) or args.timeout <= 0):
        parser.error("out-of-range workload; fixed clients must be a positive multiple of every worker count")
    if args.output.suffix.lower() != ".json":
        parser.error("--output must end in .json")
    root = Path(__file__).resolve().parent.parent
    if args.executable is None:
        build_dir = args.build_dir.resolve() if args.build_dir else root / "build"
        candidates = [path for name in ("pace_scaling", "pace_scaling.exe") for path in build_dir.rglob(name) if path.is_file()]
        if not candidates:
            parser.error("build pace_scaling or pass --executable")
        args.executable = max(candidates, key=lambda path: path.stat().st_mtime)
    try:
        executable = args.executable.resolve(strict=True)
        build = json.loads(subprocess.check_output([str(executable), "--list"], text=True, timeout=10))
    except (OSError, ValueError, subprocess.SubprocessError) as error:
        parser.error(f"cannot inspect executable: {error}")
    if (not isinstance(build, dict) or build.get("profiling_enabled") is not False or
            type(build.get("ndebug")) is not bool or type(build.get("cpp_standard")) is not int or
            build["cpp_standard"] < 202002 or not isinstance(build.get("compiler"), str) or not build["compiler"] or
            not isinstance(build.get("modes"), list) or any(mode not in build["modes"] for mode in modes)):
        parser.error("selected executable must expose the requested modes and uninstrumented C++20 build metadata")
    cells = [(mode, count) for count in workers for mode in modes]
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    markdown_path = output.with_suffix(".md")
    if markdown_path.exists():
        markdown_path.unlink()
    source_paths = [root / "benchmarks/scaling_bench.cpp", Path(__file__).resolve(),
                    *sorted((root / "include").rglob("*.hpp")), *sorted((root / "src").glob("*.cpp"))]
    report = {
        "schema_version": 1, "status": "running", "measurement_kind": "uninstrumented_worker_scaling",
        "label": args.label, "started_utc": datetime.now(timezone.utc).isoformat(),
        "environment": {"platform": platform.platform(), "machine": platform.machine(), "cpu_model": cpu_model(),
                        "logical_cpus": os.cpu_count(), "executable": str(executable),
                        "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(), "build": build,
                        "git_commit": command_output(["git", "-c", f"safe.directory={root}", "rev-parse", "HEAD"], cwd=root),
                        "git_status_tracked": command_output(["git", "-c", f"safe.directory={root}", "status", "--porcelain", "--untracked-files=no"], cwd=root),
                        "source_sha256_at_run": {str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest() for path in source_paths}},
        "configuration": {"modes": modes, "workers": workers, "clients": args.clients, "requests": args.requests,
                          "warmup": args.warmup, "bytes": args.bytes, "repeats": args.repeats,
                          "timeout_ms": args.timeout_ms, "process_timeout_seconds": args.timeout},
        "execution": {"runtime": "N spawned reactor threads; calling thread waits in runtime::run",
                      "caller": "N-1 spawned reactor threads plus calling thread in runtime::run_on_current_thread",
                      "external": "N application-owned io_context threads; calling thread joins them"},
        "timing_boundaries": {
            "setup_ns": "Context and listener construction, port binding, accept task and readiness marker submission",
            "launch_ns": "Execution call entry until every reactor dispatches its initial posted marker",
            "startup_ns": "setup_ns + launch_ns; excludes coordinator creation between these intervals",
            "client_setup_warmup_ns": "After all reactors report ready: client thread creation, connect, warmup and shared start-gate readiness",
            "steady_ns": "Immediately before releasing the shared start gate until all client threads join; includes client-side full response validation",
            "shutdown_ns": "Immediately before stop() until the execution call returns after reactor threads join; excludes object destruction"},
        "aggregation": "Median/min/max across fresh-process trials for each mode/worker cell; latency percentiles computed per trial, then medians",
        "limitations": [
            "Separate PACE-only workload: one port per worker with fixed, evenly balanced persistent clients; no Asio/libuv comparison",
            "Clients, coordinator and server run on one host and contend for CPU; this is not an isolated server-capacity or linear-scaling estimate",
            "Changing workers also changes connections per reactor while total clients and requests stay fixed; workers are never inferred from CPU count",
            "All modes use the same coordinator plus one blocking thread per client; caller mode reduces spawned reactor threads, not the N active reactor threads",
            "Startup and steady-state windows are separate; warmup and startup are excluded from throughput, but client scheduling and join overhead are included",
            "TCP_NODELAY, pipeline=1 and loopback echo do not establish HTTP, accept balancing, remote-network or mixed-workload performance",
            "No CPU affinity or frequency controls are applied; run benchmarks sequentially on an otherwise idle host and inspect trial spread",
            "Source hashes describe files at runner invocation, not proof that a supplied binary was built from them; retain the compiler command with results",
        ],
        "commands": [], "runs": [], "summary": None, "failure": None,
    }

    def save():
        output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

    save()
    for trial in range(1, args.repeats + 1):
        offset = (trial - 1) % len(cells)
        for mode, count in cells[offset:] + cells[:offset]:
            command = [str(executable), "--mode", mode, "--workers", str(count), "--clients", str(args.clients),
                       "--requests", str(args.requests), "--warmup", str(args.warmup), "--bytes", str(args.bytes),
                       "--timeout-ms", str(args.timeout_ms), "--trial", str(trial)]
            report["commands"].append(command)
            print(f"Trial {trial}/{args.repeats}: {mode}, {count} workers", flush=True)
            try:
                result = subprocess.run(command, text=True, capture_output=True, timeout=args.timeout)
                if result.returncode:
                    raise ValueError(f"exit {result.returncode}: {result.stderr.strip()[:2000]}")
                record = json.loads(result.stdout)
                validate_record(record, mode, count, trial, args)
                report["runs"].append(record)
                save()
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                report["failure"] = {"trial": trial, "mode": mode, "workers": count, "error": str(error)}
                break
        if report["failure"]:
            break
    report["status"] = "failed" if report["failure"] else "completed"
    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    if not report["failure"]:
        report["summary"] = summarize(report["runs"], cells, args.repeats)
    save()
    if report["failure"]:
        print("Scaling measurement failed: " + json.dumps(report["failure"]), file=sys.stderr)
        return 1
    text = markdown(report)
    markdown_path.write_text(text, encoding="utf-8")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with Path(os.environ["GITHUB_STEP_SUMMARY"]).open("a", encoding="utf-8") as destination:
            destination.write("\n" + text)
    print(text)
    print(f"Saved all scaling trials to {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
