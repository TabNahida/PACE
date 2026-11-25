#!/usr/bin/env python3
"""Compare two PACE builds on the same runner using alternating processes."""

import argparse
import json
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess


def executable(directory, name):
    filename = f"{name}.exe" if os.name == "nt" else name
    matches = sorted(path for path in directory.rglob(filename) if path.is_file())
    if len(matches) != 1:
        raise ValueError(f"expected one {filename} under {directory}, found {matches}")
    return matches[0].resolve()


def build(source, backend, tests):
    output = source / "build" / "runtime-performance"
    project = ("-P", str(source))
    flags = [
        "-m", "release", "-o", str(output), "--http1=n",
        "--websocket=n", "--tls=n", "--dns=n", "--quic=n", "--http2=n",
        "--http3=n", "--components=y", "--scaling=y", "--profiling=n",
        "--sanitizers=n", "--examples=n", f"--tests={'y' if tests else 'n'}",
        f"--uring={'y' if backend == 'io_uring' else 'n'}", "-y",
    ]
    targets = ("pace_runtime_hotpaths", "pace_components", "pace_scaling")
    checks = ("pace_runtime_tests", "pace_backend_tests", "pace_structured_tests")
    subprocess.run(["xmake", "f", "-c", *project, *flags], cwd=source, check=True)
    for name in (*targets, *(checks if tests else ())):
        subprocess.run(["xmake", *project, "-y", name], cwd=source, check=True)
    if tests:
        for name in checks:
            subprocess.run([str(executable(output, name))], cwd=source, check=True, timeout=90)
    return output


def measure(binary, arguments, metrics):
    result = subprocess.run([str(binary), *arguments], check=True, text=True,
                            stdout=subprocess.PIPE, timeout=120)
    record = json.loads(result.stdout)
    if record.get("validated") is not True or any(record.get(key, 0) <= 0 for key in metrics):
        raise ValueError(f"invalid benchmark result: {record}")
    return record


def run(baseline, candidate, repeats, backend):
    hotpaths = "pace_runtime_hotpaths"
    hotpath_metrics = ("p50_ns", "p99_ns")
    component = "pace_components"
    scaling = "pace_scaling"
    workloads = [
        ("cancel sparse / 0 background", hotpaths, ("sparse_cancel", "0", "1000", "200"), hotpath_metrics),
        ("cancel sparse / 65536 background", hotpaths, ("sparse_cancel", "65536", "1000", "200"), hotpath_metrics),
        ("readiness / 1 socket", hotpaths, ("ready_batch", "1", "2000", "300"), hotpath_metrics),
        ("readiness / 256 sockets", hotpaths, ("ready_batch", "256", "200", "50"), hotpath_metrics),
        ("inbox / 65536 roots", hotpaths, ("inbox_burst", "65536", "20", "5"), hotpath_metrics),
    ]
    for name in ("yield", "zero_timer", "cross_thread_post"):
        workloads.append((name, component, ("--component", name, "--iterations", "400000", "--warmup", "30000"),
                          ("ns_per_operation",)))
    for clients, workers, pipeline in ((8, 4, 1), (8, 8, 1), (32, 4, 1), (32, 8, 1), (8, 4, 8)):
        label = f"TCP / {clients} connections / {workers} workers / depth {pipeline}"
        args = ("--mode", "runtime", "--clients", str(clients), "--workers", str(workers),
                "--pipeline", str(pipeline), "--requests", "12000", "--warmup", "1000", "--timeout-ms", "15000")
        workloads.append((label, scaling, args, ("requests_per_second", "p99_us")))
    directories = {"baseline": baseline, "candidate": candidate}
    report = {"backend": backend, "platform": platform.platform(), "machine": platform.machine(),
              "logical_cpus": os.cpu_count(), "workloads": {}}
    lines = [f"### {backend} paired runtime benchmarks", "",
             "| Workload | Metric | Baseline median | Candidate median | Median paired change |",
             "|---|---|---:|---:|---:|"]
    for label, name, args, metrics in workloads:
        print(f"Comparing {label}", flush=True)
        binaries = {side: executable(directory, name) for side, directory in directories.items()}
        records = dict(baseline=[], candidate=[])
        for trial in range(repeats):
            order = ("baseline", "candidate") if trial % 2 == 0 else ("candidate", "baseline")
            for side in order:
                records[side].append(measure(binaries[side], args, metrics))
        report["workloads"][label] = {"target": name, "arguments": args, **records}
        for metric in metrics:
            before, after = (statistics.median(entry[metric] for entry in records[side]) for side in directories)
            change = statistics.median(new[metric] / old[metric] - 1
                                       for old, new in zip(records["baseline"], records["candidate"]))
            lines.append(f"| {label} | {metric} | {before:,.2f} | {after:,.2f} | {change:+.1%} |")
    return report, "\n".join(lines) + "\n"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", required=True, type=Path)
    parser.add_argument("--candidate", default=Path("."), type=Path)
    parser.add_argument("--backend", required=True, choices=("epoll", "io_uring", "kqueue", "iocp", "wsapoll"))
    parser.add_argument("--repeats", default=5, type=int)
    parser.add_argument("--output", default=Path("runtime-performance"), type=Path)
    parser.add_argument("--build", action="store_true", help="compare source trees by building both with xmake")
    args = parser.parse_args()
    if args.repeats < 1:
        parser.error("--repeats must be positive")
    os.environ["PACE_IO_BACKEND"] = args.backend
    baseline, candidate = args.baseline.resolve(), args.candidate.resolve()
    revisions = None
    if args.build:
        if baseline == candidate:
            parser.error("baseline and candidate must be separate checkouts")
        revisions = {side: subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=source, text=True).strip()
                     for side, source in (("baseline", baseline), ("candidate", candidate))}
        for path in (Path("xmake/benchmarks.lua"), Path("benchmarks/runtime_hotpaths.cpp")):
            shutil.copyfile(candidate / path, baseline / path)
        baseline = build(baseline, args.backend, tests=False)
        candidate = build(candidate, args.backend, tests=True)
    report, markdown = run(baseline, candidate, args.repeats, args.backend)
    report["revisions"] = revisions
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / "trials.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    (args.output / "summary.md").write_text(markdown, encoding="utf-8")
    print(markdown)
    if summary := os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(summary, "a", encoding="utf-8") as file:
            file.write(markdown)


if __name__ == "__main__":
    main()
