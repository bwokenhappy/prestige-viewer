"""Regression guard for #1536 SDR frame generation.

Compiles scripts/check/test_framegen_sdr.cpp with the host C++ compiler and runs it: the SDR composite
(vkdisplay::composeLayerSdr, the mirror of the shader's displayComposeUiSdr) leaves a world texel with
no label and no interface unchanged, matches drawing straight onto the 8-bit world (exact for one
8-bit draw, within one step otherwise; two stacked translucent draws within two), and composes labels then the interface in order.

Source rules:
  COMPOSE  clean_display_common.glsl's displayComposeUiSdr is exactly the mirrored expression
           (min(ui.rgb + world * (1 - clamp(ui.a, 0, 1)), 1)); the output shader's encoding 5 branch
           returns before any transfer function, dither or extended-range call (pq(, pqToNits(,
           encodeExt(, decodeExt(, outputNoise(); composeOverSdr returns the world texel itself for a
           pixel no label and no interface reaches; the mirror in vk_display_hdr.h is that expression.
  ENCODE   VkDisplayEncode has an SDR-layer mode (Mode::SdrLayer, prepare(..., sdrLayer)): it builds
           neither the FP16 composite pipeline nor the HDR10 three-output pipeline; the real picture
           (recordLayerComposite) and each generated one (recordFgComposite) draw through one private
           helper given the encoding (5 in the SDR layer, 4 in HDR10), so they cannot diverge; the HDR
           encode (record) refuses in the SDR-layer mode; fgHudlessFormat(target) is HDR10's constant
           for HDR10 and the target's own format otherwise; the sampler stays NEAREST (texel-exact).
  SLOTS    the present slots take the generator's image format from VkDisplayEncode::fgHudlessFormat
           (the chain's format and colour space) for the admission, the images made and the fit test --
           llvkframegenpresent.cpp never names kFgHudlessFormat; in SDR the slot's world image has
           kSdrWorldUsage (colour attachment, sampled, transfer src and dst: the world renders, clears
           and is captured through it as through a swapchain image); a device that cannot write that
           format as a storage image is refused, with its own text, before any image is made.
  RENDERER the renderer's two meanings of HDR are separate: mDisplayLinear (the world is linear light)
           and mDisplayUiLayer (a UI layer exists: HDR, or #1536 SDR on a frame generation chain;
           linear implies it). Every layer-sense site -- uiLayerFormat(), the UI-layer recipes'
           format and their layer-opaque specialization (bank build, RezFX commit, HUD init), the
           late layer, the HUD target and its guard, the selection guard -- reads the UI-layer flag;
           displayLinearOutput() is unchanged; the pipeline bank's key (pending request, cache hit,
           unchanged-key commit) compares the UI layer too.
  LAYER    the frame loop (LLVulkanContext::renderFrame). One local says the frame is an SDR-layer frame:
           `const bool sdrLayer = !mImpl->mHdrOutputActive && llfg::sdrLayer(..., mSwapchain.frameGenImages())`
           -- the only llfg::sdrLayer( call in renderFrame -- and `const bool layer = mImpl->mHdrOutputActive ||
           sdrLayer` is what the UI target's format, beginFrameToView's uiLayer argument, the renderer's
           setUiLayer and the overlay view hang on. The world target (view, worldImg, the UI backdrop) is
           the slot's world image (fgHudless(fgSlot)) and the UI target the slot's layer (fgUi(fgSlot)) in an
           SDR-layer frame, each a three-way select HDR / SDR layer / direct whose last arm is today's
           direct value. The backbuffer view (imgView) is written only by the direct path's select, the
           HDR encode and its fallback, and recordLayerComposite -- called once, under `if (sdrLayer)`, its
           result kept. fgWant needs `layer` (not HDR); the generator's feature is sized from the swapchain
           in SDR and made in VkDisplayEncode::fgHudlessFormat(the chain) (never kFgHudlessFormat, never
           the HDR frame's size in SDR); fillGenInputs's colorHdr is HDR10's test only. The output
           transaction carries the UI layer: mOutputRequestUiLayer, compared by outputDiffers, the pending
           request and the commit after a rebuild, and passed to requestDisplayOutput; the world renderer
           is configured with it when it is created. The SDR-layer encoder is prepared (SdrLayer mode) and
           released only after a drainFrameGen() that succeeded; sdrLayerValidForCurrentChain() -- the
           present slots ready, the encoder in SdrLayer mode for the chain, the renderer's committed UI
           layer -- gates every SDR-layer frame before anything of it is buffered.
  CAPTURE  snapshots on an SDR frame generation chain keep the interface and HUDs: capture points 2
           and 3 of an SDR-layer frame compose world and layer into the backbuffer before the copy
           (recordLayerComposite with its own capture descriptor set -- never the frame's final set,
           which the same command buffer binds later) and point the copy at it, then back at the
           world image; the offscreen path makes its transient layer whenever the renderer has a UI
           layer (not only with linear output) and composes a world + HUD shot with the same
           composite in the SDR layer.
  TEXT     nothing the player or a setting reads says frame generation needs HDR: the FrameGen tooltip,
           RenderVKFrameGen's and RenderVKFrameGenMovingLabels' comments, blockText and the refusal
           literals of the frame loop and frame generation; the tooltip and the comment name SDR and HDR10.
  RELEASE  (final review) the SDR-layer encoder is destroyed only with the present thread drained AND the last
           rebuild's device wait proven (frames recorded before a failed wait may still bind it); a renderer
           that rejects the SDR UI-layer bank refuses frame generation's images (the wish drops to a normal
           chain with a reason) instead of retrying the same bank every 2 s.
"""
import os
import re
import shutil
import subprocess
import tempfile

