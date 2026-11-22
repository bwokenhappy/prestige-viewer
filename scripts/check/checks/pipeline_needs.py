"""COMPILE ONLY WHAT IS NEEDED (indra/newview/vkgfx/llcleanpipeneeds.h).

Compiles scripts/check/test_pipeline_needs.cpp against the real header and runs it: with every
ray-traced setting off (or no RT foundation) no ray-traced pipeline family is requested; each RT
consumer needs the kernels and only ray-traced sun / projector shadows need the ray-query lit
variant; WBOIT needs exactly the selected mode; speculative spec sets are off by default.
Source guards: the renderer starts the deferred RT groups only through the needs mask and tags
them (and the WBOIT / spec-prewarm jobs) with their family, so vk_pipeline's refusal guard sees
every optional job; the RenderVKSpecPrewarm default is 0.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "pipeline-needs"
STAGE = "source"
DESCRIPTION = "no ray-traced / unselected WBOIT / speculative pipeline compiles while its settings are off"
GUARDS = ("RT and ray-query families, both WBOIT modes and speculative spec sets compiled whatever the settings "
          "(2026-11-19: ~150 s of summed compile in unselected WBOIT mode 1 alone; ~750 s of spec prewarm per session).")

REN = "indra/newview/vkgfx/llcleanrenderervk.cpp"
OIT = "indra/newview/vkgfx/llcleanalphaoitvk.cpp"
SPEC = "indra/newview/vkgfx/llcleanspecvk.cpp"
SETTINGS = "indra/newview/app_settings/settings.xml"


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


def _source_guard(ctx, result):
    ren = _strip_comments(open(ctx.path(REN), encoding="utf-8").read())
    oit = _strip_comments(open(ctx.path(OIT), encoding="utf-8").read())
    spec = _strip_comments(open(ctx.path(SPEC), encoding="utf-8").read())
    grp = _function(ren, "void LLCleanRendererVK::pollRtGroup(bool wait, bool lit)")
    if grp is None:
        result.fail("LLCleanRendererVK::pollRtGroup not found; update this check")
    else:
        need = grp.find("if (!(mPipeNeeds & fam)) return;")
        start = grp.find("pipelineJobStart(job)")
        if need < 0 or start < 0 or need > start:
            result.fail("pollRtGroup no longer tests the needs mask before starting a deferred RT group")
    for fam in ("FAM_RT_KERNELS", "FAM_RT_LIT"):
        if not re.search(r"pipelineJobSetFamily\(mRt\w+Job,\s*llpipeneeds::%s\)" % fam, ren):
            result.fail("the deferred RT group is no longer tagged with llpipeneeds::%s" % fam)
    if "updatePipelineNeeds();" not in (_function(ren, "void LLCleanRendererVK::pollPipelines(bool wait)") or ""):
        result.fail("pollPipelines no longer recomputes the pipeline needs every frame")
    if "vkSetPipelineFamiliesAllowed(mPipeNeeds)" not in (_function(ren, "void LLCleanRendererVK::updatePipelineNeeds()") or ""):
        result.fail("updatePipelineNeeds no longer hands the mask to vk_pipeline's refusal guard")
    if not re.search(r"o\.family\s*=\s*\(bm == 1u \? llpipeneeds::FAM_WBOIT_M1 : llpipeneeds::FAM_WBOIT_M2\)", oit):
        result.fail("the WBOIT job is no longer tagged with its mode's family")
    if "for (uint32_t m = 1; m <= 2u; ++m)" in oit:
        result.fail("WBOIT builds both modes again (only the selected mode compiles)")
    if spec.count("llpipeneeds::FAM_SPEC_PREWARM") < 2:
        result.fail("the spec prewarm jobs are no longer tagged FAM_SPEC_PREWARM")
    xml = open(ctx.path(SETTINGS), encoding="utf-8").read()
    m = re.search(r"<key>RenderVKSpecPrewarm</key>.*?<key>Value</key>\s*<integer>(\d+)</integer>", xml, flags=re.S)
    if not m or m.group(1) != "0":
        result.fail("RenderVKSpecPrewarm's default is not 0 (speculative sets compile what the settings do not select)")


def run(ctx, result):
    _source_guard(ctx, result)
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: pipeline-needs fixtures not run")
        result.summary = "source guard only (no compiler)"
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "pipeline_needs")
        proc = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror",
                               "-I" + ctx.path("indra/newview/vkgfx"),
                               ctx.path("scripts/check/test_pipeline_needs.cpp"), "-o", exe],
                              capture_output=True, text=True)
        if proc.returncode:
            result.fail(proc.stdout + proc.stderr)
            return
        proc = subprocess.run([exe], capture_output=True, text=True)
        if proc.returncode:
            result.fail(proc.stdout + proc.stderr)
            return
    result.summary = "RT off -> no RT family; per-consumer RT; one WBOIT mode; no speculative sets; source tags"
