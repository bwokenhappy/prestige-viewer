"""Regression guard for frame generation's generator seam (#1544): indra/newview/vkgfx/llvkframegengenerator.h
(what a generator is given, in the renderer's own terms, and the interface it implements) and the DLSS
generator behind it (llvkframegendlss.*).

Compiles llvkframegendlss.cpp and scripts/check/test_framegen_generator.cpp on the host against a fake add-on
(scripts/check/stubs: logging, and a stand-in for glm) and runs it: LLVKFrameGenDlss::toEval gives the add-on,
field by field and bit for bit, what the viewer gave it before the seam existed (a frozen copy of that
derivation is the oracle; two cameras, every RenderVKFrameGenConventionDebug value), init answers each of the
add-on's and the GPU's blocks with the right detail and log text, prepare / fits / release follow the display
size and colour format only, and evaluate hands toEval's result to the add-on.

Source rules:
  NEUTRAL   nothing above a generator names the add-on: llvkframegen.{h,cpp}, llvkframegenpresent.{h,cpp},
            llvkframegengenerator.h and llvkframegenpolicy.h name no LLVKNeuralRecon and no FgEval (and include
            neither llvkneuralrecon.h nor -- but for llvkframegen.cpp, which makes the generator -- llvkframegendlss.h);
            llvulkancontext.cpp names no FgEval, and no LLVKNeuralRecon inside the frame loop's frame generation
            code (renderFrame, recordFrameGenInputs, fillGenInputs, prepareFrameGen). Only
            llvkframegendlss.cpp calls the add-on's fgCaps / fgCreate / fgEvaluate / fgRelease, and its evaluate
            hands the add-on toEval(in) -- the one conversion. The neutral header carries none of the add-on's
            terms (mvScale, viewToClip, clipToPrevClip).
  OWNER     a generator's evaluate( is called only by LLVKFrameGen::evaluate, which only
            llvkframegenpresent.cpp calls (the present thread); its init( only by initDirect; its prepare( only by
            LLVKFrameGen::prepareFeature; its release( only by prepareFeature, applyGenerator (a live switch of
            generator, #1544 Task 6) and shutdownDirect.
  ONE-SITE  mainViewConstants( -- the camera a frame's generated frames are made with -- is called only by the
            upscalers (llcleandlssrrvk.cpp, llcleanfsr3vk.cpp), once each, from their frameGenConstants, which
            runs only for the main view's frame (f.frameGenView) while frame generation is ready. That call hands
            over the renderer's own terms: the projection, view and reprojection copied as they are, the raw
            jitter with the sign apart, the render size, the upscaler's reset -- no matrix maths and no
            generator's unit in it.
  INPUTS    either upscaler's depth and motion vectors feed the generators: LLCleanRendererVK::frameGenInputs
            answers DLSS's (mRr[0]) when DLSS ran, else FSR's (mFsr[0]) when FSR ran, and FSR's two images are
            made with TRANSFER_SRC usage (frame generation copies them into its present slot, as DLSS's).
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "framegen-generator"
STAGE = "source"
DESCRIPTION = "frame generation's generator seam: neutral inputs, DLSS behind the interface (tests, source rules)"
GUARDS = ("The DLSS generator given other inputs than before the generator seam (a warped or doubled generated "
          "frame), a second generator inheriting DLSS's units or matrices, the add-on reached from the render "
          "loop or the present thread again, or a generator's feature made / released / evaluated from the "
          "wrong thread.")

VK = "indra/newview/vkgfx"
GEN_H = VK + "/llvkframegengenerator.h"
DLSS_H = VK + "/llvkframegendlss.h"
DLSS = VK + "/llvkframegendlss.cpp"
FG_H = VK + "/llvkframegen.h"
FG = VK + "/llvkframegen.cpp"
PT_H = VK + "/llvkframegenpresent.h"
PT = VK + "/llvkframegenpresent.cpp"
POLICY_H = VK + "/llvkframegenpolicy.h"
CTX = VK + "/llvulkancontext.cpp"
NR = (VK + "/llvkneuralrecon.h", VK + "/llvkneuralrecon.cpp")
# The upscalers that write a frame's camera for frame generation: file -> the function that does.
PRODUCERS = {VK + "/llcleandlssrrvk.cpp": "void LLCleanDlssRRVK::frameGenConstants(",
             VK + "/llcleanfsr3vk.cpp": "void LLCleanFSR3VK::frameGenConstants("}
FSR_UP = VK + "/llcleanfsr3vk.cpp"
REN = VK + "/llcleanrenderervk.cpp"
TEST = "scripts/check/test_framegen_generator.cpp"
STUBS = "scripts/check/stubs"


def _raw(ctx, rel):
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def _code(text):
    """C++ source without comments (newlines kept) and with string contents blanked."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def _span(text, signature):
    """(start, end) of the brace-matched body of the definition starting with `signature`, or None."""
    at = text.find(signature)
    if at < 0:
        return None
    start = text.find("{", at)
    semi = text.find(";", at)
    if start < 0 or (0 <= semi < start):
        return None
    depth = 0
    for i in range(start, len(text)):
        if text[i] == "{":
            depth += 1
        elif text[i] == "}":
            depth -= 1
            if depth == 0:
                return (start, i + 1)
    return None


