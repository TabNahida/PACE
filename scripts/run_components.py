#!/usr/bin/env python3
"""Measure defined PACE operations; preserve raw trials, units and environment."""

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


OPERATIONS = {
    "lazy_task": "Create and destroy one unstarted task via a non-inlined indirect factory call",
    "await_chain": "Await and destroy one synchronous chain containing chain_depth nested task frames; no reactor",
    "empty_poll": "Call the native poller with timeout=0 and 256 output slots, no user descriptors registered",
    "yield": "One yield suspension/resumption, including ready queue and reactor pass",
    "post": "Enqueue and invoke one small callback from the same context; one callback outstanding",
    "cross_thread_post": "One producer post call with a concurrent reactor consumer; timed enqueue batches include mutex/wake work, exclude final drain",
    "spawn_completion": "Create, spawn, run and destroy one child task; bounded batches plus one completion marker per batch",
    "zero_timer": "One sleep_for(0ns), including deadline clock read, heap enqueue/expiry and resumption; one timer outstanding",
}


def validate_record(record, component, trial, args):
    if not isinstance(record, dict):
        raise ValueError("component output must be a JSON object")
    expected = {
        "benchmark": "components", "component": component, "trial": trial,
        "iterations": args.iterations, "warmup": args.warmup, "chain_depth": args.depth,
        "batch_size": args.batch, "threads": 2 if component == "cross_thread_post" else 1,
        "completed_operations": args.iterations,
        "checksum": 0 if component in ("lazy_task", "empty_poll") else args.iterations * (args.iterations - 1) // 2,
        "validated": True,
    }
    for field, value in expected.items():
        if type(record.get(field)) is not type(value) or record[field] != value:
            raise ValueError(f"unexpected {field}: {record.get(field)!r}, expected {value!r}")
    if type(record.get("elapsed_ns")) is not int or record["elapsed_ns"] <= 0:
        raise ValueError("elapsed_ns must be a positive integer")
    cost = record.get("ns_per_operation")
    if type(cost) not in (float, int) or not math.isfinite(cost) or cost <= 0:
        raise ValueError("ns_per_operation must be finite and positive")
    if not math.isclose(cost, record["elapsed_ns"] / args.iterations, rel_tol=1e-8, abs_tol=1e-8):
        raise ValueError("ns_per_operation does not match elapsed_ns / iterations")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, help="Default: newest build/**/pace_components(.exe)")
    parser.add_argument("--output", type=Path, default=Path("benchmark-results/components.json"))
    parser.add_argument("--components", help="Comma-separated component names; default: all eight")
    parser.add_argument("--iterations", type=int, default=100000)
    parser.add_argument("--warmup", type=int, default=5000)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--depth", type=int, default=4, help="Awaited task frames per await_chain operation")
    parser.add_argument("--batch", type=int, default=256, help="Spawn task batch size / cross-thread post enqueue batch size")
    parser.add_argument("--timeout", type=float, default=60, help="Hard wall-clock limit per process, seconds")
    parser.add_argument("--label", default="local-components")
    args = parser.parse_args()
    if args.output.suffix.lower() != ".json":
        parser.error("--output must end in .json")
    if not (1 <= args.iterations <= 100000000 and 0 <= args.warmup <= 10000000 and
            1 <= args.repeats <= 100 and 1 <= args.depth <= 64 and 1 <= args.batch <= 65536 and
            math.isfinite(args.timeout) and args.timeout > 0):
        parser.error("out-of-range component configuration")
    root = Path(__file__).resolve().parent.parent
    if args.executable is None:
        candidates = [path for name in ("pace_components", "pace_components.exe")
                      for path in (root / "build").rglob(name) if path.is_file()]
        if not candidates:
            parser.error("build pace_components with --components=y or pass --executable")
        args.executable = max(candidates, key=lambda path: path.stat().st_mtime)
    executable = args.executable.resolve(strict=True)
    build = json.loads(subprocess.check_output([str(executable), "--list"], text=True, timeout=10))
    if build.get("profiling_enabled") is not False:
        parser.error("component timing requires an uninstrumented executable (profiling_enabled=false)")
    components = args.components.split(",") if args.components else list(OPERATIONS)
    if not components or len(set(components)) != len(components) or any(
            name not in OPERATIONS or name not in build["components"] for name in components):
        parser.error("unknown, duplicate or unavailable component")
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "schema_version": 1, "status": "running", "label": args.label,
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "environment": {
            "platform": platform.platform(), "machine": platform.machine(), "cpu_model": cpu_model(),
            "logical_cpus": os.cpu_count(),
            "git_commit": command_output(["git", "-c", f"safe.directory={root}", "rev-parse", "HEAD"], cwd=root),
            "git_status": command_output(["git", "-c", f"safe.directory={root}", "status", "--porcelain"], cwd=root),
            "executable": str(executable),
            "executable_sha256": hashlib.sha256(executable.read_bytes()).hexdigest(), "build": build,
        },
        "configuration": {key: value for key, value in vars(args).items() if key not in {"executable", "output"}},
        "operation_definitions": {name: OPERATIONS[name] for name in components},
        "aggregation": "Median, minimum and maximum batch-average ns/operation across repeated processes; not operation-latency percentiles",
        "limitations": [
            "Loop/checksum and harness overhead remain included; no empty-loop subtraction",
            "Scheduler cases include real loop/poller housekeeping; costs are not additive TCP stage estimates",
            "Fixed depth/batch and a single outstanding yield, callback or zero timer do not establish scaling",
            "Only compare the same component and configuration on the same machine; no baseline-library comparison",
        ],
        "commands": [], "runs": [], "summary": None, "failure": None,
    }
    summary_path = output.with_suffix(".md")
    if summary_path.is_file():
        summary_path.unlink()
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for trial in range(1, args.repeats + 1):
        offset = (trial - 1) % len(components)
        for component in components[offset:] + components[:offset]:
            command = [str(executable), "--component", component, "--iterations", str(args.iterations),
                       "--warmup", str(args.warmup), "--depth", str(args.depth), "--batch", str(args.batch),
                       "--trial", str(trial)]
            report["commands"].append(command)
            print(f"Trial {trial}/{args.repeats}: {component}", flush=True)
            try:
                result = subprocess.run(command, text=True, capture_output=True, timeout=args.timeout)
                if result.returncode:
                    raise ValueError(f"exit {result.returncode}: {result.stderr.strip()}")
                record = json.loads(result.stdout)
                validate_record(record, component, trial, args)
                report["runs"].append(record)
                output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                report["failure"] = {"component": component, "trial": trial, "error": str(error)}
                break
        if report["failure"]:
            break
    report["status"] = "failed" if report["failure"] else "completed"
    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    if not report["failure"]:
        report["summary"] = []
        for component in components:
            costs = [row["ns_per_operation"] for row in report["runs"] if row["component"] == component]
            report["summary"].append({"component": component, "trials": len(costs),
                                      "median_ns_per_operation": statistics.median(costs),
                                      "min_ns_per_operation": min(costs), "max_ns_per_operation": max(costs)})
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if report["failure"]:
        print("Component benchmark failed: " + json.dumps(report["failure"]), file=sys.stderr)
        return 1
    lines = ["| Component | Median ns/op | Min-max ns/op | Trials |", "|---|---:|---:|---:|"]
    for row in report["summary"]:
        lines.append(f"| {row['component']} | {row['median_ns_per_operation']:.2f} | "
                     f"{row['min_ns_per_operation']:.2f}-{row['max_ns_per_operation']:.2f} | {row['trials']} |")
    lines += ["", "Each op has the component-specific definition saved in the JSON report.",
              "These are repeated batch averages, not per-operation latency percentiles.", ""]
    table = "\n".join(lines)
    summary_path.write_text(table, encoding="utf-8")
    print(table)
    print(f"Saved raw component measurements to {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
