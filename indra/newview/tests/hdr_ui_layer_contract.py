#!/usr/bin/env python3
"""Contract for the HDR UI layer (#1451): with HDR display output the UI is drawn with the SDR
pipelines into its own RGBA8 layer (premultiplied colour, coverage in alpha), and the display
encode composites it over the world at paper white. No viewer link or GPU.

The blend part compiles the production alpha-factor rule (vk_pipeline_ui.h) into a small
fixed-function blend simulation and checks, per blend mode the UI and the world overlays use,
that drawing into the layer and compositing gives what SDR shows (the same draws blended
straight onto the sRGB-encoded world).
"""
import argparse
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
context = (ROOT / "indra/newview/vkgfx/llvulkancontext.cpp").read_text()
frame_h = (ROOT / "indra/newview/vkgfx/vk_display_frame.h").read_text()
frame_cpp = (ROOT / "indra/newview/vkgfx/vk_display_frame.cpp").read_text()
encode_h = (ROOT / "indra/newview/vkgfx/vk_display_encode.h").read_text()
encode_cpp = (ROOT / "indra/newview/vkgfx/vk_display_encode.cpp").read_text()
output = (ROOT / "indra/vkclean/shaders/clean_display_output.frag").read_text()
common = (ROOT / "indra/vkclean/shaders/clean_display_common.glsl").read_text()
ui_dir = ROOT / "indra/newview/vkgfx/llrendervk"
pipeline_h = (ui_dir / "vk_pipeline_ui.h").read_text()
pipeline_cpp = (ui_dir / "vk_pipeline_ui.cpp").read_text()
renderer = (ui_dir / "llrendervk.cpp").read_text()
renderer_h = (ui_dir / "llrendervk.h").read_text()


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
                return source[opening + 1 : pos]
    raise AssertionError(f"unterminated function: {signature}")


# ---- The UI targets the layer, in its SDR format, only while HDR output is active ----
frame = body(context, "bool LLVulkanContext::renderFrame(")
# (#1536: `layer` -- HDR output, or SDR on a frame generation chain, whose interface layer has the same format)
assert "const bool layer = mImpl->mHdrOutputActive || sdrLayer;" in frame, "the UI layer follows HDR output"
assert "const VkFormat uiFormat = layer ? VK_FORMAT_R8G8B8A8_UNORM" in frame, "HDR UI target format"
assert "VK_FORMAT_R16G16B16A16_SFLOAT : mImpl->mSwapchain.format();\n        gVK.beginFrameToView" not in frame
assert "mDisplayFrame.uiLayer().view" in frame, "the UI renders into the layer"
assert "gVK.setFrameColorView(uiView)" in frame
assert "mDisplayFrame.beginUiLayer(cmd, true)" in frame, "the layer is cleared once per frame"
assert frame.index("mDisplayFrame.beginUiLayer(cmd, true)") < frame.index("recordWorld(cmd")
final_encode = frame.index("VkDisplayEncode::Content::CompleteDisplay")
assert frame.index("mDisplayFrame.sampleUiLayer(cmd)") < final_encode
assert "mDisplayFrame.uiLayer().view" in frame[frame.rindex("mDisplayEncode.record(", 0, final_encode):final_encode]
# HDR shots with the HUD (point 2) or the UI (point 3) read the world with the layer composited.
capture = body(frame, "auto recordCapturePoint = [&](uint32_t point")
assert "VkDisplayEncode::Content::CompositeNits" in capture and "W->setSwapImage(frame.capture().image" in capture
assert "recordCapturePoint(2," in frame and "recordCapturePoint(3," in frame
# Ordinary overlays / pre-HUD text retain the UI-layer path. FG world geometry
# instead enters world color with its own depth/motion; labels keep their atlas.
# Brace-match the lambda: a nested vector initializer also ends in `};`.
hook = body(frame, "mWorldOverlayHook =")
assert "uiOverlayView ? uiOverlayView : color" in hook and "depth, depthImage" in hook
assert "gVK.prepareFrameGenOverlay(slot, overlay)" in hook
assert "gVK.recordFrameGenOverlay(c, slot, overlay, depth, w, h)" in hook
assert "overlay.whiteNits = white" in hook
assert "white=(mImpl->mHdrOutputActive ? mDisplayPaperWhiteNits : 0.0f)" in frame

