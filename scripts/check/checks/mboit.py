"""Moment-based OIT (RenderVKAlphaMBOIT): avatars never routed to it, mode 0 untouched, live unsaved settings."""
import re
import xml.etree.ElementTree as ET
NAME = "mboit"
STAGE = "source"
DESCRIPTION = "MBOIT: avatar faces collapse in its VS, mode 0 SPIR-V order kept, the sorted path when off, settings Persist 0"
GUARDS = "Avatars composited out of order through MBOIT; an inserted variant renumbering (changing) every RT kernel; a saved A/B switch."

def run(ctx, result):
    def read(p):
        with open(ctx.path(p), encoding="utf-8") as f: return f.read()
    vert = read("indra/vkclean/shaders/clean_draw.vert")
    frag = read("indra/vkclean/shaders/clean_draw.frag")
    common = read("indra/vkclean/shaders/clean_draw_common.glsl")
    regen = read("indra/vkclean/regen_world_spv.py")
    cpp = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    mod = read("indra/newview/vkgfx/llcleanalphamboitvk.cpp")
    bad = []
    # 1. Avatars never reach the moments / shade passes: their VS collapses every avatar face.
    m = re.search(r"#ifdef CLEAN_VS_MB_WORLD\s*\n\s*if \(avatarDepthFace\(inst\.flags\) \|\| \(inst\.flags & 1u\) != 0u\)\s*\n\s*\{\s*\n\s*gl_Position = vec4\(0\.0, 0\.0, -2\.0, 1\.0\);", vert)
    if not m:
        bad.append("clean_draw.vert CLEAN_VS_MB_WORLD must collapse every avatar face (avatarDepthFace or rigged)")
    if "kWorldVertMbWorldSpv" not in mod or mod.count("d.vert = vs;") < 2:
        bad.append("the moments and shade pipelines must use the world-only VS (kWorldVertMbWorldSpv)")
    if "pushPad0(padStatic | kPushAvDepthPre);" not in cpp:
        bad.append("the sorted static pass beside MBOIT must draw the avatar faces only (kPushAvDepthPre)")
    # 2. Mode 0: nothing recorded, the sorted static draw unchanged; MB code only in MB variants.
    if "if (!mMbNow) return;" not in cpp:
        bad.append("mbSide must record nothing when MBOIT is off")
    if 'else if (!mOitForce) drawListBlend(mAlphaPipe, stat, stat_ctr, post ? "g_p2_post_static" : "g_p2_pre_static");' not in cpp:
        bad.append("the sorted static draw (mode 0) must stay as it was")
    if "#if defined(CLEAN_MB_SHADE) && defined(CLEAN_MB_TIER_CHEAP)" not in common:
        bad.append("the cheap-tier visibility override must exist only in the MBOIT cheap shade module")
    # 2b. Never block the frame on MBOIT compiles: no wait on its jobs outside shutdown, -O0 never used.
    if "mJob.wait()" in mod or "-O0" in regen[regen.index("MOMENT-BASED OIT (RenderVKAlphaMBOIT)"):]:
        bad.append("MBOIT must not wait on its compile jobs in the frame loop, nor ship unoptimised SPIR-V")
    if "if (mAlphaMB->pipelinesPending()) mAlphaMB->cancelJobs();" not in cpp:
        bad.append("switching MBOIT off must park a module whose compiles still run (no destroy / wait)")
    bis = frag[frag.index("// Only the sorted"):frag.index('#include "clean_draw_common.glsl"')]
    if "!defined(CLEAN_MB_SHADE)" not in bis or "!defined(CLEAN_MB_MOMENTS)" not in bis:
        bad.append("the blend-cost bisect must stay out of the MBOIT variants")
    # 3. The variants are appended after every existing entry (RT_DIAG_K = table index: an insertion renumbers).
    try:
        if regen.index('"kWorldRtShadowMaskVrSpv"') > regen.index('"kWorldVertMbWorldSpv"'):
            bad.append("the MBOIT variants must be appended after the VR variants (RT_DIAG_K renumbering)")
    except ValueError:
        bad.append("regen_world_spv.py: MBOIT / VR variant entries missing")
    # 4. Settings: live, unsaved; default off.
    root = ET.fromstring(read("indra/newview/app_settings/settings.xml")).find("map")
    children = list(root)
    settings = {children[i].text: children[i + 1] for i in range(0, len(children), 2)}
    for key in ("RenderVKAlphaMBOIT", "RenderVKMBOITHalfRes", "RenderVKMBOITCutoff", "RenderVKMBOITFullWeight", "RenderVKMBOITDebug"):
        item = settings.get(key)
        if item is None:
            bad.append(key + " missing"); continue
        f = {list(item)[i].text: list(item)[i + 1].text for i in range(0, len(list(item)), 2)}
        if f.get("Persist") != "0":
            bad.append(key + " must be Persist 0")
        if key == "RenderVKAlphaMBOIT" and f.get("Value") != "0":
            bad.append("RenderVKAlphaMBOIT must default to 0 (the sorted path)")
    for b in bad: result.fail(b)
    if not bad: result.summary = "avatar faces collapse in the MBOIT VS, sorted path untouched when off, variants appended, settings live/unsaved"
