// vkclean — DIFFUSE / SPECULAR LIGHTING DENOISER (#85 phase 2): the shared contract between a
// PRODUCER (a pass that writes a noisy lighting signal), the DENOISER (clean_dn_temporal.comp,
// clean_dn_atrous.comp; LLCleanDenoiserVK) and a CONSUMER (the lit pass that remodulates the
// result). Pure functions + constants: no bindings, no push block (each includer declares its own).
//
// THE SIGNAL (per denoiser texel; the denoiser runs at the render extent x a scale, 1 or 1/2):
//   * DIFFUSE: demodulated irradiance-like radiance, rgb >= 0 -- the producer divides its
//     diffuse term by pdnDiffFactor(albedo) (linear base / PBR diffuseColor), so texture detail
//     never enters the filter and the consumer multiplies it back at full resolution.
//   * SPECULAR: demodulated radiance -- divided by pdnSpecFactor(F0) (PBR specularColor, legacy
//     specular colour); Fresnel's angular part, D, G stay inside the signal.
//   * GUIDES: view depth (32-bit), its plane gradient (d(vz) per denoiser texel), the shading
//     normal, the specular roughness, the specular hit / light distance, the camera-only
//     previous-frame position of the surface and of its VIRTUAL (reflected) image, the expected
//     previous view depth, and the SIGNAL REFERENCE W: the producer's noise-free estimate of the
//     signal's scale (e.g. the unshadowed target sum of a light sampler) -- when it changes
//     between frames by more than the noise could, the light itself changed (a club light
//     switched / recoloured / moved) and the history is cut at once (anti-lag), whatever the
//     neighbourhood clamp would have kept.
//
// IMAGES (LLCleanDenoiserVK; all GENERAL, the denoiser extent; 112 B per texel):
//   IN    rgba16f[2]  layer 0 diffuse (rgb, a = sample confidence: the producer's sample count),
//                     layer 1 specular (rgb, a = hit / light distance in m, 0 = none).
//                     Written by the producer; the a-trous iterations reuse it as ping-pong
//                     scratch once the temporal pass has read it.
//   MOT   rgba32ui    x packHalf2x16(surface: previous texel coordinate - this texel),
//                     y packHalf2x16(virtual image: the same), z the expected previous view
//                     depth (float bits; 0 = behind last frame's eye), w spare (0).
//                     Camera-only: the temporal pass replaces x by the motion vectors
//                     (object motion: dancing avatars) when this frame wrote them.
//   GEOM  rgba32ui[2] per frame parity (the producer writes layer parity, the temporal pass
//                     reads layer parity ^ 1 as the history's guide): x view depth (float
//                     bits; <= 0 = no surface), y pdnPackNR (oct normal 12 + 12 bits,
//                     roughness 8), z packHalf2x16(d(vz)/dx, d(vz)/dy per denoiser texel),
//                     Local plane mode instead stores normalized reciprocal-depth slopes
//                     in z (linear slopes for ortho); flag travels with the whole frame.
//                     w pdnPackMeta (the producer: W; the temporal pass adds the history
//                     lengths and the specular a-trous depth).
//   HIST  rgba32ui[2] per parity: packHalf2x16 x (diff r, g), y (diff b, spec r), z (spec g, b),
//                     w (diffuse luminance variance, specular luminance variance) -- an
//                     exponentially weighted variance (never E[l^2] - E[l]^2: fp16-safe).
//                     ONLY the colour (THE LEAN HISTORY, clean_dn_common.glsl): the 1 x 1 placeholder while
//                     DLSS RR takes the signal raw. The ages / countdowns / mover record are AGE
//                     (rgba32ui[2], binding 6; the GI instance's r32ui[2] -- THE NARROW AGE, its ages alone),
//                     the split slots' ages SPLITAGE (rgba16ui[2], binding 8; the placeholder without the split
//                     channel), the sun / moon channel's history SUNHIST (rgba32ui[2], binding 7; the
//                     placeholder without the sun or while it is raw).
//   OUT   rgba16f[2]  layer 0 diffuse (rgb, a = variance; the final iteration: view depth for
//                     the consumer's upsample), layer 1 specular (rgb, a = variance).
//                     The temporal pass writes it, the a-trous iterations ping-pong OUT <-> IN:
//                     the FINAL image is OUT after an even number of iterations, IN after an
//                     odd one (LLCleanDenoiserVK::finalIsIn; the consumer's push says which).
// (#869) With the SPLIT CHANNEL (below) layers 0 / 1 are the REST's ratio -- the lights not
// split at that texel -- and GEOM / IN / OUT carry the split lights' own visibilities.
#ifndef CLEAN_PDN_COMMON_GLSL
#define CLEAN_PDN_COMMON_GLSL
#include "clean_pdn_plane.glsl"