def _body(text, signature):
    s = _span(text, signature)
    return text[s[0]:s[1]] if s else ""


def _line(text, pos):
    return text.count("\n", 0, pos) + 1


def _sources(ctx):
    for root, _dirs, files in os.walk(ctx.path("indra/newview")):
        for f in files:
            if f.endswith((".cpp", ".h")):
                p = os.path.join(root, f)
                yield os.path.relpath(p, ctx.path(".")).replace(os.sep, "/"), p


def source_rules(ctx):
    bad = []
    code = {rel: _code(_raw(ctx, rel)) for rel in (GEN_H, DLSS_H, DLSS, FG_H, FG, PT_H, PT, POLICY_H, CTX)}
    for rel in (GEN_H, DLSS_H, DLSS, FG_H, FG, PT_H, PT, POLICY_H, CTX):
        if not code[rel]:
            bad.append("%s is missing" % rel)
    if bad:
        return bad

    # ---- NEUTRAL ----
    for rel in (FG_H, FG, PT_H, PT, GEN_H, POLICY_H):
        for m in re.finditer(r"\bLLVKNeuralRecon\b|\bFgEval\b|\bFgCaps\b", code[rel]):
            bad.append("NEUTRAL: %s names %s (line %d): only the DLSS generator (%s) knows the add-on"
                       % (rel, m.group(0), _line(code[rel], m.start()), DLSS))
        raw = _raw(ctx, rel)
        for m in re.finditer(r'#\s*include\s*"(llvkneuralrecon\.h|llvkframegendlss\.h)"', raw):
            if m.group(1) == "llvkframegendlss.h" and rel == FG:
                continue   # (LLVKFrameGen makes the generator; it uses it through the interface: rule OWNER)
            bad.append("NEUTRAL: %s includes %s" % (rel, m.group(1)))
    for m in re.finditer(r"\bFgEval\b", code[CTX]):
        bad.append("NEUTRAL: %s names FgEval (line %d): the render loop fills llfg::GenInputs" % (CTX, _line(code[CTX], m.start())))
    loop = ("bool LLVulkanContext::renderFrame(", "bool LLVulkanContext::Impl::recordFrameGenInputs(",
            "static llfg::GenInputs fillGenInputs(", "bool prepareFrameGen(")
    for sig in loop:
        body = _body(code[CTX], sig)
        if not body:
            bad.append("NEUTRAL: %s has no `%s...`" % (CTX, sig))
        elif re.search(r"\bLLVKNeuralRecon\b", body):
            bad.append("NEUTRAL: %s names LLVKNeuralRecon inside `%s...`" % (CTX, sig))
    callers = set()
    for rel, p in _sources(ctx):
        if rel in NR:
            continue
        with open(p, encoding="utf-8", errors="replace") as f:
            t = _code(f.read())
        if re.search(r"(?:\.|->)\s*fg(?:Caps|Create|Evaluate|Release)\s*\(", t):
            callers.add(rel)
    if callers != {DLSS}:
        bad.append("NEUTRAL: the add-on's fgCaps / fgCreate / fgEvaluate / fgRelease are called by %s; only %s may "
                   "(and it must)" % (", ".join(sorted(callers)) or "nothing", DLSS))
    ev = _body(code[DLSS], "bool LLVKFrameGenDlss::evaluate(")
    if not re.search(r"\.\s*fgEvaluate\s*\(\s*\w+\s*,\s*toEval\s*\(\s*in\s*\)\s*,\s*index\s*,\s*count\s*,", ev):
        bad.append("NEUTRAL: LLVKFrameGenDlss::evaluate must hand the add-on toEval(in) with the index and the count "
                   "(the one conversion of the renderer's inputs)")
    for m in re.finditer(r"\b(mvScale|viewToClip|clipToView|clipToPrevClip|prevClipToClip)\b", code[GEN_H]):
        bad.append("NEUTRAL: %s carries %s (line %d): a generator's own term, derived in its backend"
                   % (GEN_H, m.group(1), _line(code[GEN_H], m.start())))

    # ---- OWNER ----
    fg = code[FG]
    spans = {name: _span(fg, sig) for name, sig in (("evaluate", "bool LLVKFrameGen::evaluate("),
                                                     ("initDirect", "void LLVKFrameGen::initDirect("),
                                                     ("prepareFeature", "bool LLVKFrameGen::prepareFeature("),
                                                     ("applyGenerator", "void LLVKFrameGen::applyGenerator("),
                                                     ("shutdownDirect", "void LLVKFrameGen::shutdownDirect("))}
    for name, s in spans.items():
        if not s:
            bad.append("OWNER: %s has no LLVKFrameGen::%s" % (FG, name))

    def inside(pos, names):
        return any(spans[n] and spans[n][0] <= pos < spans[n][1] for n in names)

    allowed = {"evaluate": ("evaluate",), "prepare": ("prepareFeature",), "release": ("prepareFeature", "applyGenerator", "shutdownDirect"),
               "init": ("initDirect",)}
    seen = set()
    for m in re.finditer(r"->\s*(evaluate|prepare|release|init)\s*\(", fg):
        seen.add(m.group(1))
        if not inside(m.start(), allowed[m.group(1)]):
            bad.append("OWNER: %s calls a generator's %s( outside LLVKFrameGen::%s (line %d)"
                       % (FG, m.group(1), " / ".join(allowed[m.group(1)]), _line(fg, m.start())))
    for call in ("evaluate", "prepare", "release", "init"):
        if call not in seen:
            bad.append("OWNER: %s never calls the generator's %s(" % (FG, call))
    for rel, p in _sources(ctx):
        if rel in (PT, FG):
            continue
        with open(p, encoding="utf-8", errors="replace") as f:
            t = _code(f.read())
        if "LLVKFrameGen" not in t:
            continue
        for m in re.finditer(r"(?:LLVKFrameGen::instance\s*\(\s*\)|\bfgd?)\s*\.\s*evaluate\s*\(", t):
            bad.append("OWNER: %s calls LLVKFrameGen's evaluate( (line %d): only the present thread (%s) does"
                       % (rel, _line(t, m.start()), PT))
    if not re.search(r"\bfg\s*\.\s*evaluate\s*\(", code[PT]):
        bad.append("OWNER: %s never calls LLVKFrameGen's evaluate(" % PT)

    # ---- ONE-SITE ----
    sites = {}
    for rel, p in _sources(ctx):
        if rel in (FG, FG_H):
            continue
        with open(p, encoding="utf-8", errors="replace") as f:
            t = _code(f.read())
        n = len(re.findall(r"\bmainViewConstants\s*\(", t))
        if n:
            sites[rel] = n
    for rel in sorted(set(sites) - set(PRODUCERS)):
        bad.append("ONE-SITE: %s calls mainViewConstants(: only the upscalers do (%s)" % (rel, ", ".join(sorted(PRODUCERS))))
    for rel, sig in sorted(PRODUCERS.items()):
        if sites.get(rel) != 1:
            bad.append("ONE-SITE: %s must call mainViewConstants( exactly once (found %d)" % (rel, sites.get(rel, 0)))
        whole = _code(_raw(ctx, rel))
        body = _body(whole, sig)
        flat = re.sub(r"\s+", "", body)
        # (only the main view's frame, only while frame generation is ready: one guarded call)
        calls = re.findall(r"if\(([^;{}]*)\)frameGenConstants\(f,", re.sub(r"\s+", "", whole))
        if len(calls) != 1 or len(re.findall(r"(?<![\w:])frameGenConstants\s*\(\s*f\s*,", whole)) != 1 \
                or "f.frameGenView&&" not in calls[0] or "LLVKFrameGen::instance().directReady()" not in calls[0]:
            bad.append("ONE-SITE: %s must call frameGenConstants(f, ...) once, under `if (f.frameGenView && ... "
                       "LLVKFrameGen::instance().directReady())`" % rel)
        if not body or "mainViewConstants(c)" not in flat:
            bad.append("ONE-SITE: %s must hand the camera over in `%s...`" % (rel, sig))
            continue
        for want, why in (("std::memcpy(c.proj,f.proj,sizeof(c.proj));", "the projection as the renderer holds it"),
                          ("std::memcpy(c.view,f.view,sizeof(c.view));", "the view matrix as the renderer holds it"),
                          ("std::memcpy(c.reproj,f.reproj,sizeof(c.reproj));", "the reprojection as the renderer holds it"),
                          ("c.jitterPx[0]=f.jitterPx[0];", "the raw jitter (x)"), ("c.jitterPx[1]=f.jitterPx[1];", "the raw jitter (y)"),
                          ("c.motion=llfg::MotionUnits::UvPrevMinusCur;", "what the inputs pass writes"),
                          ("c.depthInverted=true;", "reversed-Z depth")):
            if want not in flat:
                bad.append("ONE-SITE: %s `%s...` must copy %s (`%s`)" % (rel, sig, why, want))
        if not re.search(r"c\.jitterSign=[^;*]+;", flat) or not re.search(r"c\.renderW=[^;]+;c\.renderH=[^;]+;", flat) \
                or not re.search(r"c\.reset=[^;]+;", flat):
            bad.append("ONE-SITE: %s `%s...` must state the jitter sign, the render size and the upscaler's reset" % (rel, sig))
        for m in re.finditer(r"glm::|\bmvScale\b|jitterPx\[[01]\]\*|toRowMajor", body):
            bad.append("ONE-SITE: %s `%s...` converts (%s): the camera goes over in the renderer's own terms; each "
                       "generator derives its own" % (rel, sig, m.group(0)))

    # ---- INPUTS ----
    ren = _code(_raw(ctx, REN))
    fi = re.sub(r"\s+", "", _body(ren, "bool LLCleanRendererVK::frameGenInputs("))
    d = fi.find("if(mRrRanLast&&mRr[0])")
    f = fi.find("elseif(!mRrRanLast&&mFsrRanLast&&mFsr[0])")
    if d < 0 or f < d or "&mRr[0]->inDepth()" not in fi[d:f] or "&mRr[0]->inMotion()" not in fi[d:f] \
            or "&mFsr[0]->inDepth()" not in fi[f:] or "&mFsr[0]->inMotion()" not in fi[f:]:
        bad.append("INPUTS: %s frameGenInputs must answer DLSS's inputs when DLSS ran (`if (mRrRanLast && mRr[0])`), "
                   "else FSR's when FSR ran (`else if (!mRrRanLast && mFsrRanLast && mFsr[0])`)" % REN)
    if "if(mViewCount!=1||mOffscreen)returnfalse;" not in fi:
        bad.append("INPUTS: %s frameGenInputs answers only for the main window's single view (`if (mViewCount != 1 || "
                   "mOffscreen) return false;`)" % REN)
    fsr = _code(_raw(ctx, FSR_UP))
    for im in ("mInMotion", "mInDepth"):
        if not re.search(r"\bmk\s*\(\s*%s\s*,[^;]*VK_IMAGE_USAGE_TRANSFER_SRC_BIT\s*\)\s*;" % im, fsr):
            bad.append("INPUTS: %s must make %s with VK_IMAGE_USAGE_TRANSFER_SRC_BIT (frame generation copies it)" % (FSR_UP, im))
    return bad


