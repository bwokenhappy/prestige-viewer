"""Split transparency (RenderVKBlendSplit): setting 0 untouched, the blend shader free of the forward lighting.

RenderVKBlendSplit lights every static alpha-blend layer from a translucency lighting volume and the front layer once
per pixel (llcleanblendsplitvk.h, clean_bs_common.glsl). Its point is a SMALL blend shader -- the compile-size win
(the forward lit blend FS is ~0.73M / ~0.98M SPIR-V words and OOMs some AMD drivers, #1678) and a fixed lighting
cost whatever the layer depth. This check:
  1. compiles the split blend variants exactly as regen_world_spv.py does (glslc -O) and reads the SPIR-V: no
     RayQuery capability / ray-query instruction, and none of the forward lighting's bindings survive -- the local
     light records / cluster lists (draw set 0 bindings 3-5), the probes (10-14), the RT forward reflection (48), the
     GI state (53);
  2. keeps setting 0 the old pipelines: every pre-existing variant of the files the split touched preprocesses
     without any of its code (glslc -E: no split identifier), and the split variants are appended after every
     existing entry (RT_DIAG_K is the table index: an insertion renumbers every RT kernel);
  3. the host records nothing with the setting off (bsFrame / the D branch gated on mBsNow) and the settings are
     live and unsaved, the master switch default 0.
"""
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import xml.etree.ElementTree as ET
from concurrent.futures import ThreadPoolExecutor

NAME = "blend-split"
STAGE = "source"
DESCRIPTION = "split transparency: no ray query / light loop / probe walk / GI read in its blend FS; setting 0 untouched"
GUARDS = ("The split blend shader growing the forward lighting back (its compile size and per-layer cost), an old "
          "pipeline changed by the split's code, a renumbered RT kernel table, a saved A/B switch.")

BLEND_VARIANTS = ("kWorldFragBsBlendSpv", "kWorldFragBsBlendMbSpv")
SPLIT_VARIANTS = BLEND_VARIANTS + ("kWorldFragBsFrontSpv", "kWorldBsVolumeSpv", "kWorldBsVolumeRtSpv", "kWorldBsLightSpv",
                                   "kWorldBsLightRtSpv", "kWorldBsDebugSpv")
# (set, binding) of the forward lighting's data: local lights / clusters, probes, the RT forward reflection, GI state
DENY = {(0, 3): "local light records", (0, 4): "light cluster counts", (0, 5): "light cluster lists",
        (0, 10): "probe records", (0, 11): "probe cluster counts", (0, 12): "probe cluster lists",
        (0, 13): "probe radiance cubes", (0, 14): "probe irradiance cubes", (0, 48): "RT forward reflection",
        (0, 53): "GI state"}
# identifiers only the split's code has (a pre-existing variant's preprocessed text must name none of them)
MARKERS = ("BsParamsBlock", "bsShade(", "bsFrontMain(", "uBsKey", "bsVolumeLight(", "bsTailApply(")
SETTINGS = ("RenderVKBlendSplit", "RenderVKBlendSplitGridX", "RenderVKBlendSplitGridY", "RenderVKBlendSplitGridZ",
            "RenderVKBlendSplitNear", "RenderVKBlendSplitFar", "RenderVKBlendSplitFront", "RenderVKBlendSplitFrontAlpha",
            "RenderVKBlendSplitFrontReflect", "RenderVKBlendSplitTemporal", "RenderVKBlendSplitProbes",
            "RenderVKBlendSplitDebug", "RenderVKBlendSplitDebugSlice")


def _spirv_problems(words):
    """Ray-query capability / instructions and denied bindings in a SPIR-V module."""
    out = []
    if len(words) < 5 or words[0] != 0x07230203:
        return ["not a SPIR-V module"]
    i = 5
    sets, binds = {}, {}
    while i < len(words):
        wc, op = words[i] >> 16, words[i] & 0xFFFF
        if wc == 0:
            return out + ["malformed instruction stream"]
        if op == 17 and words[i + 1] == 4472:   # OpCapability RayQueryKHR
            out.append("RayQueryKHR capability")
        if 4472 <= op <= 4479 or 6016 <= op <= 6031:   # OpRayQuery*
            out.append("a ray-query instruction (opcode %d)" % op)
        if op == 71 and wc >= 4:   # OpDecorate target decoration literal
            if words[i + 2] == 34: sets[words[i + 1]] = words[i + 3]    # DescriptorSet
            if words[i + 2] == 33: binds[words[i + 1]] = words[i + 3]   # Binding
        i += wc
    for v, b in binds.items():
        key = (sets.get(v, -1), b)
        if key in DENY:
            out.append("set %d binding %d (%s)" % (key[0], key[1], DENY[key]))
    return sorted(set(out))


