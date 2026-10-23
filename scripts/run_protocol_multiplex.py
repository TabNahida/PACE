#!/usr/bin/env python3
"""Local Linux handler-HOL experiment; correctness CI runs the binary directly."""
import argparse
import csv
import hashlib
import io
import json
import math
import pathlib
import platform
import statistics
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("binary", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--batches", type=int, default=10)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("performance measurements are restricted to local Linux")
    if not 1 <= args.batches <= 1000:
        parser.error("batches must be in 1..1000")
    root = pathlib.Path(__file__).resolve().parent.parent
    binary = args.binary.resolve()
    # Preserve the provenance of previous measurements, including partial runs.
    args.output.mkdir(parents=True, exist_ok=False)
    commands, samples = [], []
    # Alternate the serial/concurrent order between protocols. Each invocation
    # performs ten authenticated or cleartext batches on fresh connections.
    for protocol, caps in (("h2", (1, 8)), ("h2tls", (8, 1)), ("h3", (1, 8))):
        for cap in caps:
            command = [str(binary), "--protocol", protocol, "--handlers", str(cap),
                       "--streams", "16", "--delay-ms", "10", "--body-bytes", "1024",
                       "--batches", str(args.batches), "--certs", str(root / "tests/certs")]
            commands.append(command)
            result = subprocess.run(command, cwd=root, text=True, capture_output=True, timeout=120, check=True)
            (args.output / f"{protocol}-cap{cap}.csv").write_text(result.stdout)
            rows = list(csv.DictReader(io.StringIO(result.stdout)))
            if len(rows) != args.batches or any(
                    row["validated"] != "true" or row["protocol"] != protocol or
                    int(row["handlers"]) != cap or int(row["streams"]) != 16 or
                    int(row["handler_delay_ms"]) != 10 or int(row["body_bytes"]) != 1024 or
                    int(row["trial"]) != trial or int(row["peak_handlers"]) != cap or
                    not math.isfinite(float(row["batch_ms"])) or float(row["batch_ms"]) <= 0
                    for trial, row in enumerate(rows)):
                raise RuntimeError("probe did not validate every sample")
            samples.extend(rows)
    with (args.output / "samples.csv").open("w", newline="") as output:
        writer = csv.DictWriter(output, fieldnames=list(samples[0]))
        writer.writeheader()
        writer.writerows(samples)
    summary = []
    for protocol in ("h2", "h2tls", "h3"):
        medians = {}
        for cap in (1, 8):
            values = [float(row["batch_ms"]) for row in samples if row["protocol"] == protocol and int(row["handlers"]) == cap]
            medians[cap] = statistics.median(values)
            summary.append({"protocol": protocol, "handlers": cap, "samples": len(values),
                            "median_batch_ms": medians[cap], "min_batch_ms": min(values), "max_batch_ms": max(values)})
        print(f"{protocol}: cap1={medians[1]:.3f} ms cap8={medians[8]:.3f} ms ratio={medians[1] / medians[8]:.2f}x")
    def digest(path):
        return hashlib.sha256(path.read_bytes()).hexdigest()
    sources = [*root.glob("include/pace/**/*.hpp"), *root.glob("src/**/*.cpp"),
               root / "benchmarks/protocol_multiplex.cpp", pathlib.Path(__file__).resolve(),
               *root.glob("xmake/*.lua"), root / "xmake.lua"]
    metadata = {"utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                "platform": platform.platform(), "machine": platform.machine(),
                "cpuinfo": pathlib.Path("/proc/cpuinfo").read_text().split("\n\n")[0],
                "binary": str(binary), "binary_sha256": digest(binary),
                "git_head": subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip(),
                "git_status": subprocess.check_output(["git", "status", "--short"], cwd=root, text=True),
                "compiler": subprocess.check_output(["c++", "--version"], text=True).splitlines()[0],
                "commands": commands, "source_sha256": {str(path.relative_to(root)): digest(path) for path in sorted(set(sources))},
                "method": "One reactor/thread; TCP_NODELAY both TCP/TLS peers; handshake excluded; 16 requests/batch; each handler awaits a 10ms timer; fresh connection/batch; cap1 vs cap8 on the same binary; no external-library ranking."}
    (args.output / "metadata.json").write_text(json.dumps(metadata, indent=2) + "\n")
    (args.output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")


if __name__ == "__main__":
    main()