NAME = "framegen-sdr"
STAGE = "source"
DESCRIPTION = "SDR frame generation: the composite of world, labels and interface (tests + rules)"
GUARDS = ("Generated or real SDR frames whose interface looks different from SDR without frame "
          "generation, a world re-encoded or dithered on its way to the screen, labels in the wrong order.")

TEST = "scripts/check/test_framegen_sdr.cpp"
COMMON = "indra/vkclean/shaders/clean_display_common.glsl"
LABELS = "indra/vkclean/shaders/clean_display_labels.glsl"
OUTPUT = "indra/vkclean/shaders/clean_display_output.frag"
MIRROR = "indra/newview/vkgfx/vk_display_hdr.h"
ENC_H = "indra/newview/vkgfx/vk_display_encode.h"
ENC_CPP = "indra/newview/vkgfx/vk_display_encode.cpp"
PT_H = "indra/newview/vkgfx/llvkframegenpresent.h"
PT_CPP = "indra/newview/vkgfx/llvkframegenpresent.cpp"
REN_H = "indra/newview/vkgfx/llcleanrenderervk.h"
REN_CPP = "indra/newview/vkgfx/llcleanrenderervk.cpp"
CTX_CPP = "indra/newview/vkgfx/llvulkancontext.cpp"
STORAGE_REFUSAL = "this GPU cannot write generated frames in the display's SDR format"

SDR_EXPR = "min(ui.rgb+encodedWorld*(1.0-clamp(ui.a,0.0,1.0)),vec3(1.0))"
MIRROR_EXPR_RE = r"std::fmin\(\s*ui\[i\]\s*\+\s*world\[i\]\s*\*\s*\(\s*1\.f\s*-\s*a\s*\)\s*,\s*1\.f\s*\)"


def _read(ctx, rel):
    p = ctx.path(rel)
    if not os.path.exists(p):
        return None
    with open(p, encoding="utf-8", errors="replace") as f:
        return f.read()


def _code(text):
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
    return re.sub(r"//[^\n]*", "", text)


def _body(code, signature):
    at = code.find(signature)
    if at < 0:
        return None
    open_at = code.find("{", at)
    depth = 0
    for i in range(open_at, len(code)):
        if code[i] == "{":
            depth += 1
        elif code[i] == "}":
            depth -= 1
            if depth == 0:
                return code[open_at:i + 1]
    return None


def _flat(text):
    return re.sub(r"\s+", "", text)


def source_rules(ctx):
    bad = []
    common = _code(_read(ctx, COMMON) or "")
    fn = _body(common, "vec3 displayComposeUiSdr(vec3 encodedWorld, vec4 ui)")
    if fn is None:
        bad.append("COMPOSE: %s needs vec3 displayComposeUiSdr(vec3 encodedWorld, vec4 ui)" % COMMON)
    elif _flat(fn) != "{return" + SDR_EXPR + ";}":
        bad.append("COMPOSE: displayComposeUiSdr must be exactly `return %s;` (the SDR swapchain's blend, "
                   "mirrored by vkdisplay::composeLayerSdr)" % SDR_EXPR)
    labels = _code(_read(ctx, LABELS) or "")
    over = _body(labels, "vec3 composeOverSdr(")
    if over is None:
        bad.append("COMPOSE: %s needs composeOverSdr (labels then the interface, encoded space)" % LABELS)
    else:
        f = _flat(over)
        if not re.search(r"if\(!reach&&all\(equal\(ui,vec4\(0\.0\)\)\)\)return(encoded|world|encodedWorld);", f):
            bad.append("COMPOSE: composeOverSdr must return the world texel itself where no label and no "
                       "interface reach")
        for call in ("pq(", "pqToNits(", "encodeExt(", "decodeExt(", "displayComposeUi("):
            if re.search(r"(?<![A-Za-z0-9_])" + re.escape(call), over):
                bad.append("COMPOSE: composeOverSdr calls %s (no transfer math in SDR)" % call)
        if "displayComposeUiSdr(" not in over:
            bad.append("COMPOSE: composeOverSdr must blend with displayComposeUiSdr")
    output = _code(_read(ctx, OUTPUT) or "")
    main = _body(output, "void main()")
    m = re.search(r"if\s*\(\s*encoding\s*==\s*5u\s*\)\s*\{", main or "")
    if not m:
        bad.append("COMPOSE: %s main() needs an encoding 5 branch (SDR frame generation)" % OUTPUT)
    else:
        branch = _body(main[m.start():], "{")
        if branch is None or "return;" not in branch or "composeOverSdr(" not in branch:
            bad.append("COMPOSE: encoding 5 must compose with composeOverSdr and return")
        else:
            for call in ("pq(", "pqToNits(", "encodeExt(", "decodeExt(", "outputNoise(", "displayComposeUi("):
                if re.search(r"(?<![A-Za-z0-9_])" + re.escape(call), branch):
                    bad.append("COMPOSE: encoding 5 calls %s (no transfer math, no dither in SDR)" % call)
        # (nothing before the branch may touch the colour: it is the first thing main() does after encoding 4)
        before = main[:m.start()]
        for call in ("pq(", "encodeExt(", "outputNoise(", "displaySdrWorldNits("):
            if re.search(r"(?<![A-Za-z0-9_])" + re.escape(call), before):
                bad.append("COMPOSE: %s runs before the encoding 5 branch" % call)
    mirror = _code(_read(ctx, MIRROR) or "")
    mb = _body(mirror, "composeLayerSdr(")
    if mb is None or not re.search(MIRROR_EXPR_RE, mb):
        bad.append("COMPOSE: %s needs composeLayerSdr computing std::fmin(ui[i] + world[i] * (1.f - a), 1.f) "
                   "with a = the clamped coverage" % MIRROR)
    _rule_encode(ctx, bad)
    _rule_slots(ctx, bad)
    _rule_renderer(ctx, bad)
    _rule_layer(ctx, bad)
    _rule_capture(ctx, bad)
    _rule_text(ctx, bad)
    _rule_release(ctx, bad)
    return bad