# ---- The layer exists only with HDR output, is budgeted, and is freed with the frame ----
prepare = body(frame_cpp, "bool VkDisplayFrame::prepare(")
assert prepare.index("if (!enabled) { destroy();return true; }") < prepare.index("VK_FORMAT_R8G8B8A8_UNORM")
assert "mUiLayer=context.createImage2D(" in prepare
assert "mUiLayer" in body(frame_cpp, "void VkDisplayFrame::destroy()")
assert "allocBytes(mUiLayer)" in body(frame_cpp, "uint64_t VkDisplayFrame::residentBytes()")
assert "mUiLayer.view" in body(frame_cpp, "bool VkDisplayFrame::validForExtent(")
assert "const GpuImage& uiLayer() const" in frame_h
assert "void beginUiLayer(VkCommandBuffer cmd,bool clear) const" in frame_h
assert "void sampleUiLayer(VkCommandBuffer cmd) const" in frame_h
assert "No duplicate UI layers" not in frame_h

# ---- The encode composites the layer; one pass, no extra full-screen draw ----
assert "VkImageView uiLayer" in encode_h
record = body(encode_cpp, "bool VkDisplayEncode::record(")
assert "set.bindingCount=kBindings" in body(encode_cpp, "bool VkDisplayEncode::prepare(")
assert "static constexpr uint32_t kBindings = 4;" in encode_h
assert "layer=uiLayer ? uiLayer : input" in record and "inputs{input,layer,labelAtlas ? labelAtlas : layer}" in record
assert "VK_DESCRIPTOR_TYPE_STORAGE_BUFFER" in body(encode_cpp, "bool VkDisplayEncode::prepare(")
# #1517 Task 7e: moving labels (binding 2: the slot's label atlas, binding 3: its label list) are drawn
# BETWEEN the world and the interface, only with the 0x200 flag (frames that generate); the world the
# generator gets (hudless) and the interface layer it is shown under (outUi) never contain them.
labels_glsl = (ROOT / "indra/vkclean/shaders/clean_display_labels.glsl").read_text()
flat = output.replace(" ", "")
assert '#include"clean_display_labels.glsl"' in flat
assert "uniformsampler2DuLabelAtlas;" in flat and "binding=2" in flat
assert re.search(r"binding\s*=\s*3\s*\)\s*readonly\s+buffer", labels_glsl)
assert "texelFetch(uLabelAtlas" not in flat, "the atlas is read only in composeLabels"
assert labels_glsl.count("texelFetch(uLabelAtlas") == 1
assert "*8u" in labels_glsl.replace(" ", ""), "GpuLabel is 8 words (32 bytes)"
wn = flat.index("constvec3worldNits=nits;")
cl = flat.index("composeLabels(worldNits,")
ui = flat.index("displayComposeUi(labeled,texture(uUiLayer,v_uv),display.paperWhiteNits)")
assert wn < cl < ui, "world -> labels -> interface"
assert "(display.encoding&0x200u)!=0u" in flat[flat.rindex("if(", 0, cl):cl]
assert "hudless=pq(to2020*worldNits)" in flat
assert "(labelAtlas ? 0x200u : 0u)" in record
# Final review B-I1 (#1517): the generated frames get the RAW interface layer (a per-slot RGBA8 copy of the
# UI layer's texels, never anything derived from the real frame's world) and compose it exactly as the real
# frame does -- in nits, at paper white: world -> labels (at the frame's moment) -> interface. A translucent
# window over a moving world shows no ghost of the real frame's world in the generated ones.
assert "outUi=texture(uUiLayer,v_uv);" in flat, "outUi is the UI layer's texel"
assert flat.count("outUi=") == 4 and flat.count("outUi=vec4(0.0);") == 3, "outUi: the layer's texel, or nothing"
for gone in ("encodedNoLabels", "labelsHere", "uiEnc", "labeledEnc", "labelsOverEncoded"):
    assert gone not in output and gone not in labels_glsl, gone