def run(ctx, result):
    bad = []

    def read(rel):
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()

    # 3. settings: live, unsaved; the switch default 0
    root = ET.fromstring(read("indra/newview/app_settings/settings.xml")).find("map")
    children = list(root)
    settings = {children[i].text: children[i + 1] for i in range(0, len(children), 2)}
    for key in SETTINGS:
        item = settings.get(key)
        if item is None:
            bad.append(key + " missing"); continue
        f = {list(item)[i].text: list(item)[i + 1].text for i in range(0, len(list(item)), 2)}
        if f.get("Persist") != "0":
            bad.append(key + " must be Persist 0 (an A/B never persists)")
        if key == "RenderVKBlendSplit" and f.get("Value") != "0":
            bad.append("RenderVKBlendSplit must default to 0 (today's sorted path)")
    cpp = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    if "auto bsFrame = [&]()\n    {\n        if (!mBsNow) return;" not in cpp:
        bad.append("bsFrame must record nothing when the split is off (if (!mBsNow) return;)")
    if "        else if (mBsNow && mBlendSplit->blendPipe())\n" not in cpp:
        bad.append("the split blend draw (stage D) must be gated on mBsNow")
    if 'else if (!mOitForce) drawListBlend(mAlphaPipe, stat, stat_ctr, post ? "g_p2_post_static" : "g_p2_pre_static");' not in cpp:
        bad.append("the sorted static draw (setting 0) must stay as it was")
    if not re.search(r"\n    mBsNow = false;\n    mBsFrontNow = false;\n", cpp):
        bad.append("mBsNow / mBsFrontNow must be reset every recording before the split's gate")

    # 3b. every pipeline goes through the create-info guard, and every desc copied from the recorded LIT_ALPHA state
    #     (which the renderer stores WITHOUT shader modules) gets its own vertex shader -- the first in-world run
    #     crashed in the driver on a null VS (2026-11-22)
    mod = read("indra/newview/vkgfx/llcleanblendsplitvk.cpp")
    if mod.count("createGraphicsPipelineInto(") != 1 or "void bsCreate(" not in mod or "bsDescProblem(d)" not in mod:
        bad.append("llcleanblendsplitvk.cpp: every pipeline must be created through bsCreate (the create-info guard)")
    for m in re.finditer(r"GraphicsPipelineDesc d = b\.alphaDesc;\n(.*?)bsCreate\(", mod, re.S):
        if not re.search(r"\bd\.vert = vs;", m.group(1)):
            bad.append("a pipeline built from the recorded LIT_ALPHA desc (no modules) does not set its own vertex shader")
    if not re.search(r'if \(!d\.vert && !d\.mesh\) return "no vertex shader module";', mod):
        bad.append("the create-info guard must refuse a desc without a vertex shader")
    if "blending on an integer" not in mod:
        bad.append("the create-info guard must refuse blending on an integer attachment")

    # 2a. the split variants appended after every existing entry
    vk = ctx.path("indra/vkclean")
    sys.path.insert(0, vk)
    try:
        import regen_world_spv as regen
    finally:
        sys.path.pop(0)
    names = [e[2] for e in regen.SHADERS]
    idx = {n: i for i, n in enumerate(names)}
    missing = [n for n in SPLIT_VARIANTS if n not in idx]
    if missing:
        bad.append("regen_world_spv.py builds no " + ", ".join(missing))
    else:
        first = min(idx[n] for n in SPLIT_VARIANTS)
        if any(n not in SPLIT_VARIANTS for n in names[first:]):
            bad.append("the split variants must be the LAST entries of regen_world_spv.py (RT_DIAG_K renumbering)")
        for n in BLEND_VARIANTS:
            e = regen.SHADERS[idx[n]]
            if "-DCLEAN_PROJ_SHADOW_RAYQUERY" in (e[3] if len(e) > 3 else ()):
                bad.append(n + " must not be built with ray queries")

    glslc = os.environ.get("GLSLC") or shutil.which("glslc")
    if not glslc:
        for b in bad: result.fail(b)
        if not bad:
            result.warn("no glslc: the SPIR-V / preprocessed checks not run (wiring checked)")
            result.summary = "wiring only"
        return

    # 1. the blend variants' SPIR-V
    def compile_one(n):
        path, stage, flags = regen.variant(regen.SHADERS[idx[n]])
        with tempfile.TemporaryDirectory() as td:
            out = os.path.join(td, "o.spv")
            r = subprocess.run([glslc, "--target-env=vulkan1.3", "-O", "-fshader-stage=" + stage, *flags, path, "-o", out],
                               capture_output=True, text=True)
            if r.returncode:
                return n, None, r.stderr[:2000]
            with open(out, "rb") as f:
                data = f.read()
        return n, list(struct.unpack("<%dI" % (len(data) // 4), data)), None

    # 2b. every pre-existing variant of a file the split touched: none of its code
    touched = ("clean_draw.frag", "clean_oit_resolve.frag", "clean_water.frag")
    old = [e for e in regen.SHADERS if e[0] in touched and e[2] not in SPLIT_VARIANTS]

    def pre(e):
        path, stage, flags = regen.variant(e)
        r = subprocess.run([glslc, "-E", "-fshader-stage=" + stage, "--target-env=vulkan1.3",
                            *[f for f in flags if f.startswith(("-D", "-I"))], path], capture_output=True, text=True)
        if r.returncode:
            return e[2], ["glslc -E failed: " + r.stderr[:500]]
        return e[2], [m for m in MARKERS if m in r.stdout]

    with ThreadPoolExecutor(max_workers=max(1, os.cpu_count() or 1)) as ex:
        comp = list(ex.map(compile_one, [n for n in BLEND_VARIANTS if n in idx]))
        pres = list(ex.map(pre, old))
    sizes = []
    for n, words, err in comp:
        if words is None:
            bad.append("%s does not compile:\n%s" % (n, err)); continue
        sizes.append("%s %dk words" % (n, len(words) // 1000))
        for p in _spirv_problems(words):
            bad.append("%s: %s -- the split blend shader must not carry the forward lighting" % (n, p))
    for n, hits in pres:
        if hits:
            bad.append("%s (a pre-existing variant) contains the split's code: %s" % (n, ", ".join(hits)))
    for b in bad: result.fail(b)
    if not bad:
        result.summary = "blend FS free of the forward lighting (%s); %d old variants untouched; settings live / unsaved" % (
            ", ".join(sizes), len(old))
