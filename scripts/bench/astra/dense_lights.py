#!/usr/bin/env python3
"""CPU-only synthetic dense-light fixture, derived from the sealed-room benchmark.

Write outside the checkout; never launches a viewer. Closed/visible differ only in
one wall's transform. Dark variants retain that geometry with zero local lights.
The resulting cost difference includes legitimate lighting/indirect differences;
compare a renderer option against its own same-capture baseline, not across scenes.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import sealed_room as room
import sl_capture_core as core
import numpy as np


def make_scene(visible, count):
    scene = room.scene(False)
    scene['name'] = 'ASTRA synthetic dense local lights'
    scene['objects'] = [o for o in scene['objects'] if o['name'] != 'GlassPane']
    # Uniform diffuse receivers isolate local-light sampling from glossy/glass paths.
    for obj in scene['objects']:
        obj['mesh'] = 'cube'
        if visible and obj['name'] == 'WallXp':
            obj['matrix'] = room.translate(0.0, 20.0, 0.0) @ obj['matrix']
    center = (room.ROOM_LO + room.ROOM_HI) * .5
    scene['lights'] = []
    for i in range(count):
        pos = [room.ROOM_HI[0] + 1.2, center[1] + ((i % 16) - 7.5) * .12,
               center[2] + ((i // 16) - 3.5) * .12]
        scene['lights'].append(dict(name=f'fixture-light-{i:03d}', type='point',
            matrix=room.translate(*pos) @ np.diag([.02, .02, .02, 1.0]),
            color=[1.0, .8, .6], intensity=1.0 / 128.0, radius=12.0, falloff=.5))
    # Camera, geometry count, light positions/count and environment otherwise fixed.
    key = copy.deepcopy(scene['camera']['keys'][1])
    key.update(t=0.0, segment='receiver', still=240, name='receiver')
    scene['camera']['keys'] = [key, dict(key, t=3.0, still=0)]
    scene['seconds'] = 3.0
    scene['extras'] = dict(purpose='dense occluded local-light diagnostic', local_lights=count,
        wall_open=visible, probes=0, no_live_ingestion=True)
    return scene


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    output = args.output.resolve()
    repo = Path(__file__).resolve().parents[3]
    if output == repo or repo in output.parents:
        parser.error('capture output must be outside the checkout')
    output.mkdir(parents=True, exist_ok=True)
    receipts = {}
    for name, visible, count in [('closed128', False, 128), ('visible128', True, 128),
                                  ('closed0', False, 0), ('visible0', True, 0)]:
        dest = output / name
        if dest.exists():
            parser.error(f'refusing to overwrite {dest}')
        scene = make_scene(visible, count)
        core.write_capture(scene, str(dest), log=lambda *args: None)
        entries = [[str(p.relative_to(dest)), p.stat().st_size, hashlib.sha256(p.read_bytes()).hexdigest()]
                   for p in sorted(dest.rglob('*')) if p.is_file()]
        receipts[name] = dict(manifest_sha256=hashlib.sha256((dest/'manifest.json').read_bytes()).hexdigest(),
            tree_sha256=hashlib.sha256(json.dumps(entries, separators=(',', ':')).encode()).hexdigest(),
            files=entries, local_lights=count, visible=visible)
    path = core.camera_path_json(make_scene(False, 128)['camera'])
    (output/'path.json').write_text(json.dumps(path, indent=2))
    receipts['path_sha256'] = hashlib.sha256((output/'path.json').read_bytes()).hexdigest()
    (output/'identities.json').write_text(json.dumps(receipts, indent=2))
    print('Created four synthetic captures and identities; no viewer launched.')


if __name__ == '__main__':
    main()
