#!/usr/bin/env python3
"""Export numeric worker evidence without retaining private log lines or paths."""
import argparse
import json
from pathlib import Path
import re

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('directories', type=Path, nargs='+')
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
rows = []
patterns = {
    'batches': (
        r'\[compile worker\] batch: (\d+) pipelines \((\d+) compiled \+ (\d+) dedup twins\), '
        r'sum of compiles (\d+) ms, wall (\d+) ms; driver cache feedback: (\d+) hits, (\d+) misses, (\d+) without',
        ('items', 'compiled', 'dedup', 'sum_ms', 'wall_ms', 'hits', 'misses', 'no_feedback')),
    'respin': (
        r'respin: (\d+) of (\d+) module\(s\) replaced by this build\x27s SPIR-V \((\d+) of them changed\), (\d+) kept as recorded',
        ('replaced', 'total', 'changed', 'kept')),
    'viewer_reuse': (
        r'create-info hashes: (\d+) match the worker\x27s, (\d+) differ, (\d+) not compared \(of (\d+)\); '
        r'driver cache feedback here: (\d+) hits, (\d+) misses, (\d+) no feedback; (\d+) missed with the same create info',
        ('matched', 'different', 'not_compared', 'items', 'hits', 'misses', 'no_feedback', 'same_identity_misses')),
}
for directory in args.directories:
    for path in sorted(directory.glob('*.log')):
        log = path.read_text(encoding='utf-8-sig', errors='replace')
        row = {'run': path.stem}
        for name, (pattern, keys) in patterns.items():
            row[name] = [dict(zip(keys, map(int, values))) for values in re.findall(pattern, log)]
        assert row['batches'] and row['viewer_reuse'], path.name
        row['viewer_wire_values'] = sorted(set(map(int, re.findall(r'rezfx_wire=(\d+)', log))))
        row['identity_differences'] = [
            dict(index=int(i), pipeline=name, viewer_hash=a, worker_hash=b)
            for i, name, a, b in re.findall(
                r'worker reuse \([^\r\n]+?\): #(\d+) ([^\r\n]+?): create info differs from the worker\x27s '
                r'\(here ([0-9a-f]+), worker ([0-9a-f]+);', log)]
        rows.append(row)
assert rows and len({r['run'] for r in rows}) == len(rows)
args.out.write_text(json.dumps(rows, indent=2) + '\n')
print('%d worker receipts' % len(rows))