def run(ctx, result):
    missing = [p for p in (GEN_H, DLSS_H, DLSS, TEST) if not os.path.exists(ctx.path(p))]
    if missing:
        result.fail("missing: %s" % ", ".join(missing))
        result.summary = "files missing"
        return
    bad = source_rules(ctx)
    for b in bad:
        result.fail(b)
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: frame generation generator tests not run")
        if not bad:
            result.summary = "source rules hold; tests not run"
        return
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "test_framegen_generator")
        c = subprocess.run([cxx, "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-missing-field-initializers",
                            "-pthread", "-I" + ctx.path(STUBS), "-I" + ctx.path(VK),
                            "-I" + ctx.path(VK + "/third_party/vulkan/include"), ctx.path(DLSS), ctx.path(TEST), "-o", exe],
                           capture_output=True, text=True)
        if c.returncode:
            result.fail("compile failed:\n" + c.stdout + c.stderr)
            return
        try:
            r = subprocess.run([exe], capture_output=True, text=True, timeout=120)
        except subprocess.TimeoutExpired:
            result.fail("the test timed out after 120 s")
            return
        if r.returncode:
            result.fail(r.stdout + r.stderr)
            return
    if not bad:
        result.summary = "tests pass, NEUTRAL / OWNER / ONE-SITE / INPUTS hold"