assert "static constexpr VkFormat kFgUiFormat = VK_FORMAT_R8G8B8A8_UNORM;" in encode_h
assert "RGBA16F" not in encode_h[encode_h.index("kFgHudlessFormat ="):encode_h.index("kFgUiFormat =")]
gen = flat[flat.index("if(encoding==4u){"):flat.index("return;", flat.index("if(encoding==4u){"))]
assert "composeOverEncoded(world,texture(uUiLayer,v_uv),(display.encoding&0x200u)!=0u," in gen
assert gen.index("composeOverEncoded(") < gen.index("outColor=") or "outColor=vec4(composeOverEncoded(" in gen
assert "display.paperWhiteNits,display.peakNits)" in gen
for pq_space in ("1.0-", "*world", "+world", "layer.", ".a)", "pq(", "displayComposeUi("):
    assert pq_space not in gen, "encoding 4 composes only through composeOverEncoded (in nits): " + pq_space
lflat = labels_glsl.replace(" ", "")
over = lflat[lflat.index("vec3composeOverEncoded(vec3encoded,vec4ui,boollabels,"):]
over = over[:over.index("\n}")]
assert "if(!reach&&all(equal(ui,vec4(0.0))))returnencoded;" in over, \
    "pixels without interface and without labels keep the world texel bit for bit"
assert "constboolreach=labels&&labelTile(pixel,idxBase)!=0u;" in over, "the label list is read only with 0x200"
d = over.index("to709*pqToNits(encoded)")
l = over.index("composeLabels(nits,pixel,t,paperWhiteNits,false)")
u = over.index("nits=clamp(displayComposeUi(nits,ui,paperWhiteNits),0.0,peak);")
e = over.index("pq(to2020*nits)")
assert d < l < u < e, "decode -> labels -> interface (displayComposeUi, nits) -> encode"
assert over.count("pq(") == 1 and over.count("pqToNits(") == 1, "one decode, one encode"
# #1536 SDR frame generation (encoding 5): every picture -- the real one too -- composes the world, the labels and
# the interface in the SDR encoded space (displayComposeUiSdr: the SDR swapchain's blend), never in nits.
sdr = flat[flat.index("if(encoding==5u){"):flat.index("return;", flat.index("if(encoding==5u){"))]
assert "outColor=vec4(composeOverSdr(texture(uLinearNits,v_uv).rgb,texture(uUiLayer,v_uv)," in sdr
for nits_space in ("paperWhiteNits", "peakNits", "pq(", "displayComposeUi(", "outputNoise("):
    assert nits_space not in sdr, "encoding 5 stays in the encoded space: " + nits_space
sover = lflat[lflat.index("vec3composeOverSdr(vec3encoded,vec4ui,boollabels,"):]
sover = sover[:sover.index("\n}")]
assert "if(!reach&&all(equal(ui,vec4(0.0))))returnencoded;" in sover
assert sover.index("composeLabels(c,pixel,t,0.0,true)") < sover.index("displayComposeUiSdr(c,ui)"), "labels, then the interface"
assert over[u:].count("ui") == 1, "the interface is composed once, in nits, never in the encoded space"
# The real frame keeps its exact compose (world -> labels -> interface), with no special case.
enc1 = flat[flat.index("}elseif(encoding==1u){"):flat.index("}elseif(encoding==2u){")]
assert "vec3encoded=pq(to2020*nits);" in enc1 and "outColor=vec4(encoded,1.0);" in enc1
assert enc1.count("encoded=") == 2, "encoded: pq of the composite and its clamp (+ the dither) -- nothing else"
# Every composite gets the encode's paper white and peak (the interface needs them, labels or not).
comp = body(encode_cpp, "bool VkDisplayEncode::recordFgComposite(")
assert "const uint32_t encoding=(mMode==Mode::SdrLayer ? 5u : 4u)|(labels ? 0x200u : 0u);" in comp
assert "drawComposite(cmd,set,encoding,phase,paperWhiteNits,peakNits,target,extent);" in comp
assert "const Push push{encoding,paperWhiteNits,peakNits,0.f,phase};" in body(encode_cpp, "void VkDisplayEncode::drawComposite(")
assert "float paperWhiteNits = " not in encode_h and "float peakNits = " not in encode_h, \
    "recordFgComposite: paper white and peak are not optional"
