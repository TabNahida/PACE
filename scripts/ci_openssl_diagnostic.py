#!/usr/bin/env python3
"""Run isolated OpenSSL diagnostics and retain every stage even after a crash."""

import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()
    args.output_dir.mkdir(parents=True, exist_ok=True)
    binaries = list(args.build_dir.rglob("pace_openssl_diagnostic.exe"))
    if len(binaries) != 1:
        raise RuntimeError(f"expected one OpenSSL diagnostic executable, found {len(binaries)}")
    binary = binaries[0].resolve()
    fixtures = Path(__file__).resolve().parents[1] / "tests" / "certs"
    report = {"kind": "openssl_diagnostic", "stages": [], "status": "running"}
    for stage in ("crypto", "context", "bio", "handshake", "handshake-tls12",
                  "handshake-tls13", "handshake-x25519", "handshake-p256", "crypto-mlkem"):
        print(f"OpenSSL diagnostic stage: {stage}", flush=True)
        record = {"stage": stage, "status": "failed"}
        try:
            result = subprocess.run([str(binary), stage, str(fixtures)],
                                    capture_output=True, timeout=90)
            record["exit_code"] = result.returncode
            record["exit_code_hex"] = f"0x{result.returncode & 0xffffffff:08x}"
            record["status"] = "passed" if result.returncode == 0 else "failed"
            (args.output_dir / f"{stage}.stdout.log").write_bytes(result.stdout)
            (args.output_dir / f"{stage}.stderr.log").write_bytes(result.stderr)
            print(result.stdout.decode("utf-8", errors="replace"), end="")
            print(result.stderr.decode("utf-8", errors="replace"), end="")
        except subprocess.TimeoutExpired as error:
            record["error"] = "timed out after 90 seconds"
            (args.output_dir / f"{stage}.stdout.log").write_bytes(error.stdout or b"")
            (args.output_dir / f"{stage}.stderr.log").write_bytes(error.stderr or b"")
        except OSError as error:
            record["error"] = str(error)
        report["stages"].append(record)
        print(json.dumps(record), flush=True)
    report["status"] = "passed" if all(item["status"] == "passed" for item in report["stages"]) else "failed"
    (args.output_dir / "summary.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    return 0 if report["status"] == "passed" else 1


if __name__ == "__main__":
    raise SystemExit(main())
