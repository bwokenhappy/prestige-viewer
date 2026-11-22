#!/usr/bin/env python3
"""Compare complete-scene mean/noise stills, retaining only numeric evidence."""
import argparse
import json
import os
from pathlib import Path
import sys

os.environ['OPENBLAS_NUM_THREADS'] = '1'
os.environ['OMP_NUM_THREADS'] = '1'
os.setpriority(os.PRIO_PROCESS, 0, 19)
sys.path.insert(0, str(Path(__file__).resolve().parents[3] / 'scripts/bench'))
import still_detail as detail

p = argparse.ArgumentParser(description=__doc__)
p.add_argument('directory', type=Path)
p.add_argument('output', type=Path)
p.add_argument('--crops', type=Path)
a = p.parse_args()
result = {}
cases = sorted({f.name.rsplit('_', 1)[0] for f in a.directory.glob('*.still0.bin')})
for case in cases:
    files = sorted(a.directory.glob(case + '_*.still0.bin'))
    ref = detail.load(a.directory / (case + '_a2.still0.bin'))
    edge = detail.edge_mask(ref['lum'])
    rows = {}
    for f in files:
        arm = f.name.removeprefix(case + '_').removesuffix('.still0.bin')
        s = detail.load(f)
        assert (s['w'], s['h'], s['frames']) == (ref['w'], ref['h'], ref['frames'])
        row = {'noise': detail.noise_figs(s, edge)}
        if arm != 'a2':
            out = str(a.crops) if a.crops and arm == 'b3' else None
            row['versus_a2'] = detail.compare(ref, s, out_dir=out, tag=case)
        rows[arm] = row
    result[case] = {'dimensions': [ref['w'], ref['h']], 'frames': ref['frames'], 'runs': rows}
    print(case, len(rows), flush=True)
a.output.write_text(json.dumps(result, indent=2))