// Invert the actual truncated render-pixel sample centres (including odd extents).
vec2 pdnExactHc(vec2 q, vec2 r, vec2 hc0)
{
    vec2 hc;
    for (int a = 0; a < 2; ++a)
    {
        float b = floor(hc0[a]);
        float c0 = floor((b + 0.5) * r[a]) + 0.5;
        float c1 = floor((b + 1.5) * r[a]) + 0.5;
        if (q[a] < c0)       { b -= 1.0; c1 = c0; c0 = floor((b + 0.5) * r[a]) + 0.5; }
        else if (q[a] >= c1) { b += 1.0; c0 = c1; c1 = floor((b + 1.5) * r[a]) + 0.5; }
        hc[a] = b + clamp((q[a] - c0) / max(c1 - c0, 1e-3), 0.0, 1.0);
    }
    return hc;
}

// Inverse of the producer's actual raster-centre lattice. q is in previous
// raster pixels (including its raster jitter); gridJitter is the producer's
// denoiser-texel phase, zero at full resolution. Fixed two neighbours per axis.
vec2 pdnSampleGridHc(vec2 q, vec2 r, vec2 gridJitter)
{
    vec2 hc;
    for (int a = 0; a < 2; ++a)
    {
        float b = floor(q[a] / r[a] - 0.5 - gridJitter[a]);
        float c0 = floor((b + 0.5 + gridJitter[a]) * r[a]) + 0.5;
        float c1 = floor((b + 1.5 + gridJitter[a]) * r[a]) + 0.5;
        if (q[a] < c0) { b -= 1.0; c1 = c0; c0 = floor((b + 0.5 + gridJitter[a]) * r[a]) + 0.5; }
        else if (q[a] >= c1) { b += 1.0; c0 = c1; c1 = floor((b + 1.5 + gridJitter[a]) * r[a]) + 0.5; }
        hc[a] = b + clamp((q[a] - c0) / max(c1 - c0, 1e-3), 0.0, 1.0);
    }
    return hc;
}

// Demodulation floor: a black albedo / F0 divides by this (its signal is ~0 there anyway).
#define PDN_DEMOD_MIN 0.02
vec3 pdnDiffFactor(vec3 albedo) { return max(albedo, vec3(PDN_DEMOD_MIN)); }
vec3 pdnSpecFactor(vec3 f0)     { return max(f0, vec3(PDN_DEMOD_MIN)); }

float pdnLum(vec3 c) { return dot(c, vec3(0.2126, 0.7152, 0.0722)); }

// Finite, non-negative, below the fp16 range (a producer's 1 / pdf weight can explode).
vec3 pdnSane(vec3 c)
{
    if (any(isnan(c)) || any(isinf(c))) return vec3(0.0);
    return clamp(c, vec3(0.0), vec3(60000.0));
}

// ---- normal + roughness word (12 + 12 bit octahedral normal, 8-bit roughness) ----
vec2 pdnOctWrap(vec2 v) { return (1.0 - abs(v.yx)) * vec2(v.x >= 0.0 ? 1.0 : -1.0, v.y >= 0.0 ? 1.0 : -1.0); }
uint pdnPackNR(vec3 n, float rough)
{
    n /= max(abs(n.x) + abs(n.y) + abs(n.z), 1e-6);
    vec2 e = n.z >= 0.0 ? n.xy : pdnOctWrap(n.xy);
    e = clamp(e * 0.5 + 0.5, 0.0, 1.0);
    const uvec2 q = uvec2(e * 4095.0 + 0.5);
    const uint r = uint(clamp(rough, 0.0, 1.0) * 255.0 + 0.5);
    return q.x | (q.y << 12u) | (r << 24u);
}
vec3 pdnUnpackN(uint v)
{
    const vec2 e = vec2(float(v & 4095u), float((v >> 12u) & 4095u)) * (2.0 / 4095.0) - 1.0;
    vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
    const float t = max(-n.z, 0.0);
    n.x += n.x >= 0.0 ? -t : t;
    n.y += n.y >= 0.0 ? -t : t;
    return normalize(n);
}
float pdnUnpackR(uint v) { return float(v >> 24u) * (1.0 / 255.0); }