fg_job = context[context.index("fgJob.ui = mImpl->mFgPresent.fgUi(fgSlot).view;"):]
fg_job = fg_job[:fg_job.index("fgJob.showGenerated")]
assert "fgJob.paperWhiteNits = " in fg_job and "fgJob.peakNits = " in fg_job and "if (" not in fg_job
assert "binding=1" in output.replace(" ", "") and "uniform sampler2D uUiLayer" in output
for name in ("encodeExt", "decodeExt", "displayComposeUi"):
    assert re.search(r"^vec3 " + name + r"\(", common, re.M), name

# ---- No UI pipeline carries the linear-nits specialization any more ----
for text, where in ((pipeline_h, "vk_pipeline_ui.h"), (pipeline_cpp, "vk_pipeline_ui.cpp"),
                    (renderer, "llrendervk.cpp"), (renderer_h, "llrendervk.h")):
    assert "displayLinearNits" not in text and "kDisplayLinearNits" not in text, where
    assert "mDisplayPaperWhiteNits" not in text and "setDisplayPaperWhite" not in text, where
assert not (ui_dir / "shaders/ui_display.glsl").exists()
for shader in ("ui.frag", "ui_occl.frag"):
    text = (ui_dir / "shaders" / shader).read_text()
    assert "ui_display.glsl" not in text and "kUiLayerOpaque" in text, shader
assert "gVK.setDisplayPaperWhite" not in context

# ---- Blend modes (Review Focus 1). glAlpha (GL glBlendFunc alpha) is the offscreen render
# targets' only: liveKey sets it inside the offscreen branch, which never targets the layer. ----
live_key = body(renderer, "LLRenderVK::BatchKey LLRenderVK::liveKey(")
offscreen = live_key[live_key.index("if (!mOffStack.empty())"):live_key.index("return k;")]
assert "k.glAlpha = true;" in offscreen and "k.uiLayer = false;" in offscreen
assert live_key.count("k.glAlpha = true;") == 1
assert "k.uiLayer = mFrameUiLayer;" in live_key
assert "d.uiLayer = key.uiLayer;" in body(renderer, "VkPipeline LLRenderVK::pipelineFor(")
assert "uiLayer == o.uiLayer" in renderer_h and "(size_t)k.uiLayer" in renderer_h
assert "colorFmt == o.colorFmt" in renderer_h and "(size_t)k.colorFmt" in renderer_h   # no stale-format reuse
build = body(pipeline_cpp, "VkPipeline createUiPipeline(")
assert "uiAlphaFactors(d, cba.srcAlphaBlendFactor, cba.dstAlphaBlendFactor);" in build

rule = pipeline_h[pipeline_h.index("inline void uiAlphaFactors("):]
rule = rule[:rule.index("\n}\n") + 3]
spec = pipeline_h[pipeline_h.index("inline bool uiLayerOpaque("):]
spec = spec[:spec.index("\n}\n") + 3]
struct = pipeline_h[pipeline_h.index("struct UiPipelineDesc {"):]
struct = struct[:struct.index("};") + 2]

