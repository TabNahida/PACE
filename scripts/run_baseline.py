#!/usr/bin/env python3
"""Measure every PACE benchmark suite once, as a regression anchor for later versions.

    python3 scripts/run_baseline.py --source ../pace-0.6.1 --build-dir ../pace-0.6.1/build/baseline \\
        --output benchmarks/results/baseline-0.6.1-macos-arm64 --label "Apple M-series laptop, idle"

`--source` is the tree the binaries were built from. Runner scripts are taken from that tree, so their
provenance (revision, source hashes) describes the measured code. Build it first with:

    xmake f -c -m release -o build/baseline --tls=y --dns=y --quic=y --http2=y --http3=y \\
        --benchmarks=y --components=y --scaling=y --protocol_benchmarks=y --tests=n --examples=n -y
    xmake -y

Suites whose binary was not built, or that need something this host lacks (for example nginx or Linux),
are recorded as skipped with the reason. A suite that fails stops nothing else but makes the run fail.
Run on an otherwise idle machine; nothing here pins CPUs or frequencies.
"""

import argparse
import csv
import hashlib
import io
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

LINUX = sys.platform.startswith("linux")
EXE = ".exe" if os.name == "nt" else ""


class Skip(Exception):
    pass


class Baseline:
    def __init__(self, args):
        self.source = args.source.resolve(strict=True)
        self.build = args.build_dir.resolve(strict=True)
        self.output = args.output.resolve()
        self.repeats = args.repeats
        self.label = args.label
        self.quick = args.quick
        self.raw = self.output / "raw"
        self.summary_rows = []
        self.suites = {}

    # -- helpers -----------------------------------------------------------------------------------------------

    def binary(self, name):
        matches = sorted(self.build.glob(f"**/{name}{EXE}"), key=lambda path: path.stat().st_mtime, reverse=True)
        if not matches:
            raise Skip(f"{name} was not built in {self.build}")
        return matches[0]

    def runner(self, name):
        script = self.source / "scripts" / name
        if not script.is_file():
            raise Skip(f"{script} does not exist in the measured source tree")
        return script

    def run(self, command, timeout=300):
        result = subprocess.run([str(part) for part in command], cwd=self.source, text=True, capture_output=True,
                                timeout=timeout)
        if result.returncode:
            raise RuntimeError(f"{' '.join(map(str, command))} exited {result.returncode}:\n{result.stderr.strip()}")
        return result.stdout

    def trials(self, suite, binary, workloads, metrics, parse=json.loads):
        """Run each workload `repeats` times, rotating workload order, and keep every raw record."""
        records = {name: [] for name, _ in workloads}
        path = self.raw / f"{suite}.jsonl"
        with path.open("w") as raw:
            for trial in range(self.repeats):
                order = workloads if trial % 2 == 0 else list(reversed(workloads))
                for name, arguments in order:
                    output = self.run([binary, *arguments])
                    for record in parse(output):
                        if not record.get("validated", True) in (True, "true", "1", 1):
                            raise ValueError(f"{suite}/{name} trial {trial} did not validate: {record}")
                        records[name].append(record)
                        raw.write(json.dumps({"workload": name, "trial": trial, "arguments": arguments,
                                              "record": record}) + "\n")
        for name, _ in workloads:
            for metric in metrics:
                values = [float(record[metric]) for record in records[name] if metric in record]
                if values:
                    self.summary_rows.append((suite, name, metric, statistics.median(values), min(values),
                                              max(values), len(values)))
        return {"binary": str(binary), "binary_sha256": sha256(binary), "raw": str(path.relative_to(self.output)),
                "workloads": dict(workloads)}

    # -- suites ------------------------------------------------------------------------------------------------

    def tcp_echo(self):
        binary = self.binary("pace_bench")
        destination = self.output / "tcp-echo"
        self.run([sys.executable, self.runner("run_benchmarks.py"), "--executable", binary, "--require-all",
                  "--repeats", self.repeats, "--clients", 8, "--requests", 1000 if self.quick else 5000,
                  "--warmup", 500, "--bytes", 64, "--label", self.label, "--output", destination], timeout=1800)
        return {"binary": str(binary), "binary_sha256": sha256(binary), "results": "tcp-echo/"}

    def components(self):
        binary = self.binary("pace_components")
        destination = self.output / "components.json"
        self.run([sys.executable, self.runner("run_components.py"), "--executable", binary, "--repeats",
                  self.repeats, "--label", self.label, "--output", destination], timeout=1800)
        return {"binary": str(binary), "binary_sha256": sha256(binary), "results": "components.json"}

    def scaling(self):
        binary = self.binary("pace_scaling")
        destination = self.output / "scaling.json"
        self.run([sys.executable, self.runner("run_scaling.py"), "--executable", binary, "--repeats", self.repeats,
                  "--label", self.label, "--output", destination], timeout=3600)
        return {"binary": str(binary), "binary_sha256": sha256(binary), "results": "scaling.json"}

    def runtime_hotpaths(self):
        workloads = [
            ("cancel sparse / 0 background", ("sparse_cancel", "0", "1000", "200")),
            ("cancel sparse / 65536 background", ("sparse_cancel", "65536", "1000", "200")),
            ("readiness / 1 socket", ("ready_batch", "1", "2000", "300")),
            ("readiness / 256 sockets", ("ready_batch", "256", "200", "50")),
            ("inbox / 65536 roots", ("inbox_burst", "65536", "20", "5")),
        ]
        return self.trials("runtime-hotpaths", self.binary("pace_runtime_hotpaths"), workloads,
                           ("p50_ns", "p99_ns"), parse=lambda text: [json.loads(text)])

    def net_hotpaths(self):
        workloads = [(name, (name, str(count))) for name, count in
                     (("poll-dense", 400), ("poll-sparse", 2500), ("udp", 800))]
        return self.trials("net-hotpaths", self.binary("pace_net_hotpaths"), workloads, ("ns_per_operation",),
                           parse=lambda text: [json.loads(text)])

    def router(self):
        workloads = [(f"{routes} routes", (str(routes), "200000")) for routes in (16, 1024)]
        return self.trials("router", self.binary("pace_router_bench"), workloads, ("ns_per_operation",),
                           parse=lambda text: [json.loads(text)])

    def inbox_allocations(self):
        binary = self.binary("pace_inbox_allocations")
        record = json.loads(self.run([binary]))
        (self.raw / "inbox-allocations.json").write_text(json.dumps(record, indent=2) + "\n")
        self.summary_rows.append(("inbox-allocations", f"{record['rounds']} rounds", "reactor_allocations",
                                  record["reactor_allocations"], record["reactor_allocations"],
                                  record["reactor_allocations"], 1))
        return {"binary": str(binary), "binary_sha256": sha256(binary), "raw": "raw/inbox-allocations.json"}

    def websocket(self):
        iterations = "5000" if self.quick else "30000"
        workloads = [
            ("64 B", ("--bytes", "64", "--iterations", iterations)),
            ("16 KiB", ("--bytes", "16384", "--iterations", iterations)),
            ("16 KiB / 1 KiB fragments", ("--bytes", "16384", "--fragment-bytes", "1024", "--iterations", iterations)),
        ]
        return self.trials("websocket", self.binary("pace_websocket_bench"), workloads,
                           ("roundtrips_per_second", "p50_us", "p99_us"), parse=lambda text: [json.loads(text)])

    def protocol_multiplex(self):
        certs = self.source / "tests" / "certs"
        batches = "3" if self.quick else "10"
        workloads = [(f"{protocol} / {cap} handlers",
                      ("--protocol", protocol, "--handlers", str(cap), "--streams", "16", "--delay-ms", "10",
                       "--body-bytes", "1024", "--batches", batches, "--certs", str(certs)))
                     for protocol in ("h2", "h2tls", "h3") for cap in (1, 8)]
        return self.trials("protocol-multiplex", self.binary("pace_protocol_multiplex"), workloads, ("batch_ms",),
                           parse=parse_csv)

    def gateway_components(self):
        iterations = "50000" if self.quick else "200000"
        workloads = [(f"{mode} / {workers} workers", (mode, str(workers), iterations))
                     for mode, workers in [("fnv", 1), ("xxh", 1)] + [(mode, workers) for workers in (1, 2, 4, 8)
                                                                      for mode in ("shared", "local")]]
        return self.trials("gateway-components", self.binary("pace_gateway_components"), workloads,
                           ("operations_per_second",), parse=lambda text: [json.loads(text)])

    def http_close(self):
        certs = self.source / "tests" / "certs"
        trials = "5" if self.quick else "20"
        # One workload per peer-close delay, so medians never mix delays.
        workloads = [(f"{transport} / {framing} / peer close +{delay} ms",
                      ("--transport", transport, "--framing", framing, "--connection", "both", "--trials", trials,
                       "--delays-ms", delay, "--certs", str(certs)))
                     for transport in ("tcp", "tls") for framing in ("length", "chunked") for delay in ("0", "10")]
        return self.trials("http-close", self.binary("pace_http_close_probe"), workloads,
                           ("request_ms", "after_response_written_ms"), parse=parse_csv)

    def quic_transport(self):
        if not LINUX:
            raise Skip("pace_quic_transport is built only on Linux")
        binary = self.binary("pace_quic_transport")
        output = self.run([binary, "--help"], timeout=30)
        raise Skip(f"no fixed baseline workload defined for pace_quic_transport yet: {output.strip()[:200]}")

    def http_gateway(self):
        if not LINUX:
            raise Skip("the nginx gateway comparison uses pace_http_load, which is built only on Linux")
        nginx = shutil.which("nginx")
        if not nginx:
            raise Skip("nginx is not installed")
        destination = self.output / "http-gateway"
        self.run([sys.executable, self.runner("run_gateway_benchmarks.py"), "--pace", self.binary("pace_http_gateway"),
                  "--nginx", nginx, "--load", self.binary("pace_http_load"), "--repeats", self.repeats,
                  "--output", destination], timeout=3600)
        return {"results": "http-gateway/"}

    SUITES = ("runtime_hotpaths", "net_hotpaths", "components", "router", "inbox_allocations", "tcp_echo",
              "scaling", "websocket", "http_close", "protocol_multiplex", "gateway_components", "quic_transport",
              "http_gateway")

    # -- driver ------------------------------------------------------------------------------------------------

    def measure(self, selected):
        self.output.mkdir(parents=True, exist_ok=False)
        self.raw.mkdir()
        failed = False
        for name in selected:
            started = time.monotonic()
            print(f"== {name}", flush=True)
            try:
                result = getattr(self, name)()
                self.suites[name] = {"status": "ok", **result}
            except Skip as reason:
                self.suites[name] = {"status": "skipped", "reason": str(reason)}
                print(f"   skipped: {reason}", flush=True)
            except Exception as error:  # Keep measuring the other suites; the run still fails.
                failed = True
                self.suites[name] = {"status": "failed", "error": str(error)}
                print(f"   FAILED: {error}", file=sys.stderr, flush=True)
            self.suites[name]["seconds"] = round(time.monotonic() - started, 1)
        self.write_manifest()
        self.write_summary()
        redact_paths(self.output, self.source)
        return 1 if failed else 0

    def write_manifest(self):
        manifest = {
            "schema": 1,
            "measured_at": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "label": self.label,
            "repeats": self.repeats,
            "quick": self.quick,
            "source": {
                "revision": git(self.source, "rev-parse", "HEAD"),
                "describe": git(self.source, "describe", "--tags", "--always", "--dirty"),
                "status": git(self.source, "status", "--short"),
            },
            "runner": {"script_sha256": sha256(Path(__file__)), "revision": git(Path(__file__).parent, "rev-parse",
                                                                                  "HEAD")},
            "host": host(),
            "build_config": read_xmake_config(self.source),
            "suites": self.suites,
        }
        (self.output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    def write_summary(self):
        lines = [f"# Baseline: {git(self.source, 'describe', '--tags', '--always', '--dirty')}", "",
                 f"- Host: {self.label} ({platform.platform()}, {platform.machine()}, {os.cpu_count()} logical CPUs)",
                 f"- Repeats per workload: {self.repeats}{' (quick mode, not a baseline)' if self.quick else ''}",
                 "- Values are the median of all trials, with min–max spread; raw records are in `raw/` and the "
                 "runner-owned files next to this summary.", "",
                 "## Suites", "", "| Suite | Status | Seconds | Note |", "|:--|:--|--:|:--|"]
        for name, suite in self.suites.items():
            note = (suite.get("reason") or suite.get("error") or "").splitlines()[:1]
            note = note[0].replace("|", "\\|") if note else ""
            lines.append(f"| {name} | {suite['status']} | {suite['seconds']} | {note} |")
        lines += ["", "## Directly measured workloads", "",
                  "| Suite | Workload | Metric | Median | Min | Max | Samples |", "|:--|:--|:--|--:|--:|--:|--:|"]
        for suite, workload, metric, median, low, high, count in self.summary_rows:
            lines.append(f"| {suite} | {workload} | {metric} | {median:,.3f} | {low:,.3f} | {high:,.3f} | {count} |")
        lines += ["", "TCP echo, components and scaling keep their own reports (`tcp-echo/`, `components.json`, "
                  "`scaling.json`), written by the existing runners.", ""]
        (self.output / "README.md").write_text("\n".join(lines))


def redact_paths(output, source):
    """Results are committed: replace host-specific absolute paths with stable placeholders."""
    replacements = [(str(source), "$SOURCE"), (str(Path.home()), "~")]
    real = str(source.resolve())
    if real != str(source):
        replacements.insert(0, (real, "$SOURCE"))
    for path in output.rglob("*"):
        if path.suffix in (".json", ".jsonl", ".md", ".csv") and path.is_file():
            text = original = path.read_text()
            for old, new in replacements:
                text = text.replace(old, new)
            if text != original:
                path.write_text(text)


def parse_csv(text):
    return list(csv.DictReader(io.StringIO(text)))


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def git(directory, *arguments):
    try:
        return subprocess.check_output(["git", *arguments], cwd=directory, text=True,
                                       stderr=subprocess.DEVNULL).strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return None


def host():
    info = {"platform": platform.platform(), "machine": platform.machine(), "python": platform.python_version(),
            "logical_cpus": os.cpu_count()}
    probes = {
        "darwin": [["sysctl", "-n", "machdep.cpu.brand_string"], ["sysctl", "-n", "hw.memsize"]],
        "linux": [["sh", "-c", "grep -m1 'model name' /proc/cpuinfo || lscpu"], ["sh", "-c", "grep MemTotal /proc/meminfo"]],
    }
    for command in probes.get(sys.platform if sys.platform != "linux2" else "linux", []):
        try:
            info.setdefault("details", []).append(subprocess.check_output(command, text=True).strip())
        except (subprocess.CalledProcessError, FileNotFoundError):
            pass
    for compiler in ("c++", "cl"):
        if shutil.which(compiler):
            result = subprocess.run([compiler, "--version"], text=True, capture_output=True)
            info["compiler"] = (result.stdout or result.stderr).splitlines()[0] if (result.stdout or result.stderr) else None
            break
    return info


def read_xmake_config(source):
    config = source / ".xmake" / platform_dir() / "xmake.conf"
    if not config.is_file():
        candidates = sorted((source / ".xmake").glob("*/*/xmake.conf"))
        config = candidates[0] if candidates else None
    return config.read_text() if config else None


def platform_dir():
    return {"darwin": "macosx", "win32": "windows"}.get(sys.platform, sys.platform)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--source", type=Path, required=True, help="source tree the binaries were built from")
    parser.add_argument("--build-dir", type=Path, required=True, help="xmake output directory (-o) of that tree")
    parser.add_argument("--output", type=Path, required=True, help="new directory for this baseline")
    parser.add_argument("--label", required=True, help="hardware / host label, without performance claims")
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--suites", default=",".join(Baseline.SUITES))
    parser.add_argument("--quick", action="store_true", help="small workloads to check the pipeline; not a baseline")
    args = parser.parse_args()
    if not 1 <= args.repeats <= 50:
        parser.error("--repeats must be in 1..50")
    selected = [name.strip() for name in args.suites.split(",") if name.strip()]
    unknown = sorted(set(selected) - set(Baseline.SUITES))
    if unknown:
        parser.error(f"unknown suites: {', '.join(unknown)}")
    return Baseline(args).measure(selected)


if __name__ == "__main__":
    sys.exit(main())