def _call_args(code, at):
    """The argument text of the call whose `(` is the first one at or after `at`."""
    open_at = code.find("(", at)
    depth = 0
    for i in range(open_at, len(code)):
        if code[i] == "(":
            depth += 1
        elif code[i] == ")":
            depth -= 1
            if depth == 0:
                return code[open_at + 1:i]
    return ""


def _rule_layer(ctx, bad):
    cx = _code(_read(ctx, CTX_CPP) or "")
    rf = _body(cx, "bool LLVulkanContext::renderFrame(")
    if rf is None:
        bad.append("LAYER: %s has no LLVulkanContext::renderFrame" % CTX_CPP)
        return
    f = _flat(rf)
    fx = _flat(cx)
    # The one predicate.
    locals_ = re.findall(r"constboolsdrLayer=([^;]*);", f)
    if len(locals_) != 1 or not locals_[0].startswith("!mImpl->mHdrOutputActive&&llfg::sdrLayer(") \
            or "mSwapchain.frameGenImages()" not in locals_[0] or ".output" not in locals_[0]:
        bad.append("LAYER: renderFrame needs exactly one `const bool sdrLayer = !mImpl->mHdrOutputActive && "
                   "llfg::sdrLayer(frameGenContext().output, mImpl->mSwapchain.frameGenImages());`")
    if f.count("llfg::sdrLayer(") != 1:
        bad.append("LAYER: renderFrame calls llfg::sdrLayer( once (the local every SDR-layer branch tests)")
    if f.count("constboollayer=mImpl->mHdrOutputActive||sdrLayer;") != 1:
        bad.append("LAYER: renderFrame needs `const bool layer = mImpl->mHdrOutputActive || sdrLayer;`")
    # The targets: three-way selects, the last arm today's direct value.
    for want, why in (
            ("VkImageViewview=mImpl->mHdrOutputActive?mImpl->mDisplayFrame.linear().view:sdrLayer?"
             "mImpl->mFgPresent.fgHudless(fgSlot).view:imgView;", "the world target's view"),
            ("constVkImageViewuiView=mImpl->mHdrOutputActive?mImpl->mDisplayFrame.uiLayer().view:sdrLayer?"
             "mImpl->mFgPresent.fgUi(fgSlot).view:view;", "the UI target's view"),
            ("VkImageworldImg=mImpl->mHdrOutputActive?mImpl->mDisplayFrame.linear().image:sdrLayer?"
             "mImpl->mFgPresent.fgHudless(fgSlot).image:img;", "the world image"),
            ("target.colorImage=mImpl->mHdrOutputActive?mImpl->mDisplayFrame.uiLayer().image:sdrLayer?"
             "mImpl->mFgPresent.fgUi(fgSlot).image:img;", "the UI recorder's colour image"),
            ("target.colorFormat=layer?VK_FORMAT_R8G8B8A8_UNORM:mImpl->mSwapchain.format();",
             "the UI recorder's colour format"),
            ("target.sceneImage=mImpl->mHdrOutputActive?mImpl->mDisplayFrame.linear().image:sdrLayer?"
             "mImpl->mFgPresent.fgHudless(fgSlot).image:img;", "the UI backdrop (the world alone)"),
            ("target.sceneFormat=mImpl->mHdrOutputActive?VK_FORMAT_R16G16B16A16_SFLOAT:mImpl->mSwapchain.format();",
             "the UI backdrop's format (the chain's in SDR)"),
            ("constVkFormatuiFormat=layer?VK_FORMAT_R8G8B8A8_UNORM:mImpl->mSwapchain.format();", "the UI's format"),
            ("mImpl->mWorld->setUiLayer((layer&&!vrWorld)?uiView:VK_NULL_HANDLE);", "the renderer's UI layer"),
            ("constVkImageViewuiOverlayView=!layer?VK_NULL_HANDLE:uiView;", "the overlays' view")):
        if want not in f:
            bad.append("LAYER: renderFrame: %s must read `%s`" % (why, want))
    m = re.search(r"gVK\.beginFrameToView\s*\(", rf)
    if not m or not re.search(r",\s*layer\s*$", _call_args(rf, m.start())):
        bad.append("LAYER: gVK.beginFrameToView's uiLayer argument is `layer`")
    # The backbuffer: only the direct select, the HDR encode / fallback, and the SDR composite write it.
    uses = [m.start() for m in re.finditer(r"\bimgView\b", rf)]
    comp = [m.start() for m in re.finditer(r"recordLayerComposite\s*\(", rf)]
    enc = [m.start() for m in re.finditer(r"mDisplayEncode\.record\s*\(", rf)]
    spans = [(c, c + len(_call_args(rf, c)) + 40) for c in comp + enc]
    # (#1536 Task 8: a snapshot's composite at a capture point passes capture = true -- its own descriptor set;
    # the frame's picture is the one call without it)
    comp = [c for c in comp if not re.search(r",\s*true\s*$", _call_args(rf, c))]
    for u in uses:
        head = rf[max(0, u - 40):u]
        if re.search(r"const\s+VkImageView\s+$", head) or re.search(r":\s*$", head) \
                or re.search(r"fallbackColour\.imageView\s*=\s*$", head) or any(a <= u <= b for a, b in spans):
            continue
        bad.append("LAYER: renderFrame uses the backbuffer view imgView outside the direct select, the HDR encode and "
                   "recordLayerComposite (line %d of renderFrame)" % (rf.count("\n", 0, u) + 1))
    if len(comp) != 1:
        bad.append("LAYER: renderFrame records the SDR picture once (one recordLayerComposite( call without capture)")
    else:
        head = rf[max(0, comp[0] - 60):comp[0]]
        if not re.search(r"=\s*(?:\w+\s*(?:\.|->)\s*)*$", head):
            bad.append("LAYER: recordLayerComposite('s result is kept")
        before = rf[:comp[0]]
        at = before.rfind("if (sdrLayer)")
        blk = _body(rf[at:], "if (sdrLayer)") if at >= 0 else None
        if blk is None or "recordLayerComposite(" not in blk:
            bad.append("LAYER: recordLayerComposite( sits in an `if (sdrLayer) { ... }` block")
        args = _flat(_call_args(rf, comp[0]))
        if "mImpl->mFgPresent.fgHudless(fgSlot)" not in _flat(blk or "") or "imgView" not in args:
            bad.append("LAYER: the SDR picture composes the slot's world image and layer into the backbuffer (imgView)")
    # Generation: gates and the generator's feature.
    if "if(fgWant&&(!doWorld||vrWorld||!layer||snapshotFrame))" not in f:
        bad.append("LAYER: fgWant is dropped on a frame without a layer (`!layer`), not on a frame without HDR")
    if "kFgHudlessFormat" in rf:
        bad.append("LAYER: renderFrame names kFgHudlessFormat; the generator's format is "
                   "VkDisplayEncode::fgHudlessFormat(the chain)")
    if not re.search(r"constVkFormat(\w+)=VkDisplayEncode::fgHudlessFormat\(", f):
        bad.append("LAYER: renderFrame takes the generator's format from VkDisplayEncode::fgHudlessFormat(the chain)")
    else:
        name = re.search(r"constVkFormat(\w+)=VkDisplayEncode::fgHudlessFormat\(", f).group(1)
        # (#1544: the feature's size is one llfg::GenSize; its colour and scratch formats are that format)
        # (#1544 Task 6: the scratch image's format is the generator's -- llfg::scratchFormat: FSR has no BGRA8
        # storage format -- and the inputs and the feature are made in one place, prepareFrameGen)
        gs = re.search(r"(\w+)\.colorFormat=%s;" % name, f)
        if not gs or not re.search(r"%s\.scratchFormat=llfg::scratchFormat\(fgd\.generator\(\),%s\);" % (gs.group(1), name), f):
            bad.append("LAYER: the generator's size (llfg::GenSize) takes its colorFormat from %s and its scratchFormat "
                       "from llfg::scratchFormat(fgd.generator(), %s)" % (name, name))
        cm = re.search(r"mImpl->prepareFrameGen\s*\(", rf)
        if not cm or not gs or not re.search(r"(?:^|,)\s*%s\s*(?:,|$)" % gs.group(1), _call_args(rf, cm.start())):
            bad.append("LAYER: mImpl->prepareFrameGen( gets the chain's generator format (%s, in its llfg::GenSize)" % name)
        pg = _body(cx, "bool prepareFrameGen(") or ""
        for call in ("featureFits", "prepareFeature"):
            m2 = re.search(r"\.%s\s*\(" % call, pg)
            if not m2 or not re.search(r"(?:^|,)\s*size\s*$", _call_args(pg, m2.start())):
                bad.append("LAYER: prepareFrameGen's %s( gets its llfg::GenSize (size)" % call)
    if not re.search(r"constVkExtent2D\w+=sdrLayer\?mImpl->mSwapchain\.extent\(\):mImpl->mDisplayFrame\.linear\(\)\.extent;", f):
        bad.append("LAYER: the generator's feature is sized from the swapchain in SDR (the HDR frame does not exist there): "
                   "`const VkExtent2D x = sdrLayer ? mImpl->mSwapchain.extent() : mImpl->mDisplayFrame.linear().extent;`")
    if "linear.extent" in rf:
        bad.append("LAYER: renderFrame sizes something from `linear.extent` (0x0 in SDR)")
    if not re.search(r"constboolhdr10=mImpl->mSwapchain\.displayStatus\(\)\.encoding==vkdisplay::Encoding::Hdr10;", f) \
            or not re.search(r"fillGenInputs\([^;]*,hdr10,", f):
        bad.append("LAYER: fillGenInputs's colorHdr is the chain's HDR10 test (0 in SDR)")
    ev = _body(cx, "static llfg::GenInputs fillGenInputs(") or ""
    if "e.colorHdr=hdr10;" not in _flat(ev):
        bad.append("LAYER: fillGenInputs sets e.colorHdr from its hdr10 argument only")
    if "paperWhiteNits=sdrLayer?0.f:" not in f or "peakNits=sdrLayer?0.f:" not in f:
        bad.append("LAYER: the job's paper white and peak are 0 in SDR")
    # The output transaction.
    for want, why in (
            ("boolmOutputRequestUiLayer=false;", "Impl::mOutputRequestUiLayer"),
            ("active.uiLayer!=targetUiLayer", "outputDiffers compares the UI layer"),
            ("mImpl->mOutputRequestUiLayer==targetUiLayer", "the pending request's key compares the UI layer"),
            ("mImpl->mOutputRequestUiLayer=targetUiLayer;", "the request keeps the UI layer"),
            ("preview.peakNits,requestGeneration,targetUiLayer);", "requestDisplayOutput gets the UI layer"),
            ("actualUiLayer==mImpl->mOutputRequestUiLayer", "the commit after a rebuild compares the UI layer"),
            ("mDisplayPaperWhiteNits,mDisplayPeakNits,sdrLayer);", "a new world renderer is configured with the SDR layer")):
        if want not in fx:
            bad.append("LAYER: %s (%s)" % (why, want))
    # The encoder and the last-line gate.
    for sig, first in (("bool prepareSdrLayerEncode(", r"mDisplayEncode\.prepare\s*\("),
                       ("void releaseSdrLayerEncode(", r"mDisplayEncode\.destroy\s*\(")):
        b = _body(cx, sig)
        if b is None:
            bad.append("LAYER: %s needs %s...)" % (CTX_CPP, sig))
            continue
        g = re.search(r"!\s*drainFrameGen\s*\(\s*\)\s*\)\s*return\b", b)
        use = re.search(first, b)
        if not g or not use or use.start() < g.end():
            bad.append("LAYER: %s...) returns when drainFrameGen() fails, before it touches the live encoder" % sig)
    prep = _body(cx, "bool prepareSdrLayerEncode(") or ""
    pm = re.search(r"mDisplayEncode\.prepare\s*\(", prep)
    if not pm or not re.search(r",\s*true\s*$", _call_args(prep, pm.start())):
        bad.append("LAYER: prepareSdrLayerEncode prepares the encoder in the SDR-layer mode (prepare(..., true))")
    gate = _body(cx, "bool sdrLayerValidForCurrentChain() const")
    if gate is None:
        bad.append("LAYER: %s needs bool sdrLayerValidForCurrentChain() const" % CTX_CPP)
    else:
        for need in ("mFgPresent.ready()", "VkDisplayEncode::Mode::SdrLayer", "mDisplayEncode.validForTarget(", ".uiLayer"):
            if need not in gate:
                bad.append("LAYER: sdrLayerValidForCurrentChain must test %s" % need)
    g = f.find("sdrLayer&&!mImpl->sdrLayerValidForCurrentChain()")
    b = f.find("gVK.beginFrameToView(")
    if g < 0 or b < 0 or g > b:
        bad.append("LAYER: renderFrame withholds an SDR-layer frame (`sdrLayer && !mImpl->sdrLayerValidForCurrentChain()`) "
                   "before the UI is buffered")


