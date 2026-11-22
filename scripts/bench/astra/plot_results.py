#!/usr/bin/env python3
"""CPU-only figures from sanitized accepted receipts; intervals are not additive."""
import argparse
import json
import statistics
from pathlib import Path

import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('evidence', type=Path)
p.add_argument('output', type=Path)
a = p.parse_args()
runs = {r['name']: r for r in map(json.loads, a.evidence.read_text().splitlines())}
a.output.mkdir(parents=True, exist_ok=True)
plt.rcParams.update({'font.size': 10, 'svg.fonttype': 'none'})
fig, axes = plt.subplots(1, 3, figsize=(16, 5), constrained_layout=True)
groups = [
    ('Historical 3190', ['historical_a2', 'historical_a3']),
    ('Before #1604', ['before1604_a2', 'before1604_a3']),
    ('After #1604', ['after1604_a2', 'after1604_a3']),
    ('Current ae1', ['current_a1', 'current_a2', 'current_a3']),
    ('Prototype OFF', ['prototype_a1', 'prototype_a2', 'prototype_a3']),
    ('GI prefix ON', ['prototype_gi1']),
    ('Empty avatar ON', ['prototype_avatar1', 'prototype_avatar2']),
]
ax = axes[0]
for i, (label, names) in enumerate(groups):
    vals = [runs[n]['timing']['avg'] for n in names]
    center = statistics.mean(vals)
    ax.barh(i, center, color='#5276a7', alpha=.25)
    ax.plot([min(vals), max(vals)], [i, i], color='#203e62', lw=3)
    ax.scatter(vals, [i] * len(vals), color='#203e62', s=18, zorder=3)
    ax.text(max(vals) + .1, i, f'{center:.3f}', va='center', fontsize=9)
ax.set(yticks=range(len(groups)), yticklabels=[x[0] for x in groups],
       xlabel='GPU frame mean (ms)', xlim=(0, 25),
       title='Matched HOUSE timing\nDots: run means; line: observed range')
ax.invert_yaxis()

ax = axes[1]
stages = ['setup', 'assign', 'dirty', 'plan', 'build', 'finalize']
r = runs['instrument_stages']['passes']
for i, stage in enumerate(stages):
    v = r['c_lvc_' + stage + '@async']
    ax.barh(i, v['avg'], color='#4c8c76', alpha=.5)
    ax.plot([v['avg'], v['p95']], [i, i], color='#205843', lw=2)
    ax.scatter([v['p95']], [i], color='#205843', marker='|', s=65)
ax.set(yticks=range(len(stages)), yticklabels=stages, xlabel='Interval (ms)',
       title='Optional reach-map timestamps\nBar: mean; tick: per-frame p95')
ax.invert_yaxis()

ax = axes[2]
base = [runs[n] for n in ['prototype_a1', 'prototype_a2']]
b = runs['prototype_avatar1']
keys = ['g_av_depth_copy', 'c_vcloud_march', 'c_part_light']
for i, key in enumerate(keys):
    av = statistics.mean(x['passes'].get(key, {'avg': 0})['avg'] for x in base)
    bv = b['passes'].get(key, {'avg': 0})['avg']
    ax.barh(i - .17, av, height=.3, color='#5276a7', label='OFF A1/A2' if i == 0 else None)
    ax.barh(i + .17, bv, height=.3, color='#b57b36', label='ON B1' if i == 0 else None)
ax.set(yticks=range(len(keys)), yticklabels=['Avatar depth copy', 'Cloud march interval', 'Particle light interval'],
       xlabel='Interval mean (ms)', title='Deleted mark is mostly reassigned time\nTotal-frame difference is much smaller')
ax.invert_yaxis()
ax.legend(loc='lower right', frameon=False)
for ax in axes:
    ax.spines[['top', 'right']].set_visible(False)
    ax.grid(axis='x', alpha=.15)
fig.suptitle('RX7900XTX • no FG • truncated HOUSE: cost attribution only, no quality or RTX5090 claim', fontsize=12)
fig.savefig(a.output / 'amd-attribution.png', dpi=160)
fig.savefig(a.output / 'amd-attribution.svg')
plt.close(fig)

# Keep generated vector output friendly to repository whitespace checks.
for svg in a.output.glob("*.svg"):
    svg.write_text("\n".join(line.rstrip() for line in svg.read_text().splitlines()) + "\n")
