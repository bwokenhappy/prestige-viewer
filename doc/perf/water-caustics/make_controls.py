#!/usr/bin/env python3
"""Derive independent material controls from the validated synthetic water fixture.

No viewer/GPU, captured assets or rigged data. Manifest hashes are refreshed for
all binary edits. Original input directories are read only.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import struct
import sys

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / 'scripts/bench'))
import sl_capture_core as core


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    receipts = []
    for mode in ('fullbright', 'specular'):
        out = args.output / mode
        shutil.copytree(args.source, out)
        changed = []
        if mode == 'fullbright':
            p = out / 'instances.bin'
            blob = bytearray(p.read_bytes())
            for row in range(8):
                offset = 24 + row * 232 + 192
                flags, = struct.unpack_from('<I', blob, offset)
                struct.pack_into('<I', blob, offset, flags | core.FLAG_FULLBRIGHT)
            p.write_bytes(blob)
            changed.append(p.name)
        else:
            p = out / 'materials.bin'
            blob = bytearray(p.read_bytes())
            for material in range(3):
                offset = 24 + material * 220
                # All opaque receiver materials: exact metal=1 => no PBR diffuse.
                struct.pack_into('<ff', blob, offset + 116, 1.0, 0.25)
                blob[offset + 136] = 0
                blob[offset + 138] = 1
            p.write_bytes(blob)
            changed.append(p.name)
        manifest_path = out / 'manifest.json'
        manifest = json.loads(manifest_path.read_text())
        for name in changed:
            blob = (out / name).read_bytes()
            manifest['files'][name] = {'bytes': len(blob), 'xxh64': core.hex64(core.xxh3_64(blob))}
        manifest['synthetic_water']['material_control'] = mode
        manifest_path.write_text(json.dumps(manifest, separators=(',', ':')) + '\n')
        receipts.append({'capture': str(out), 'control': mode,
                         'manifest_sha256': hashlib.sha256(manifest_path.read_bytes()).hexdigest()})
    print(json.dumps(receipts, indent=2))


if __name__ == '__main__':
    main()