def _rule_renderer(ctx, bad):
    h = _flat(_code(_read(ctx, REN_H) or ""))
    cpp = _code(_read(ctx, REN_CPP) or "")
    f = _flat(cpp)
    for want, why in (
            ("VkFormatuiLayerFormat()const{returnmDisplayUiLayer?VK_FORMAT_R8G8B8A8_UNORM:mColorFormat;}",
             "uiLayerFormat() reads mDisplayUiLayer"),
            ("booldisplayLinearOutput()const{returnmDisplayLinear&&!mSdrFrame&&(!mOffscreen||mOffscreenHdrPair);}",
             "displayLinearOutput() unchanged"),
            ("boolmDisplayUiLayer=false;", "bool mDisplayUiLayer"),
            ("boolmDisplayRequestUiLayer=false;", "bool mDisplayRequestUiLayer"),
            ("booluiLayer=false;", "DisplayOutputState::uiLayer and DisplayPipelineBank::uiLayer")):
        if want not in h:
            bad.append("RENDERER: %s: %s" % (REN_H, why))
    if h.count("booluiLayer=false;") < 3:   # (the recipe's own, the state's, the bank's)
        bad.append("RENDERER: DisplayOutputState and DisplayPipelineBank each carry bool uiLayer")
    for gone, why in (
            ("mDisplayLinear?mUiLayerView", "the late layer / HUD target keyed on linear output"),
            ("!mDisplayLinear||mUiLayerView", "the HUD target guard keyed on linear output"),
            ("mDisplayLinear&&!selLayer", "the selection guard keyed on linear output"),
            ("mDisplayLinear?&kHudLayerOpaqueSpec", "the HUD layer-opaque spec keyed on linear output"),
            ("mDisplayRequestLinear?&kHudLayerOpaqueSpec", "the RezFX HUD spec keyed on linear output"),
            ("uiLayer&&mDisplayRequestLinear", "a UI-layer recipe's format keyed on linear output")):
        if gone in f:
            bad.append("RENDERER: %s still has %s (%s)" % (REN_CPP, gone, why))
    for want, why in (
            ("constVkImageViewlateLayer=mDisplayUiLayer?mUiLayerView:colorView;", "lateLayer"),
            ("(mDisplayUiLayer?mUiLayerView:colorView)", "hudColor"),
            ("hudOwnTarget||!mDisplayUiLayer||mUiLayerView!=VK_NULL_HANDLE", "hudTargetReady"),
            ("gd.fragSpec=mDisplayUiLayer?&kHudLayerOpaqueSpec:nullptr;", "the HUD pipelines' init"),
            ("r.uiLayer&&mDisplayRequestUiLayer?VK_FORMAT_R8G8B8A8_UNORM:mDisplayRequestFormat", "the bank build's format"),
            ("mDisplayPipeRecipes[i].uiLayer&&mDisplayRequestUiLayer?VK_FORMAT_R8G8B8A8_UNORM", "the RezFX commit's format"),
            ("rez.desc.fragSpec=mDisplayRequestUiLayer?&kHudLayerOpaqueSpec:nullptr;", "the RezFX commit's spec"),
            ("r.uiLayer?mDisplayRequestUiLayer:mDisplayRequestLinear", "the bank build's specialization (22: layer-opaque, 0: linear)"),
            ("mDisplayPipeCache.uiLayer==mDisplayRequestUiLayer", "the cache hit compares the UI layer"),
            ("mDisplayRequestUiLayer==mDisplayUiLayer", "the unchanged-key commit compares the UI layer"),
            ("mDisplayRequestUiLayer==requestUiLayer", "a pending request's key compares the UI layer"),
            ("incoming.uiLayer=mDisplayRequestUiLayer;", "the incoming bank's key"),
            ("previous.uiLayer=mDisplayUiLayer;", "the previous bank's key"),
            ("mDisplayUiLayer=mDisplayRequestUiLayer;", "the commit"),
            ("constboolrequestUiLayer=request.enabled||uiLayer;", "linear output implies a UI layer")):
        if want not in f:
            bad.append("RENDERER: %s needs %s (%s)" % (REN_CPP, want, why))


