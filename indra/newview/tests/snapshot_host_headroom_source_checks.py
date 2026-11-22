#!/usr/bin/env python3
"""Both snapshot paths size system memory with LLSnapshotBufferLimits::hostHeadroomBytes.

vk_live_snapshot (llviewerwindow.cpp) and snapshotFit (llappviewer.cpp) used avail - reserve, which
is zero whenever free RAM dips below the memory governor's reserve, so every snapshot was refused.

    python3 indra/newview/tests/snapshot_host_headroom_source_checks.py   (from the repo root)
"""
import pathlib
import re
import sys

root = pathlib.Path(__file__).resolve().parents[3]
win = (root / "indra/newview/llviewerwindow.cpp").read_text()
app = (root / "indra/newview/llappviewer.cpp").read_text()
fails = []
for name, src in (("llviewerwindow.cpp", win), ("llappviewer.cpp", app)):
    if "LLSnapshotBufferLimits::hostHeadroomBytes(" not in src:
        fails.append(f"{name}: sizes the snapshot with hostHeadroomBytes")
    if re.search(r"avail\s*>\s*reserve\s*\?\s*\(?\w*\)?\s*\(?avail\s*-\s*reserve", src):
        fails.append(f"{name}: no snapshot check left that uses avail - reserve")
    if "above the reserve" in src:
        fails.append(f"{name}: messages no longer say 'above the reserve'")
if "#include \"vkgfx/llsnapshotbufferlimits.h\"" not in app and "#include \"llsnapshotbufferlimits.h\"" not in app:
    fails.append("llappviewer.cpp includes llsnapshotbufferlimits.h")
for f in fails:
    print("FAIL:", f)
if fails:
    sys.exit(1)
print("snapshot_host_headroom_source_checks: ok")