// GEOM length bits are a rounded 60 Hz duration projection for producer compatibility.
// The fine ages are in AGE / SPLITAGE (bindings 6 / 8; clean_dn_common.glsl); they
// alone feed temporal accumulation and spatial young-history rules. Local radiance
// reuses projection bit 10 for a capped history and bit 11 for the mover carry's low bit
// (pdnMetaCarry); no accumulation reader consumes the compatibility length projection.
// ---- GEOM w: history lengths (6 bits each, 0..63 frames), the specular a-trous depth
//      (3 bits: iterations the specular takes, 0..7) and W (fp16, high half) ----
// Local radiance visibility evidence, independent of the chosen light's energy.
// Code0 is unknown;1..31 represent [0,1]. MOT stores this frame's (diffuse) evidence at bits9..13;
// radiance GEOM's unused compatibility ages hold its 10-bit prior (pdnMetaVisibility).
uint pdnVisibilityCode(float value)
{
    return !(value >= 0.0) || isinf(value) ? 0u : 1u + uint(clamp(value, 0.0, 1.0) * 30.0 + 0.5);
}
float pdnVisibilityValue(uint code)
{
    return (code & 31u) == 0u ? -1.0 : float((code & 31u) - 1u) / 30.0;
}
uint pdnSampleVisibility(vec3 num, vec3 den)
{
    const float d = pdnLum(den), n = pdnLum(num);
    return d > 1e-8 ? pdnVisibilityCode(n / d) : 0u;
}
// The weight sample j of a pick list adds to that evidence. A reused reservoir (sample 0) is
// shared by its spatial-reuse neighbourhood: its 0 / 1 flips arrive in the denoiser's 3x3 at once,
// a correlated "change" no neighbourhood test can tell from a moving caster. Without it, the Ke - 1
// fresh picks of a Ke-pick list (per-texel seed: independent across texels) stand for all Ke
// (x Ke / (Ke - 1): x 2 for two picks); a lone reused pick gives no evidence. Exact and dense lists
// are never reused: every sample counts.
float pdnEvidenceWeight(bool reused, uint Ke, uint j)
{
    return !reused ? 1.0 : (j == 0u ? 0.0 : float(Ke) / float(max(Ke, 2u) - 1u));
}
// MOT w bit 19: the evidence is deterministic (every candidate evaluated, PDN_SEL_EXACT). Clear:
// a stochastic estimate whose frame-to-frame spread the denoiser bounds (dnCasterSamplingNoise).
#define PDN_MOT_EVIDENCE_EXACT 0x80000u
// MOT w bit 20 (radiance; the TEMPORAL pass writes it, the producer leaves it clear): this texel's light really
// dropped within the last few frames (clean_dn_temporal.comp THE DROP MARK) -- DLSS RR's responsivity guide
// (clean_rr_guides.comp rrLightDrop) gives such pixels no history.
#define PDN_MOT_LIGHT_DROP 0x100000u
// MOT w bits 21-29 (radiance; clean_deferred_light.comp, read by clean_dn_temporal.comp THE MOVER RECORD): this
// texel's MOVER REPORT -- one radiance sample's light (bits 21-28: light index + 1, 0 = no report) and its outcome
// (bit 29: 1 the ray reached the light, 0 a MOVING caster blocked it). A mover-blocked sample is reported first;
// else one that reached its light (a per-frame alternation among several). Statically blocked samples, split
// lights, emitters and light indices >= 255 are never reported.
#define PDN_MOT_REPORT_SHIFT 21u
#define PDN_MOT_REPORT_VISIBLE 0x20000000u
// THE REPORT'S LIGHT TAG (RenderVKShadowFixLightTag bit 0, LightInfo.fwd.z LIGHT_TAG_REPORT; default off: light index + 1
// as above). The record (HIST age word y) carries the report's light from frame to frame and compares it with later
// reports, so it must name the SAME light next frame. The list is re-sorted by importance every frame (a moving camera,
// a dancer's worn light), so after a reorder an index-keyed record names another light: that light reaching an avatar's
// umbra counts as the mover leaving (THE REVEAL: the shadow's history reset to its lit neighbours -- a flash) and the
// real light's return goes unseen. The tag is the light's split id (Light.tex.w: one per light key while it is listed,
// LLCleanRendererVK::splitIdsFrame) folded to 1..255: stable while the light is listed. Two lights at one texel share a
// tag only when their ids differ by a multiple of 255, never by a reorder.
uint pdnMotReportTag(uint splitId)
{
    return splitId == 0u ? 0u : ((splitId - 1u) % 255u) + 1u;
}
uint pdnMotReportBits(uint lightPlus1, bool visible)
{
    return lightPlus1 == 0u ? 0u : ((min(lightPlus1, 255u) << PDN_MOT_REPORT_SHIFT) | (visible ? PDN_MOT_REPORT_VISIBLE : 0u));
}
uint pdnMotReportLight(uint w) { return (w >> PDN_MOT_REPORT_SHIFT) & 255u; }
// MOT w bits 14-17 (RenderVKShadowMoverMask 2; clean_deferred_light.comp THE MOVER COUNTDOWN): frames this texel stays
// mover-capped after one of its rays last met a MOVING caster (the sun channel's countdown: max(RenderVKRTShadowMoverPersist,
// RenderVKRTShadowDynamicMaxHistory) on a hit, - 1 a frame); 0 = none. Bit 18: the texel is MOVER-EXACT this frame (every
// light evaluated -- the a-trous takes only exact taps around it). Written only while mode 2 runs (else 0).
// MOT w bit 30 (radiance; RenderVKProjStochasticAdaptive): the texel's estimate took the ADAPTIVE EXTRA PASSES this frame
// (clean_deferred_light.comp: the extra SHADE of pass 1 sets it over pass 0's word). Read only by the temporal pass's
// adaptive mover history rule (DN_T_ADAPT_SHIFT); never set while the setting is 0.
#define PDN_MOT_EXTRA_RAYS 0x40000000u
#define PDN_MOT_MOVER_CD_SHIFT 14u
#define PDN_MOT_EXACT_SET 0x40000u
uint pdnMotMoverCd(uint w) { return (w >> PDN_MOT_MOVER_CD_SHIFT) & 15u; }
bool pdnMotReportVisible(uint w) { return pdnMotReportLight(w) != 0u && (w & PDN_MOT_REPORT_VISIBLE) != 0u; }
bool pdnMotReportMover(uint w) { return pdnMotReportLight(w) != 0u && (w & PDN_MOT_REPORT_VISIBLE) == 0u; }

