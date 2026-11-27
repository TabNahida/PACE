#!/usr/bin/env python3
"""Run every doctest-based test executable once more and keep a JUnit XML report of each.

CI calls this only after `xmake test` fails: doctest writes one report format per run, and the
console output of the first run stays the primary log. The reports go to --output as
<executable>.xml and are uploaded as artifacts.
"""

import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SUFFIX = ".exe" if os.name == "nt" else ""


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=300, help="seconds per executable")
    arguments = parser.parse_args()
    arguments.output.mkdir(parents=True, exist_ok=True)
    executables = sorted(path for path in arguments.build_dir.rglob(f"pace_*_tests{SUFFIX}") if path.is_file())
    if not executables:
        sys.exit(f"no pace_*_tests executables under {arguments.build_dir}")
    failed = []
    for executable in executables:
        report = arguments.output / f"{executable.stem}.xml"
        try:
            result = subprocess.run([str(executable), "--reporters=junit", f"--out={report}"], cwd=ROOT,
                                    timeout=arguments.timeout)
            if result.returncode:
                failed.append(f"{executable.stem} (exit {result.returncode})")
        except subprocess.TimeoutExpired:
            failed.append(f"{executable.stem} (timed out)")
    print(f"{len(executables)} reports in {arguments.output}")
    for name in failed:
        print(f"failed: {name}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
