#!/usr/bin/env python3
"""Format PACE C++ sources with the pinned clang-format, or check that they are formatted.

    python3 scripts/format.py            # rewrite files in place
    python3 scripts/format.py --check    # exit 1 and list files that would change
    python3 scripts/format.py src/runtime/poller.cpp   # only the given files

Set CLANG_FORMAT to choose the executable. CI installs the pinned version with
`pip install clang-format==<REQUIRED_VERSION>`.
"""

import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

REQUIRED_VERSION = "22.1.8"
ROOT = Path(__file__).resolve().parent.parent
SOURCE_DIRECTORIES = ("include", "src", "modules", "tests", "examples", "benchmarks")
SOURCE_SUFFIXES = {".c", ".cc", ".cpp", ".cppm", ".h", ".hpp", ".ipp", ".inl"}
# Archived measurement sources stay byte-for-byte as recorded.
EXCLUDED_PREFIXES = ("benchmarks/results/",)


def find_clang_format():
    candidate = os.environ.get("CLANG_FORMAT") or shutil.which("clang-format")
    if not candidate:
        sys.exit(f"clang-format not found; install it with: pip install clang-format=={REQUIRED_VERSION}")
    output = subprocess.run([candidate, "--version"], text=True, capture_output=True, check=True).stdout
    match = re.search(r"version (\d+\.\d+\.\d+)", output)
    version = match.group(1) if match else "unknown"
    if version != REQUIRED_VERSION:
        sys.exit(
            f"{candidate} is clang-format {version}; PACE requires {REQUIRED_VERSION} "
            f"(pip install clang-format=={REQUIRED_VERSION}, then set CLANG_FORMAT)"
        )
    return candidate


def tracked_sources():
    result = subprocess.run(
        ["git", "ls-files", "-z", "--", *SOURCE_DIRECTORIES],
        cwd=ROOT,
        capture_output=True,
        check=True,
    )
    for name in result.stdout.decode().split("\0"):
        if not name or name.startswith(EXCLUDED_PREFIXES):
            continue
        if Path(name).suffix in SOURCE_SUFFIXES:
            yield name


def unformatted_files(clang_format, files):
    unformatted = set()
    for start in range(0, len(files), 64):
        result = subprocess.run(
            [clang_format, "--dry-run", "--Werror", *files[start:start + 64]],
            cwd=ROOT,
            text=True,
            capture_output=True,
        )
        if result.returncode:
            unformatted.update(line.split(":", 1)[0] for line in result.stderr.splitlines() if ": error:" in line)
    return sorted(unformatted)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--check", action="store_true", help="report unformatted files instead of rewriting them")
    parser.add_argument("files", nargs="*", help="files relative to the repository root (default: all sources)")
    arguments = parser.parse_args()

    clang_format = find_clang_format()
    files = arguments.files or sorted(tracked_sources())
    if not files:
        return 0

    if not arguments.check:
        # Inserting braces can change indentation that only a later pass settles.
        for _ in range(3):
            for start in range(0, len(files), 64):
                subprocess.run([clang_format, "-i", *files[start:start + 64]], cwd=ROOT, check=True)
            if not unformatted_files(clang_format, files):
                return 0
        sys.exit("clang-format did not converge after three passes")

    unformatted = unformatted_files(clang_format, files)
    if unformatted:
        print("These files are not formatted (run: python3 scripts/format.py):", file=sys.stderr)
        for name in unformatted:
            print(f"  {name}", file=sys.stderr)
        return 1
    print(f"{len(files)} files formatted correctly")
    return 0


if __name__ == "__main__":
    sys.exit(main())
