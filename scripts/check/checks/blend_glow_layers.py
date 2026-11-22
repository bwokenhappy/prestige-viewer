"""Regression guard for the stacked glowing alpha-blend layers (Moochie's fairy-light curtains).

~12.9k glowing tint-alpha strand faces stacked 20-40 M blended fragments a frame: pass 2 cost 430-600 ms
on a GTX 1080 Ti and 175 ms on an RTX 5090. Three rules keep that bounded:

1. GLOW TEXEL DISCARD (clean_draw.frag decodeSurface, RenderVKBlendGlowTexelDiscard): the transparent-texel
   discard used to skip EVERY glowing face (#26: a 100 % transparent prim with glow is a halo), so the empty
   card area of a glowing strand texture ran the full lit FS. A legacy face's glow coverage is the material
   alpha x the TEXEL alpha (main's glow_a; LL emissiveF.glsl: texture alpha x glow), a PBR face's its final
   alpha, an emissive-mask face's the rez fade: a texel with colour alpha and glow coverage both < 1/255 adds
   nothing and is discarded. #26's halo (opaque texture, face alpha 0) keeps running.
2. NO PER-LAYER RT HIT LIGHTING for glowing / faint layers (blendHitOn tests g_blend_cheap).
3. RenderVKBlendMaxLights: the blended layers' light-walk cap (both walks), tiered by the quality level.

The fixture models rule 1's truth table from the shader's own expressions, and pins the wiring the model
cannot see: main's glow_a rule the discard mirrors, the frame-info bits the host writes, the settings, the
feature-table tiers and the preferences slider.
"""
import re

NAME = "blend-glow-layers"
STAGE = "source"
DESCRIPTION = ("stacked glowing blend layers: invisible glowing texels discarded (not #26 halos), no per-layer "
               "RT hit lighting for glow / faint layers, the blended light cap wired")
GUARDS = ("every texel of a glowing alpha texture running the full lit FS again (Moochie: 2 FPS), a #26 halo "
          "(transparent prim, opaque glowing texture) discarded, or the blend light cap / hit-lighting gate "
          "silently disconnected")

NEWVIEW = "indra/newview/"
SHADERS = "indra/vkclean/shaders/"
DRAW_FRAG = SHADERS + "clean_draw.frag"
COMMON = SHADERS + "clean_draw_common.glsl"
RENDERER = NEWVIEW + "vkgfx/llcleanrenderervk.cpp"
SETTINGS = NEWVIEW + "app_settings/settings.xml"
FEATURES = (NEWVIEW + "featuretable.txt", NEWVIEW + "featuretable_linux.txt")
PREFS_XML = NEWVIEW + "skins/default/xui/en/floater_preferences_graphics_advanced.xml"


