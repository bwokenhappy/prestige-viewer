#!/usr/bin/env python3
"""Export whitelisted numeric evidence, excluding raw replay metadata and logs."""
import argparse
import csv
import json
from pathlib import Path

from summarize import summarize


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directories', type=Path, nargs='+')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    summaries = []
    items = []
    for directory in args.directories:
        for path in sorted(directory.glob('*.json')):
            if path.name.count('.') != 1:
                continue
            raw = json.loads(path.read_text(encoding="utf-8-sig"))
            if 'pipelines' not in raw:
                continue
            row = summarize(path)
            for item in row.pop('pipelines'):
                items.append(dict(run=row['run'], **item))
            identity = json.loads(path.with_suffix('.identity.json').read_text(encoding="utf-8-sig"))
            row['identity'] = {k: identity[k] for k in (
                'source', 'executable_sha256', 'capture_manifest_sha256',
                'path_sha256', 'settings', 'app_cache_start',
                'driver_disk_cache_requested', 'worker_requested',
                'synthetic_oversize_shim_sha256') if k in identity}
            row['process'] = json.loads(path.with_suffix('.memory.json').read_text(encoding="utf-8-sig"))
            for phase in ('start', 'end'):
                row['cache_' + phase] = json.loads(
                    path.with_suffix('.cache-' + phase + '.json').read_text(encoding="utf-8-sig"))
            fds = path.with_suffix('.driver-fds.json')
            if fds.exists():
                receipt = json.loads(fds.read_text(encoding="utf-8-sig"))
                row['driver_fds'] = dict(count=len(receipt['paths']),
                                         all_isolated=receipt['all_isolated'])
            driver = path.with_suffix('.driver-cache-end.json')
            if driver.exists():
                sizes = json.loads(driver.read_text(encoding="utf-8-sig")).values()
                row['driver_cache_end'] = dict(files=len(sizes), bytes=sum(sizes))
            summaries.append(row)
    assert summaries and len({x['run'] for x in summaries}) == len(summaries)
    (args.out / 'runs.json').write_text(json.dumps(summaries, indent=2) + '\n')
    with (args.out / 'pipelines.csv').open('w', newline='') as stream:
        writer = csv.DictWriter(stream, fieldnames=list(items[0]), lineterminator="\n")
        writer.writeheader()
        writer.writerows(items)
    print('%d runs, %d pipeline records' % (len(summaries), len(items)))


if __name__ == '__main__':
    main()
