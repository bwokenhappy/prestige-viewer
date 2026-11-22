#!/usr/bin/env python3
"""Extract anonymous compile receipts; never include captures, settings or log text.

Batch windows overlap. Their summed wall_ms is NOT startup wall time. wait_s is
replay readiness, including injection; span_ms covers first-start to last-end.
Memory is process commit on Windows, RSS on Linux; concurrent item attribution
is a sampling estimate, not an isolated compiler allocation measurement.
"""
import argparse
import collections
import json
from pathlib import Path


def summarize(path):
    d = json.loads(path.read_text(encoding="utf-8-sig"))
    p = d['pipelines']
    assert d['ok'] and p['settled'], 'failed/unsettled run'
    assert p['batches_dropped'] == 0 and p['items_dropped'] == 0, 'truncated telemetry'
    batches = p['batches']
    assert all(not b['failed'] and len(b['items']) == b['total'] for b in batches)
    assert d['overall']['unfocused_frames'] == 0, 'unfocused timing'
    rows = [dict(x, batch=b['what']) for b in batches for x in b['items']]
    assert all(not x['failed'] for x in rows)
    first = min(b['at_s'] - b['wall_ms'] / 1000 for b in batches)
    last = max(b['at_s'] for b in batches)
    timing_start = p.get('timed_start_s')
    runtime_eligible = timing_start is not None and last <= timing_start
    families = collections.defaultdict(lambda: {'count': 0, 'ms': 0., 'hits': 0, 'misses': 0})
    for x in rows:
        f = families[x['batch']]
        f['count'] += 1
        f['ms'] += x['ms']
        f['hits'] += x['app_cache_hit'] == 1
        f['misses'] += x['app_cache_hit'] == 0
    return dict(run=path.stem, gpu=d['gpu'], driver=d['driver'],
        readiness_s=p['wait_s'], batch_span_ms=(last-first)*1000,
        compile_sum_ms=sum(x['ms'] for x in rows),
        batch_peak_bytes=max(b['peak_bytes'] for b in batches),
        app_hits=sum(x['app_cache_hit'] == 1 for x in rows),
        app_misses=sum(x['app_cache_hit'] == 0 for x in rows),
        no_feedback=sum(x['app_cache_hit'] == -1 for x in rows),
        runtime_eligible=runtime_eligible, timed_start_s=timing_start,
        count=len(rows), dedup=sum(x['dedup'] for x in rows),
        runtime_gpu_ms=d['overall']['gpu_ms'], window=d['window'],
        families=dict(families),
        slowest=sorted(rows, key=lambda x: x['ms'], reverse=True)[:20],
        batches=[{k:v for k,v in b.items() if k != 'items'} for b in batches],
        pipelines=rows)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('reports', type=Path, nargs='+')
    parser.add_argument('--out', type=Path, required=True)
    args=parser.parse_args()
    rows=[summarize(p) for p in args.reports]
    args.out.write_text(json.dumps(rows, indent=2)+'\n')
    for r in rows:
        timing = ('GPU %.3fms' % r['runtime_gpu_ms']['avg']) if r['runtime_eligible'] else 'runtime excluded'
        print('%s: ready %.3fs, compile sum %.3fs, peak %.1f MiB, hits %d/%d, %s' % (
            r['run'],r['readiness_s'],r['compile_sum_ms']/1000,r['batch_peak_bytes']/2**20,
            r['app_hits'],r['count'],timing))


if __name__ == '__main__':
    main()
