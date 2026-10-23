#!/usr/bin/env python3
"""Check benchmark correctness in CI; discard all timings and retain only pass/fail."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
from types import SimpleNamespace

from run_benchmarks import check_memory_provider, validate_record as validate_echo
from run_components import OPERATIONS, validate_record as validate_component
from run_scaling import MODES, validate_record as validate_scaling


def executable(build_dir, name):
    matches = [path for filename in (name, name + ".exe")
               for path in build_dir.rglob(filename) if path.is_file()]
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {name} executable under {build_dir}, found {len(matches)}")
    return matches[0].resolve()


def run_json(binary, arguments):
    result = subprocess.run([str(binary), *arguments], text=True, capture_output=True, timeout=30)
    if result.returncode:
        raise RuntimeError(f"{binary.name} failed ({result.returncode}): {result.stderr.strip()}")
    # Never print or persist the benchmark's stdout: it contains measurements.
    value = json.loads(result.stdout)
    if not isinstance(value, dict):
        raise ValueError(f"{binary.name} did not return a JSON object")
    return value


def smoke(build_dir, profile_only):
    passed = []
    binary = executable(build_dir, "pace_bench")
    capabilities = run_json(binary, ["--list"])
    if capabilities.get("pace_profiling") is not profile_only:
        raise ValueError("benchmark instrumentation differs from the requested smoke mode")
    backends = ("pace",) if profile_only else ("pace", "asio", "libuv")
    if any(name not in capabilities.get("backends", []) for name in backends):
        raise ValueError("benchmark executable is missing a required backend")
    if capabilities.get("extended_metrics_version") != 2:
        raise ValueError("benchmark executable does not expose current result schemas")
    config = SimpleNamespace(clients=2, requests=32, warmup=4, bytes=64)
    for backend in backends:
        record = run_json(binary, ["--backend", backend, "--clients", str(config.clients),
                                  "--requests", str(config.requests), "--warmup", str(config.warmup),
                                  "--bytes", str(config.bytes), "--timeout-ms", "5000", "--trial", "1"])
        validate_echo(record, backend, 1, config, instrumented=profile_only, require_extended=True,
                      expected_memory_source=capabilities["peak_process_memory_source"], expected_metrics_version=2)
        passed.append(f"tcp_payload_and_schema:{backend}")
    if profile_only:
        return passed
    memory = check_memory_provider(binary, capabilities)
    passed.append(f"memory_provider_contract:{memory['status']}")

    binary = executable(build_dir, "pace_components")
    capabilities = run_json(binary, ["--list"])
    if capabilities.get("profiling_enabled") is not False or set(capabilities.get("components", [])) != set(OPERATIONS):
        raise ValueError("component executable does not expose the expected uninstrumented operations")
    config = SimpleNamespace(iterations=1000, warmup=32, depth=4, batch=64)
    for component in OPERATIONS:
        record = run_json(binary, ["--component", component, "--iterations", str(config.iterations),
                                  "--warmup", str(config.warmup), "--depth", str(config.depth),
                                  "--batch", str(config.batch), "--trial", "1"])
        validate_component(record, component, 1, config)
        passed.append(f"component_checksum:{component}")

    binary = executable(build_dir, "pace_scaling")
    capabilities = run_json(binary, ["--list"])
    if capabilities.get("profiling_enabled") is not False or set(capabilities.get("modes", [])) != set(MODES):
        raise ValueError("scaling executable does not expose the expected uninstrumented execution modes")
    config = SimpleNamespace(clients=2, requests=16, warmup=2, bytes=64, timeout_ms=5000)
    for mode in MODES:
        for workers in (1, 2):
            record = run_json(binary, ["--mode", mode, "--workers", str(workers), "--clients", str(config.clients),
                                      "--requests", str(config.requests), "--warmup", str(config.warmup),
                                      "--bytes", str(config.bytes), "--timeout-ms", str(config.timeout_ms), "--trial", "1"])
            validate_scaling(record, mode, workers, 1, config)
            passed.append(f"worker_counts_and_checksum:{mode}:{workers}")
    return passed


def windows_rejections():
    if sys.platform != "win32":
        raise ValueError("Windows rejection checks must run on Windows")
    passed = []
    for feature in ("quic", "http3"):
        command = ["xmake", "f", "-p", "windows", "-c", "-m", "release", "-o", "build/unsupported-" + feature,
                   "--quic=" + ("y" if feature == "quic" else "n"),
                   "--http3=" + ("y" if feature == "http3" else "n"), "-y"]
        result = subprocess.run(command, text=True, capture_output=True, timeout=60)
        diagnostic = result.stdout + result.stderr
        if result.returncode == 0 or "QUIC/HTTP3 currently require a POSIX platform" not in diagnostic:
            raise RuntimeError(f"Windows {feature} did not fail with the documented unsupported-platform error "
                               f"(exit {result.returncode}). xmake diagnostic:\n{diagnostic[-4000:].strip()}")
        passed.append(f"unsupported_on_windows:{feature}")
    return passed


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--profile-only", action="store_true")
    parser.add_argument("--check-windows-quic-rejection", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.check_windows_quic_rejection:
        if args.build_dir or args.profile_only:
            parser.error("Windows platform checks cannot be combined with executable smoke checks")
    elif not args.build_dir:
        parser.error("--build-dir is required for executable smoke checks")
    report = {"schema_version": 1, "kind": "functional_smoke", "status": "running",
              "revision": os.environ.get("GITHUB_SHA"), "platform": sys.platform,
              "performance_data_retained": False, "checks_passed": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        report["checks_passed"] = windows_rejections() if args.check_windows_quic_rejection else smoke(args.build_dir, args.profile_only)
        report["status"] = "passed"
    except (OSError, ValueError, RuntimeError, subprocess.SubprocessError) as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    for check in report["checks_passed"]:
        print("PASS " + check)
    print("Functional checks passed; no performance measurements retained.")


if __name__ == "__main__":
    main()