harness = r'''
#include <vulkan/vulkan.h>
#include "vkgfx/vk_display_hdr.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <initializer_list>
''' + struct + "\n" + rule + "\n" + spec + r'''
struct C { float r, g, b, a; };
static float factor(VkBlendFactor f, const C& s, const C& d, int ch) {
    const float sc = ch == 0 ? s.r : ch == 1 ? s.g : ch == 2 ? s.b : s.a;
    const float dc = ch == 0 ? d.r : ch == 1 ? d.g : ch == 2 ? d.b : d.a;
    switch (f) {
    case VK_BLEND_FACTOR_ZERO: return 0.f;
    case VK_BLEND_FACTOR_ONE: return 1.f;
    case VK_BLEND_FACTOR_SRC_COLOR: return sc;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR: return 1.f - sc;
    case VK_BLEND_FACTOR_SRC_ALPHA: return s.a;
    case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA: return 1.f - s.a;
    case VK_BLEND_FACTOR_DST_COLOR: return dc;
    case VK_BLEND_FACTOR_DST_ALPHA: return d.a;
    default: assert(!"factor not simulated"); return 0.f;
    }
}
static bool clipped = false;   // an SDR blend saturated (UNORM): the layer cannot see the world's part
static float sat(float v) { if (v > 1.f) clipped = true; return v < 0.f ? 0.f : (v > 1.f ? 1.f : v); }
static C blend(const UiPipelineDesc& d, C s, const C& dst, bool layer) {
    if (layer && uiLayerOpaque(d)) s.a = 1.f;   // the fragment shader's specialization
    if (!d.blendEnable) return {sat(s.r), sat(s.g), sat(s.b), sat(s.a)};
    VkBlendFactor sa = VK_BLEND_FACTOR_ONE, da = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    uiAlphaFactors(d, sa, da);
    C o;
    o.r = sat(s.r * factor(d.srcFactor, s, dst, 0) + dst.r * factor(d.dstFactor, s, dst, 0));
    o.g = sat(s.g * factor(d.srcFactor, s, dst, 1) + dst.g * factor(d.dstFactor, s, dst, 1));
    o.b = sat(s.b * factor(d.srcFactor, s, dst, 2) + dst.b * factor(d.dstFactor, s, dst, 2));
    o.a = sat(s.a * factor(sa, s, dst, 3) + dst.a * factor(da, s, dst, 3));
    return o;
}
static float enc(float x) { return x <= .0031308f ? 12.92f * x : 1.055f * std::pow(x, 1.f / 2.4f) - .055f; }
int main() {
    struct Mode { bool blend; VkBlendFactor sf, df; const char* name; };
    // The swapchain UI and world-overlay modes: alpha over, premultiplied over, additive
    // (BT_ADD), the hidden selection edges (SRC_COLOR, ONE), the manipulator grid's darken
    // (ZERO, 1-SRC_ALPHA), replace (BT_REPLACE: blending off).
    const Mode modes[] = {
        {true, VK_BLEND_FACTOR_SRC_ALPHA, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, "over"},
        {true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, "premultiplied"},
        {true, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ONE, "additive"},
        {true, VK_BLEND_FACTOR_SRC_COLOR, VK_BLEND_FACTOR_ONE, "colour-weighted additive"},
        {true, VK_BLEND_FACTOR_ZERO, VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA, "darken"},
        {false, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, "replace"},
    };
    const C draws[] = {{.10f, .20f, .55f, .70f}, {.9f, .9f, .9f, .05f}, {.30f, .05f, .00f, .40f},
                       {.05f, .05f, .05f, .00f}, {.6f, .4f, .2f, .35f}};
    for (const Mode& first : modes)
        for (const Mode& second : modes)
            for (float x : {0.f, .02f, .18f, .5f, .9f, 1.f}) {
                UiPipelineDesc a, b;
                a.blendEnable = first.blend; a.srcFactor = first.sf; a.dstFactor = first.df; a.uiLayer = true;
                b.blendEnable = second.blend; b.srcFactor = second.sf; b.dstFactor = second.df; b.uiLayer = true;
                UiPipelineDesc sa = a, sb = b; sa.uiLayer = sb.uiLayer = false;   // the SDR pipelines
                for (const C& s0 : draws) for (const C& s1 : draws) {
                    const float w = enc(x);
                    C sdr{w, w * .9f, w * .8f, 1.f};
                    clipped = false;
                    sdr = blend(sa, s0, sdr, false);
                    // Saturation on the swapchain before a later blend loses the part of the world
                    // that clipped; the layer keeps the world out of the blend, so those cases differ
                    // (additive draws over a bright background only).
                    if (clipped) continue;
                    sdr = blend(sb, s1, sdr, false);
                    C layer{0.f, 0.f, 0.f, 0.f};
                    layer = blend(a, s0, layer, true); layer = blend(b, s1, layer, true);
                    // The composite in sRGB-encoded space, clamped at white as the SDR swapchain is: #1536's
                    // SDR frame generation composite (vkdisplay::composeLayerSdr, displayComposeUiSdr's mirror).
                    const auto o = vkdisplay::composeLayerSdr({w, w * .9f, w * .8f}, {layer.r, layer.g, layer.b, layer.a});
                    assert(std::fabs(o[0] - sdr.r) < 1e-5f && std::fabs(o[1] - sdr.g) < 1e-5f && std::fabs(o[2] - sdr.b) < 1e-5f);
                }
            }
    // The SDR pipelines keep today's alpha factors (the swapchain's alpha stays byte-identical).
    for (const Mode& m : modes) {
        UiPipelineDesc d; d.blendEnable = m.blend; d.srcFactor = m.sf; d.dstFactor = m.df;
        VkBlendFactor sa = VK_BLEND_FACTOR_MAX_ENUM, da = VK_BLEND_FACTOR_MAX_ENUM;
        uiAlphaFactors(d, sa, da);
        assert(sa == VK_BLEND_FACTOR_ONE && da == VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA && !uiLayerOpaque(d));
        d.glAlphaBlend = true;
        uiAlphaFactors(d, sa, da);
        assert(sa == m.sf && da == m.df);
    }
    return 0;
}
'''
# ---- HUD attachments, selection outlines and the rez variants (Task 4, Review Focus 2) ----
world = (ROOT / "indra/newview/vkgfx/llcleanrenderervk.cpp").read_text()
world_h = (ROOT / "indra/newview/vkgfx/llcleanrenderervk.h").read_text()
draw_frag = (ROOT / "indra/vkclean/shaders/clean_draw.frag").read_text()
select_frag = (ROOT / "indra/vkclean/shaders/clean_select.frag").read_text()
pipeline_cpp_world = (ROOT / "indra/newview/vkgfx/vk_pipeline.cpp").read_text()
assert "kHudLinearDisplaySpec" not in world, "HUD / selection / rez pipelines keep no linear-nits spec"
assert "kDisplayHudLinear" not in draw_frag and "kDisplayOverlayLinear" not in select_frag
assert "ssr.display_output.x" not in draw_frag and "ssr.display_output" not in select_frag
# Opaque / masked HUD faces cover the world in the layer: coverage 1 as the very last write,
# after the rez fade (which blending-off SDR ignores too).
assert "hud = vec4(stored * m.base_color_factor.rgb * v_tint.rgb, base.a);" in draw_frag
force = draw_frag.index("if (kHudLayerOpaque && hud) outColor.a = 1.0;")
assert draw_frag.index("outReact = vec4(cov") < force < draw_frag.index("#ifdef CLEAN_OIT_ACCUM", force - 400)
# Task 5: the HUD pipelines build from the placeholder-tier module (#1411 CLEAN_BASIC, ~50k SPIR-V
# words) instead of the full forward FS (~670k words, 45 s per pipeline cold on the user's
# driver: the whole first SDR <-> HDR switch). A HUD fragment returns from shade() before the
# tiers differ; every CLEAN_BASIC site is a lighting / non-HUD / terrain branch (the sixth keeps
# CLEAN_BLEND_HIT, the blend layers' own light, out of the placeholder tier). A new
# CLEAN_BASIC site must re-check that HUD output stays identical, then update this count.
# The seventh excludes the shared blend-visibility helper from the basic tier;
# HUD still returns before either lighting path and never reads shared visibility.
assert draw_frag.count("CLEAN_BASIC") == 7, draw_frag.count("CLEAN_BASIC")
shade_fn = draw_frag[draw_frag.index("vec4 shade(Material m)"):]
assert shade_fn.index("if (!decodeSurface(m, s, alpha, hud)) return hud;") < shade_fn.index("#ifdef CLEAN_BASIC")
for pipe in ("mHudDrawPipe", "mHudMaskPipe", "mHudAlphaPipe"):
    assert re.search(r"registerDisplayPipeline\(gd, &" + pipe + r", \"" + pipe + r"\", kWorldVertSpv,\s*"
                     r"kWorldVertSpv_bytes, kWorldFragBasicSpv, kWorldFragBasicSpv_bytes,", world), pipe
