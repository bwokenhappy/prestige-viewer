"""Regression guard for frame generation's rules (indra/newview/vkgfx/llvkframegenpolicy.h).

Compiles scripts/check/test_framegen_policy.cpp with the host C++ compiler and runs it: which
condition makes frame generation unavailable (and the reason text the player sees), how the
requested multiplier maps to generated frames on RTX 50 / 40 / none, and how a frame cap (on-screen
frames) maps to Reflex's limit on real frames; (#1544) which generator runs (DLSS, FSR, a forced one, the
reason shown when neither does), FSR's caps (2x only, never driver-paced), the scratch image's format per
generator, and the generator's images in the one-set admission.

Source rules (the final fix pass) for where the viewer uses those rules:
  CLAMP    LLVKFrameGen::adopt (initDirect's and a live switch's) takes the generator's maxGenerated only through llfg::maxGeneratedFor with
           VkSwapchain::kMaxBatchPresents (the present thread's per-batch arrays have that size).
  WHY      the DLSS generator's init (LLVKFrameGenDlss::init) turns the add-on's answer into a block through
           llfg::capsBlock (the missing library, Windows' GPU scheduling and a failed capability call each have
           their own text), and hands the add-on's maxGenerated on as it is (the clamp is initDirect's), and
           LLVKNeuralRecon pins the add-on's SLIP_FG_WHY_* values to llfg::AddonWhy.
  GENERATOR  (#1544) initDirect picks the generator (a later change switches live: applyGenerator) with llfg::chooseGenerator(RenderVKFrameGenGenerator, DLSS's
           block, FSR's block) and states it in mCaps; the driver is asked to pace only through
           llfg::meteringFeature(the device's present metering, the generator's caps); chainGenerated tests the
           chosen upscaler with llfg::upscalerWritesInputs (FSR or DLSS); the Context's inputsOn is the renderer's
           upscalerInputsLastFrame() (DLSS or FSR ran); settings.xml has RenderVKFrameGenGenerator (U32, 0).
  TOP      every llfg::generatedFrames call of LLVKFrameGen passes driverPacingAccepted() (5x and 6x only while
           the driver paces the presents; 4x otherwise), and Preferences lists the multipliers through
           LLVKFrameGen::multiplierOffered (added and removed, never greyed; #1544 FSR: Off and 2x).
  STOPPED  LLVKFrameGen::directFailed shows llfg::stoppedText (never the add-on's raw error, which goes to
           the log), and the Preferences control shows that line whenever generation stopped.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "framegen-policy"
STAGE = "source"
DESCRIPTION = "frame generation availability, multiplier and frame cap rules (tests)"
GUARDS = ("Frame generation running where it must not (VR, background, no DLSS, scRGB, Linux), a "
          "wrong reason shown, a multiplier the GPU cannot do, or a frame cap off by the multiplier.")

HEADER = "indra/newview/vkgfx/llvkframegenpolicy.h"
TEST = "scripts/check/test_framegen_policy.cpp"
FG_CPP = "indra/newview/vkgfx/llvkframegen.cpp"
FG_DLSS = "indra/newview/vkgfx/llvkframegendlss.cpp"
CTX = "indra/newview/vkgfx/llvulkancontext.cpp"
REN_H = "indra/newview/vkgfx/llcleanrenderervk.h"
SETTINGS = "indra/newview/app_settings/settings.xml"
FG_H = "indra/newview/vkgfx/llvkframegen.h"
NR_CPP = "indra/newview/vkgfx/llvkneuralrecon.cpp"
PREFS = "indra/newview/llfloaterpreferencesgraphicsadvanced.cpp"


def _code(ctx, rel):
    """The file without comments ("" when it is missing)."""
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return re.sub(r"//[^\n]*|/\*.*?\*/", "", f.read(), flags=re.S)
    except OSError:
        return ""


def _body(code, signature):
    """The body of the function whose definition matches `signature` ("" when there is none)."""
    m = re.search(signature + r"[^;{]*\{", code)
    if not m:
        return ""
    depth, i = 1, m.end()
    while i < len(code) and depth:
        depth += {"{": 1, "}": -1}.get(code[i], 0)
        i += 1
    return code[m.end():i]


def source_rules(ctx):
    bad = []
    fg = _code(ctx, FG_CPP)
    init = _body(fg, r"void\s+LLVKFrameGen::initDirect\s*\(")
    # (#1544 Task 6: the generator in use is adopted in ONE place, LLVKFrameGen::adopt -- by initDirect and by a live
    # switch, applyGenerator -- with the chosen backend's caps)
    adopt = _body(fg, r"void\s+LLVKFrameGen::adopt\s*\(")
    sw = _body(fg, r"void\s+LLVKFrameGen::applyGenerator\s*\(")
    if not re.search(r"\badopt\s*\(\s*\*chosen\s*,\s*dev\s*\)", init) or not re.search(r"\badopt\s*\(\s*\*next\s*,\s*dev\s*\)", sw) \
            or len(re.findall(r"\badopt\s*\(", fg)) != 3:
        bad.append("GENERATOR: %s the generator in use is adopted only by adopt(...): initDirect's adopt(*chosen, dev) "
                   "and applyGenerator's adopt(*next, dev)" % FG_CPP)
    # (#1544: the generator's caps -- `gc`, filled by mGen->init -- are clamped here, for every generator)
    if not re.search(r"mCaps\.maxGenerated\s*=\s*llfg::maxGeneratedFor\s*\(\s*gc\.maxGenerated\s*,\s*"
                     r"VkSwapchain::kMaxBatchPresents\s*\)", adopt) \
            or len(re.findall(r"mCaps\.maxGenerated\s*=", fg)) != 1 \
            or not re.search(r"const\s+llfg::GeneratorCaps\s*&\s*gc\s*=\s*b\.caps\s*;", adopt):
        bad.append("CLAMP: %s adopt must set mCaps.maxGenerated only as llfg::maxGeneratedFor(gc.maxGenerated, "
                   "VkSwapchain::kMaxBatchPresents), gc being the adopted generator's caps (b.caps)" % FG_CPP)
    dlss_init = _body(_code(ctx, FG_DLSS), r"llfg::Block\s+LLVKFrameGenDlss::init\s*\(")
    if not re.search(r"llfg::capsBlock\s*\(", dlss_init) or re.search(r"!\s*fc\.available\b", dlss_init) \
            or not re.search(r"caps\.maxGenerated\s*=\s*fc\.maxGenerated\s*;", dlss_init):
        bad.append("WHY: %s LLVKFrameGenDlss::init must decide the GPU's block with llfg::capsBlock (not its own test "
                   "of fc.available) and report the add-on's maxGenerated as it is" % FG_DLSS)
    nr = _code(ctx, NR_CPP)
    for name, why in (("SLIP_FG_WHY_LIBRARY_MISSING", "LibraryMissing"), ("SLIP_FG_WHY_GPU_SCHEDULING_OFF", "GpuSchedulingOff"),
                      ("SLIP_FG_WHY_QUERY_FAILED", "QueryFailed"), ("SLIP_FG_WHY_NOT_STATED", "NotStated")):
        if not re.search(r"static_assert\s*\(\s*%s\s*==\s*\(uint32_t\)\s*llfg::AddonWhy::%s\b" % (name, why), nr):
            bad.append("WHY: %s must static_assert %s == (uint32_t)llfg::AddonWhy::%s" % (NR_CPP, name, why))
    # GENERATOR (#1544): which generator runs, what it may do, and what both need.
    if not re.search(r'LLCachedControl<U32>\s+(\w+)\s*\(\s*gSavedSettings\s*,\s*"RenderVKFrameGenGenerator"\s*,\s*0\s*\)', init) \
            or not re.search(r"llfg::chooseGenerator\s*\(\s*\(U32\)\s*\w+\s*,\s*mDlss\.block\s*,\s*mFsr\.block\s*\)", init) \
            or not re.search(r"llfg::chooseGenerator\s*\(\s*setting\s*,\s*mDlss\.block\s*,\s*mFsr\.block\s*\)",
                             _body(fg, r"LLVKFrameGen::chosenFor\s*\(")) \
            or len(re.findall(r"chooseGenerator\s*\(", fg)) != 2:
        bad.append("GENERATOR: %s initDirect (and chosenFor, for a live switch) must pick the generator with "
                   "llfg::chooseGenerator(RenderVKFrameGenGenerator, mDlss.block, mFsr.block), nowhere else" % FG_CPP)
    if not re.search(r"mCaps\.generator\s*=\s*b\.gen->kind\s*\(\s*\)\s*;", adopt):
        bad.append("GENERATOR: %s adopt must state the generator in use in mCaps (the add-on's blocks are DLSS's "
                   "only, FSR's multiplier text)" % FG_CPP)
    if not re.search(r"mMeteringFeature\s*=\s*llfg::meteringFeature\s*\(\s*dev\.presentMetering\s*\(\s*\)\s*,\s*gc\s*\)\s*;", adopt) \
            or len(re.findall(r"mMeteringFeature\s*=[^=]", fg)) != 1:
        bad.append("GENERATOR: %s adopt must set mMeteringFeature only as llfg::meteringFeature(dev.presentMetering(), "
                   "gc): the driver is asked to pace only a generator that lets it" % FG_CPP)
    chain = _body(fg, r"uint32_t\s+LLVKFrameGen::chainGenerated\s*\(")
    if not re.search(r"llfg::upscalerWritesInputs\s*\(\s*\(U32\)\s*\w+\s*\)", chain) or re.search(r">=\s*7u?\b", chain):
        bad.append("GENERATOR: %s chainGenerated must test the chosen upscaler with llfg::upscalerWritesInputs (FSR or "
                   "DLSS), not DLSS's modes alone" % FG_CPP)
    cx = _code(ctx, CTX)
    if not re.search(r"\.inputsOn\s*=\s*mImpl->mWorld\s*&&\s*mImpl->mWorld->upscalerInputsLastFrame\s*\(\s*\)\s*;", cx):
        bad.append("GENERATOR: %s must set the Context's inputsOn from the renderer's upscalerInputsLastFrame()" % CTX)
    if not re.search(r"bool\s+upscalerInputsLastFrame\s*\(\s*\)\s*const\s*\{\s*return\s+mRrRanLast\s*\|\|\s*mFsrRanLast\s*;",
                     _code(ctx, REN_H)):
        bad.append("GENERATOR: %s upscalerInputsLastFrame() must be `mRrRanLast || mFsrRanLast` (either upscaler writes "
                   "the generators' inputs)" % REN_H)
    try:
        with open(ctx.path(SETTINGS), encoding="utf-8", errors="replace") as f:
            settings = f.read()
    except OSError:
        settings = ""
    sm = re.search(r"<key>RenderVKFrameGenGenerator</key>\s*<map>(.*?)</map>", settings, re.S)
    if not sm or not re.search(r"<key>Type</key>\s*<string>U32</string>", sm.group(1)) \
            or not re.search(r"<key>Value</key>\s*<integer>0</integer>", sm.group(1)) \
            or not all(w in sm.group(1) for w in ("0 automatic", "1 DLSS", "2 FSR")):
        bad.append("GENERATOR: %s needs RenderVKFrameGenGenerator (U32, default 0; its comment says 0 automatic, 1 DLSS, "
                   "2 FSR)" % SETTINGS)
    calls = re.findall(r"llfg::generatedFrames\s*\(([^;]*);", fg)
    if len(calls) < 2 or any(not re.search(r",\s*driverPacingAccepted\s*\(\s*\)\s*\)", c) for c in calls):
        bad.append("TOP: every llfg::generatedFrames( in %s must pass driverPacingAccepted() (above 4x only while the "
                   "driver paces the presents)" % FG_CPP)
    prefs_code = _code(ctx, PREFS)
    # (#1544: every multiplier is listed or not by multiplierOffered -- FSR lists Off and 2x; rule PREFS of
    # framegen-fsr pins the list)
    if not re.search(r"if\s*\(\s*fg\.multiplierOffered\s*\(\s*\(U32\)\s*m\s*\)\s*\)", prefs_code) \
            or not re.search(r"fgc->add\s*\(", prefs_code) or not re.search(r"fgc->remove\s*\(", prefs_code) \
            or re.search(r"fg\.maxGenerated\s*\(", prefs_code):
        bad.append("TOP: %s must list (add / remove) the multipliers through fg.multiplierOffered()" % PREFS)
    failed = _body(fg, r"void\s+LLVKFrameGen::directFailed\s*\(")
    if not re.search(r"mReason\s*=\s*llfg::stoppedText\s*\(", failed) or not re.search(r"mStatusText\s*=\s*mReason\b", failed) \
            or not re.search(r"LL_WARNS\s*\([^)]*\)[^;]*\bwhy\b", failed):
        bad.append("STOPPED: %s directFailed must show llfg::stoppedText (mReason, mStatusText = mReason) and log "
                   "the add-on's own text" % FG_CPP)
    if not re.search(r"\bbool\s+stopped\s*\(\s*\)\s*const\s*\{\s*return\s+mDirectFailed\s*;", _code(ctx, FG_H)):
        bad.append("STOPPED: %s must offer stopped() (mDirectFailed)" % FG_H)
    prefs = _code(ctx, PREFS)
    m = re.search(r"if\s*\(\s*fg\.stopped\s*\(\s*\)\s*\)(.{0,200})", prefs, re.S)
    if not m or not re.search(r"\bfg\.reason\s*\(\s*\)", m.group(1)):
        bad.append("STOPPED: %s must show fg.reason() (the stopped line) whenever fg.stopped()" % PREFS)
    return bad


def run(ctx, result):
    if not os.path.exists(ctx.path(HEADER)):
        result.fail("%s is missing" % HEADER)
        result.summary = "header missing"
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: frame generation policy tests not run")
        result.summary = "not run"
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_framegen_policy")
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
        result.summary = "tests pass, 5 source rules hold"