uint  pdnPackMeta(float lenD, float lenS, uint specIt, float W)
{
    const uint d = uint(clamp(lenD, 0.0, 63.0) + 0.5);
    const uint s = uint(clamp(lenS, 0.0, 63.0) + 0.5);
    return d | (s << 6u) | (min(specIt, 7u) << 12u) | ((packHalf2x16(vec2(clamp(W, 0.0, 60000.0), 0.0)) & 0xFFF0u) << 16u);
}
// Bits 16-19 are W's four lowest fp16 mantissa bits (W keeps 1 / 64 relative precision; its cut acts at 25 %).
// Bits 16-17: the radiance caster test's PENDING deviation of the last frame (clean_dn_temporal.comp
// dnCasterPending): 0 none, 1 darker, 2 brighter (3: the drop mark).
#define PDN_META_PEND_SHIFT 16u
uint  pdnMetaPending(uint w) { return (w >> PDN_META_PEND_SHIFT) & 3u; }
// THE MOVER CARRY (radiance; clean_dn_temporal.comp dnCasterCarry): frames this texel stays moving-caster
// affected after its rays last met a mover, 0..7 -- bit 11 its lowest bit, bits 18-19 the rest.
uint  pdnMetaCarry(uint w) { return ((w >> 11u) & 1u) | (((w >> 18u) & 3u) << 1u); }
uint  pdnMetaCarryBits(uint c) { return ((c & 1u) << 11u) | (((c >> 1u) & 3u) << 18u); }
// Bits 0-9 (radiance): the caster test's evidence prior, 10 bits (code 0 unknown, 1..1023 = [0, 1])
uint  pdnMetaVisibilityCode(float value)
{
    return !(value >= 0.0) || isinf(value) ? 0u : 1u + uint(clamp(value, 0.0, 1.0) * 1022.0 + 0.5);
}
float pdnMetaVisibility(uint w) { return (w & 1023u) == 0u ? -1.0 : float((w & 1023u) - 1u) / 1022.0; }
// bit 15 (the temporal pass): the texel's diffuse is CONVERGED under ReSTIR DI (#869 / #85: a reused
// reservoir of high M and a long history) -- the a-trous gives it fewer iterations (DN_CONV_SHIFT)
// Radiance mode disables convergence trimming and uses this bit for the current
// moved-light tag instead (temporal reset on departure, no moving spatial feedback).
bool  pdnMetaConv(uint w)   { return (w & 0x8000u) != 0u; }
float pdnMetaLenD(uint w)   { return float(w & 63u); }
float pdnMetaLenS(uint w)   { return float((w >> 6u) & 63u); }
uint  pdnMetaSpecIt(uint w) { return (w >> 12u) & 7u; }
float pdnMetaW(uint w)      { return unpackHalf2x16((w >> 16u) & 0xFFF0u).x; }

