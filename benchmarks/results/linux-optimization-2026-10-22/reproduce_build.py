#!/usr/bin/env python3
"""Rebuild the measured pre-change and final core from revision + retained patch.

Run from any directory. Requires GCC and the measured Asio 1.34.2/libuv 1.52.1
headers/static library; normal dependency installation is deliberately separate.
The snapshots retain the pre-refactor source paths for exact reproducibility.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--asio-include', type=Path, required=True)
parser.add_argument('--libuv-root', type=Path, required=True)
parser.add_argument('--output', type=Path, default=REPO / 'build/linux-optimization-reproduce')
args = parser.parse_args()
args.asio_include = args.asio_include.resolve(strict=True)
args.libuv_root = args.libuv_root.resolve(strict=True)
args.output = args.output.resolve()
manifest = json.loads((HERE / 'build.json').read_text())
files = ['benchmarks/echo_bench.cpp', 'src/io_context.cpp', 'src/poller.cpp', 'src/net.cpp',
         'include/pace/io_context.hpp', 'include/pace/task.hpp', 'include/pace/tcp.hpp',
         'include/pace/platform.hpp', 'include/pace/detail/poller.hpp',
         'include/pace/detail/profiling.hpp']
before = args.output / 'before'
if before.exists() or (args.output / 'final').exists():
    parser.error('choose a fresh output directory; existing source snapshots are not overwritten')
for name in files:
    path = before / name
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(subprocess.check_output(
        ['git', 'show', manifest['before_revision'] + ':' + name], cwd=REPO))
final = args.output / 'final'
shutil.copytree(before, final)
subprocess.run(['patch', '-p1', '-i', str(HERE / 'final-runtime.patch')], cwd=final, check=True)
for name, digest in manifest['final_source_sha256'].items():
    if hashlib.sha256((final / name).read_bytes()).hexdigest() != digest:
        raise ValueError('source identity mismatch: ' + name)
commands = []
for variant in ['before', 'final']:
    source = args.output / variant
    command = ['g++', '-std=c++20', '-O3', '-DNDEBUG', '-fvisibility=hidden',
               '-fvisibility-inlines-hidden', '-pthread', '-I' + str(source / 'include'),
               '-isystem', str(args.asio_include), '-isystem', str(args.libuv_root / 'include'),
               '-DPACE_HAS_ASIO', '-DPACE_HAS_LIBUV', '-DASIO_STANDALONE',
               str(source / 'benchmarks/echo_bench.cpp'), str(source / 'src/io_context.cpp'),
               str(source / 'src/poller.cpp'), str(source / 'src/net.cpp'),
               str(args.libuv_root / 'lib/libuv.a'), '-ldl', '-o', str(args.output / (variant + '-bench'))]
    subprocess.run(command, check=True)
    commands.append(command)
(args.output / 'commands.json').write_text(json.dumps(commands, indent=2) + '\n')
print('Built', args.output / 'before-bench', 'and', args.output / 'final-bench')
