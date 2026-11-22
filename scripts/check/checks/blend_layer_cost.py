"""Sorted transparency uses a layer-count-independent shadow budget; diagnostic is opt-in."""
import re
import xml.etree.ElementTree as ET
NAME = "blend-layer-cost"
STAGE = "source"
DESCRIPTION = "shared sorted blend visibility, no-ray pipeline selection, OIT mean has no hit-light walk"
GUARDS = "Ray queries repeated for thousands of sorted layers; OIT mean lit inside the canopy; cost probe changing OIT routing."

def run(ctx, result):
    def read(p):
        with open(ctx.path(p), encoding="utf-8") as f: return f.read()
    cpp = read("indra/newview/vkgfx/llcleanrenderervk.cpp")
    draw = read("indra/vkclean/shaders/clean_draw.frag")
    common = read("indra/vkclean/shaders/clean_draw_common.glsl")
    surface = read("indra/vkclean/shaders/clean_surface_light.glsl")
    oit = read("indra/vkclean/shaders/clean_oit_resolve.frag")
    bad = []
    root = ET.fromstring(read("indra/newview/app_settings/settings.xml")).find("map")
    children = list(root)
    settings = {children[i].text: children[i+1] for i in range(0, len(children), 2)}
    item = settings.get("RenderVKBlendCostBisect")
    fields = {} if item is None else {list(item)[i].text: list(item)[i+1].text for i in range(0, len(list(item)), 2)}
    if fields.get("Persist") != "0" or fields.get("Value") != "0" or fields.get("Type") != "U32":
        bad.append("cost mask must be U32 / Persist0 / default0")
    if not re.search(r"const int variant = \(blendFrontLightOn\(\) && \(i == LIT_ALPHA \|\| i == LIT_RIG_ALPHA\)\) \? 0 : v", cpp):
        bad.append("shared main sorted slots must use the no-ray module; captures/opaque slots retain their variant")
    # Do not add the probe to litDebugWanted: that disables WBOIT and invalidates A/B.
    debug = cpp[cpp.index("bool LLCleanRendererVK::litDebugWanted()"):cpp.index("void LLCleanRendererVK::ensureLitDebug(")]
    if "RenderVKBlendCostBisect" in debug or "blendCostBisectMask" in debug:
        bad.append("cost mask must not change global debug/OIT routing")
    if "if (!debugAll && !sortedBlend) continue;" not in cpp:
        bad.append("cost-only debug selection must touch sorted slots only")
    if "| (blendCostBisectMask() << 16u)" not in cpp or "(blendFrontLightOn() ? 0x40000000u : 0u)" not in cpp:
        bad.append("separate live diagnostic and policy fields must be published")
    if "return kDebugViews &&" not in common or "#ifdef CLEAN_BLEND_COST_BISECT" not in common:
        bad.append("default production pipelines must fold diagnostic gates away")
    for excluded in ("!defined(CLEAN_OIT_ACCUM)", "!defined(CLEAN_OIT_GBUF)", "!defined(CLEAN_CAPTURE)"):
        if excluded not in draw[draw.index("// Only the sorted"):draw.index('#include "clean_draw_common.glsl"')]:
            bad.append("cost policy must not change OIT/capture surfaces: " + excluded)
    if "return !blendSharedVisibility() && (linfo.fwd.w & BLEND_HIT_ON)" not in common:
        bad.append("shared layers must not fall back to per-layer hit shadow/occlusion rays")
    policy = common[common.index("bool blendSharedVisibility()"):common.index("// ---- PBR")]
    if "glow" in re.sub(r"//[^\n]*", "", policy).lower() or "g_blend_cheap" in policy or "0x40000000u" not in policy:
        bad.append("shared policy must not depend on glow or hit-light activation")
    if "if (blendSharedFront(P, sharedVis)) return vec3(1.0);" not in common or "local *= sharedVis.g; glare *= sharedVis.g;" not in surface:
        bad.append("front local visibility ratio must be applied once, not per-light and again to the sum")
    if "vis = v.rgb * v.rgb * (v.a * v.a);" not in common:
        bad.append("shared split sun must decode both tint and scalar visibility (avoid light leaks)")
    if re.search(r"^\s*#define CLEAN_BLEND_HIT\b", oit, re.M) or "if (blendHitOn()) g_oit_near = false;" in oit:
        bad.append("weighted-mean OIT surface must never use per-layer hit lighting")
    if "sunShadowT(g_oit_P, g_oit_Ng, tint)" not in oit:
        bad.append("OIT resolve still needs its ordinary sun visibility; do not simply delete shadowing")
    for b in bad: result.fail(b)
    if not bad: result.summary = "no-ray sorted slots, independent policy, diagnostic gating/routing, single local ratio, sun tint+visibility and OIT contract pass"