// ---- HIST word ----
uvec4 pdnPackHist(vec3 d, vec3 s, float varD, float varS)
{
    return uvec4(packHalf2x16(d.rg), packHalf2x16(vec2(d.b, s.r)), packHalf2x16(s.gb),
                 packHalf2x16(clamp(vec2(varD, varS), vec2(0.0), vec2(60000.0))));
}
void pdnUnpackHist(uvec4 h, out vec3 d, out vec3 s, out float varD, out float varS)
{
    const vec2 a = unpackHalf2x16(h.x), b = unpackHalf2x16(h.y), c = unpackHalf2x16(h.z), v = unpackHalf2x16(h.w);
    d = vec3(a, b.x);
    s = vec3(b.y, c);
    varD = v.x; varS = v.y;
}

// The specular a-trous depth from roughness and the hit distance (the lobe's footprint):
// a mirror keeps its own texel (0 iterations), a rough lobe takes all of them. f: the lobe's
// angular footprint ~ roughness^2, scaled by how far the reflected light lies relative to the
// receiver's distance (a distant reflected source keeps a tighter footprint on screen).
float pdnSpecRadius(float rough, float hitDist, float vz)
{
    const float a = rough * rough;
    const float hf = hitDist > 0.0 ? clamp(hitDist / max(hitDist + vz, 1e-3) * 2.0, 0.25, 1.0) : 1.0;
    return a * hf * 48.0;   // denoiser texels, same support as the iteration selector
}
uint pdnSpecIters(float rough, float hitDist, float vz, uint maxIt)
{
    const float radius = pdnSpecRadius(rough, hitDist, vz);
    if (radius < 0.75) return 0u;
    return min(uint(floor(log2(radius))) + 1u, maxIt);
}

// RATIO SIGNALS (the projector sampler, #85 phase 2 ratio estimator): a producer may instead
// write a VISIBILITY RATIO per signal (shadowed / unshadowed estimate of the same samples, rgb in
// [0, 1]; Heitz et al. 2018) -- no light colour ever enters the history; the consumer multiplies
// its exact, fresh unshadowed lighting by the denoised ratio.
// Debug view codes (RenderVKDenoiseDebug; the consumer's push bits): 1 raw input (layer 0, one
// frame), 2 history length (r diffuse, g specular, of the max), 3 variance (r diffuse, g
// specular, sqrt relative to the signal), 4 the denoised layer 0 (diffuse), 5 the denoised
// layer 1 (specular), 6 the denoised layer 0 (left half) beside the lit image (right half).
#define PDN_DBG_RAW 1u
#define PDN_DBG_LEN 2u
#define PDN_DBG_VAR 3u
#define PDN_DBG_DEMOD 4u
#define PDN_DBG_REMOD 5u
#define PDN_DBG_SPLIT 6u
// 7 (#869) the ReSTIR view: the producer writes a false colour into layer 0 (pass 15 PDN_OPT_DBG_RS:
// the chosen light's hue x the reservoir's M); the denoiser passes it through like RAW.
#define PDN_DBG_RESTIR 7u
// 8 / 9 (#869) the split lights by id / x their denoised visibility (SPLIT CHANNEL below; the
// denoiser's debug field is 4 bits wide for them, the consumer's 3: the host sends it 4).

