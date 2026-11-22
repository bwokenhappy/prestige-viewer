"""Regression guard for adaptive two-phase occlusion (RenderVKOcclusion2Phase 2;
indra/newview/vkgfx/llcleanoccl2adapt.h).

Compiles scripts/check/test_occl2_adapt.cpp with the host C++ compiler and runs it: a steady open
view (low culled share) stands down after the window and then only runs short, backed-off probes;
an indoor scene (high share) never stands down; per-frame noise around the threshold does not
stand down; a scene that keeps flipping produces a bounded number of transitions; a camera cut
while stood down probes at once and a probe into a building resumes; a probe never decides
before its harvested-sample count (the stats ring lags 3 frames).
Also a source guard: the renderer's two-phase decision goes through the Governor
(occlusion2AdaptFrame), feeds it from the stats harvest, seeds on a probe's first frame, keeps
the single-phase cull from taking over while stood down, and the setting is the persistent
U32 mode (0 off, 1 on, 2 adaptive; default 2) bound in the Advanced Graphics floater.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "occl2-adapt"
STAGE = "source"
DESCRIPTION = "adaptive two-phase occlusion stands down in open views, stays on indoors, does not flap"
GUARDS = ("two-phase occlusion costs more than it culls in open views (~3 ms at 4K on a GTX 1080; "
          "RTX 5090 house off-sim 84 -> 82-83 FPS) but wins indoors (55 -> 71 FPS); the adaptive "
          "default must stand down there without flapping or seeding phase 1 empty.")

REN = "indra/newview/vkgfx/llcleanrenderervk.cpp"
SETTINGS = "indra/newview/app_settings/settings.xml"
XUI = "indra/newview/skins/default/xui/en/floater_preferences_graphics_advanced.xml"


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
    fn = _function(ren, "bool LLCleanRendererVK::occlusion2AdaptFrame()")
    if fn is None:
        result.fail("LLCleanRendererVK::occlusion2AdaptFrame not found; update this check")
    else:
        for call, why in (("mOccl2Adapt.frame(", "the Governor decides each frame"),
                          ("mOccl2Cut = true", "a probe's first frame must seed (phase 1 never empty)"),
                          ("mOccl2StatsValid", "stood-down frames invalidate the pending stats slots")):
            if call not in fn:
                result.fail("occlusion2AdaptFrame no longer contains %s (%s)" % (call, why))
    split = _function(ren, "void LLCleanRendererVK::recordOcclusion2Split(VkCommandBuffer cmd)")
    if split is None or "mOccl2Adapt.sample(" not in split:
        result.fail("recordOcclusion2Split no longer feeds the harvested stats to mOccl2Adapt.sample")
    seed = _function(ren, "void LLCleanRendererVK::recordOcclusion2Seed(VkCommandBuffer cmd)")
    if seed is None or "mOccl2StatsValid" not in seed:
        result.fail("recordOcclusion2Seed no longer invalidates the pending stats slots")
    if not re.search(r'LLCachedControl<U32>\s+s_occl2\(gSavedSettings,\s*"RenderVKOcclusion2Phase",\s*2\)', ren):
        result.fail("RenderVKOcclusion2Phase is no longer read as the U32 mode with default 2")
    if "occl2Down = !occlusion2AdaptFrame()" not in ren:
        result.fail("the two-phase decision no longer goes through occlusion2AdaptFrame")
    for flag in ("occlPlanned = ", "occlNow = "):
        m = re.search(re.escape(flag) + r"s_occl[^;]*;", ren)
        if not m or "!occl2Down" not in m.group(0):
            result.fail("%s no longer excludes the stood-down frames (single-phase would take over)" % flag.strip())

    settings = open(ctx.path(SETTINGS), encoding="utf-8").read()
    m = re.search(r"<key>RenderVKOcclusion2Phase</key>\s*<map>(.*?)</map>", settings, re.S)
    if not m:
        result.fail("RenderVKOcclusion2Phase missing from settings.xml")
    else:
        body = m.group(1)
        if not re.search(r"<key>Type</key>\s*<string>U32</string>", body):
            result.fail("RenderVKOcclusion2Phase must be U32 (0 off, 1 on, 2 adaptive)")
        if not re.search(r"<key>Value</key>\s*<integer>2</integer>", body):
            result.fail("RenderVKOcclusion2Phase default must be 2 (adaptive)")
        if not re.search(r"<key>Persist</key>\s*<integer>1</integer>", body):
            result.fail("RenderVKOcclusion2Phase is a preference: Persist 1")
    xui = open(ctx.path(XUI), encoding="utf-8").read()
    if 'control_name="RenderVKOcclusion2Phase"' not in xui:
        result.fail("RenderVKOcclusion2Phase is no longer bound in the Advanced Graphics floater (presets read it from there, #1067)")


def run(ctx, result):
    _source_guard(ctx, result)
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: adaptive occlusion fixtures not run")
        result.summary = "source guard only (no compiler)"
        return
    src = ctx.path("scripts/check/test_occl2_adapt.cpp")
    inc = ctx.path("indra/newview")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_occl2_adapt")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Werror", "-I" + inc, src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    result.summary = "outdoor stands down, indoor stays on, noise and flipping bounded, probes seed and wait for the lag"