def _rule_slots(ctx, bad):
    h = _code(_read(ctx, PT_H) or "")
    raw = _read(ctx, PT_CPP) or ""
    cpp = _code(raw)
    if "kFgHudlessFormat" in cpp:
        bad.append("SLOTS: %s names kFgHudlessFormat; the format is VkDisplayEncode::fgHudlessFormat(chain)" % PT_CPP)
    m = re.search(r"kSdrWorldUsage\s*=\s*([^;]*);", h)
    if not m or not all(b in m.group(1) or (b != "VK_IMAGE_USAGE_TRANSFER_DST_BIT" and "kInputUsage" in m.group(1))
                        for b in ("VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT", "VK_IMAGE_USAGE_SAMPLED_BIT",
                                  "VK_IMAGE_USAGE_TRANSFER_SRC_BIT", "VK_IMAGE_USAGE_TRANSFER_DST_BIT")):
        bad.append("SLOTS: %s needs kSdrWorldUsage = colour attachment | sampled | transfer src | transfer dst" % PT_H)
    prep = _body(cpp, "bool LLVKFrameGenPresent::prepare(")
    mk = _body(cpp, "bool LLVKFrameGenPresent::makeImages(")
    fit = _body(cpp, "bool LLVKFrameGenPresent::imagesFit(")
    if prep is None or mk is None or fit is None:
        bad.append("SLOTS: %s needs prepare, makeImages and imagesFit" % PT_CPP)
        return
    if not re.search(r"VkDisplayEncode::fgHudlessFormat\s*\(", prep):
        bad.append("SLOTS: prepare must take the generator's format from VkDisplayEncode::fgHudlessFormat(the chain)")
    if not re.search(r"bytes\(\s*hudless\s*,", prep):
        bad.append("SLOTS: the admission counts the world images in the chain's generator format")
    # (#1544 Task 6: the scratch image in the generator's format -- ONE expression, used by the storage check,
    # the admission, makeImages and imagesFit -- and the generator's own images in the same admission)
    sc = re.findall(r"constVkFormatscratch=([^;]*);", _flat(prep))
    if sc != ["llfg::scratchFormat(LLVKFrameGen::instance().generator(),hudless)"]:
        bad.append("SLOTS: prepare takes the scratch format once: `const VkFormat scratch = "
                   "llfg::scratchFormat(LLVKFrameGen::instance().generator(), hudless);`")
    if not re.search(r"set\.shared\s*=\s*bytes\(\s*scratch\s*,", prep):
        bad.append("SLOTS: the admission counts the scratch image in the scratch format (set.shared = bytes(scratch, ...))")
    gb = re.search(r"const\s+uint64_t\s+(\w+)\s*=\s*LLVKFrameGen::instance\(\)\.generatorBytes\([^;]*\bscratch\b", prep)
    if not gb or not re.search(r"set\.generatorBytes\s*=\s*%s\s*;" % gb.group(1), prep) \
            or not re.search(r"imagesFit\([^;]*\)\s*\|\|\s*%s\s*!=\s*mImagesGeneratorBytes" % gb.group(1), prep):
        bad.append("SLOTS: the admission counts the generator's own images (const uint64_t x = "
                   "LLVKFrameGen::instance().generatorBytes(..., scratch); set.generatorBytes = x), and another figure "
                   "(a live switch) admits the set again (`!imagesFit(...) || x != mImagesGeneratorBytes`)")
    if not re.search(r"makeImages\([^;]*\bhudless\b[^;]*\bscratch\b", prep) or \
            not re.search(r"imagesFit\([^;]*\bhudless\b[^;]*\bscratch\b", prep):
        bad.append("SLOTS: makeImages and imagesFit get the same generator and scratch formats the admission used")
    if "kSdrWorldUsage" not in mk or "VK_IMAGE_USAGE_STORAGE_BIT" not in mk:
        bad.append("SLOTS: makeImages: the SDR world image with kSdrWorldUsage, the scratch with STORAGE")
    if not re.search(r"make\(\s*mScratch\s*,\s*scratch\s*,", mk):
        bad.append("SLOTS: makeImages makes the scratch image in the scratch format")
    if "mImagesHudless" not in fit or "mImagesScratch" not in fit:
        bad.append("SLOTS: imagesFit compares the generator and scratch formats the images were made in")
    if not re.search(r"vkGetPhysicalDeviceFormatProperties\([^;]*,\s*scratch\s*,", prep):
        bad.append("SLOTS: the storage refusal tests the scratch format (the image the generator writes), not hudless")
    st = re.search(r"VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT", prep)
    mi = re.search(r"\bmakeImages\s*\(", prep)
    if not st or not mi or st.start() > mi.start():
        bad.append("SLOTS: prepare checks the format's STORAGE_IMAGE support before it makes any image")
    if STORAGE_REFUSAL not in raw:
        bad.append('SLOTS: the storage refusal reads "%s"' % STORAGE_REFUSAL)


