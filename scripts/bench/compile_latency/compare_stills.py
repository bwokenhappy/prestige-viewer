#!/usr/bin/env python3
"""Private held-image inputs -> anonymous A/A and A/B numeric quality receipts."""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import still_detail

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('reference_prefix', type=Path)
parser.add_argument('repeat_prefix', type=Path)
parser.add_argument('candidate_prefix', type=Path)
parser.add_argument('--out', type=Path, required=True)
args = parser.parse_args()
rows = []
for reference in sorted(args.reference_prefix.parent.glob(args.reference_prefix.name + '.still*.bin')):
    suffix = reference.name[len(args.reference_prefix.name):]
    files = [reference, Path(str(args.repeat_prefix) + suffix), Path(str(args.candidate_prefix) + suffix)]
    images = [still_detail.load(str(p)) for p in files]
    assert len({(x['w'], x['h'], x['frames']) for x in images}) == 1
    assert images[0]['frames'] >= 120
    rows.append(dict(key=suffix, extent=[images[0]['w'], images[0]['h']], frames=images[0]['frames'],
                     hashes=[hashlib.sha256(p.read_bytes()).hexdigest() for p in files],
                     aa=still_detail.compare(images[0], images[1]),
                     ab=still_detail.compare(images[0], images[2]),
                     noise=[still_detail.noise_figs(x) for x in images]))
assert rows, 'no stills found'
args.out.write_text(json.dumps(rows, indent=2) + '\n')
for row in rows:
    print(row['key'], 'A/A error', row['aa']['err_mean'], 'A/B error', row['ab']['err_mean'])