assert "VkFormat uiLayerFormat() const" in world_h
assert "mDisplayUiLayer ? VK_FORMAT_R8G8B8A8_UNORM : mColorFormat" in world_h   # (#1536: while a layer exists)
# The pipelines that draw over the finished world: built in the layer's format in HDR, at init
# and by the live mode switch's bank, and their rez variants with them.
build = body(world, "bool LLCleanRendererVK::startDisplayPipelineBuild()")
assert "r.uiLayer && mDisplayRequestUiLayer ? VK_FORMAT_R8G8B8A8_UNORM : mDisplayRequestFormat" in build
commit = body(world, "bool LLCleanRendererVK::commitDisplayOutput(")
assert "mDisplayPipeRecipes[i].uiLayer && mDisplayRequestUiLayer ? VK_FORMAT_R8G8B8A8_UNORM" in commit
assert "&kHudLayerOpaqueSpec" in commit and "active == &mHudAlphaPipe" not in commit.split("kHudLayerOpaqueSpec")[0][-300:]
for pipe in ("mHudDrawPipe", "mHudMaskPipe", "mHudAlphaPipe", "mSelVisPipe", "mSelHidPipe", "mSelHudVisPipe",
             "mSelHudHidPipe", "mSelRigVisPipe", "mSelRigHidPipe"):
    m = re.search(r"registerDisplayPipeline\((\w+), &" + pipe + r",[^;]*?,\s*(false|true),\s*(22u|UINT32_MAX),\s*true,\s*true\);",
                  world, re.S)
    assert m, pipe + " is a UI-layer display pipeline"
    desc = m.group(1)
    before = world[:m.start()]
    if pipe.startswith("mHud"):
        # The recipe is the state the pipeline is built with (registered after it is set).
        state = before[before.rindex(desc + ".depthTest="):]
        blend = "blend=true" in state
        assert blend == (pipe == "mHudAlphaPipe"), pipe
        # Blending off forces coverage 1 in the layer; the alpha HUD blends its own coverage.
        assert (m.group(3) == "22u") == (pipe != "mHudAlphaPipe"), pipe
    else:
        assert m.group(3) == "UINT32_MAX", pipe