def _rule_encode(ctx, bad):
    h = _code(_read(ctx, ENC_H) or "")
    cpp = _code(_read(ctx, ENC_CPP) or "")
    fh = _flat(h)
    for want, why in (("enumclassMode:uint8_t{Hdr,SdrCapture,SdrLayer};", "enum class Mode : uint8_t { Hdr, SdrCapture, SdrLayer }"),
                      ("Modemode()const", "Mode mode() const"),
                      ("staticVkFormatfgHudlessFormat(VkSurfaceFormatKHRtarget)", "static VkFormat fgHudlessFormat(VkSurfaceFormatKHR target)"),
                      ("boolframeGenReady()const", "bool frameGenReady() const"),
                      ("boolrecordLayerComposite(", "bool recordLayerComposite(...)"),
                      ("boolsdrLayer=false);", "prepare(..., bool sdrLayer = false)")):
        if want not in fh:
            bad.append("ENCODE: %s needs %s" % (ENC_H, why))
    fmt = _body(h, "static VkFormat fgHudlessFormat(") or _body(cpp, "VkFormat VkDisplayEncode::fgHudlessFormat(")
    if fmt is None or not re.search(r"Encoding::Hdr10\s*\?\s*kFgHudlessFormat\s*:\s*target\.format", fmt):
        bad.append("ENCODE: fgHudlessFormat must be `encodingOf(target) == Encoding::Hdr10 ? kFgHudlessFormat : target.format`")
    prep = _body(cpp, "bool VkDisplayEncode::prepare(")
    if prep is None:
        bad.append("ENCODE: %s has no VkDisplayEncode::prepare" % ENC_CPP)
    else:
        for pipe in ("mCompositePipeline", "mFgPipeline"):
            for m in re.finditer(r"\b%s\s*=\s*createGraphicsPipeline" % pipe, prep):
                cond = prep[:m.start()]
                cond = cond[cond.rindex("if"):] if "if" in cond else ""
                if "Encoding::Sdr" not in cond and "Encoding::Hdr10" not in cond:
                    bad.append("ENCODE: %s is built only for HDR output (its if names the encoding)" % pipe)
        if "VK_FILTER_NEAREST" not in prep or "VK_FILTER_LINEAR" in prep:
            bad.append("ENCODE: the encode's sampler stays NEAREST (the SDR composite is texel-exact)")
    helpers = set()
    for sig in ("bool VkDisplayEncode::recordLayerComposite(", "bool VkDisplayEncode::recordFgComposite("):
        b = _body(cpp, sig)
        if b is None:
            bad.append("ENCODE: %s needs %s...)" % (ENC_CPP, sig))
            continue
        if "vkCmdDraw" in b or "vkCmdPushConstants" in b:
            bad.append("ENCODE: %s...) draws itself; it must go through the shared composite helper" % sig)
        calls = set(re.findall(r"\b(drawComposite)\s*\(", b))
        if not calls:
            bad.append("ENCODE: %s...) must draw through drawComposite(..., encoding, ...)" % sig)
        helpers |= calls
    fg = _body(cpp, "bool VkDisplayEncode::recordFgComposite(") or ""
    if not re.search(r"mMode\s*==\s*Mode::SdrLayer\s*\?\s*5u\s*:\s*4u", fg):
        bad.append("ENCODE: recordFgComposite's encoding is the mode's: mMode == Mode::SdrLayer ? 5u : 4u")
    lay = _body(cpp, "bool VkDisplayEncode::recordLayerComposite(") or ""
    if not re.search(r"mMode\s*!=\s*Mode::SdrLayer", lay) or not re.search(r"\b5u\b", lay):
        bad.append("ENCODE: recordLayerComposite runs only in the SDR-layer mode, with encoding 5")
    rec = _body(cpp, "bool VkDisplayEncode::record(") or ""
    if not re.search(r"mMode\s*==\s*Mode::SdrLayer[^;]*\)\s*return\s+false", rec):
        bad.append("ENCODE: record (the HDR encode) must refuse in the SDR-layer mode")
    if helpers and _body(cpp, "void VkDisplayEncode::drawComposite(") is None:
        bad.append("ENCODE: %s needs the private helper void VkDisplayEncode::drawComposite(...)" % ENC_CPP)


