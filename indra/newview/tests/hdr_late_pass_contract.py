#!/usr/bin/env python3
"""The late-pass rule for HDR display output (#1451). No viewer link or GPU.

With HDR on, the world image after the tonemap holds linear light (relative to paper white until
the final pass, absolute nits after it), while every SDR draw goes into the UI layer the display
encode composites at paper white. So, from the tonemap to the present:
  - markers and annotations (selection, overlays, debug views, HUDs, UI) draw into the UI layer,
    in uiLayerFormat(), with their SDR shaders;
  - effects on the world image are display-aware: a display recipe that carries the linear
    specialization (rebuilt by the live SDR <-> HDR switch), or an SDR twin bound only while the
    output is not linear.

Guard 1 (formats): every use of the world / display format (mColorFormat) in the renderer's
sources is an image, a comparison or bookkeeping, or carries an "hdr-late:" annotation in a //
comment on its line or the comment lines right above it. Clauses, separated by ";":
    linear P   P is registered as a display recipe with the linear specialization (a makePipe
               call with linearDisplay true), its format following the switch
    twin P     P is registered (format follows the switch) and bound only as the SDR branch of
               "displayLinearOutput() ? Q : P", Q a linear recipe
    allow ...  not a live display pipeline, or a deliberate exception: the reason, in words
Guard 2 (targets): in recordWorld, after the tonemap, every use of the world image (colorView)
is one of the reviewed ones below; a new draw into it must be added here deliberately.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[3]
VKGFX = ROOT / "indra/newview/vkgfx"
SOURCES = sorted(p for p in VKGFX.glob("*.cpp"))
TEXT = {p: p.read_text() for p in SOURCES}
world = TEXT[VKGFX / "llcleanrenderervk.cpp"]
rt = TEXT[VKGFX / "llcleanrtvk.cpp"]
failures = []


def fail(msg):
    failures.append(msg)


def body(source: str, signature: str) -> str:
    start = source.index(signature)
    opening = source.index("{", start)
    depth = 0
    for pos in range(opening, len(source)):
        if source[pos] == "{":
            depth += 1
        elif source[pos] == "}":
            depth -= 1
            if depth == 0:
                return source[opening + 1:pos]
    raise AssertionError(f"unterminated function: {signature}")


def code_of(line: str) -> str:
    return line.split("//", 1)[0]


def split_args(text: str):
    out, depth, cur = [], 0, ""
    for ch in text:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur.strip()); cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


# ---- the display recipes: pipe -> (linear spec?, format follows the switch?) ----
REGISTER = re.compile(r"registerDisplayPipeline\(\s*(\w+)\s*,\s*&?([\w\[\]]+)\s*,(.*?)\);", re.S)
recipes = {}
for text in TEXT.values():
    for m in REGISTER.finditer(text):
        a = split_args(m.group(3))   # name, vs, vs bytes, fs, fs bytes, linearOnly, linearSpecId, formatTarget[, uiLayer]
        spec = a[6] if len(a) > 6 else "UINT32_MAX"
        fmt = a[7] if len(a) > 7 else "true"
        recipes.setdefault(m.group(2), []).append((spec != "UINT32_MAX", fmt == "true"))
# The AA lambda registers each makePipe destination: linear when the call's linearDisplay is true.
make_pipe = world[world.index("auto makePipe = [&]("):]
make_pipe = make_pipe[:make_pipe.index("\n    };")]
assert "linearDisplay ? 0u : UINT32_MAX" in make_pipe and "if (linearDisplay) d.fragSpec=&specialization;" in make_pipe
for m in re.finditer(r"makePipe\(([^;]*?)\);", world, re.S):
    a = split_args(re.sub(r"//[^\n]*", "", m.group(1)))   # vs, vs bytes, fs, fs bytes, format, out, name, linearDisplay, outputTarget
    if len(a) == 9 and a[5].startswith("&"):
        dest = re.sub(r"\[\w+\]", "", a[5][1:])
        if a[7] == "true" or a[8] == "true":
            recipes.setdefault(dest, []).append((a[7] == "true", True))


def base(pipe: str) -> str:
    return re.sub(r"\[\w+\]", "", pipe)


def is_linear(pipe: str) -> bool:
    regs = recipes.get(base(pipe), []) + recipes.get(pipe, [])
    return bool(regs) and all(lin and fmt for lin, fmt in regs)


# ---- Guard 1: every world-format use is classified ----
EXEMPT = [
    (re.compile(r"createImage2D\("), "an image (target), not a pipeline"),
    (re.compile(r"[!=]=\s*(mR\s*(\.|->)\s*)?mColorFormat\b|\bmColorFormat\s*[!=]="), "a comparison"),
    (re.compile(r"^\s*mColorFormat\s*=[^=]"), "the member's own assignment"),
    (re.compile(r"^\s*previous\.format\s*=\s*mColorFormat;"), "the switch's bank bookkeeping"),
    (re.compile(r"<<\s*unsigned\(mColorFormat\)"), "a log line"),
]
sites = 0
for path, text in TEXT.items():
    lines = text.splitlines()
    for i, line in enumerate(lines):
        code = code_of(line)
        if not re.search(r"\bmColorFormat\b", code):
            continue
        if any(rx.search(code) for rx, _ in EXEMPT):
            continue
        sites += 1
        where = f"{path.name}:{i + 1}"
        note = line.split("//", 1)[1] if "//" in line else ""
        j = i - 1
        while j >= 0 and lines[j].strip().startswith("//"):
            note = lines[j].strip()[2:] + " " + note
            j -= 1
        m = re.search(r"hdr-late:\s*(.*)", note)
        if not m:
            fail(f"{where}: the world format with no hdr-late annotation: {line.strip()}")
            continue
        window = "\n".join(lines[i:i + 40])
        for clause in [c.strip() for c in m.group(1).split(";") if c.strip()]:
            kind, _, rest = clause.partition(" ")
            rest = rest.strip()
            if kind == "allow":
                if len(rest.split()) < 4:
                    fail(f"{where}: 'allow' needs a reason in words")
            elif kind in ("linear", "twin"):
                pipe = rest.split()[0] if rest else ""
                if not pipe or not re.search(r"\b" + re.escape(base(pipe)) + r"\b", window):
                    fail(f"{where}: {kind} {pipe!r} is not built from this line")
                    continue
                if kind == "linear":
                    if not is_linear(pipe):
                        fail(f"{where}: {pipe} is not a display recipe with the linear specialization")
                else:
                    if not recipes.get(base(pipe)) or not all(fmt for _, fmt in recipes[base(pipe)]):
                        fail(f"{where}: twin {pipe} is not a display recipe whose format follows the switch")
                    for t in TEXT.values():
                        for bl in t.splitlines():
                            c = code_of(bl)
                            if not re.search(r"\b" + re.escape(base(pipe)) + r"\b", c):
                                continue
                            if not re.search(r"bindGraphicsPipeline\(|vkCmdBindPipeline\(|fullscreen\(", c):
                                continue
                            g = re.search(r"displayLinearOutput\(\)\s*\?\s*([\w\[\]]+)\s*:\s*" + re.escape(base(pipe)) + r"\b", c)
                            if not g or not is_linear(g.group(1)):
                                fail(f"{where}: twin {pipe} is bound outside 'displayLinearOutput() ? <linear> : {pipe}': {bl.strip()}")
            else:
                fail(f"{where}: unknown hdr-late clause {clause!r}")
assert sites >= 10, sites   # the scan sees the renderer's sites (a rename must not empty it)

# ---- Guard 2: the world image's users after the tonemap ----
record = body(world, "void LLCleanRendererVK::recordWorld(VkCommandBuffer cmd")
late = record[record.index("recordTonemap(cmd, toneView"):]
REVIEWED = [
    "recordFinal(cmd, colorView",                                   # display recipe, linear spec
    "const VkImageView lateLayer = mDisplayUiLayer ? mUiLayerView : colorView;",   # (#1536: while a layer exists)
    "if (selLayer == colorView)",
    "overlayScope(colorView, wireframe",                             # SDR: the world is the swapchain
    "overlayScope(colorView, true, [&]() {});",                      # HDR wireframe: the clear only
    "mWorldOverlayHook(cmd, colorView",                              # the context hands gVK the UI layer in HDR
    "hudOwn ? mHudTarget.color : (mDisplayUiLayer ? mUiLayerView : colorView)",
    "mWorldTextViewsHook(cmd, colorView",                            # VR only: active XR forces SDR output
]
for line in late.splitlines():
    c = code_of(line)
    if re.search(r"\bcolorView\b", c) and not any(r in c for r in REVIEWED):
        fail(f"recordWorld after the tonemap: an unreviewed use of the world image: {line.strip()}")
hook = TEXT[VKGFX / "llvulkancontext.cpp"]
hook = hook[hook.index("mImpl->mWorld->mWorldOverlayHook ="):]
hook = hook[:hook.index("};")]
if "uiOverlayView ? uiOverlayView : color" not in hook:
    fail("the world overlay hook no longer redirects gVK's overlays into the UI layer")
rule = record[:record.index("recordTonemap(cmd, toneView")]
if "LATE-PASS RULE" not in rule or "hdr_late_pass_contract.py" not in rule:
    fail("recordWorld: the late-pass rule is not documented ahead of the tonemap")

# ---- RT debug views (RenderVKDebugView 13/14/15/18/30): SDR false colour over the whole frame.
# In HDR they go into the UI layer (alpha 1: the world is hidden under them as in SDR), in its
# format, rebuilt when it changes (a live switch) and never bound in another.
if "mRT->recordDebug(cmd, lateLayer," not in late:
    fail("RT debug views do not draw into the late layer")
if "mRT->recordDebug(" not in late or "const VkImageView lateLayer" not in late \
        or late.index("const VkImageView lateLayer") > late.index("mRT->recordDebug("):
    fail("lateLayer is not defined before the RT debug views")
if "const VkImageView selLayer = lateLayer;" not in late:
    fail("the selection lines and the RT debug views do not share the late layer")
debug = body(rt, "void LLCleanRTVK::recordDebug(")
if "ensureDebugPipe(mR.uiLayerFormat())" not in debug:
    fail("recordDebug does not (re)build its pipeline in the UI layer's format")
if "ensureDebugPipe(" in debug and debug.index("ensureDebugPipe(") > debug.index("bindGraphicsPipeline(cmd, mDebugPipe)"):
    fail("recordDebug binds before ensuring the pipeline's format")
ensure = body(rt, "bool LLCleanRTVK::ensureDebugPipe(") if "bool LLCleanRTVK::ensureDebugPipe(" in rt else ""
for need in ("mDebugFormat == format", "mR.retirePipeline(mDebugPipe)", "d.colorFormat = format;", "mDebugFormat = format;"):
    if need not in ensure:
        fail(f"ensureDebugPipe: missing {need!r}")
shader = (ROOT / "indra/vkclean/shaders/clean_rt_debug.frag").read_text()
writes = re.findall(r"\boutColor\s*=\s*vec4\((.*)\);", shader)
if "discard" in shader or not writes or any(split_args(w)[-1] != "1.0" for w in writes):
    fail("clean_rt_debug.frag: every pixel must be written with alpha 1 (it covers the world in the layer)")

# ---- RLVa sphere and the Lightbox final pass: display recipes with the linear specialization ----
sphere_init = body(world, "bool LLCleanRendererVK::initSphere()")
if "if (mDisplayLinear) d.fragSpec=&displaySpec;" not in sphere_init:
    fail("initSphere: the sphere is not built with the linear specialization in HDR")
sphere_rec = body(world, "void LLCleanRendererVK::recordSphere(")
if "p.display[0] = displayLinearOutput() ? 1.f : 0.f;" not in sphere_rec:
    fail("recordSphere: the frame's units are not pushed (an SDR frame under HDR blends as SDR)")
sphere_frag = (ROOT / "indra/vkclean/shaders/clean_rlv_sphere.frag").read_text()
for need in ("layout(constant_id=0) const bool kDisplayLinear=false;", '#include "clean_display_common.glsl"',
             "fragColor = sphereBlend(fragColor, SPHERE_PARAMS.rgb, effectStrength);"):
    if need not in sphere_frag:
        fail(f"clean_rlv_sphere.frag: missing {need!r}")
final_frag = (ROOT / "indra/vkclean/shaders/clean_final.frag").read_text()
main = body(final_frag, "void main()")
look = body(final_frag, "vec3 applyLook(") if "vec3 applyLook(" in final_frag else ""
order = ["applyVignette(", "applyADSVignette(", "applyCVDCompensation(", "applyFilmGrain(", "applyDither(", "applyPreview("]
pos = [look.find(f) for f in order]
if -1 in pos or pos != sorted(pos) or "applyWipeSeam(" in look:
    fail("clean_final.frag: applyLook must hold the look effects, in order, and not the wipe seam")
for f in order:
    if f in main:
        fail(f"clean_final.frag: main() calls {f} outside applyLook (it would see linear values in HDR)")
if "finalLook(" not in main or main.find("finalLook(") > main.find("applyWipeSeam("):
    fail("clean_final.frag: main() must apply finalLook before the wipe seam")
on = body(final_frag, "bool lookOn()") if "bool lookOn()" in final_frag else ""
# lookOn() mirrors each effect's own early-out, so skipping the conversion never skips an effect.
for gate, fn in (("uVignetteAmount > 0.0", "vec3 applyVignette("), ("fx.ads.x > 0.0", "vec3 applyADSVignette("),
                 ("uCompensateMode > 0 && uCompensateAmount > 0.0", "vec3 applyCVDCompensation("),
                 ("uGrainAmount > 0.0", "vec3 applyFilmGrain("), ("uDitherAmount > 0.0", "vec3 applyDither("),
                 ("uPreviewMode != 0", "vec3 applyPreview(")):
    if gate not in on:
        fail(f"clean_final.frag: lookOn() does not test {gate!r}")
early = {"vec3 applyVignette(": "if (uVignetteAmount <= 0.0)", "vec3 applyADSVignette(": "if (fx.ads.x <= 0.0)",
         "vec3 applyCVDCompensation(": "if (uCompensateMode <= 0 || uCompensateAmount <= 0.0)",
         "vec3 applyFilmGrain(": "if (uGrainAmount <= 0.0)", "vec3 applyDither(": "if (uDitherAmount <= 0.0)",
         "vec3 applyPreview(": "if (uPreviewMode == 0)"}
for fn, test in early.items():
    if test not in body(final_frag, fn):
        fail(f"clean_final.frag: {fn} no longer early-outs on {test!r}: update lookOn()")

if failures:
    for f in failures:
        print("FAIL:", f)
    raise SystemExit(1)
print(f"PASS: {sites} world-format uses classified; the late passes after the tonemap are the reviewed ones")
print("PASS: RT debug views draw into the UI layer in its format; the RLV sphere and the Lightbox look are display-aware")