assert "gd.colorFormat=uiLayerFormat();" in world and "sp.colorFormat = uiLayerFormat();" in world
# Blended draws accumulate coverage (ONE, ONE_MINUS_SRC_ALPHA), as the UI does.
std_blend = pipeline_cpp_world[pipeline_cpp_world.index("} else if (desc.blend) {"):]
std_blend = std_blend[:std_blend.index("} else {")]
assert "srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;" in std_blend
assert "dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;" in std_blend
# The HUD pass and the selection lines draw into the layer the context hands over each frame,
# after the world and in today's order: world, selection, overlays / in-world text, HUD, UI.
record = body(world, "void LLCleanRendererVK::recordWorld(VkCommandBuffer cmd")
assert "hudOwn ? mHudTarget.color : (mDisplayUiLayer ? mUiLayerView : colorView)" in record
assert "selLayer" in record and record.index("selLayer") < record.index("mWorldOverlayHook(")
assert record.index("mWorldOverlayHook(") < record.index("hudOwn ? mHudTarget.color")
assert "void setUiLayer(VkImageView view)" in world_h
assert frame.index("mImpl->mWorld->setUiLayer(") < frame.index("recordWorld(cmd")
assert frame.index("mDisplayFrame.beginUiLayer(cmd, true)") < frame.index("mImpl->mWorld->setUiLayer(")
offscreen = body(context, "bool LLVulkanContext::renderOffscreen(")
assert "W->setUiLayer(offLayer.image ? offLayer.view : VK_NULL_HANDLE)" in offscreen   # Task 6: a HUD shot's own layer
assert "W->setUiLayer(VK_NULL_HANDLE)" in offscreen
# The view is this frame's only (the layer is reallocated on resize and freed with HDR).
assert frame.index("mImpl->mWorld->setUiLayer(VK_NULL_HANDLE)") > frame.index("mImpl->mWorld->recordWorld(cmd")
assert context.count("setUiLayer(") >= 4

