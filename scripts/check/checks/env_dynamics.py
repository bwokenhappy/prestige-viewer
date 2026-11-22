"""Regression guard for how bright an environment's light may drive the frame (slenvdynamics.h).

Compiles scripts/check/test_env_dynamics.cpp with the host C++ compiler and runs it (the default
sky keeps LL's clamp and the bloom threshold; a creator sky with explicit zero probe ambiance and a
bright sun takes the HDR path and lifts the bloom threshold to its sunlit white; the dynamics ends),
then pins the wiring: the frame packer's no-post share and sky HDR scale, the tonemap's blended
tail, the bloom extract's weight, and the persisted Environment dynamics preference on the
Graphics tab.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "env-dynamics"
STAGE = "source"
DESCRIPTION = ("explicit-zero probe ambiance HDR path, bloom sunlit limit and the Environment dynamics "
               "preference (slenvdynamics.h)")
GUARDS = ("A creator sky with explicit zero probe ambiance and a bright sun clipping every sunlit surface "
          "to flat pale yellow / white through LL's NoPost clamp, and its bloom whiting out the screen; "
          "the default sky drifting off LL's clamp; the Environment dynamics preference losing its "
          "persistence or its consumers.")

NEWVIEW = "indra/newview/"
SHADERS = "indra/vkclean/shaders/"


def _read(ctx, rel, result):
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        result.fail("%s: missing" % rel)
        return ""


def _setting(xml, key):
    at = xml.find("<key>%s</key>" % key)
    if at < 0:
        return None
    end = xml.find("</map>", at)
    return xml[at:end]


def _fixtures(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("g++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler found: environment dynamics fixtures not run")
        return False
    src = ctx.path("scripts/check/test_env_dynamics.cpp")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_env_dynamics")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-I" + ctx.path("indra/newview"), src, "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return False
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return False
    return True


def run(ctx, result):
    ran = _fixtures(ctx, result)

    app = _read(ctx, NEWVIEW + "llappviewer.cpp", result)
    for needle, why in (
            ("!sky->canAutoAdjust() && probe_amb == 0.f", "the explicit-zero test (the key present, value 0)"),
            ("slenv::sunlitWhiteFromPacked(a)", "the sunlit white from the packed rows"),
            ("slenv::explicitZeroClampWeight(white)", "the no-post share"),
            ("a[6 * 4 + 1] = 1.f + (sqrtf(g) * 2.f - 1.f) * (1.f - zero_clamp)", "the sky HDR scale by the share"),
            ("explicit_zero ? zero_clamp", "the no-post lane by the share"),
            ("Sky post class:", "the post-class log line")):
        if needle not in app:
            result.fail("%sllappviewer.cpp: %s is gone (%s)" % (NEWVIEW, needle, why))

    common = _read(ctx, SHADERS + "clean_tonemap_common.glsl", result)
    if "float tmNoPost()" not in common or "mix(mix(clamped, t, pc.p0.y), clamped, np)" not in common:
        result.fail(SHADERS + "clean_tonemap_common.glsl: tonemapTail no longer blends by the no-post share")
    for shader in ("clean_tonemap.frag", "clean_weather.frag"):
        code = _read(ctx, SHADERS + shader, result)
        if "tmExpose(" not in code:
            result.fail(SHADERS + shader + ": exposure no longer goes through tmExpose (the no-post share)")
        if re.search(r"if \(pc\.p1\.x < 0\.5\)\s*\{\s*(//[^\n]*\n\s*)*(float|if \(night)", code):
            result.fail(SHADERS + shader + ": the exposure is gated on a boolean no_post again")

    bloom = _read(ctx, SHADERS + "clean_bloom.frag", result)
    if "factor *= max(pc.p0.w, 0.0);" not in bloom:
        result.fail(SHADERS + "clean_bloom.frag: the extract's threshold-term weight (p0.w) is not applied as is "
                    "(0 must mean no luminance bloom)")

    renderer = _read(ctx, NEWVIEW + "vkgfx/llcleanrenderervk.cpp", result)
    for needle in ("slenv::bloomThreshold(threshold", "slenv::bloomLuminanceWeight(dyn)",
                   "slenv::headroomRatio(mDisplayPeakNits/mDisplayPaperWhiteNits,dyn)"):
        if needle not in renderer:
            result.fail("%svkgfx/llcleanrenderervk.cpp: %s is gone" % (NEWVIEW, needle))
    if renderer.count('"RenderEnvironmentDynamics"') < 2:
        result.fail("%svkgfx/llcleanrenderervk.cpp: RenderEnvironmentDynamics must drive the bloom and the HDR "
                    "headroom" % NEWVIEW)

    settings = _read(ctx, NEWVIEW + "app_settings/settings.xml", result)
    entry = _setting(settings, "RenderEnvironmentDynamics")
    if entry is None:
        result.fail("settings.xml: RenderEnvironmentDynamics is gone")
    else:
        if not re.search(r"<key>Persist</key>\s*<integer>1</integer>", entry):
            result.fail("settings.xml: RenderEnvironmentDynamics must persist (a user preference)")
        if not re.search(r"<key>Type</key>\s*<string>F32</string>", entry):
            result.fail("settings.xml: RenderEnvironmentDynamics must be an F32 slider value")
        m = re.search(r"<key>Value</key>\s*<real>([0-9.]+)</real>", entry)
        if not m or not (0.0 <= float(m.group(1)) <= 1.0):
            result.fail("settings.xml: RenderEnvironmentDynamics default must lie in 0..1")

    prefs = _read(ctx, NEWVIEW + "skins/default/xui/en/floater_preferences_graphics_advanced.xml", result)
    m = re.search(r'<slider\s[^>]*control_name="RenderEnvironmentDynamics"[^>]*>', prefs, re.S)
    if not m:
        result.fail("floater_preferences_graphics_advanced.xml: the Environment dynamics slider is gone")
    else:
        tag = m.group(0)
        for attr in ('min_val="0.0"', 'max_val="1.0"', "tool_tip="):
            if attr not in tag:
                result.fail("floater_preferences_graphics_advanced.xml: the Environment dynamics slider lost %s" % attr)

    result.summary = ("default sky keeps LL's clamp; creator sky HDR path + bloom limit; dynamics wiring"
                      + ("" if ran else " (fixtures skipped)"))