// ---- SUN / MOON CHANNEL (the merged RT shadow pass, RenderVKRTShadowMerge: #195 + #85) ----
// Present only when LLCleanDenoiserVK::ensure(.., sun = true) allocated it (IN / OUT 3 layers,
// SUNHIST) and used only on frames whose push says so (clean_dn_common.glsl DN_F_SUN); every
// other frame runs the projector-only code exactly.
//   IN  layer 2  rgb = this frame's raw sun / moon visibility x translucent tint (the RT shadow
//                mask's sun convention: [0, 1], the cascade-range fade and the weak-sun fade
//                applied by the producer), a = pdnSunEncA (penumbra radius in denoiser texels
//                + the moving-caster bit).
//   SUNHIST layer parity (rgba32ui, binding 7): x packHalf2x16(acc r, g), y (acc b, variance),
//                z (versioned negative metadata: -(length + 65 x countdown + 1), fast luminance mean),
//                w (the penumbra radius the history holds, -1 none; fast luminance 2nd moment).
//   OUT layer 2  rgb = the accumulated / filtered visibility, a = variance (the final a-trous
//                iteration: the view depth, for the SHADOW kernel's depth-aware upsample).
// Its history is never cut by W (a club light switching says nothing about the sun).
float pdnSunEncA(float r, bool dyn) { return r < 0.0 ? -1.0 : (dyn ? -(2.0 + r) : r); }
void  pdnSunDecA(float a, out float r, out bool dyn)
{
    if (isnan(a) || isinf(a)) { r = -1.0; dyn = false; return; }
    dyn = a < -1.5;
    r = a > -0.5 ? a : (dyn ? -a - 2.0 : -1.0);
}
uvec4 pdnPackSunHist(vec3 acc, float var, float len, float cnt, float fm, float fm2, float r)
{
    const float lc = clamp(floor(len + 0.5), 0.0, 64.0) + 65.0 * floor(clamp(cnt, 0.0, 7.0) + 0.5);
    return uvec4(packHalf2x16(acc.rg), packHalf2x16(vec2(acc.b, clamp(var, 0.0, 60000.0))),
                 packHalf2x16(vec2(-(lc + 1.0), clamp(fm, 0.0, 1.0))), packHalf2x16(vec2(clamp(r, -1.0, 60000.0), clamp(fm2, 0.0, 1.0))));
}
struct PdnSunHist { vec3 acc; float var; float len; float cnt; float fm; float fm2; float r; };
PdnSunHist pdnUnpackSunHist(uvec4 h)
{
    PdnSunHist o;
    const vec2 a = unpackHalf2x16(h.x), b = unpackHalf2x16(h.y), c = unpackHalf2x16(h.z), d = unpackHalf2x16(h.w);
    o.acc = vec3(a, b.x);
    o.var = b.y;
    // Version 2: every metadata integer [-520,-1] is exact in FP16. Old radix-64
    // histories and cleared texels are nonnegative: reject, never reinterpret them.
    // Old decoders likewise clamp this negative tag to zero history length.
    if (!(c.x <= -1.0 && c.x >= -520.0))
    {
        o.acc = vec3(0.0); o.var = 0.0; o.len = 0.0; o.cnt = 0.0;
        o.fm = 0.0; o.fm2 = 0.0; o.r = -1.0;
        return o;
    }
    const float lc = -c.x - 1.0;
    o.cnt = floor(lc / 65.0);
    o.len = lc - 65.0 * float(o.cnt);
    o.fm = c.y;
    o.fm2 = d.y;
    o.r = d.x;
    return o;
}
// Visibility in [0, 1] (NaN / Inf -> lit).
vec3 pdnSunVis(vec3 v) { return (any(isnan(v)) || any(isinf(v))) ? vec3(1.0) : clamp(v, vec3(0.0), vec3(1.0)); }

