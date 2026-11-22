#!/usr/bin/env python3
"""Sanitized timing/identity summary of ASTRA runs; raw logs and captures stay private.

Run after copying a runner output directory locally. Fails on incomplete replays,
missing identities, mismatched captures/paths/windows, or absent split path proof.
Individual frame p50/p95/p99 are not pooled from per-run percentiles.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import statistics


def summarize(root, names):
    runs = []
    keys = None
    for name in names:
        report_path = root / (name + '.json')
        report = json.loads(report_path.read_text(encoding='utf-8-sig'))
        identity = json.loads((root / (name + '.identity.json')).read_text(encoding='utf-8-sig'))
        log = (root / (name + '.log')).read_text(errors='replace')
        if not report.get('ok') or report.get('mode') != 'replay':
            raise ValueError(name + ': incomplete/non-replay run')
        if 'starting anyway (this run may time a fallback variant)' in log:
            raise ValueError(name + ': pipeline readiness deadline expired')
        if 'replay: injected skins 196 (0 failed), armatures 81' not in log:
            raise ValueError(name + ': complete club skin injection not proved')
        if report['overall'].get('unfocused_frames', 0):
            raise ValueError(name + ': unfocused frames')
        key = (identity['capture_manifest_sha256'], identity['path_sha256'],
               tuple(report['window']), report.get('seed'), report.get('fixed_dt'))
        if keys is None:
            keys = key
        if key != keys:
            raise ValueError(name + ': scene/path/window/seed/time-step differs')
        paths = re.findall(r'VKDeferSplit: deferred lighting runs ([^\r\n]+)', log)
        if not paths or not paths[-1].startswith('SPLIT'):
            raise ValueError(name + ': no final split path proof')
        extents = re.findall(r'VKDeferSplit: [^\r\n]*\b(\d+)x(\d+)[^\r\n]*; (half|full)-resolution reflections', log)
        requested = bool(int(identity['settings']['RenderVKDeferReflectHalfRes']))
        if not extents or (extents[-1][2] == 'half') != requested:
            raise ValueError(name + ': missing/mismatched actual reflection extent marker')
        if re.search(r'GRAPHICS DEVICE LOST|VK_ERROR_DEVICE_LOST', log):
            raise ValueError(name + ': device loss')
        runs.append({'name': name, 'source': identity['source'],
                     'executable_sha256': identity['executable_sha256'],
                     'report_sha256': hashlib.sha256(report_path.read_bytes()).hexdigest(),
                     'window': report['window'], 'render': [int(x) for x in extents[-1][:2]],
                     'reflection_resolution': extents[-1][2], 'path': paths[-1],
                     'settings': identity['settings'], 'gpu': report['gpu'],
                     'timing': {'overall': report['overall']['gpu_ms']} |
                               {s['name']: s['gpu_ms'] for s in report['segments']},
                     'vram': report.get('vram'),
                     'passes': {k: v for k, v in report['overall']['passes'].items()
                                if any(t in k for t in ('defer', 'fsr', 'gi_', 'rtmask', 'async_busy'))},
                     'noise': [{k: n[k] for k in ('segment', 'frames', 'width', 'height', 'psnr_db')}
                               for n in report.get('noise', [])]})
    if len({tuple(r['render']) for r in runs}) != 1:
        raise ValueError('render extents differ')
    arms = {}
    for run in runs:
        arm = re.sub(r'\d+$', '', run['name'])
        arms.setdefault(arm, []).append(run)
    comparisons = {}
    for arm, group in arms.items():
        comparisons[arm] = {}
        for segment in group[0]['timing']:
            data = [r['timing'][segment] for r in group]
            avgs = [x['avg'] for x in data]
            comparisons[arm][segment] = {'runs': len(data), 'median_run_avg_ms': statistics.median(avgs),
                'run_avg_min_ms': min(avgs), 'run_avg_max_ms': max(avgs),
                'per_run_p50_ms': [x['p50'] for x in data], 'per_run_p95_ms': [x['p95'] for x in data],
                'per_run_p99_ms': [x['p99'] for x in data]}
    return {'capture_manifest_sha256': keys[0], 'path_sha256': keys[1], 'runs': runs, 'arms': comparisons}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('directory', type=Path)
    parser.add_argument('runs', nargs='+')
    args = parser.parse_args()
    print(json.dumps(summarize(args.directory, args.runs), indent=2))


if __name__ == '__main__':
    main()
