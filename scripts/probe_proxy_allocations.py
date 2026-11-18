#!/usr/bin/env python3
"""Linux/glibc allocation census; intrusive diagnostics, never performance evidence."""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
from run_gateway_benchmarks import digest, port, wait_listening


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('pace', 'baseline', 'load', 'preload', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    args = parser.parse_args()
    result = {'scope': 'malloc/calloc/realloc calls on one frontend reactor thread; startup + 32 * (1000 warmup + 10000 measured) requests',
              'measurement_kind': 'intrusive_allocation_diagnostic',
              'binary_sha256': {key: digest(getattr(args, key)) for key in ('pace', 'baseline', 'load', 'preload')},
              'trials': []}
    for key in ('baseline', 'pace'):
        processes = []
        with tempfile.TemporaryFile(mode='w+t') as log:
            try:
                origins = []
                for _ in range(2):
                    number = port()
                    p = subprocess.Popen([str(args.baseline), 'origin', str(number), '2', '-', '0', '4096'], stdout=subprocess.DEVNULL)
                    processes.append(p); wait_listening(p, number); origins.append(number)
                number = port()
                p = subprocess.Popen([str(getattr(args, key)), 'proxy', str(number), '1',
                                      ','.join('http://127.0.0.1:' + str(n) for n in origins), '0', '4096'],
                                     env=dict(os.environ, LD_PRELOAD=str(args.preload.resolve()), PACE_IO_BACKEND='epoll'),
                                     stdout=subprocess.DEVNULL, stderr=log)
                processes.append(p); wait_listening(p, number)
                load = json.loads(subprocess.check_output([str(args.load), str(number), '32', '10000', '1000', '4096'], text=True, timeout=120))
                if not load['validated'] or load['completed_requests'] != 320000:
                    raise RuntimeError('invalid diagnostic workload')
                p.terminate(); p.wait(timeout=10)
                log.seek(0); counters = json.loads(log.read())
                result['trials'].append({'backend': key, 'completed_including_warmup': 352000,
                                         'allocation_calls': counters, 'calls_per_exchange_including_startup': sum(counters.values()) / 352000})
            finally:
                for p in reversed(processes):
                    if p.poll() is None: p.terminate()
                    p.wait(timeout=10)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
