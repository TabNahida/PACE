#!/usr/bin/env python3
"""Compare identical, validated WebSocket loopback workloads on local Linux.

Prepare binaries first, then run --measure-only while other builds are paused.
Both peers share one reactor/thread. This compares PACE revisions, not libraries.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import platform
import statistics
import subprocess

ROOT = Path(__file__).resolve().parents[1]
CASES = [(64, 0, 300000), (16384, 0, 30000), (65536, 1024, 3000)]


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def command(args):
    return subprocess.check_output(args, text=True, cwd=ROOT, timeout=180).strip()


def write(path, value):
    path.write_text(json.dumps(value, indent=2) + "\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline-root", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--trials", type=int, default=10)
    parser.add_argument("--measure-only", action="store_true")
    args = parser.parse_args()
    if platform.system() != "Linux" or not 1 <= args.trials <= 100:
        parser.error("requires local Linux and 1..100 trials")
    build, output = args.build_dir.resolve(), args.output.resolve()
    build.mkdir(parents=True, exist_ok=True)
    output.mkdir(parents=True, exist_ok=True)
    if not args.measure_only:
        if (output / "trials.json").exists() or (output / "environment.json").exists():
            raise ValueError("refusing to replace the build manifest for existing measurements")
        manifest = {"compiler": command(["g++", "--version"]), "builds": {}}
        for label, root in (("before", args.baseline_root.resolve()), ("after", ROOT)):
            sources = [ROOT / "benchmarks/websocket_bench.cpp", *sorted(root.glob("src/runtime/*.cpp")),
                       *sorted(root.glob("src/networking/*.cpp")), root / "src/application/websocket.cpp"]
            headers = [root / "include/pace" / name for name in (
                "websocket.hpp", "tcp.hpp", "udp.hpp", "ip.hpp", "platform.hpp", "io_context.hpp",
                "task.hpp", "runtime.hpp", "detail/poller.hpp", "detail/profiling.hpp", "detail/socket_address.hpp")]
            for diagnostic in (False, True):
                name = label + ("-diagnostic" if diagnostic else "")
                binary = build / name
                invocation = ["g++", "-O3", "-DNDEBUG", "-std=c++20", "-pthread", "-I" + str(root / "include")]
                if diagnostic:
                    invocation += ["-DPACE_WEBSOCKET_DIAGNOSTICS", "-Wl,--wrap=recv,--wrap=send,--wrap=getrandom"]
                invocation += [str(path) for path in sources] + ["-o", str(binary)]
                subprocess.run(invocation, check=True)
                manifest["builds"][name] = {"command": invocation, "binary_sha256": sha(binary),
                    "source_sha256": {str(path): sha(path) for path in sources + headers}}
        write(output / "build.json", manifest)
        print("Binaries prepared. Pause other builds, then repeat with --measure-only.")
        return
    manifest = json.loads((output / "build.json").read_text())
    for name, item in manifest["builds"].items():
        if sha(build / name) != item["binary_sha256"]:
            raise ValueError("binary differs from build manifest: " + name)
    if (output / "trials.json").exists():
        raise ValueError("output already contains measurements")
    environment = {"started_utc": datetime.now(timezone.utc).isoformat(), "status": "running",
        "platform": platform.platform(), "machine": platform.machine(),
        "cpu": next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                     if line.startswith("model name")), "unknown"),
        "revision": command(["git", "rev-parse", "HEAD"]), "working_tree": command(["git", "status", "--short"]),
        "scope": "one connection, client and server share one reactor/thread, TCP_NODELAY, binary echo",
        "limitations": ["Linux loopback; no CPU affinity or frequency control", "No TLS, external server or other-library comparison",
                        "Throughput includes client payload generation and validation; per-operation latency ends before validation",
                        "Handshake and 1000 warmup exchanges excluded; every measured payload includes an incrementing sequence",
                        "Diagnostic syscall counts are separate runs; diagnostic timings are not performance evidence"],
        "trials_per_case": args.trials, "commands": []}
    write(output / "environment.json", environment)
    rows, diagnostics = [], []
    try:
        for trial in range(args.trials):
            for size, fragment, count in CASES:
                for label in (("before", "after") if trial % 2 == 0 else ("after", "before")):
                    invocation = [str(build / label), "--bytes", str(size), "--fragment-bytes", str(fragment),
                                  "--iterations", str(count)]
                    environment["commands"].append(invocation)
                    value = json.loads(command(invocation))
                    if (value["bytes"], value["fragment_bytes"], value["iterations"]) != (size, fragment, count):
                        raise ValueError("workload mismatch")
                    if not value["validated"] or value["instrumented"] or any(
                        not math.isfinite(value[key]) or value[key] <= 0 for key in ("seconds", "roundtrips_per_second", "p50_us", "p95_us", "p99_us")):
                        raise ValueError("invalid trial")
                    rows.append({"label": label, "trial": trial + 1, **value})
                    write(output / "trials.json", rows)
        for size, fragment, _ in CASES:
            for label in ("before", "after"):
                invocation = [str(build / (label + "-diagnostic")), "--bytes", str(size),
                              "--fragment-bytes", str(fragment), "--iterations", "1000"]
                environment["commands"].append(invocation)
                value = json.loads(command(invocation))
                if not value["validated"] or not value["instrumented"]:
                    raise ValueError("invalid diagnostic")
                diagnostics.append({"label": label, **{key: value[key] for key in (
                    "bytes", "fragment_bytes", "iterations", "recv_calls", "send_calls", "getrandom_calls", "validated")}})
        write(output / "diagnostics.json", diagnostics)
        summary = []
        for size, fragment, _ in CASES:
            for label in ("before", "after"):
                samples = [row for row in rows if row["label"] == label and row["bytes"] == size and row["fragment_bytes"] == fragment]
                item = {"label": label, "bytes": size, "fragment_bytes": fragment, "trials": len(samples)}
                for metric in ("roundtrips_per_second", "p50_us", "p95_us", "p99_us"):
                    values = [row[metric] for row in samples]
                    item[metric] = {"median": statistics.median(values), "min": min(values), "max": max(values)}
                summary.append(item)
        write(output / "summary.json", summary)
        environment["status"] = "completed"
        print(f"{len(rows)} validated trials, {len(diagnostics)} diagnostic runs")
    finally:
        environment["finished_utc"] = datetime.now(timezone.utc).isoformat()
        write(output / "environment.json", environment)


if __name__ == "__main__":
    main()
