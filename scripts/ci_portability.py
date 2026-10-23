#!/usr/bin/env python3
"""Verify target machine types and run bounded functional tests, never benchmarks.

QEMU user mode translates target instructions/syscalls on the host Linux kernel;
its successful results are explicitly recorded as emulated, not native hardware.
Compile-only reports inspect archive members without claiming any execution.
"""

import argparse
import json
import os
from pathlib import Path
import platform
import subprocess


TARGETS = {
    "i386": ("x86", 32, "little"), "x86": ("x86", 32, "little"),
    "x86_64": ("x86_64", 64, "little"), "x64": ("x86_64", 64, "little"),
    "armv7": ("arm", 32, "little"), "armeabi-v7a": ("arm", 32, "little"),
    "arm64": ("aarch64", 64, "little"), "arm64-v8a": ("aarch64", 64, "little"),
    "riscv64": ("riscv", 64, "little"), "ppc64le": ("powerpc64", 64, "little"),
    "s390x": ("s390", 64, "big"),
}
TESTS = ("pace_runtime_tests", "pace_network_tests", "pace_udp_tests",
         "pace_http_extended_tests", "pace_websocket_tests")
TUTORIALS = ("tutorial_tasks", "tutorial_tcp", "tutorial_udp", "tutorial_workers",
             "tutorial_shutdown", "tutorial_http", "tutorial_http_client", "tutorial_websocket")


def binary_metadata(data):
    """Read machine/word size/endian from ELF, PE or thin Mach-O headers."""
    if data[:4] == b"\x7fELF":
        bits = {1: 32, 2: 64}[data[4]]
        byte_order = {1: "little", 2: "big"}[data[5]]
        machine = int.from_bytes(data[18:20], byte_order)
        family = {3: "x86", 62: "x86_64", 40: "arm", 183: "aarch64",
                  243: "riscv", 21: "powerpc64", 22: "s390"}[machine]
        return {"format": "ELF", "machine": family, "bits": bits, "byte_order": byte_order}
    if data[:2] == b"MZ":
        offset = int.from_bytes(data[60:64], "little")
        if data[offset:offset + 4] != b"PE\0\0":
            raise ValueError("invalid PE signature")
        machine = int.from_bytes(data[offset + 4:offset + 6], "little")
        family, bits = {0x14c: ("x86", 32), 0x8664: ("x86_64", 64),
                        0xaa64: ("aarch64", 64)}[machine]
        return {"format": "PE", "machine": family, "bits": bits, "byte_order": "little"}
    if data[:4] in (b"\xcf\xfa\xed\xfe", b"\xfe\xed\xfa\xcf"):
        byte_order = "little" if data[0] == 0xcf else "big"
        machine = int.from_bytes(data[4:8], byte_order)
        family = {0x1000007: "x86_64", 0x100000c: "aarch64"}[machine]
        return {"format": "Mach-O", "machine": family, "bits": 64, "byte_order": byte_order}
    raise ValueError("unrecognized executable/object header")


def archive_metadata(path):
    data = path.read_bytes()
    if not data.startswith(b"!<arch>\n"):
        raise ValueError(f"{path} is not a static archive")
    offset = 8
    members = []
    while offset < len(data):
        header = data[offset:offset + 60]
        if len(header) != 60 or header[-2:] != b"`\n":
            raise ValueError(f"invalid archive member in {path}")
        size = int(header[48:58])
        name = header[:16].decode("ascii").strip()
        member = data[offset + 60:offset + 60 + size]
        if len(member) != size:
            raise ValueError(f"truncated archive member in {path}")
        if name.startswith("#1/"):
            name_length = int(name[3:])
            name = member[:name_length].rstrip(b"\0").decode("ascii")
            member = member[name_length:]
        if name not in ("/", "//", "/SYM64/") and not name.startswith("__.SYMDEF"):
            members.append(binary_metadata(member))
        offset += 60 + size + size % 2
    if not members:
        raise ValueError(f"{path} contains no object files")
    if any(member != members[0] for member in members):
        raise ValueError(f"{path} mixes target architectures")
    return members[0]


def executable(build_dir, name):
    matches = [path for filename in (name, name + ".exe")
               for path in build_dir.rglob(filename) if path.is_file()]
    if len(matches) != 1:
        raise ValueError(f"expected exactly one {name} under {build_dir}, found {len(matches)}")
    return matches[0].resolve()


def require_target(metadata, target):
    actual = (metadata["machine"], metadata["bits"], metadata["byte_order"])
    if actual != TARGETS[target]:
        raise ValueError(f"target {target} expects {TARGETS[target]}, binary contains {actual}")


def validate(args, report):
    if args.mode == "compile-only":
        for name in ("pace_runtime", "pace_net", "pace_http1", "pace_websocket"):
            paths = list(args.build_dir.rglob("lib" + name + ".a"))
            if len(paths) != 1:
                raise ValueError(f"expected one {name} archive, found {len(paths)}")
            metadata = archive_metadata(paths[0])
            require_target(metadata, args.target)
            report["artifacts"].append({"name": paths[0].name, **metadata})
        return

    names = ("pace_runtime_tests",) if args.metadata_only else TESTS + TUTORIALS
    prefix = [args.emulator, "-L", str(args.sysroot)] if args.mode == "qemu-user" else []
    for name in names:
        binary = executable(args.build_dir, name)
        metadata = binary_metadata(binary.read_bytes())
        require_target(metadata, args.target)
        report["artifacts"].append({"name": name, **metadata})
        if not args.metadata_only:
            print(f"RUN ({args.mode}, {args.target}) {name}", flush=True)
            report["execution_performed"] = True
            subprocess.run([*prefix, str(binary)], cwd=Path(__file__).resolve().parents[1],
                           check=True, timeout=120)
            report["checks_passed"].append(name)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--target", choices=TARGETS, required=True)
    parser.add_argument("--mode", choices=("native", "qemu-user", "compile-only"), required=True)
    parser.add_argument("--metadata-only", action="store_true")
    parser.add_argument("--emulator")
    parser.add_argument("--sysroot", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.mode == "qemu-user" and (not args.emulator or not args.sysroot):
        parser.error("QEMU mode requires --emulator and --sysroot")
    if args.mode != "qemu-user" and (args.emulator or args.sysroot):
        parser.error("--emulator and --sysroot are only valid for QEMU mode")
    if args.metadata_only and args.mode != "native":
        parser.error("--metadata-only is for the native xmake-test workflow")
    report = {"schema_version": 1, "kind": "portability_validation", "status": "running",
              "revision": os.environ.get("GITHUB_SHA"), "execution_mode": args.mode,
              "host_machine": platform.machine(), "target": args.target,
              "execution_performed": False,
              "metadata_only": args.metadata_only, "performance_data_retained": False,
              "artifacts": [], "checks_passed": []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    try:
        validate(args, report)
        report["status"] = "passed"
    except (OSError, ValueError, KeyError, IndexError, subprocess.SubprocessError) as error:
        report["status"] = "failed"
        report["error"] = str(error)
        raise
    finally:
        args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"PASS {args.mode}: {args.target}; architecture verified; no performance data retained.")


if __name__ == "__main__":
    main()