// ---- SPLIT CHANNEL: PER-LIGHT VISIBILITY OF THE DOMINANT LIGHTS (#869, RenderVKProjStochasticSplitK) ----
// THE LEAK IT FIXES: the ratio estimator above gives every light at a texel ONE shared ratio
// (sum of shadowed / sum of unshadowed over the sampled lights). Where an unblocked light and a
// blocked one meet -- a sealed room's own lamps beside the club's projector behind its wall -- the
// ratio is the lamps' visibility x their share, well above 0, and the consumer multiplied the
// BLOCKED projector's exact unshadowed gobo by it: its gobo on the room's wall at full detail, only
// dimmer; and the lamps were darkened by the projector's 0. The standard remedy (one ratio per
// light where it matters): the K strongest lights of the lighting set at the texel (by their
// gobo-free unshadowed target, pass 15 pdnSplitSelect: a deterministic list -- the pixel cluster's,
// never the random RIS window -- with hysteresis for last frame's choice, ties by id) each get
// their OWN shadow ray every frame and their OWN visibility, denoised by key; the shared ratio
// (layers 0 / 1, now THE REST) is estimated over the other lights only (excluded from its
// numerator and its denominator: candidates, reused reservoirs, the dense evaluation). The
// consumer: direct = sum_k unshadowed_k x v_k + (the rest's unshadowed) x the rest ratio. The
// candidate set still equals the lighting set: rest = lighting set - split, split c lighting set.
// One light, one ray: its visibility serves its diffuse and specular share alike (MONO -- a split
// light's shadow through a coloured translucent caster shows its luminance, not its tint; the
// rest keeps rgb). Allocated only with K > 0 (LLCleanDenoiserVK::ensure(.., split = true));
// present <=> GEOM has PDN_SPLIT_GEOM_LAYERS layers (imageSize(..).z: no flag anywhere). K = 0
// or a producer that does not split (the per-light-type pass 15) writes id 0 in every slot: the
// consumer then finds no split light and every light takes the rest = the old shared ratio.
//   IDENTITY: Light.tex.w = the light's SPLIT ID, a host-assigned persistent 16-bit id per light
//     key (llcleanrenderervk.cpp splitIdsFrame; 0 = none: never split). The light list is
//     re-sorted every frame -- an index names another light next frame, the id does not -- so
//     the history follows the LIGHT, not the slot, with no remap (the denoiser has no ReSTIR
//     header): a light in slot 0 last frame and slot 1 now keeps its history; a new one starts.
//   GEOM layers 2 + parity (rgba32ui; the producer writes x..w, the temporal pass rewrites z, w):
//     x id0 | id1 << 16, y id2 | id3 << 16 (0 = empty slot);
//     z, w the slots' 16-bit words, slot k = word k of (z, w): the producer's RAW sample
//     (pdnSplitRawPack: value x 32766, 0x7FFF = no information -- the light has no term here --,
//     bit 15 = the light MOVED this frame: the moving-light history cap), then the temporal
//     pass's HISTORY (pdnSplitHistPack: the accumulated visibility, 11 bits, + its length, 5 bits).
//   GEOM layer 4: immutable current RAW snapshot during the temporal dispatch
//     (host copy from 2 + parity), then FINAL written by the last a-trous iteration.
//     FINAL is parity-free: x, y the ids, z, w denoised visibility (unorm16).
//     Temporal reads RAW only here, writes HISTORY to 2 + parity, and reads old
//     HISTORY at 2 + (parity ^ 1). No final consumer runs during the snapshot lifetime.
//   IN / OUT the LAST layer (rgba16f): the slots' visibilities (temporal output, a-trous
//     ping-pong; empty slots 1). The a-trous luminance stop's sigma is the Bernoulli bound
//     sqrt(v (1 - v) / length) of the one-ray-per-frame estimate (no stored variance).
//   +64 B per denoiser texel (GEOM 3 layers, IN / OUT 1 each; +16 B more without the sun channel:
//     its layer is allocated then too, so the split layer is always the last).
// Debug (RenderVKDenoiseDebug 8 / 9, the denoiser's last a-trous iteration writes it into layer 0;
// the consumer shows layer 0): 8 the split lights by id (a stable hue per id; with several slots,
// 4-texel diagonal bands cycle through them), 9 the same hue x that light's denoised visibility.
#define PDN_SPLIT_MAX 4u
#define PDN_SPLIT_GEOM_PAR 2          // + parity
#define PDN_SPLIT_GEOM_FINAL 4
#define PDN_SPLIT_GEOM_LAYERS 5       // GEOM's layer count with the split channel
#define PDN_SPLIT_RAW_NONE 0x7FFFu
#define PDN_DBG_SPLIT_ID 8u
#define PDN_DBG_SPLIT_RATIO 9u
#define PDN_DBG_SKIN_RECOVERY 10u
#define PDN_DBG_HISTORY_CAUSE 11u
#define PDN_DBG_EXACT_SPECULAR 12u
#define PDN_DBG_DROP 13u
// (RenderVKShadowMoverMask 2) the mover-exact set (clean_dn_atrous.comp, the last iteration): red = exact this frame,
// green = its mover countdown runs (history capped), blue = the countdown / 15
#define PDN_DBG_MOVER_EXACT 14u              // (clean_dn_temporal.comp THE LIGHT DROP's per-texel view; the a-trous passes it)
// slot k's 16-bit word of a (lo, hi) pair: k 0 / 1 in x, 2 / 3 in y
uint  pdnSplitWord(uvec2 p, uint k) { return ((k < 2u ? p.x : p.y) >> ((k & 1u) * 16u)) & 0xFFFFu; }
uvec2 pdnSplitPack4(uvec4 w) { return uvec2((w.x & 0xFFFFu) | (w.y << 16u), (w.z & 0xFFFFu) | (w.w << 16u)); }
// the slot holding id (-1: none)
int   pdnSplitFind(uvec2 ids, uint id)
{
    int at = -1;
    for (uint k = 0u; k < PDN_SPLIT_MAX; ++k)
        if (at < 0 && id != 0u && pdnSplitWord(ids, k) == id) at = int(k);
    return at;
}
// (pass 15) light l is one of this texel's split lights spLi (empty: 0xFFFFFFFF) -- the rest leaves it out
bool  pdnSplitHas(uvec4 spLi, uint l) { return any(equal(spLi, uvec4(l))); }
uint  pdnSplitRawPack(float v, bool info, bool moved)
{
    const uint q = info ? uint(clamp(v, 0.0, 1.0) * 32766.0 + 0.5) : PDN_SPLIT_RAW_NONE;
    return q | (moved ? 0x8000u : 0u);
}
float pdnSplitRawVal(uint w)   { const uint q = w & 0x7FFFu; return q == PDN_SPLIT_RAW_NONE ? -1.0 : float(q) * (1.0 / 32766.0); }
bool  pdnSplitRawMoved(uint w) { return (w & 0x8000u) != 0u; }
// History word: v in 11 bits rounded TOWARD this frame's sample (toward): every frame moves it at
// least one step to the sample, so a constant lit / shadowed light converges to exactly 1 / 0
// (nearest rounding stalled ~1 % short at a 31-frame history: the step fell under half a unit).
uint  pdnSplitHistPack(float v, float len, float toward)
{
    const float x = clamp(v, 0.0, 1.0) * 2047.0;
    const float q = toward * 2047.0 >= x ? ceil(x - 1e-3) : floor(x + 1e-3);
    return uint(clamp(q, 0.0, 2047.0)) | (uint(clamp(len, 0.0, 31.0) + 0.5) << 11u);
}
float pdnSplitHistVal(uint w) { return float(w & 2047u) * (1.0 / 2047.0); }
float pdnSplitHistLen(uint w) { return float(w >> 11u); }
uint  pdnSplitFinalPack(float v) { return uint(clamp(v, 0.0, 1.0) * 65535.0 + 0.5); }
float pdnSplitFinalVal(uint w)   { return float(w) * (1.0 / 65535.0); }
// The debug views' colour of split id (a stable hue per id).
vec3 pdnSplitHue(uint id)
{
    const float h = fract(float(id) * 0.61803399);
    return clamp(abs(fract(vec3(h) + vec3(0.0, 2.0, 1.0) / 3.0) * 6.0 - 3.0) - 1.0, 0.0, 1.0);
}

#endif
