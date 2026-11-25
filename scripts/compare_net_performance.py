#!/usr/bin/env python3
"""Compare the same network workloads on two revisions on a single host."""

import argparse
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import tempfile
import time

from ci_smoke import executable


ROOT = Path(__file__).resolve().parents[1]
BUILD = Path("build/net-performance")
TESTS = ("pace_backend_tests", "pace_network_tests", "pace_udp_tests")
BENCHMARKS = ("pace_net_hotpaths", "pace_scaling")
HOTPATHS = {"poll-dense": 400, "poll-sparse": 2500, "udp": 800}
TCP = {f"tcp-{workers}w-{clients}c-depth{depth}": (workers, clients, depth)
       for workers, clients, depth in ((1, 8, 1), (4, 8, 1), (1, 8, 8), (1, 32, 1))}


def build(root, args, candidate):
    subprocess.run([
        "xmake", "f", "-c", "-m", "release", "-o", str(BUILD), "-y",
        "--http1=n", "--websocket=n", "--examples=n", "--scaling=y",
        "--uring=" + ("y" if args.backend == "io_uring" else "n"),
        "--tests=" + ("y" if candidate else "n"),
        *(["-a", args.arch] if args.arch else []),
    ], cwd=root, check=True)
    for target in BENCHMARKS + (TESTS if candidate else ()):
        subprocess.run(["xmake", "build", "-y", target], cwd=root, check=True)
    return root / BUILD


def measure(directory, backend, trial, workload):
    if workload in HOTPATHS:
        count = HOTPATHS[workload]
        output = subprocess.check_output([
            str(executable(directory, "pace_net_hotpaths")), workload, str(count)
        ], text=True, timeout=40)
        result = json.loads(output)
        if (result["backend"], result["workload"], result["iterations"]) != (backend, workload, count):
            raise ValueError(f"unexpected hot path result: {result}")
        return {"ns_per_operation": result["ns_per_operation"]}

    workers, clients, depth = TCP[workload]
    requests = 4000
    command = [str(executable(directory, "pace_scaling")), "--mode", "runtime", "--workers", str(workers),
               "--clients", str(clients), "--pipeline", str(depth), "--bytes", "64", "--requests", str(requests),
               "--warmup", "256", "--timeout-ms", "10000", "--trial", str(trial)]
    for delay in (2, 5, 0):
        run = subprocess.run(command, text=True, capture_output=True, timeout=60)
        if not run.returncode:
            break
        if not delay or os.name != "nt" or "client connect: 10055" not in run.stderr:
            raise RuntimeError(f"{workload} trial {trial} failed: {run.stderr.strip()}")
        print(f"{workload} trial {trial}: Winsock resource pressure, retrying in {delay}s", flush=True)
        time.sleep(delay)
    result = json.loads(run.stdout)
    checksum = clients * (clients + 1) // 2 * (requests * (requests + 1) // 2)
    if (result["validated"] is not True or result["completed_requests"] != clients * requests or
            result["checksum"] != checksum or result["pipeline"] != depth or
            result["per_worker_completed_requests"] != [clients * requests // workers] * workers):
        raise ValueError(f"unexpected TCP workload result: {result}")
    return {metric: result[metric] for metric in ("requests_per_second", "p99_us")}


def compare(args, builds, revisions):
    records = {}
    # Complete TCP before the UDP case, which opens a fresh socket per exchange.
    for workload in ("poll-dense", "poll-sparse", *TCP, "udp"):
        for trial in range(1, args.trials + 1):
            for revision in tuple(builds)[::1 if trial % 2 else -1]:
                metrics = measure(builds[revision], args.backend, trial, workload)
                for metric, value in metrics.items():
                    if not math.isfinite(value) or value <= 0:
                        raise ValueError(f"invalid measurement for {workload}/{metric}: {value}")
                    records.setdefault((workload, metric), {"baseline": [], "candidate": []})[revision].append(value)

    header = ["| Workload | Metric | Baseline | Candidate | Change |",
              "|---|---|---:|---:|---:|"]
    rows = []
    for (workload, metric), trials in records.items():
        baseline, candidate = map(statistics.median, (trials["baseline"], trials["candidate"]))
        change = 100 * (statistics.median(new / old for old, new in zip(trials["baseline"], trials["candidate"])) - 1)
        rows.append({"workload": workload, "metric": metric, "baseline": baseline, "candidate": candidate,
                     "paired_change_percent": change, "trials": trials})
        header.append(f"| {workload} | {metric} | {baseline:,.2f} | {candidate:,.2f} | {change:+.1f}% |")
    args.output.mkdir(parents=True, exist_ok=True)
    report = "\n".join([f"### Network performance · {args.backend}", "",
                        "Alternating pairs on one runner. Change is the median paired ratio.", "", *header, ""])
    (args.output / "summary.md").write_text(report, encoding="utf-8")
    (args.output / "results.json").write_text(json.dumps({"backend": args.backend, "platform": platform.platform(),
                                                          "revisions": revisions, "results": rows}, indent=2) + "\n",
                                               encoding="utf-8")
    if output := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(output, "a", encoding="utf-8") as summary:
            summary.write(report)
    print(report)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--backend", required=True)
    parser.add_argument("--arch")
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--base-ref", default="origin/main")
    parser.add_argument("--trials", default=5, type=int, choices=range(1, 11))
    parser.add_argument("--build-dirs", nargs=2, type=Path, metavar=("BASELINE", "CANDIDATE"))
    args = parser.parse_args()
    os.environ["PACE_IO_BACKEND"] = args.backend

    def git(*command):
        return subprocess.check_output(["git", *command], cwd=ROOT, text=True).strip()

    revisions = {"baseline": git("merge-base", "HEAD", args.base_ref), "candidate": git("rev-parse", "HEAD")}
    if args.build_dirs:
        compare(args, dict(zip(revisions, args.build_dirs)), None)
        return
    with tempfile.TemporaryDirectory(prefix="pace-net-baseline-") as scratch:
        baseline = Path(scratch) / "checkout"
        git("worktree", "add", "--detach", str(baseline), revisions["baseline"])
        try:
            # Use identical harnesses, headers and libraries from each respective revision.
            for source in ("benchmarks/net_hotpaths.cpp", "benchmarks/scaling_bench.cpp", "xmake/benchmarks.lua"):
                shutil.copyfile(ROOT / source, baseline / source)
            builds = {"baseline": build(baseline, args, False), "candidate": build(ROOT, args, True)}
            for target in TESTS:
                subprocess.run([str(executable(builds["candidate"], target))], check=True, timeout=60)
            compare(args, builds, revisions)
        finally:
            git("worktree", "remove", "--force", str(baseline))


if __name__ == "__main__":
    main()
