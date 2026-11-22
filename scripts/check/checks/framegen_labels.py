"""Regression guard for frame generation's moving labels (indra/newview/vkgfx/llvkframegenlabels.h, #1517 Task 7e).

Compiles scripts/check/test_framegen_labels.cpp with the host C++ compiler and runs it: how far a label
moved since the last real frame (and when it steps instead), packing labels into the per-slot atlas, the
atlas size (grow on overflow, cap, shrink when idle), the tiles each label can reach, the in-between phase.
Source rules: labels carry their object's id; recorded into the atlas only on generating frames; the atlas is
admitted, never freed under the GPU, and goes back after llfgl::kIdleShrinkFrames frames without labels
(tendLabels; its behaviour: test_framegen_present.cpp, run by framegen-present).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "framegen-labels"
STAGE = "source"
DESCRIPTION = "frame generation moving labels: history, atlas, tiles (tests)"
GUARDS = ("A label streaking across the screen after a teleport, labels overlapping in the atlas, an "
          "atlas that grows without bound or never shrinks, a label missing from a tile it reaches.")

HEADER = "indra/newview/vkgfx/llvkframegenlabels.h"
TEST = "scripts/check/test_framegen_labels.cpp"


def run(ctx, result):
    if not os.path.exists(ctx.path(HEADER)):
        result.fail("%s is missing" % HEADER)
        result.summary = "header missing"
        return
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: frame generation label tests not run")
        result.summary = "not run"
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_framegen_labels")
        c = subprocess.run([cxx, "-std=c++17", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + ctx.path("indra/newview"),
                            ctx.path(TEST), "-o", exe], capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        r = subprocess.run([exe], capture_output=True, text=True)
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    problems = source_rules(ctx)
    for p in problems:
        result.fail(p)
    result.summary = "tests pass" if not problems else "%d rule(s) broken" % len(problems)


def _read(ctx, rel):
    with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
        return f.read()


def _function_body(text, signature):
    """The body of the function whose definition starts with `signature` (to the closing brace in column 0)."""
    i = text.find(signature)
    if i < 0:
        return None
    j = text.find("\n}\n", i)
    return text[i:j + 2] if j >= 0 else text[i:]


def _enclosing_block(body, needle):
    """The opening line of the innermost block around the first `needle` in `body`."""
    i = body.find(needle)
    if i < 0:
        return None
    depth = 0
    k = i
    while k > 0:
        k -= 1
        c = body[k]
        if c == "}":
            depth += 1
        elif c == "{":
            if depth == 0:
                start = body.rfind("\n", 0, k)
                prev = body.rfind("\n", 0, start) if body[start + 1:k].strip() == "" else start
                return body[prev + 1:k + 1]
            depth -= 1
    return ""


def source_rules(ctx):
    """Task 7e.2+: each label says which object it belongs to, whatever its occlusion settings."""
    problems = []
    hud = {
        "indra/newview/llhudnametag.cpp": "void LLHUDNameTag::renderText(",
        "indra/newview/llhudtext.cpp": "void LLHUDText::render(",
        # #1537: look-at targets (crosshair and name) are world labels too
        "indra/newview/llhudeffectlookat.cpp": "void LLHUDEffectLookAt::render(",
    }
    for rel, sig in hud.items():
        body = _function_body(_read(ctx, rel), sig)
        if body is None:
            problems.append("%s: %s not found" % (rel, sig))
            continue
        if "gGL.setLabelMotion(" not in body or "gGL.clearLabelMotion(" not in body:
            problems.append("%s: %s must set and clear the label's motion id (gGL.setLabelMotion / "
                            "clearLabelMotion)" % (rel, sig.split("::")[1]))
            continue
        block = _enclosing_block(body, "gGL.setLabelMotion(")
        if block is not None and re.search(r"\b(mZCompare|occlude)\b", block):
            problems.append("%s: setLabelMotion is inside the occlusion condition (%s): labels with "
                            "z-compare off must move too" % (rel, block.strip()))
    hud_all = _function_body(_read(ctx, "indra/newview/llhudtext.cpp"), "void LLHUDText::renderAllHUD(")
    if hud_all and "setLabelMotion" in hud_all:
        problems.append("llhudtext.cpp: HUD-attached text (renderAllHUD) is interface, not a world label")
    vk = _read(ctx, "indra/newview/vkgfx/llrendervk/llrendervk.cpp")
    body = _function_body(vk, "void LLRenderVK::setLabelMotion(")
    if body is None or "flush()" not in body:
        problems.append("llrendervk.cpp: LLRenderVK::setLabelMotion must close the open batch (flush) on a change")
    hdr = _read(ctx, "indra/newview/vkgfx/llrendervk/llrendervk.h")
    m = re.search(r"struct PendingDraw \{(.*?)\n    \};", hdr, re.S)
    if not m or "labelId" not in m.group(1) or "labelPoint" not in m.group(1):
        problems.append("llrendervk.h: PendingDraw must carry labelId and labelPoint")
    problems += present_rules(ctx)
    problems += recording_rules(ctx)
    problems += review_rules(ctx)
    return problems


def review_rules(ctx):
    """Task 7e final review fixes: stable ids, still labels over a reset, motion without occlusion."""
    problems = []
    for rel in ("indra/newview/llhudnametag.cpp", "indra/newview/llhudtext.cpp", "indra/newview/llhudeffectlookat.cpp"):
        if "gGL.setLabelMotion(mLabelSerial," not in _read(ctx, rel):
            problems.append("%s: a label's id is its object's session serial (an address can be reused by the next "
                            "object and inherit its motion)" % rel)
    # (final review B M-a) the evaluate's OWN reset: the present thread adds its broken history (a dropped or
    # withheld batch) to the job's -- the labels hold still exactly when the generator's world does.
    if "(evalFailed || eval.reset) ? 1.f : llfgl::phase(k + 1, n)" not in _read(ctx, "indra/newview/vkgfx/llvkframegenpresent.cpp"):
        problems.append("llvkframegenpresent.cpp: over a generator reset (no history) the labels hold still (phase 1): "
                        "the reset the evaluate got (eval.reset, the job's or the present thread's broken history), "
                        "not only the job's (j.eval.reset)")
    app = re.sub(r"\s+", "", _read(ctx, "indra/newview/llappviewer.cpp"))
    if "beginPreHudLayer(world.proj,world.view,s_text_occl||LLHUDObject::sWorldBillboards)" not in app:
        problems.append("llappviewer.cpp: the pre-HUD layer always gets the world camera (labels' motion); "
                        "RenderVKTextOcclusion only switches occlusion")
    ctxt = _read(ctx, "indra/newview/vkgfx/llvulkancontext.cpp")
    # (#1536: the SDR layer's pictures compose in the SDR encoded space -- no paper white there)
    if "fgJob.paperWhiteNits = sdrLayer ? 0.f : labelRequest.paperWhiteNits;" not in ctxt:
        problems.append("llvulkancontext.cpp: generated frames compose labels at the encode's (sanitized) paper white")
    return problems


def recording_rules(ctx):
    """Task 7e.5: labels into the atlas on generating frames only, occluded as on screen, and moved by phase."""
    problems = []
    vk = _read(ctx, "indra/newview/vkgfx/llrendervk/llrendervk.cpp")
    overlay = _function_body(vk, "void LLRenderVK::recordWorldOverlay(") or ""
    flat = re.sub(r"\s+", "", overlay)
    if "if(mHasLabelTarget)recordLabelAtlas(" not in flat:
        problems.append("llrendervk.cpp: recordWorldOverlay must draw labels into the atlas only with a label target")
    if not re.search(r"recordDraws\(cmd,w,h,mFrameVbo,mPreHudBegin,mPreHudEnd,labelDepth,nullptr,inAtlas\.empty\(\)\?nullptr:&inAtlas\)", flat):
        problems.append("llrendervk.cpp: the UI layer's pre-HUD draw must skip the labels drawn into the atlas")
    atlas = _function_body(vk, "void LLRenderVK::recordLabelAtlas(") or ""
    aflat = re.sub(r"\s+", "", atlas)
    if "(float)w,(float)h,0.0f,1.0f}" not in aflat or "recordDraws(cmd,w,h,mFrameVbo,run.begin,run.end,labelDepth" not in aflat:
        problems.append("llrendervk.cpp: a label goes into the atlas with the screen-sized viewport moved (its occlusion "
                        "and pixels exactly the screen's) and the scene depth bound")
    occl = _read(ctx, "indra/newview/vkgfx/llrendervk/shaders/ui_occl.frag")
    if "gl_FragCoord" in occl:
        problems.append("ui_occl.frag: label occlusion must not read gl_FragCoord (labels drawn into the atlas are moved "
                        "by the viewport; their depth UV comes from clip space)")
    pres = _read(ctx, "indra/newview/vkgfx/llvkframegenpresent.cpp")
    if not re.search(r"\(evalFailed[^)]*\) \? 1\.f : llfgl::phase\(k \+ 1, n\)", pres):
        problems.append("llvkframegenpresent.cpp: generated frame k + 1 of n draws the labels at llfgl::phase(k + 1, n) "
                        "(and where they are now when its world is the real one)")
    ctxt = _read(ctx, "indra/newview/vkgfx/llvulkancontext.cpp")
    m = re.search(r"const bool fgLabels = (.*?);", ctxt, re.S)
    if not m:
        problems.append("llvulkancontext.cpp: fgLabels (the frame draws its labels into the slot's atlas) is missing")
    else:
        cond = re.sub(r"\s+", "", m.group(1))
        for need in ("fgCount", "!vrWorld", "!sdrFrame", "capturePending()==0u", "liveSnapPending(1u)",
                     "liveSnapPending(2u)", "liveSnapPending(3u)", "Encoding::Hdr10||sdrLayer)", "ensureLabels(",
                     "layer&&fgCount"):
            if need not in cond:
                problems.append("llvulkancontext.cpp: fgLabels must require %s" % need)
    for rel in ("indra/newview/vkgfx/llvulkancontext.cpp", "indra/newview/app_settings/settings.xml",
                "indra/newview/vkgfx/vk_display_frame.h", "indra/newview/vkgfx/vk_display_encode.h"):
        text = _read(ctx, rel)
        for gone in ("fgWorldText", "RenderVKFrameGenWorldText", "prepareWorldText", "mWorldTextIdleFrames"):
            if gone in text:
                problems.append("%s: %s (Task 7d's world-text layer) is replaced by the label atlas" % (rel, gone))
    return problems


def present_rules(ctx):
    """Task 7e.3: the per-slot atlas and label buffer: admitted, never freed under the GPU, main thread only."""
    problems = []
    rel = "indra/newview/vkgfx/llvkframegenpresent.cpp"
    cpp = _read(ctx, rel)
    body = _function_body(cpp, "bool LLVKFrameGenPresent::ensureLabels(")
    if body is None:
        return ["%s: LLVKFrameGenPresent::ensureLabels is missing" % rel]
    b = body.find("budget(")
    c = body.find("createImage2D(")
    if b < 0 or c < 0 or b > c:
        problems.append("%s: ensureLabels must ask the budget before it creates an atlas" % rel)
    if "destroyImage(" in body.replace("collectRetiredLabels", ""):
        problems.append("%s: ensureLabels must not destroy an atlas directly (the GPU may still read it): "
                        "retire it" % rel)
    retire = _function_body(cpp, "void LLVKFrameGenPresent::collectRetiredLabels(")
    if retire is None or "vkGetSemaphoreCounterValue" not in retire or retire.count("vkGetSemaphoreCounterValue") < 2:
        problems.append("%s: a retired atlas is freed only once both the frames (frame done) and the present "
                        "thread's jobs (released) that could read it are done" % rel)
    rel_body = _function_body(cpp, "void LLVKFrameGenPresent::releaseImages(")
    if rel_body is None or "labelAtlas" not in rel_body or "labelBuffer" not in rel_body or "mRetiredLabels" not in rel_body:
        problems.append("%s: releaseImages must free every slot's atlas, label buffer and the retired atlases" % rel)
    # (final review, 7e M7) atlases nobody uses go back: retired through the same list (never destroyed under
    # the GPU), and the list is collected every frame of a frame generation chain, labels or not
    tend = _function_body(cpp, "void LLVKFrameGenPresent::tendLabels(")
    if tend is None:
        problems.append("%s: LLVKFrameGenPresent::tendLabels (idle atlases retired; retired ones collected each "
                        "frame) is missing" % rel)
    else:
        if "mRetiredLabels.push_back(" not in tend or "destroyImage(" in tend:
            problems.append("%s: tendLabels must retire an idle atlas through mRetiredLabels, never destroy it "
                            "directly (the GPU may still read it)" % rel)
        if "collectRetiredLabels(false)" not in tend:
            problems.append("%s: tendLabels must collect the retired atlases (collectRetiredLabels(false): frame "
                            "done and released both reached)" % rel)
        if "mSerial" not in tend:
            problems.append("%s: an atlas retired by tendLabels waits for every job pushed so far (mSerial)" % rel)
    ctxt = re.sub(r"\s+", " ", _read(ctx, "indra/newview/vkgfx/llvulkancontext.cpp"))
    call = ctxt.find("mImpl->mFgPresent.tendLabels(fgSlot, mImpl->mLabelIdleFrames >= llfgl::kIdleShrinkFrames,")
    if call < 0:
        problems.append("llvulkancontext.cpp: each frame must tend the label atlases (mFgPresent.tendLabels(fgSlot, "
                        "mImpl->mLabelIdleFrames >= llfgl::kIdleShrinkFrames, ...)): idle after kIdleShrinkFrames "
                        "frames without labels")
    else:
        head = ctxt[max(0, call - 400):call]
        if "if (fgChain && !headless)" not in head or "if (fgLabels)" in head[head.rfind("if (fgChain && !headless)"):]:
            problems.append("llvulkancontext.cpp: tendLabels runs on every frame that owns a present slot (`if (fgChain "
                            "&& !headless)`), whether or not the frame has labels (never inside `if (fgLabels)`)")
        if "mImpl->mLabelIdleFrames = fgLabels ? 0 :" not in head:
            problems.append("llvulkancontext.cpp: mLabelIdleFrames counts the frames in a row without labels "
                            "(`mImpl->mLabelIdleFrames = fgLabels ? 0 : ...`)")
    for fn in ("void LLVKFrameGenPresent::runJob(", "void LLVKFrameGenPresent::presentJob("):
        fb = _function_body(cpp, fn)
        if fb and "ensureLabels" in fb:
            problems.append("%s: %s (the present thread) must not create or resize label atlases" % (rel, fn))
    return problems