# ---- Final review fixes ----
# Every selection pipeline bound inside the layer scopes is built in the layer's format: the
# classic style's pipes (selClassicPipes) and the outline style's edge pass (recordSelOutline),
# whose edge pass now also draws into the layer (world and HUD), over the HUDs as on the swapchain.
outline = (ROOT / "indra/newview/vkgfx/llcleanseloutlinevk.cpp").read_text()
classic = body(outline, "const VkPipeline* LLCleanRendererVK::selClassicPipes()")
assert "s.classicFormat != uiLayerFormat()" in classic and "d.colorFormat = uiLayerFormat();" in classic
assert "s.classicFormat = uiLayerFormat();" in classic and "mColorFormat" not in classic
sel_outline = body(outline, "void LLCleanRendererVK::recordSelOutline(")
assert "ensureSelOutline(s, dev, mLitDesc, mDslDraw0, mDslDraw1, uiLayerFormat())" in sel_outline
assert "recordSelOutline(cmd, dsets, drawPushIn, false, selLayer," in record
assert "recordSelOutline(cmd, dsets, hudPush, true, hudColor," in record
# The encoder's per-slot descriptor cache never trusts a destroyed view's handle value: an
# offscreen shot's transient views and a new frame's images (resize) invalidate it.
assert "void invalidateInputs()" in encode_h
off_after = offscreen[offscreen.index("VkDisplayEncode::Content::CompositeNits"):]
assert "mImpl->mDisplayEncode.invalidateInputs();" in off_after[:off_after.index("W->recordCaptureAt(cmd, 2, w, h);")]
assert "mDisplayEncode.invalidateInputs();" in body(context, "bool commitHdrOutputStage(")
# The snapshot capture image is freed once shots stop (free on liveness), after its uses retired.
assert "void releaseCapture()" in frame_h
assert "mImpl->mDisplayFrame.releaseCapture();" in frame and "kCaptureIdleFrames" in frame

# ---- Physics shapes (#1176) draw with the selection lines ----
# Their flat SDR colours go into the HDR UI layer, not the linear world image: pipelines built in
# the layer's format (rebuilt when it changes, never bound in another), the standard blend
# (colour over, coverage ONE / ONE_MINUS_SRC_ALPHA, the "over" mode simulated below), and
# recorded in the layer scope between the wireframe's edges and the selection lines, as in SDR.
phys = (ROOT / "indra/newview/vkgfx/llcleanphysshapevk.cpp").read_text()
phys_ensure = body(phys, "bool LLCleanRendererVK::physShapeEnsure()")
assert "mColorFormat" not in phys_ensure
assert "s.format == uiLayerFormat()" in phys_ensure and "s.format = uiLayerFormat();" in phys_ensure
assert "d.colorFormat = uiLayerFormat();" in phys_ensure
assert "d.blend = true;" in phys_ensure
assert "d.additive = false; d.customBlend = false;" in phys_ensure
assert "uiLayerFormat()" in body(phys, "bool LLCleanRendererVK::physShapesWanted() const")
layer_scope = record[record.index("overlayScope(selLayer, false"):]
layer_scope = layer_scope[:layer_scope.index("});")]
assert layer_scope.index("recordWireframe(") < layer_scope.index("recordPhysShapes(cmd, drawPushIn)") \
    < layer_scope.index("recordSelection(")
assert "if (selLayer && (wireframe || phys_shapes || sel_world_n > 0))" in record
# The SDR scope is the only other place they draw; the HDR world scope only clears for the wireframe.
assert record.count("recordPhysShapes(") == 2
sdr_scope = record[record.index("if (selLayer == colorView)"):record.index("else", record.index("if (selLayer == colorView)"))]
assert "recordPhysShapes(cmd, drawPushIn)" in sdr_scope
assert "(sel_world_n > 0 || wireframe || phys_shapes) && !stereoActive())\n        LL_WARNS_ONCE" in record

parser = argparse.ArgumentParser()
parser.add_argument("--compiler", default="c++")
args = parser.parse_args()
with tempfile.TemporaryDirectory() as tmp:
    src = Path(tmp) / "ui_layer_blend.cpp"
    exe = Path(tmp) / "ui_layer_blend"
    src.write_text(harness)
    subprocess.run([args.compiler, "-std=c++20", "-O1", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(ROOT / "indra/newview"), str(src), "-o", str(exe)],
                   check=True)
    subprocess.run([str(exe)], check=True)

print("PASS: HDR draws the UI into an RGBA8 layer (allocated with HDR output only) and composites it in the encode")
print("PASS: no UI pipeline keeps the linear-nits specialization")
print("PASS: over, premultiplied, additive, darken and replace draws in the layer composite as in SDR")
print("PASS: HUDs, selection outlines and their rez variants draw into the layer with SDR shaders; opaque HUD faces cover")
print("PASS: physics shapes draw into the layer with the selection lines, in its format and blend")
