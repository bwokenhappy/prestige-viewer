// vkclean -- WEIGHTED BLENDED ORDER-INDEPENDENT TRANSPARENCY (#1410, RenderVKAlphaOIT; McGuire &
// Bavoil 2013, "Weighted Blended Order-Independent Transparency", JCGT 2(2)). The accumulation
// variants of clean_draw.frag (-DCLEAN_OIT_ACCUM: the lit FS, mode 1; -DCLEAN_OIT_GBUF: the unlit
// surface, mode 2) and the resolve (clean_oit_resolve.frag) share this file.
//
// Every OIT fragment adds  w * a * (its value)  into additive RGBA16F targets and multiplies the
// revealage target by (1 - a) (R16F, ZERO / ONE_MINUS_SRC_COLOR). The resolve divides by the
// accumulated w * a and composites  value * (1 - revealage)  over the scene. One layer is exact;
// several layers are a weighted average in place of the ordered over-chain.
//
// WEIGHT (oitWeight): McGuire's 2015 alpha term (min(1, 10 a) + 0.01)^3 -- a near-opaque strand is
// not washed out by the faint fringes around it -- times the paper's near-field 1 / z^2 falloff
// (eq. 9's (z / 5)^2 term), normalised to [0.01, 1] for fp16:
//     w(z, a) = (min(1, 10 a) + 0.01)^3 * clamp((zr / z)^2, 0.01, 1)
// zr = RenderVKAlphaOITDepthRef (default 0.5 m): flat inside zr, 1 / z^2 out to 10 zr (5 m), flat
// beyond (SL view depths 0.1 .. ~500 m). Two heads at 2 m and 5 m weigh 6.25 : 1 (as eq. 9); layers
// of one head a few cm apart differ by a few percent, so inside a hair stack the alpha term rules.
// RANGE: w <= 1.03, so a sum of L layers of colour C stays below L * C -- no fp16 overflow below
// ~6000 HDR units over 10 layers (the paper's 3e3 cap overflowed at sunlit HDR values) -- and the
// layers that matter (a >= 0.1) keep w >= 0.01, well inside the fp16 normal range.
#ifndef CLEAN_OIT_GLSL
#define CLEAN_OIT_GLSL

// The OIT draws' push word (clean_draw_view.glsl g_draw_push.pad1, reset by the next
// pushDrawView): bit 0 the debug tint (RenderVKAlphaOITDebug bit 1), bits 6-7 the glow sum
// (below; the resolve's pad1 carries the same bits next to its debug bits 2-5), bits 8-31 zr in mm.
const uint OIT_PAD_TINT = 1u;
float oitDepthRef(uint pad) { const float mm = float(pad >> 8u); return mm > 0.0 ? mm * 0.001 : 0.5; }
bool  oitTint(uint pad) { return (pad & OIT_PAD_TINT) != 0u; }

// ADDITIVE GLOW (RenderVKAlphaOITGlowAdditive, live A/B, not saved; LLCleanRendererVK kOitPadGlowAdd*).
// The sorted pass composites each layer's glow OVER the glow target with the layer's GLOW coverage
// glow_a (clean_draw.frag main: legacy = material alpha x texel alpha, NOT the face alpha, #26; PBR =
// the final alpha) -- a faint (face alpha 0.05) glowing strand glows at full strength there. The
// weighted average instead scales the mean glow by the FACE coverage 1 - revealage: one such strand
// keeps 5 % of its glow. With a bit set the glow channel is not weighted: every layer adds
//     glow x glow_a                        (bit 6, 1: the sum -- one layer exact, a stack adds up)
//     -log(1 - min(glow x glow_a, 0.999))  (bit 7, 2: resolved as 1 - exp(-sum) = 1 - prod(1 - g_i),
//                                           one layer exact, a stack saturates at full glow)
// and the resolve composites that amount as is (no division by the weights, no x coverage). The
// colour, its weights and the revealage are unchanged. Mode 2: the glow factor (target 2 .a) times
// the resolved lit colour, as before; mode 1: the lit glow colour (target 1 .rgb; bit 7 = bit 6).
const uint OIT_PAD_GLOW_ADD = 0x40u;
const uint OIT_PAD_GLOW_SAT = 0x80u;
uint oitGlowSum(uint pad) { return pad & (OIT_PAD_GLOW_ADD | OIT_PAD_GLOW_SAT); }
// One layer's contribution to the glow sum (gA: glow x glow_a, in [0, 1] for the factor).
float oitGlowTerm(float gA, uint pad)
{
    gA = max(gA, 0.0);
    return (pad & OIT_PAD_GLOW_SAT) != 0u ? -log(1.0 - min(gA, 0.999)) : gA;
}
// The composited glow factor from the sum (mode 2).
float oitGlowResolve(float sum, uint pad)
{
    return (pad & OIT_PAD_GLOW_SAT) != 0u ? 1.0 - exp(-max(sum, 0.0)) : max(sum, 0.0);
}

// z: the fragment's view depth (m); a: its coverage.
float oitWeight(float a, float z, uint pad)
{
    const float zr = oitDepthRef(pad);
    const float at = min(1.0, 10.0 * a) + 0.01;
    const float dt = clamp((zr * zr) / max(z * z, 1e-6), 0.01, 1.0);
    return at * at * at * dt;
}

// The debug tint (RenderVKAlphaOITDebug bit 1): what the classification put in the OIT group.
const vec3 OIT_TINT = vec3(0.0, 1.0, 0.85);

// Largest colour the mode-1 accumulation takes (the sum stays finite in fp16, see above).
const float OIT_MAX_HDR = 4096.0;

// Smallest coverage an accumulated fragment has (the accumulation FS discards below it). 1 - 2^-10
// rounds to an fp16 value below 1.0, so every accumulated fragment moves the revealage off its clear
// value: the resolve's "revealage == 1" test (self-clearing targets) is exact. What is dropped is
// invisible: such a fragment alone leaves the pixel uncovered (the resolve needs revealage < 0.998)
// and among others it weighs (0.02^3 * a) -- under 1e-8 of a solid layer's w a.
const float OIT_MIN_A = 1.0 / 1024.0;

#endif   // CLEAN_OIT_GLSL
