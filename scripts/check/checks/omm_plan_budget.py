"""Source guard: opacity-micromap planning walks a bounded number of triangles per frame
(2026-11-08 micro-stutters: vk_rtBuild / vk_rtP_record 9-12 ms with vk_rtOmmPlan 5-10 ms
whenever rt_ommBuilt > 0).

A new OMM entry walks every source triangle of its face on the main thread (UV decode,
subdivision level, the fit loop, then the bake records). kOmmMaxBuilds bounds entries and
kOmmMaxMicro the micro-triangles, but nothing bounded the triangles walked: 16 big faces
in one frame cost 5-10 ms.
  PRIM-BUDGET  ommPlanFrame tests the frame's kOmmMaxPlanPrims budget against the face's
               triangle count before the per-triangle walk (the first walk of a frame
               always goes), and counts what it walks.
"""
import re

NAME = "omm-plan-budget"
STAGE = "source"
DESCRIPTION = "OMM planning walks at most kOmmMaxPlanPrims source triangles per frame"
GUARDS = "5-10 ms vk_rtOmmPlan frames planning many large micromap entries at once (2026-11-08)."

OMM = "indra/newview/vkgfx/llcleanrtomm.cpp"


def _strip_comments(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _function(text, signature):
    at = text.find(signature)
    if at < 0:
        return None
    start = text.index("{", at)
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return None


def run(ctx, result):
    src = _strip_comments(open(ctx.path(OMM), encoding="utf-8").read())
    bad = []
    if not re.search(r"constexpr\s+uint32_t\s+kOmmMaxPlanPrims\s*=", src):
        bad.append("PRIM-BUDGET: kOmmMaxPlanPrims is gone")
    fn = _function(src, "VkDeviceSize LLCleanRTVK::ommPlanFrame(")
    if not fn:
        bad.append("LLCleanRTVK::ommPlanFrame not found; update this check")
    else:
        walk = fn.find("for (uint32_t p = 0; p < b.prims; ++p)")
        gate = re.search(r"plan_prims\s*\+\s*si->index_count\s*/\s*3\s*>\s*kOmmMaxPlanPrims", fn)
        count = re.search(r"plan_prims\s*\+=\s*si->index_count\s*/\s*3", fn)
        if walk < 0:
            bad.append("the per-triangle walk of ommPlanFrame changed; update this check")
        else:
            if not gate or gate.start() > walk:
                bad.append("PRIM-BUDGET: ommPlanFrame walks a new entry's triangles without testing kOmmMaxPlanPrims first")
            if not count or count.start() > walk:
                bad.append("PRIM-BUDGET: ommPlanFrame does not count the triangles it walks")
    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "new OMM entries walk at most kOmmMaxPlanPrims triangles per frame"
