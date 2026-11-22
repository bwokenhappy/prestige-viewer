"""Regression guard for the removal of our TAA resolve: FSR 3.1 / DLSS are the only temporal anti-aliasing.

Compiles scripts/check/test_taa_removed.cpp with the host C++ compiler and runs it: the upscaler mode the
renderer runs for each setting / device combination (indra/newview/vkgfx/llupscalermode.h, llups::effectiveMode:
a legacy RenderFSAAType 3 is FSR Native AA, an upscaler that cannot run is off) and the one-time move of TAA
users (llups::migrate: DLAA where DLSS is available, else FSR Native AA; Antialiasing becomes SMAA).

Source rules:
  MODE    the renderer (llcleanrenderervk.cpp) reads RenderVKUpscaler through one cached control, in
          upscalerSetting(); upscalerMode() asks llups::effectiveMode; taaRequested() reads neither
          RenderVKTAA nor RenderFSAAType (a temporal resolve runs exactly when the upscaler mode is on).
  LATCH   FSR that fails kFsrFailLatch (3) frames running is given up on: fsrUsable() tests !mFsrLatched, and
          prepareFSR honours RenderVKFSRDebugFailPrepare (the test switch for the fallback and the latch). "Running"
          means consecutive: a frame DLSS resolves resets the count too. fsrUsable asks the device once, and
          only with a device context (never caches a "no" from before init).
  MIGRATE llappviewer.cpp moves TAA users once through llups::migrate with LLVKNeuralRecon's availability, called
          right after LLVulkanContext::init succeeds (DLSS is known by then); RenderVKTAA stays in settings.xml only
          as a hidden key (HideFromEditor 1) for that move and the usage statistics.
  GLOW    on an FSR / DLSS frame the glow is resolved at the render size by the glow temporal only
          (recordGlowResolve): no glowLowResWanted / mTaaGlowLo / mGlowUpscalerRan / RenderVKGlowTemporalLowRes;
          ensureGlowTemporal rewrites its sets when mTargetGen changes; the glow temporal and sharpening
          pipelines are not built inside a TAA block.
  FALLBACK a frame meant for FSR where FSR did not run takes recordResolveFallback: it clears mTaaNow (every later
          pass takes its non-temporal path, the bloom / LDR glow transition the raw glow target themselves),
          blits a scaled frame, and never records recordTaaInputBarriers.
  GONE    our TAA resolve is gone: no clean_taa.comp (file or SPIR-V manifest row), no recordTAA / ensureTAA / TAA
          history / sets / pipeline / param-ring region / its settings, and no "p_taa" GPU timer.
  STANDDOWN the jitter follows llups::temporalRuns (a DLSS mode whose DLSS stands down with FSR unusable renders
          native without jitter, never jittered and unresolved), the render scale off DLSS follows llups::fsrTier,
          and the FXAA / SMAA pass takes llups::postAaType (a legacy 3 that cannot run as FSR gets SMAA).
  UI      the Antialiasing lists (Preferences "fsaa", Photo Studio "aa_method_combo") offer no TAA (value 3); no
          tooltip says an upscaler "turns TAA on"; the Preferences, the bug report, the perf capture and the
          GPU-lost settings line no longer name RenderVKTAA; the test plan doc/testplans/*-remove-taa.md exists.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "taa-removed"
STAGE = "source"
DESCRIPTION = "FSR / DLSS are the only temporal anti-aliasing: mode rules (tests) and the removed TAA resolve"
GUARDS = ("The TAA resolve or its settings coming back, a TAA user left without anti-aliasing, a legacy "
          "RenderFSAAType 3 meaning no AA, or a frame where FSR fails left jittered and unresolved.")

HEADER = "indra/newview/vkgfx/llupscalermode.h"
TEST = "scripts/check/test_taa_removed.cpp"


def read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def body(src, signature):
    """The brace-balanced body of the first function whose definition contains `signature`."""
    i = src.find(signature)
    if i < 0:
        return None
    j = src.find("{", i)
    if j < 0:
        return None
    depth = 0
    for k in range(j, len(src)):
        if src[k] == "{":
            depth += 1
        elif src[k] == "}":
            depth -= 1
            if depth == 0:
                return src[j:k + 1]
    return None


RENDERER = "indra/newview/vkgfx/llcleanrenderervk.cpp"
RENDERER_H = "indra/newview/vkgfx/llcleanrenderervk.h"


def source_rules(ctx):
    bad = []
    cpp = read(ctx, RENDERER)
    hdr = read(ctx, RENDERER_H)

    # MODE
    reads = cpp.count('gSavedSettings, "RenderVKUpscaler"')
    setting = body(cpp, "uint32_t LLCleanRendererVK::upscalerSetting() const") or ""
    if reads != 1 or 'gSavedSettings, "RenderVKUpscaler"' not in setting:
        bad.append("MODE: RenderVKUpscaler must be read once in the renderer, in upscalerSetting() (found %d)" % reads)
    mode = body(cpp, "uint32_t LLCleanRendererVK::upscalerMode() const") or ""
    if "llups::effectiveMode(" not in mode:
        bad.append("MODE: upscalerMode() must ask llups::effectiveMode")
    taa = body(cpp, "bool LLCleanRendererVK::taaRequested() const") or ""
    if not taa or "RenderVKTAA" in taa or "RenderFSAAType" in taa or "upscalerMode()" not in taa:
        bad.append("MODE: taaRequested() must be the upscaler mode only (no RenderVKTAA / RenderFSAAType)")

    # LATCH
    if not re.search(r"kFsrFailLatch\s*=\s*3\b", hdr):
        bad.append("LATCH: kFsrFailLatch = 3 is not declared in llcleanrenderervk.h")
    usable = body(cpp, "bool LLCleanRendererVK::fsrUsable() const") or ""
    if "!mFsrLatched" not in usable:
        bad.append("LATCH: fsrUsable() must test !mFsrLatched")
    prep = body(cpp, "bool LLCleanRendererVK::prepareFSR(") or ""
    if "RenderVKFSRDebugFailPrepare" not in prep:
        bad.append("LATCH: prepareFSR must honour RenderVKFSRDebugFailPrepare")
    if not re.search(r"if \(fsrNow \|\| rrNow\)[^\n]*mFsrPrepFails = 0;", cpp):
        bad.append("LATCH: a frame FSR or DLSS resolves must reset mFsrPrepFails (3 failures running)")
    if not re.search(r"if \(mCtx && mFsrDeviceOk < 0\)", usable):
        bad.append("LATCH: fsrUsable must cache deviceSupported only with a device context")
    # MIGRATE
    app = read(ctx, "indra/newview/llappviewer.cpp")
    mig = body(app, "static void migrateTaaToUpscaler()") or ""
    if "llups::migrate(" not in mig or "LLVKNeuralRecon::instance().available()" not in mig:
        bad.append("MIGRATE: migrateTaaToUpscaler must use llups::migrate with LLVKNeuralRecon::instance().available()")
    init = app.find("LLVulkanContext::instance().init(")
    call = app.find("migrateTaaToUpscaler();", init if init >= 0 else 0)
    if init < 0 or call < 0 or "return false;" not in app[init:call] or app[init:call].count("\n") > 8:
        bad.append("MIGRATE: migrateTaaToUpscaler() must be called right after LLVulkanContext::instance().init succeeds")
    xml = read(ctx, "indra/newview/app_settings/settings.xml")
    m = re.search(r"<key>RenderVKTAA</key>\s*<map>(.*?)</map>", xml, re.S)
    if not m or not re.search(r"<key>HideFromEditor</key>\s*<integer>1</integer>", m.group(1)):
        bad.append("MIGRATE: settings.xml RenderVKTAA must stay as a hidden key (HideFromEditor 1)")

    # GLOW
    for name in ("glowLowResWanted", "mTaaGlowLo", "mGlowUpscalerRan", "RenderVKGlowTemporalLowRes"):
        if name in cpp or name in hdr or name in xml:
            bad.append("GLOW: %s is still referenced" % name)
    eg = body(cpp, "void LLCleanRendererVK::ensureGlowTemporal(VkCommandBuffer cmd)") or ""
    if "mTargetGen" not in eg:
        bad.append("GLOW: ensureGlowTemporal must rewrite its sets when mTargetGen changes")
    gr = body(cpp, "void LLCleanRendererVK::recordGlowResolve(") or ""
    if "recordGlowTemporal(" not in gr:
        bad.append("GLOW: recordGlowResolve must run the glow temporal")
    if "// ---- TAA resolve" in cpp:
        bad.append("GLOW: the glow temporal / sharpening pipelines still sit in the TAA resolve's init block")

    # FALLBACK
    fb = body(cpp, "void LLCleanRendererVK::recordResolveFallback(VkCommandBuffer cmd)") or ""
    if not fb or "mTaaNow = false" not in fb or "recordSceneUpscaleBlit(" not in fb or "recordTaaInputBarriers" in fb:
        bad.append("FALLBACK: recordResolveFallback must clear mTaaNow, blit a scaled frame and not record the input barriers")
    if cpp.count("recordResolveFallback(cmd)") < 1:
        bad.append("FALLBACK: the resolve block never takes recordResolveFallback")

    # GONE
    if os.path.exists(ctx.path("indra/vkclean/shaders/clean_taa.comp")):
        bad.append("GONE: indra/vkclean/shaders/clean_taa.comp still exists")
    if "clean_taa" in read(ctx, "indra/vkclean/regen_world_spv.py"):
        bad.append("GONE: regen_world_spv.py still builds clean_taa.comp")
    for name in ("recordTAA", "ensureTAA", "mTaaHist", "mTaaAcc", "mTaaPipe", "mDslTaa", "mDsetTaa", "kWorldTaa",
                 "mTaaRgn", "kTaaParams", "RenderVKTAADebug", "RenderVKTAAStability"):
        if name in cpp or name in hdr:
            bad.append("GONE: %s is still in the renderer" % name)
    for key in ("RenderVKTAADebug", "RenderVKTAAStability"):
        if "<key>%s</key>" % key in xml:
            bad.append("GONE: settings.xml still declares %s" % key)
    for root, _dirs, files in os.walk(ctx.path("indra/newview")):
        for f in files:
            if f.endswith((".cpp", ".h")):
                with open(os.path.join(root, f), encoding="utf-8", errors="replace") as fh:
                    if '"p_taa"' in fh.read():
                        bad.append("GONE: \"p_taa\" is still named in %s" % os.path.relpath(os.path.join(root, f), ctx.path(".")))

    # STANDDOWN
    if not re.search(r"mTaaJitterOn\s*=\s*[^;]*llups::temporalRuns\(", cpp):
        bad.append("STANDDOWN: mTaaJitterOn must follow llups::temporalRuns")
    rs = body(cpp, "float LLCleanRendererVK::renderScaleWanted()") or ""
    if "llups::fsrTier(" not in rs:
        bad.append("STANDDOWN: renderScaleWanted must take the FSR preset from llups::fsrTier")
    if not re.search(r"aa_type\s*=\s*llups::postAaType\(", cpp):
        bad.append("STANDDOWN: the FXAA / SMAA pass must take llups::postAaType of RenderFSAAType")

    # UI
    xui = "indra/newview/skins/default/xui/en/"
    for f, combo in (("floater_preferences_graphics_advanced.xml", "fsaa"), ("panel_photo_studio_scene.xml", "aa_method_combo")):
        t = read(ctx, xui + f)
        m = re.search(r'<combo_box[^>]*name="%s".*?</combo_box>' % combo, t, re.S)
        if not m:
            bad.append("UI: %s has no combo %s" % (f, combo))
        elif re.search(r'value="3"', m.group(0)) or ">TAA<" in m.group(0) or 'label="TAA"' in m.group(0):
            bad.append("UI: %s's %s still offers TAA" % (f, combo))
    for root, _dirs, files in os.walk(ctx.path(xui)):
        for f in files:
            with open(os.path.join(root, f), encoding="utf-8", errors="replace") as fh:
                if "turns TAA on" in fh.read():
                    bad.append("UI: %s still says an upscaler turns TAA on" % f)
    for f in ("indra/newview/llfloaterpreferencesgraphicsadvanced.cpp", "indra/newview/llfloaterreportbug.cpp",
              "indra/newview/llvkperfcapture.cpp"):
        if '"RenderVKTAA"' in read(ctx, f):
            bad.append("UI: %s still names RenderVKTAA" % f)
    gpu_lost = re.search(r"static const char\* const kSettings\[\] = \{(.*?)\};", app, re.S)
    if not gpu_lost or '"RenderVKTAA"' in gpu_lost.group(1):
        bad.append("UI: llappviewer.cpp's GPU-lost settings line still names RenderVKTAA (or was not found)")
    plans = [f for f in os.listdir(ctx.path("doc/testplans")) if f.endswith("-remove-taa.md")]
    if not plans:
        bad.append("UI: the test plan doc/testplans/*-remove-taa.md is missing")
    return bad


def run(ctx, result):
    if not os.path.exists(ctx.path(HEADER)):
        result.fail("%s is missing" % HEADER)
        result.summary = "header missing"
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: upscaler mode tests not run")
        result.summary = "not run"
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_taa_removed")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + ctx.path("indra/newview"),
                            ctx.path(TEST), "-o", exe], capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    bad = source_rules(ctx)
    for b in bad:
        result.fail(b)
    if not bad:
        result.summary = "tests pass, source rules hold"
