"""Regression guard: alpha-blend layers never borrow the opaque surface's lighting records again.

Two fixes once lit a blended layer (glass, a decal, makeup, hair) with light recorded for the OPAQUE surface
behind it instead of its own:
  * #1440's LAMP RECORD (VISR_RAD): BASE / COMBINE stored the surface's demodulated shadowed lamp light (RG32UI,
    set 3 binding 29 = draw set 0 binding 65, + the light grid copy at draw set 0 binding 66); a matched layer
    skipped the deferred walk's lights and added the record (mDsLamp, kPushVisReuseRad, visReuseLamp).
  * #87's forward GI OCCLUSION RECORD (VISR_GI_OCC): BASE / COMBINE stored the surface's GI occlusion (R32UI,
    set 3 binding 31 = draw set 0 binding 68); glass with no radiance-cache cell took it (mDsGiOcc, kPushGiOcc,
    giFwdScreenOcc, giOccPack).
Both lost the A/B against the layers lighting themselves at their own point (RenderVKBlendHitLighting) and were
deleted with every writer, reader, binding, image, push bit and barrier. This check fails if any of their
symbols or bindings come back. (The 4 mm same-point sun / projector reuse -- visReuseSun / visReuseProj,
RenderVKDeferVisReuse -- is the same lookup at the same point, not borrowing, and stays.)

It reads the production source with comments stripped: every viewer shader in indra/vkclean/shaders and the
Vulkan host sources in indra/newview/vkgfx. The tree before the deletion fails it.
"""
import glob
import os
import re

NAME = "blend-borrowing"
STAGE = "source"
DESCRIPTION = "alpha-blend layers: the deleted lamp / GI occlusion records (borrowed from the surface behind) stay gone"
GUARDS = ("RenderVKBlendHitLighting A/B: #1440's lamp record (VISR_RAD, set 3 b29 / draw set 0 b65 / b66) and #87's "
          "forward GI occlusion record (kPushGiOcc, set 3 b31 / draw set 0 b68) were deleted; a blend layer is lit "
          "at its own point, never with the opaque surface's records.")

# The deleted mechanisms' symbols (shader and host).
SYMBOLS = (
    # #1440 the lamp record
    "VISR_RAD", "kPushVisReuseRad", "mDsLamp", "mDsLampWant", "mDsLampFull", "kDsBindingLamp", "uLampOut",
    "uVisReuseLamp", "visReuseLamp", "visReuseRadOn", "visReuseRad", "visReuseRsStoch", "visReuseVisLight",
    "visReuseLightTaken", "g_visr_rad", "g_visr_rs_in", "deferSplitVisLamp",
    # #87 the forward GI occlusion record
    "VISR_GI_OCC", "kPushGiOcc", "mDsGiOcc", "mDsGiOccFull", "mDsGiOccWant", "mDsGiOccRetryAt", "mDsGiOccLastWant",
    "kDsBindingGiOcc", "giOccFwdNow", "uGiOccOut", "uGiOccFwd", "giFwdScreenOcc", "giOccPack", "giOccUnpack",
    "giOccStore", "g_gi_occ_rec", "GI_FWD_OCC_GAP0", "GI_FWD_OCC_GAP1", "deferGiOccRecord",
    # the A/B's gates on them
    "mBlendHitLive", "mBlendHitLastLive",
)
_SYM = re.compile(r"\b(" + "|".join(SYMBOLS) + r")\b")
# Shaders that are not viewer kernels on the world layouts (their own sets).
_NOT_WORLD = {"clean_sea_probe.comp"}
_DRAW0_RETIRED = (65, 66, 68)
_SET3_RETIRED = (29, 31)


def _strip_comments(src):
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    return re.sub(r"//[^\n]*", "", src)


def run(ctx, result):
    shaders = sorted(glob.glob(ctx.path("indra/vkclean/shaders", "*")))
    host = sorted(glob.glob(ctx.path("indra/newview/vkgfx", "*.cpp")) + glob.glob(ctx.path("indra/newview/vkgfx", "*.h")))
    if not shaders or not host:
        result.fail("no shaders / Vulkan host sources found: the check covers nothing")
        return
    failures = []
    for path in shaders + host:
        if not os.path.isfile(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            src = _strip_comments(f.read())
        rel = ctx.relative(path)
        hits = sorted(set(_SYM.findall(src)))
        if hits:
            failures.append("%s: the deleted borrowing's symbols are back: %s" % (rel, ", ".join(hits)))
        if path in shaders and os.path.basename(path) not in _NOT_WORLD:
            for b in _DRAW0_RETIRED:
                if re.search(r"layout\s*\([^)]*\bset\s*=\s*0\s*,\s*binding\s*=\s*%d\b" % b, src):
                    failures.append("%s: declares draw set 0 binding %d (retired: a deleted borrowed record)" % (rel, b))
            if re.search(r"#\s*define\s+REGIR_BINDING\s+66\b", src):
                failures.append("%s: the light grid at draw set 0 binding 66 (the lamp record's reader, retired)" % rel)
            for b in _SET3_RETIRED:
                if re.search(r"layout\s*\([^)]*\bset\s*=\s*3\s*,\s*binding\s*=\s*%d\b" % b, src) \
                        and "clean_deferred_light" in os.path.basename(path):
                    failures.append("%s: declares set 3 binding %d (retired: a deleted borrowed record)" % (rel, b))
    # The host's layouts keep the slots retired (no descriptor written to them, the draw set 0 layout skips them).
    cpp_path = ctx.path("indra/newview/vkgfx/llcleanrenderervk.cpp")
    with open(cpp_path, encoding="utf-8", errors="replace") as f:
        cpp = _strip_comments(f.read())
    for b in _DRAW0_RETIRED:
        if re.search(r"db0\s*\[\s*%d\s*\]\s*\.\s*binding\s*=" % b, cpp):
            failures.append("llcleanrenderervk.cpp: draw set 0 binding %d is declared again (retired)" % b)
        if re.search(r"dstBinding\s*=\s*%du?\s*;" % b, cpp):
            failures.append("llcleanrenderervk.cpp: a descriptor write to binding %d (draw set 0's retired slot?)" % b)
    if not re.search(r"db0Retired\s*=\s*\[\]\s*\(\s*uint32_t\s+b\s*\)\s*\{\s*return\s+b\s*==\s*65u\s*\|\|\s*b\s*==\s*66u"
                     r"\s*\|\|\s*b\s*==\s*68u\s*;", cpp):
        failures.append("llcleanrenderervk.cpp: the draw set 0 layout no longer skips the retired slots 65 / 66 / 68")
    if not re.search(r"if\s*\(\s*i\s*==\s*kDsBindingRetired\s*\)\s*continue\s*;", cpp):
        failures.append("llcleanrenderervk.cpp: the set 3 layout (initDeferSplit) no longer skips the retired slot 29")
    if failures:
        for msg in failures:
            result.fail(msg)
        return
    result.summary = "%d shaders + %d host files: no lamp / GI occlusion record, slots 65 / 66 / 68 and set 3 29 / 31 retired" % (
        len(shaders), len(host))