def run(ctx, result):
    cxx = os.environ.get("CXX") or shutil.which("c++") or shutil.which("clang++")
    if not cxx:
        result.warn("no C++ compiler: SDR frame generation tests not run")
    else:
        with tempfile.TemporaryDirectory() as tmp:
            exe = os.path.join(tmp, "test_framegen_sdr")
            c = subprocess.run([cxx, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror", "-I" + ctx.path("indra/newview"),
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
    result.summary = "%d problem(s)" % len(bad) if bad else "composite tests pass, COMPOSE holds"


def _rule_capture(ctx, bad):
    cpp = _code(_read(ctx, CTX_CPP) or "")
    h = _flat(_code(_read(ctx, ENC_H) or ""))
    ecpp = _code(_read(ctx, ENC_CPP) or "")
    if "boolcapture=false);" not in h or "mCaptureSets" not in h:
        bad.append("CAPTURE: recordLayerComposite(..., bool capture = false) with its own descriptor sets (mCaptureSets)")
    lay = _body(ecpp, "bool VkDisplayEncode::recordLayerComposite(") or ""
    if not re.search(r"capture\s*\?\s*mCaptureSets\[slot\]", lay):
        bad.append("CAPTURE: recordLayerComposite draws a capture with mCaptureSets[slot]")
    m = re.search(r"auto\s+recordCapturePoint\s*=\s*\[&\]\s*\([^)]*\)\s*\{", cpp)
    lam = _body(cpp[m.start():], "{") if m else None
    if not lam:
        bad.append("CAPTURE: renderFrame's recordCapturePoint lambda not found")
    else:
        if not re.search(r"\bsdrLayer\b", lam) or "recordLayerComposite(" not in lam:
            bad.append("CAPTURE: recordCapturePoint must compose world and layer in an SDR-layer frame")
        else:
            call = lam[lam.index("recordLayerComposite("):]
            call = call[:call.index(";")]
            if not re.search(r",\s*true\s*\)\s*$", call):
                bad.append("CAPTURE: the capture composite passes capture = true (its own descriptor set)")
            if not re.search(r"setSwapImage\(\s*img\s*,", lam):
                bad.append("CAPTURE: the copy reads the composed backbuffer (setSwapImage(img, ...))")
            after = lam[lam.index("recordCaptureAt("):]
            if not re.search(r"setSwapImage\(\s*\w*[Ww]orld\w*\.image\s*,", after):
                bad.append("CAPTURE: after the copy the swap image is the slot's world image again")
    off = _body(cpp, "bool LLVulkanContext::renderOffscreen(") or ""
    mk = re.search(r"if\s*\(([^{]*)\)\s*\{[^}]*offLayer\s*=", off, re.S)
    if not mk or "uiLayer" not in mk.group(1):
        bad.append("CAPTURE: renderOffscreen makes its transient layer whenever the renderer has a UI layer "
                   "(displayOutputState().uiLayer)")
    if "recordLayerComposite(" not in off:
        bad.append("CAPTURE: renderOffscreen composes an SDR-layer world + HUD shot with recordLayerComposite")
    # (final review: the shot's colour target is sampled by that composite -- SAMPLED whenever a layer exists)
    mi = re.search(r"img\s*=\s*mImpl->mDevice\.createImage2D\(([^;]*)\);", off)
    if not mi or not re.search(r"displayOutputState\(\)\.uiLayer\s*\?\s*VK_IMAGE_USAGE_SAMPLED_BIT", mi.group(1)):
        bad.append("CAPTURE: renderOffscreen's colour target is SAMPLED whenever the renderer has a UI layer "
                   "(the HUD shot's composite samples it)")


PREFS_XUI = "indra/newview/skins/default/xui/en/floater_preferences_graphics_advanced.xml"
SETTINGS = "indra/newview/app_settings/settings.xml"
FG_CPP = "indra/newview/vkgfx/llvkframegen.cpp"
POLICY = "indra/newview/vkgfx/llvkframegenpolicy.h"
HDR_ONLY = re.compile(r"HDR10 output for now|needs HDR output|HDR10 output and DLSS|HDR output on Wayland|"
                      r"HDR picture could not|HDR10 only", re.I)


def _setting_comment(xml, key):
    m = re.search(r"<key>%s</key>\s*<map>\s*<key>Comment</key>\s*<string>(.*?)</string>" % re.escape(key), xml, re.S)
    return m.group(1) if m else None


def _rule_text(ctx, bad):
    xui = _read(ctx, PREFS_XUI) or ""
    m = re.search(r'name="FrameGen"[^>]*?tool_tip="([^"]*)"|tool_tip="([^"]*)"[^>]*?name="FrameGen"', xui, re.S)
    tip = (m.group(1) or m.group(2)) if m else None
    settings = _read(ctx, SETTINGS) or ""
    texts = {"the FrameGen tooltip": tip,
             "RenderVKFrameGen's comment": _setting_comment(settings, "RenderVKFrameGen"),
             "RenderVKFrameGenMovingLabels' comment": _setting_comment(settings, "RenderVKFrameGenMovingLabels")}
    for what, text in texts.items():
        if text is None:
            bad.append("TEXT: %s not found" % what)
        elif HDR_ONLY.search(text) or (what != "RenderVKFrameGenMovingLabels' comment" and "HDR output" in text):
            bad.append("TEXT: %s still says frame generation needs HDR" % what)
    for what in ("the FrameGen tooltip", "RenderVKFrameGen's comment"):
        text = texts[what] or ""
        if not (re.search(r"\bSDR\b", text) and "HDR10" in text):
            bad.append("TEXT: %s names the outputs it runs with (SDR and HDR10)" % what)
    for rel in (POLICY, FG_CPP, CTX_CPP):
        raw = _read(ctx, rel) or ""
        for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', raw):
            if HDR_ONLY.search(lit):
                bad.append("TEXT: %s has the literal \"%s\"" % (rel, lit[:80]))


def _rule_release(ctx, bad):
    cpp = _code(_read(ctx, CTX_CPP) or "")
    rel = _body(cpp, "void releaseSdrLayerEncode()") or ""
    if "drainFrameGen()" not in rel or "deviceIdled()" not in rel:
        bad.append("RELEASE: releaseSdrLayerEncode destroys only after drainFrameGen() and mSwapchain.deviceIdled()")
    at = cpp.find("Display output candidate rejected")
    blk = cpp[at:at + 2500] if at >= 0 else ""
    if not re.search(r"targetUiLayer\s*&&\s*!targetLinear[^;{]*\)\s*\{?[^}]*imagesRefused\(", blk, re.S):
        bad.append("RELEASE: a rejected SDR UI-layer candidate (targetUiLayer && !targetLinear) refuses frame "
                   "generation's images")
    elif "mFgRefusalRebuild = true" not in blk[blk.find("imagesRefused("):blk.find("imagesRefused(") + 300]:
        bad.append("RELEASE: that refusal marks the next rebuild as its own (mFgRefusalRebuild), or the rebuild re-arms it")
