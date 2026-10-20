#!/usr/bin/env python3
"""Alternate before/after binaries, validate every run and retain paired estimates."""

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform
import random
import statistics
import subprocess
import sys

from run_benchmarks import command_output, cpu_model, validate_record as validate_tcp
from run_components import OPERATIONS, validate_record as validate_component


def paired_summary(pairs, metric, lower_is_better, bootstrap_samples, seed):
    ratios = [pair["after"] / pair["before"] for pair in pairs]
    generator = random.Random(seed)
    bootstrap = sorted(statistics.median(generator.choices(ratios, k=len(ratios)))
                       for _ in range(bootstrap_samples))
    low = bootstrap[math.floor(0.025 * (bootstrap_samples - 1))]
    high = bootstrap[math.ceil(0.975 * (bootstrap_samples - 1))]
    return {
        "metric": metric, "lower_is_better": lower_is_better,
        "before_median": statistics.median(pair["before"] for pair in pairs),
        "after_median": statistics.median(pair["after"] for pair in pairs),
        "before_min": min(pair["before"] for pair in pairs),
        "before_max": max(pair["before"] for pair in pairs),
        "after_min": min(pair["after"] for pair in pairs),
        "after_max": max(pair["after"] for pair in pairs),
        "median_paired_after_before_ratio": statistics.median(ratios),
        "min_paired_after_before_ratio": min(ratios), "max_paired_after_before_ratio": max(ratios),
        "median_ratio_bootstrap_95_percentile_interval": [low, high],
        "after_wins": sum(ratio < 1 if lower_is_better else ratio > 1 for ratio in ratios),
        "ties": sum(ratio == 1 for ratio in ratios), "pairs": len(pairs),
        "bootstrap_samples": bootstrap_samples, "bootstrap_seed": seed,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--before", required=True, type=Path)
    parser.add_argument("--after", required=True, type=Path)
    kind = parser.add_mutually_exclusive_group(required=True)
    kind.add_argument("--component", choices=list(OPERATIONS))
    kind.add_argument("--tcp", action="store_true", help="Compare TCP echo with --backend pace")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--pairs", type=int, default=8)
    parser.add_argument("--iterations", type=int, default=1000000)
    parser.add_argument("--depth", type=int, default=4)
    parser.add_argument("--batch", type=int, default=256)
    parser.add_argument("--clients", type=int, default=4)
    parser.add_argument("--requests", type=int, default=100000)
    parser.add_argument("--bytes", "--message-size", dest="bytes", type=int, default=64)
    parser.add_argument("--warmup", type=int, help="Default: 1000 TCP requests/client or 10000 component operations")
    parser.add_argument("--timeout", type=float, default=120)
    parser.add_argument("--bootstrap-samples", type=int, default=10000)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--label", default="paired-comparison")
    args = parser.parse_args()
    if args.warmup is None:
        args.warmup = 1000 if args.tcp else 10000
    if args.output.suffix.lower() != ".json" or not 2 <= args.pairs <= 100:
        parser.error("--output must end in .json and --pairs must be 2..100")
    if not math.isfinite(args.timeout) or args.timeout <= 0 or not 1000 <= args.bootstrap_samples <= 100000:
        parser.error("positive finite timeout and 1000..100000 bootstrap samples required")
    if args.tcp:
        valid = (1 <= args.clients <= 256 and 1 <= args.requests <= 10000000 and
                 args.clients * args.requests <= 20000000 and 0 <= args.warmup <= 1000000 and 8 <= args.bytes <= 1048576)
    else:
        valid = (1 <= args.iterations <= 100000000 and 0 <= args.warmup <= 10000000 and
                 1 <= args.depth <= 64 and 1 <= args.batch <= 65536)
    if not valid:
        parser.error("out-of-range workload configuration")
    binaries = {}
    for variant in ("before", "after"):
        executable = getattr(args, variant).resolve(strict=True)
        build = json.loads(subprocess.check_output([str(executable), "--list"], text=True, timeout=10))
        if args.tcp:
            if "pace" not in build.get("backends", []) or build.get("pace_profiling", False) is not False:
                parser.error(f"{variant} must be an uninstrumented TCP benchmark with the pace backend")
        elif args.component not in build.get("components", []) or build.get("profiling_enabled") is not False:
            parser.error(f"{variant} must be an uninstrumented component benchmark")
        binaries[variant] = {"path": str(executable), "sha256": hashlib.sha256(executable.read_bytes()).hexdigest(), "build": build}
    for field in ("compiler", "cpp_standard", "ndebug"):
        if binaries["before"]["build"].get(field) != binaries["after"]["build"].get(field):
            parser.error(f"before/after build metadata differs for {field}; compare matching compiler/configurations")
    root = Path(__file__).resolve().parent.parent
    output = args.output.resolve()
    output.parent.mkdir(parents=True, exist_ok=True)
    report = {
        "schema_version": 1, "status": "running", "label": args.label,
        "started_utc": datetime.now(timezone.utc).isoformat(), "binaries": binaries,
        "environment": {"platform": platform.platform(), "machine": platform.machine(), "cpu_model": cpu_model(),
                        "git_commit": command_output(["git", "-c", f"safe.directory={root}", "rev-parse", "HEAD"], cwd=root),
                        "git_status": command_output(["git", "-c", f"safe.directory={root}", "status", "--porcelain"], cwd=root)},
        "configuration": {key: value for key, value in vars(args).items() if key not in {"before", "after", "output"}},
        "operation": "TCP echo throughput, one PACE server thread" if args.tcp else OPERATIONS[args.component],
        "ratio_definition": "after / before; above 1 favors after for requests/s, below 1 favors after for ns/op",
        "uncertainty_method": "Resample whole paired ratios with replacement; percentile interval of bootstrap medians",
        "limitations": ["Exploratory interval assumes representative independent pairs; scheduling/frequency drift and autocorrelation can violate this",
                        "Small samples and short runs do not establish a portable performance improvement",
                        "No performance threshold changes the process exit code; correctness/measurement failures do"],
        "commands": [], "runs": [], "pairs": [], "summary": None, "failure": None,
    }
    metric = "requests_per_second" if args.tcp else "ns_per_operation"
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for trial in range(1, args.pairs + 1):
        values = {}
        order = ("before", "after") if trial % 2 else ("after", "before")
        for variant in order:
            command = [binaries[variant]["path"]]
            if args.tcp:
                command += ["--backend", "pace", "--clients", str(args.clients), "--requests", str(args.requests), "--bytes", str(args.bytes)]
            else:
                command += ["--component", args.component, "--iterations", str(args.iterations), "--depth", str(args.depth), "--batch", str(args.batch)]
            command += ["--warmup", str(args.warmup), "--trial", str(trial)]
            report["commands"].append({"pair": trial, "variant": variant, "command": command})
            print(f"Pair {trial}/{args.pairs}: {variant}", flush=True)
            try:
                result = subprocess.run(command, text=True, capture_output=True, timeout=args.timeout)
                if result.returncode:
                    raise ValueError(f"exit {result.returncode}: {result.stderr.strip()}")
                record = json.loads(result.stdout)
                if args.tcp:
                    validate_tcp(record, "pace", trial, args)
                else:
                    validate_component(record, args.component, trial, args)
                values[variant] = record[metric]
                report["runs"].append({**record, "variant": variant})
                output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
            except (OSError, ValueError, subprocess.TimeoutExpired) as error:
                report["failure"] = {"pair": trial, "variant": variant, "error": str(error)}
                break
        if report["failure"]:
            break
        report["pairs"].append({"pair": trial, **values, "after_before_ratio": values["after"] / values["before"]})
    report["status"] = "failed" if report["failure"] else "completed"
    report["finished_utc"] = datetime.now(timezone.utc).isoformat()
    if not report["failure"]:
        report["summary"] = paired_summary(report["pairs"], metric, not args.tcp, args.bootstrap_samples, args.seed)
    output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    if report["failure"]:
        print("Paired comparison failed: " + json.dumps(report["failure"]), file=sys.stderr)
        return 1
    summary = report["summary"]
    interval = summary["median_ratio_bootstrap_95_percentile_interval"]
    print(f"{metric}: before median {summary['before_median']:.3f}, after median {summary['after_median']:.3f}")
    print(f"Median paired after/before ratio: {summary['median_paired_after_before_ratio']:.5f}; "
          f"exploratory bootstrap 95% interval [{interval[0]:.5f}, {interval[1]:.5f}]")
    print(f"After wins {summary['after_wins']}/{args.pairs} pairs. No hard performance gate.")
    print(f"Raw evidence: {output}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
