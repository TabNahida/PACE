#!/usr/bin/env python3
"""Capture local Linux HTTP close-completion samples; never a throughput ranking.

The probe measures HTTP send(), excluding connect/TLS handshake. Every response
is validated. Record both request latency and delay after the server finishes
writing a complete response. A timer deliberately delays the server's close.
"""
import argparse
import csv
from datetime import datetime, timezone
import hashlib
import io
import json
import math
from pathlib import Path
import platform
import statistics
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]


def command(args):
    return subprocess.run(args, cwd=ROOT, check=True, text=True, capture_output=True).stdout.strip()


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--trials", type=int, default=10)
    parser.add_argument("--delays-ms", default="0,1,10,50,200")
    parser.add_argument("--transports", nargs="+", choices=("tcp", "tls"), default=["tcp", "tls"])
    parser.add_argument("--framings", nargs="+", choices=("length", "chunked", "head", "204", "eof"), default=["length"])
    parser.add_argument("--close-policy", choices=("default", "graceful"), default="default")
    parser.add_argument("--close-timeout-ms", type=int, default=100)
    parser.add_argument("--connection", choices=("close", "keepalive", "both"), default="both")
    parser.add_argument("--timeout", type=float, default=180)
    args = parser.parse_args()
    if platform.system() != "Linux":
        parser.error("published close-completion measurements must use local Linux")
    delays = [int(value) for value in args.delays_ms.split(",")]
    if not 1 <= args.trials <= 1000 or any(not 0 <= delay <= 10000 for delay in delays) or len(set(delays)) != len(delays):
        parser.error("invalid trial count or delays")
    if not 0 <= args.close_timeout_ms <= 10000 or args.timeout <= 0:
        parser.error("invalid timeout")
    binary = args.executable.resolve()
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    source_paths = [*ROOT.glob("include/pace/*.hpp"), *ROOT.glob("include/pace/detail/*.hpp"),
                    *ROOT.glob("src/runtime/*.cpp"), *ROOT.glob("src/networking/*.cpp"),
                    ROOT / "src/application/http.cpp", ROOT / "src/extensions/tls.cpp",
                    ROOT / "benchmarks/http_close.cpp", Path(__file__).resolve()]
    environment = {
        "schema_version": 1, "status": "running", "label": args.label,
        "started_utc": datetime.now(timezone.utc).isoformat(),
        "platform": platform.platform(), "machine": platform.machine(),
        "cpu_model": next((line.split(":", 1)[1].strip() for line in Path("/proc/cpuinfo").read_text().splitlines()
                           if line.startswith("model name")), "unknown"),
        "python": platform.python_version(), "revision": command(["git", "rev-parse", "HEAD"]),
        "working_tree": command(["git", "status", "--short"]),
        "executable": str(binary), "executable_sha256": sha(binary),
        "source_sha256": {str(path.relative_to(ROOT)): sha(path) for path in sorted(set(source_paths))},
        "configuration": {key: str(value) if isinstance(value, Path) else value for key, value in vars(args).items()},
        "scope": "single HTTP/1 exchange completion on one local reactor, one client at a time",
        "timing": "send() only; TCP connect and TLS handshake excluded; server write completion is a local enqueue timestamp",
        "limitations": ["Artificial server close delays isolate teardown waiting, not overall HTTP throughput",
                        "Loopback includes client, server, kernel and scheduler; no CPU affinity/frequency control",
                        "Fixed response payload is two bytes or an intentionally bodyless response",
                        "Each trial constructs a fresh context/connection; no warmed persistent throughput claim",
                        "No Windows issue measurements are included; all recorded data is produced here on Linux"],
        "commands": [],
    }
    env_file = output / "environment.json"
    env_file.write_text(json.dumps(environment, indent=2) + "\n")
    rows = []
    try:
        for transport in args.transports:
            for framing in args.framings:
                if framing == "eof" and args.connection == "keepalive":
                    raise ValueError("EOF framing cannot be a reusable connection")
                invocation = [str(binary), "--transport", transport, "--framing", framing,
                              "--connection", args.connection, "--trials", str(args.trials),
                              "--delays-ms", args.delays_ms, "--close-policy", args.close_policy,
                              "--close-timeout-ms", str(args.close_timeout_ms), "--certs", str(ROOT / "tests/certs")]
                environment["commands"].append(invocation)
                result = subprocess.run(invocation, check=True, text=True, capture_output=True, timeout=args.timeout)
                (output / f"{transport}-{framing}.csv").write_text(result.stdout)
                if result.stderr:
                    (output / f"{transport}-{framing}.stderr").write_text(result.stderr)
                records = list(csv.DictReader(io.StringIO(result.stdout)))
                persistent = [0] if framing == "eof" or args.connection == "close" else [1] if args.connection == "keepalive" else [0, 1]
                expected = {(keep, delay, trial) for keep in persistent for delay in delays for trial in range(1, args.trials + 1)}
                observed = set()
                for record in records:
                    for key in ("close_timeout_ms", "keep_alive", "peer_close_delay_ms", "trial", "completed_before_peer_close", "validated"):
                        record[key] = int(record[key])
                    for key in ("request_ms", "after_response_written_ms", "actual_peer_delay_ms"):
                        record[key] = float(record[key])
                        if not math.isfinite(record[key]) or record[key] < 0:
                            raise ValueError("invalid timing sample")
                    identity = (record["keep_alive"], record["peer_close_delay_ms"], record["trial"])
                    if identity not in expected or identity in observed or record["validated"] != 1:
                        raise ValueError("missing/duplicate/unvalidated trial")
                    if (record["transport"] != transport or record["framing"] != framing or
                            record["close_policy"] != args.close_policy or record["close_timeout_ms"] != args.close_timeout_ms or
                            record["completed_before_peer_close"] not in (0, 1)):
                        raise ValueError("probe configuration mismatch")
                    observed.add(identity)
                if observed != expected:
                    raise ValueError("incomplete probe matrix")
                rows.extend(records)
        groups = {}
        for row in rows:
            key = (row["transport"], row["framing"], row["keep_alive"], row["peer_close_delay_ms"])
            groups.setdefault(key, []).append(row)
        summary = []
        for (transport, framing, keep_alive, delay), samples in sorted(groups.items()):
            item = {"transport": transport, "framing": framing, "keep_alive": bool(keep_alive),
                    "peer_close_delay_ms": delay, "trials": len(samples),
                    "completed_before_peer_close": sum(row["completed_before_peer_close"] for row in samples)}
            for metric in ("request_ms", "after_response_written_ms", "actual_peer_delay_ms"):
                values = [row[metric] for row in samples]
                item.update({f"median_{metric}": statistics.median(values), f"min_{metric}": min(values), f"max_{metric}": max(values)})
            summary.append(item)
        (output / "samples.json").write_text(json.dumps(rows, indent=2) + "\n")
        (output / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
        environment["status"] = "completed"
        environment["samples"] = len(rows)
        print(f"{output}: {len(rows)} validated samples")
    except Exception as error:
        environment["status"] = "failed"
        environment["failure"] = str(error)
        raise
    finally:
        environment["finished_utc"] = datetime.now(timezone.utc).isoformat()
        env_file.write_text(json.dumps(environment, indent=2) + "\n")


if __name__ == "__main__":
    main()