def _read(ctx, rel, result):
    try:
        with open(ctx.path(rel), encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        result.fail("%s: missing" % rel)
        return ""


def _discarded(flags, factor_a, tint_a, texel_a, glow_on=True, glare=False):
    """Rule 1 as the shader states it (decodeSurface): True = the blend texel is discarded."""
    base_a = factor_a * tint_a * (texel_a if flags & 1 else 1.0)
    glowing = ((flags >> 8) & 0xFF) != 0
    hidden = False
    if glowing and glow_on:
        if flags & 64:
            hidden = True
        elif not flags & 128:
            hidden = factor_a * (texel_a if flags & 1 else 1.0) < 1.0 / 255.0
    return base_a < 1.0 / 255.0 and (not glowing or hidden) and not glare


def _visible(flags, factor_a, tint_a, texel_a):
    """What main() composites: colour coverage (base alpha) and glow coverage (glow_a)."""
    base_a = factor_a * tint_a * (texel_a if flags & 1 else 1.0)
    glowing = ((flags >> 8) & 0xFF) != 0
    if flags & 128 and not flags & 64:
        glow_a = 1.0
    elif not flags & 64 and glowing:
        glow_a = factor_a * (texel_a if flags & 1 else 1.0)
    else:
        glow_a = base_a
    return base_a >= 1.0 / 255.0 or (glowing and glow_a >= 1.0 / 255.0)


def _fixture(result):
    TEX, PBR, EMASK, GLOW = 1, 64, 128, 0x40 << 8
    cases = [
        # (name, flags, factor a, tint a, texel a, expect discarded)
        ("glowing strand, empty card texel", TEX | GLOW, 1.0, 0.6, 0.0, True),
        ("glowing strand, light dot", TEX | GLOW, 1.0, 0.6, 1.0, False),
        ("#26 halo: tint 0, opaque glowing texture", TEX | GLOW, 1.0, 0.0, 1.0, False),
        ("untextured glowing transparent prim (#26)", GLOW, 1.0, 0.0, 1.0, False),
        ("emissive-mask glowing face, transparent texel", TEX | GLOW | EMASK, 1.0, 1.0, 0.0, False),
        ("PBR glowing face, transparent texel", TEX | GLOW | PBR, 1.0, 1.0, 0.0, True),
        ("plain transparent texel", TEX, 1.0, 1.0, 0.0, True),
        ("plain visible texel", TEX, 1.0, 0.5, 0.5, False),
    ]
    for name, flags, fa, ta, xa, want in cases:
        got = _discarded(flags, fa, ta, xa)
        if got != want:
            result.fail("glow texel rule: %s -> %s, expected %s" % (name, "discarded" if got else "shaded",
                                                                    "discarded" if want else "shaded"))
        if got and _visible(flags, fa, ta, xa):
            result.fail("glow texel rule: %s is discarded but main() would composite it" % name)
        if _discarded(flags, fa, ta, xa, glow_on=False) and ((flags >> 8) & 0xFF):
            result.fail("glow texel rule: %s discarded with RenderVKBlendGlowTexelDiscard off" % name)


def run(ctx, result):
    _fixture(result)

    # Beta 4 removes the unaccepted per-prim self-depth experiment, not the
    # independent avatar depth or glow-subset ordering rules.
    for path in (RENDERER, NEWVIEW + "vkgfx/llcleanrenderervk.h", DRAW_FRAG,
                 SHADERS + "clean_draw.vert", SHADERS + "clean_draw_rigged.vert",
                 SHADERS + "clean_prepass_blend.frag", SHADERS + "clean_draw_view.glsl",
                 SHADERS + "clean_blend_depth_rule.glsl", SETTINGS):
        text = _read(ctx, path, result)
        if re.search(r"RenderVKObjectAlphaDepth|mObjDepth|v_primkey|OBJD_(?:DEPTH|KEY)_PUSH|objectDepth", text):
            result.fail("%s: removed object self-depth experiment returned" % path)
    renderer = _read(ctx, RENDERER, result)
    if "if (mOitSubNow && mOitSubSplit) mAlphaCoverNow = mAlphaKNow = false;" not in renderer:
        result.fail("glow-subset cover/K-list suppression lost with self-depth removal")

    frag = _read(ctx, DRAW_FRAG, result)
    need = [
        (r"glowHidden\s*=\s*m\.base_color_factor\.a\s*\*\s*\(\(m\.flags\s*&\s*1u\)\s*!=\s*0u\s*\?\s*texel\.a\s*:\s*1\.0\)"
         r"\s*<\s*\(1\.0\s*/\s*255\.0\)", "legacy glow coverage = material alpha x texel alpha (no tint)"),
        (r"if\s*\(\(m\.flags\s*&\s*64u\)\s*!=\s*0u\)\s*glowHidden\s*=\s*true", "PBR glow coverage = final alpha"),
        (r"else\s+if\s*\(\(m\.flags\s*&\s*128u\)\s*==\s*0u\)", "emissive-mask faces never hidden"),
        (r"glowing\s*&&\s*\(ssr\.blend_policy\.z\s*&\s*0x40000000u\)", "the A/B bit (blend_policy.z bit 30)"),
        (r"base\.a\s*<\s*\(1\.0\s*/\s*255\.0\)\s*&&\s*\(!glowing\s*\|\|\s*glowHidden\)", "the discard"),
        # main()'s glow coverage, which rule 1 mirrors
        (r"glow_a\s*=\s*m\.base_color_factor\.a;\s*\n\s*if\s*\(\(m\.flags\s*&\s*1u\)\s*!=\s*0u\s*&&\s*V_NOT_TERRAIN\)\s*\n"
         r"\s*glow_a\s*\*=\s*baseTexelA\(m\);", "main's legacy glow_a = factor alpha x texel alpha"),
        (r"g_blend_cheap\s*=\s*\(v_flags\s*&\s*2u\)\s*!=\s*0u\s*&&\s*\(materialGlow\(m\)\s*>\s*0\.0\s*\|\|\s*alpha\s*<\s*"
         r"BLEND_CHEAP_ALPHA\)", "glow / faint layers flagged"),
        (r"g_light_cap\s*=\s*min\(g_light_cap,\s*BLEND_CHEAP_LIGHTS\)", "glow / faint layers' light cap"),
        (r"\(ssr\.blend_policy\.z\s*>>\s*16u\)\s*&\s*0xFFu", "the user light cap (blend_policy.z bits 16-23)"),
    ]
    for pat, what in need:
        if not re.search(pat, frag):
            result.fail("%s: %s changed or gone" % (DRAW_FRAG, what))

    common = _read(ctx, COMMON, result)
    if not re.search(r"bool blendHitOn\(\)\s*\{[^}]*!g_blend_cheap", common):
        result.fail("%s: blendHitOn() no longer skips glow / faint layers (g_blend_cheap)" % COMMON)
    if len(re.findall(r"lit\s*<\s*g_light_cap", common)) < 2:
        result.fail("%s: both light walks (froxel, cluster) must stop at g_light_cap" % COMMON)

    rend = _read(ctx, RENDERER, result)
    for pat, what in [
        (r'"RenderVKBlendGlowTexelDiscard"', "reads RenderVKBlendGlowTexelDiscard"),
        (r"if\s*\(s_glowTexel\)\s*si\.blend_policy\[2\]\s*\|=\s*0x40000000u", "sets blend_policy.z bit 30"),
        (r'"RenderVKBlendMaxLights"', "reads RenderVKBlendMaxLights"),
        (r"si\.blend_policy\[2\]\s*=\s*\(cap\s*==\s*0u\s*\|\|\s*cap\s*>=\s*255u\)\s*\?\s*0u\s*:\s*\(cap\s*<<\s*16\)",
         "packs the light cap into blend_policy.z bits 16-23"),
    ]:
        if not re.search(pat, rend):
            result.fail("%s: %s -- changed or gone" % (RENDERER, what))

    settings = _read(ctx, SETTINGS, result)
    for name, persist in (("RenderVKBlendGlowTexelDiscard", "0"), ("RenderVKBlendMaxLights", "1")):
        m = re.search(r"<key>%s</key>\s*<map>(.*?)</map>" % name, settings, re.S)
        if not m:
            result.fail("%s: %s missing" % (SETTINGS, name))
        elif not re.search(r"<key>Persist</key>\s*<integer>%s</integer>" % persist, m.group(1)):
            result.fail("%s: %s must have Persist %s" % (SETTINGS, name, persist))

    for ft in FEATURES:
        text = _read(ctx, ft, result)
        vals = re.findall(r"^RenderVKBlendMaxLights\s+1\s+(\d+)", text, re.M)
        if len(vals) != 8:
            result.fail("%s: RenderVKBlendMaxLights in %d lists (all + the 7 quality levels expected)" % (ft, len(vals)))
        elif int(vals[0]) < 255:
            result.fail("%s: the 'all' list caps RenderVKBlendMaxLights (the table takes the MIN: 256 = every light)" % ft)

    if 'control_name="RenderVKBlendMaxLights"' not in _read(ctx, PREFS_XML, result):
        result.fail("%s: the 'Lights per transparent layer' slider is gone" % PREFS_XML)

    # 4. GLOW SUBSET -> WBOIT (RenderVKAlphaOITGlowSubset): the VS rule, the order split, the resolve between.
    vert = _read(ctx, SHADERS + "clean_draw.vert", result)
    for pat, what in [
        (r"\(flags\s*&\s*\(1u\s*\|\s*8u\s*\|\s*512u\s*\|\s*2048u\s*\|\s*65536u\)\)\s*!=\s*0u\)\s*return false",
         "subset never rigged / HUD / worn / water"),
        (r"\(\(m\.flags\s*>>\s*8u\)\s*&\s*0xFFu\)\s*==\s*0u\)\s*return false", "subset glowing only"),
        (r"m\.spec_color_gloss\.a\s*>\s*0\.0", "subset never a legacy shiny / glare face"),
        (r"\(subBits\s*&\s*OITSUB_ONLY\)\s*!=\s*0u\s*&&\s*!subFace\)\s*\|\|\s*\(\(subBits\s*&\s*OITSUB_EXCL\)\s*!=\s*0u\s*&&\s*subFace\)",
         "the VS sorts the subset between the WBOIT and the sorted draws"),
    ]:
        if not re.search(pat, vert):
            result.fail("%s: %s -- changed or gone" % (SHADERS + "clean_draw.vert", what))
    if not re.search(r"splitOut\s*=\s*\(splitBits\s*==\s*OITSUB_FAR\)\s*!=\s*behind", frag):
        result.fail("%s: the sorted static list's FAR / NEAR split around the subset is gone" % DRAW_FRAG)
    if not re.search(r"dFdy\(gl_FragCoord\.z\)\)\s*/\s*fsz;\s*\n\s*if\s*\(splitOut\)\s*discard;", frag):
        result.fail("%s: the split's discard must follow blendDepthTolerance's derivatives" % DRAW_FRAG)
    if not re.search(r"kPushOitSplitFar[^;]*;\s*\n\s*drawListBlend[^\n]*\n[^\n]*\n\s*oitResolve\(\);[\s\S]{0,400}kPushOitSplitNear",
                     rend):
        result.fail("%s: the sorted static list must draw FAR, resolve, NEAR (glass in front of strands)" % RENDERER)
    if not re.search(r"db0\[1\]\.stageFlags=VK_SHADER_STAGE_VERTEX_BIT\|VK_SHADER_STAGE_FRAGMENT_BIT", rend):
        result.fail("%s: draw set 0 binding 1 (materials) must be vertex-visible (oitSubsetFace)" % RENDERER)
    if not re.search(r"if\s*\(mOitSubNow\s*&&\s*mOitSubSplit\)\s*mAlphaCoverNow\s*=\s*mAlphaKNow\s*=\s*false", rend):
        result.fail("%s: covers / K lists must be off while the glow subset draws (and only then)" % RENDERER)
    if not re.search(r"const bool behind\s*=\s*zb\s*==\s*0u\s*\|\|", frag):
        result.fail("%s: a pixel with no subset layer must draw in the FAR pass (static before rigged)" % DRAW_FRAG)

    # 5. ADDITIVE GLOW (RenderVKAlphaOITGlowAdditive, live A/B): the OIT group's glow summed unweighted with each
    #    layer's GLOW coverage (main's glow_a: texel x material alpha, not the face alpha) -- one layer equals the
    #    sorted pass's glow over black; the weighted mean x face coverage dims a faint strand to its face alpha.
    _glow_add_fixture(result)
    oit = _read(ctx, SHADERS + "clean_oit.glsl", result)
    res = _read(ctx, SHADERS + "clean_oit_resolve.frag", result)
    for text, rel, pat, what in [
        (oit, "clean_oit.glsl", r"OIT_PAD_GLOW_ADD\s*=\s*0x40u", "the sum bit (pad1 bit 6)"),
        (oit, "clean_oit.glsl", r"OIT_PAD_GLOW_SAT\s*=\s*0x80u", "the saturating bit (pad1 bit 7)"),
        (frag, "clean_draw.frag", r"glowA\s*=\s*m\.base_color_factor\.a;\s*\n\s*if\s*\(\(m\.flags\s*&\s*1u\)\s*!=\s*0u\s*&&\s*V_NOT_TERRAIN\)"
         r"\s*\n\s*glowA\s*\*=\s*baseTexelA\(m\);", "the WBOIT glow coverage mirrors main's legacy glow_a"),
        (frag, "clean_draw.frag", r"oitGlowSum\(pad\)\s*!=\s*0u\s*\?\s*oitGlowTerm\(glow\s*\*\s*clamp\(glowA", "mode 2 sums glow x glowA"),
        (res, "clean_oit_resolve.frag", r"glowSum\s*!=\s*0u\s*\?\s*oitGlowResolve\(em\.a,\s*glowSum\)\s*:\s*em\.a\s*\*\s*iw\s*\*\s*cov",
         "mode 2's resolve takes the sum as is (no weights, no coverage)"),
    ]:
        if not re.search(pat, text):
            result.fail("%s: %s -- changed or gone" % (SHADERS + rel, what))
    if not re.search(r"resolvePad1\s*=\s*mOitDbgView\s*\|\s*mOitGlowBits", rend):
        result.fail("%s: the resolve's pad1 must carry the additive-glow bits" % RENDERER)
    if not re.search(r"mOitPad1\s*=[^;]*mOitGlowBits", rend):
        result.fail("%s: the accumulation's pad1 must carry the additive-glow bits" % RENDERER)
    m = re.search(r"<key>RenderVKAlphaOITGlowAdditive</key>\s*<map>(.*?)</map>", settings, re.S)
    if not m:
        result.fail("%s: RenderVKAlphaOITGlowAdditive missing" % SETTINGS)
    elif not (re.search(r"<key>Persist</key>\s*<integer>0</integer>", m.group(1))
              and re.search(r"<key>Value</key>\s*<integer>0</integer>", m.group(1))):
        result.fail("%s: RenderVKAlphaOITGlowAdditive must be Persist 0, default 0 (an A/B)" % SETTINGS)


def _glow_add_fixture(result):
    """The glow the three composites leave over a black glow target: L layers of one strand (glow g, face
    alpha a, glow coverage ga). Sorted: each layer OVER with ga. WBOIT 0: mean g x (1 - prod(1 - a)).
    1: sum g ga. 2: 1 - prod(1 - g ga)."""
    import math

    def sorted_glow(g, ga, n):
        out = 0.0
        for _ in range(n):
            out = g * ga + out * (1.0 - ga)
        return out

    def oit(g, a, ga, n, mode):
        if mode == 0:
            return g * (1.0 - (1.0 - a) ** n)
        s = sum((g * ga) if mode == 1 else -math.log(1.0 - min(g * ga, 0.999)) for _ in range(n))
        return s if mode == 1 else 1.0 - math.exp(-s)

    g, a, ga = 0.5, 0.05, 1.0   # a faint strand: face alpha 5 %, opaque glowing texel
    for mode in (1, 2):
        if abs(oit(g, a, ga, 1, mode) - sorted_glow(g, ga, 1)) > 1e-6:
            result.fail("additive glow %d: one layer must equal the sorted pass's glow" % mode)
    if oit(g, a, ga, 1, 0) > 0.1 * sorted_glow(g, ga, 1):
        result.fail("additive glow fixture: the weighted mean no longer dims a faint strand (model out of date?)")
    if oit(g, a, ga, 28, 2) > 1.0 + 1e-6 or oit(g, a, ga, 28, 2) < sorted_glow(g, ga, 28) - 1e-6:
        result.fail("additive glow 2: a stack must saturate at full glow, never below the sorted pass")
