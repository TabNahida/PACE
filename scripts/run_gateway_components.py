#!/usr/bin/env python3
"""Local Linux component diagnostics; not an end-to-end server ranking."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import statistics
import subprocess


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--binary', type=Path, required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--repeats', type=int, default=5)
    p.add_argument('--iterations', type=int, default=200000)
    args = p.parse_args()
    if platform.system() != 'Linux': p.error('measurements are Linux-only')
    if not 1 <= args.repeats <= 100 or not 1 <= args.iterations <= 10000000: p.error('invalid workload')
    binary = args.binary.resolve(strict=True)
    data = {'schema': 1, 'environment': {'platform': platform.platform(),
            'cpuinfo': Path('/proc/cpuinfo').read_text(),
            'source_commit': subprocess.check_output(['git', 'rev-parse', 'HEAD'], text=True).strip(),
            'binary_sha256': hashlib.sha256(binary.read_bytes()).hexdigest(),
            'runner_sha256': hashlib.sha256(Path(__file__).read_bytes()).hexdigest()}, 'trials': []}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    for modes, workers in [(('fnv', 'xxh'), 1)] + [(('shared', 'local'), w) for w in (1, 2, 4, 8)]:
        for repeat in range(args.repeats):
            for mode in modes[::1 if repeat % 2 == 0 else -1]:
                load_before = os.getloadavg()
                record = json.loads(subprocess.check_output([str(binary), mode, str(workers), str(args.iterations)], text=True, timeout=120))
                if not record['validated']: raise ValueError('unvalidated trial')
                if mode in ('shared', 'local') and record['checksum'] != workers * args.iterations * 4096:
                    raise ValueError('wrong copy count')
                record.update(trial=repeat+1, load_before=load_before, load_after=os.getloadavg())
                data['trials'].append(record)
                args.output.write_text(json.dumps(data, indent=2) + '\n')
                print(f'{mode} workers={workers}: {record["operations_per_second"]:.0f} ops/s', flush=True)
    data['summaries'] = []
    for mode, workers in sorted({(r['mode'], r['workers']) for r in data['trials']}):
        rows = [r for r in data['trials'] if (r['mode'], r['workers']) == (mode, workers)]
        data['summaries'].append({'mode': mode, 'workers': workers, **{
            metric: {'median': statistics.median(r[metric] for r in rows), 'min': min(r[metric] for r in rows), 'max': max(r[metric] for r in rows)}
            for metric in ('operations_per_second', 'cpu_seconds')}})
    args.output.write_text(json.dumps(data, indent=2) + '\n')


if __name__ == '__main__':
    main()
