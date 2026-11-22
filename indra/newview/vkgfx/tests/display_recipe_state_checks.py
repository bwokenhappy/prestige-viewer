#!/usr/bin/env python3
"""Source checks for the live SDR <-> HDR switch (#1451): it rebuilds the output pipelines from
their registered recipes (registerDisplayPipeline), so a recipe must hold the state its pipeline
is built with. A field set between the registration and the build is lost on the first switch:
the HUD's alpha pipeline once came back without blending and with depth writes that way.

The one field a recipe may leave to the switch is fragSpec when the recipe names the
specialization the switch sets (linearSpecId != UINT32_MAX). No viewer link or GPU.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[4]
SOURCES = [ROOT / "indra/newview/vkgfx/llcleanrenderervk.cpp", ROOT / "indra/newview/vkgfx/llcleansnapmattevk.cpp"]

REGISTER = re.compile(r"registerDisplayPipeline\(\s*(\w+)\s*,\s*&?(\w+)\s*,(.*?)\);", re.S)


def args(call_tail: str):
    # name, vert spv, vert bytes, frag spv, frag bytes, linearOnly, linearSpecId, formatTarget[, uiLayer]
    return [a.strip() for a in call_tail.split(",")]


checked = 0
for path in SOURCES:
    source = path.read_text()
    for m in REGISTER.finditer(source):
        desc, dest, tail = m.group(1), m.group(2), m.group(3)
        if desc == "desc" and dest == "active":
            continue   # registerDisplayOutputPipeline's pass-through
        rest = source[m.end():]
        build = re.search(r"(createGraphicsPipelineInto\(\s*dev\s*,\s*" + desc + r"\s*,\s*&?" + dest + r"\b"
                          r"|\b" + dest + r"\s*=\s*createGraphicsPipeline\(\s*dev\s*,\s*" + desc + r"\s*\)"
                          r"|postInto\(\s*dev\s*,\s*" + desc + r"\s*,\s*&?" + dest + r"\b"
                          r"|registerDisplayPipeline\(\s*\w+\s*,\s*&?" + dest + r"\b)", rest)
        line = source.count("\n", 0, m.start()) + 1
        linear_only = len(args(tail)) > 5 and args(tail)[5] == "true"
        if linear_only and (not build or build.group(0).startswith("registerDisplayPipeline")):
            continue   # an SDR bank's HDR-only recipe: built by the first switch, not here
        assert build, f"{path.name}:{line}: no build of {dest} after its registration"
        if build.group(0).startswith("registerDisplayPipeline"):
            continue   # registered again before any build
        between = rest[:build.start()]
        changed = set(re.findall(r"\b" + desc + r"\.(\w+)\s*=(?!=)", between))
        spec_id = args(tail)[6] if len(args(tail)) > 6 else "UINT32_MAX"
        if spec_id != "UINT32_MAX":
            changed.discard("fragSpec")
        assert not changed, f"{path.name}:{line}: {dest} is built with {sorted(changed)} set after its recipe"
        checked += 1

assert checked >= 18, checked
renderer = SOURCES[0].read_text()
# Stereo builds a one-view tonemap twin; it follows the output mode like the layered one.
assert re.search(r"registerDisplayPipeline\(\s*td\s*,\s*&mTonemapPipe1\b", renderer), "mTonemapPipe1 recipe"
print(f"display recipe state checks passed ({checked} recipes)")
