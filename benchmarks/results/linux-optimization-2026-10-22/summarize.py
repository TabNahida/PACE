#!/usr/bin/env python3
"""Revalidate complete comparison reports and derive paired backend ratios."""
import hashlib
import json
from pathlib import Path
from types import SimpleNamespace
import sys

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parents[2] / 'scripts'))
from compare_builds import paired_summary
from run_benchmarks import validate_record

output = {}
for path in sorted(HERE.glob('*.json')):
    report = json.loads(path.read_text())
    environment = report.get('environment', {})
    if not environment.get('fair_backend_comparison'):
        continue
    if environment.get('status') != 'completed':
        raise ValueError(f'{path.name} is incomplete')
    configuration = SimpleNamespace(**environment['configuration'])
    records = {}
    for record in report['runs']:
        validate_record(record, record['backend'], record['trial'], configuration)
        key = record['backend'], record['trial']
        if key in records:
            raise ValueError(f'{path.name} duplicate {key}')
        records[key] = record
    if len(records) != 3 * configuration.repeats:
        raise ValueError(f'{path.name} missing records')
    comparisons = {}
    for baseline in ['asio', 'libuv']:
        comparisons[baseline] = {}
        for metric in ['requests_per_second', 'p50_us', 'p95_us', 'p99_us']:
            pairs = [{'before': records[baseline, trial][metric],
                      'after': records['pace', trial][metric]}
                     for trial in range(1, configuration.repeats + 1)]
            comparisons[baseline][metric] = paired_summary(
                pairs, metric, metric != 'requests_per_second', 10000, 0)
    output[path.name] = {
        'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
        'ratio_definition': 'PACE / comparator, matched by rotated trial number',
        'uncertainty': 'Exploratory whole-pair bootstrap medians, 10000 resamples, seed 0. '
                       'Selection, short sessions and correlated host noise limit inference.',
        'comparisons': comparisons,
    }
path = HERE / 'final-vs-head.json'
report = json.loads(path.read_text())
if report['status'] != 'completed':
    raise ValueError('before/after comparison is incomplete')
configuration = SimpleNamespace(**report['configuration'])
records = {}
for record in report['runs']:
    validate_record(record, 'pace', record['trial'], configuration)
    key = record['variant'], record['trial']
    if key in records:
        raise ValueError('duplicate before/after record')
    records[key] = record
if len(records) != 2 * configuration.pairs:
    raise ValueError('missing before/after record')
output[path.name] = {
    'source_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
    'ratio_definition': 'final PACE / original HEAD PACE, alternating execution order',
    'comparisons': {metric: paired_summary(
        [{'before': records['before', trial][metric], 'after': records['after', trial][metric]}
         for trial in range(1, configuration.pairs + 1)],
        metric, metric != 'requests_per_second', 10000, 0)
        for metric in ['requests_per_second', 'p50_us', 'p95_us', 'p99_us']},
}
(HERE / 'comparisons.json').write_text(json.dumps(output, indent=2) + '\n')
print(f'Validated {len(output) - 1} complete three-backend reports and one before/after report')
