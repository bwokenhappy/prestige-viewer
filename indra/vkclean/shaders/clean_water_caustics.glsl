// Bounded water lens for the deferred main view. This is an analytical approximation,
// not a refracted-ray map: the wave Hessian omits horizontal Gerstner remapping and
// breaker curvature. A CPU lower envelope certifies submerged receivers, including
// horizontal displacement and convex grid morph/triangle interpolation. Ambiguous
// shallow receivers get no caustics. No extra targets, passes or rays.
#ifndef CLEAN_WATER_CAUSTICS_GLSL
#define CLEAN_WATER_CAUSTICS_GLSL
#define CLEAN_SEA_CAUSTICS_CURVATURE
#include "clean_sea_state.glsl"

// Signed compression, rather than an unbounded reciprocal determinant. For a small
// symmetric oscillating lens this is odd and approximately zero mean, so the gain
// redistributes direct diffuse instead of adding a positive-only glowing pattern.
// The denominator regularizes folds (det J near zero) and grazing light alike.
float waterCausticFocus(vec3 lens)
{
    const float trace = lens.x + lens.z;
    const float detJ = (1.0 + lens.x) * (1.0 + lens.z) - lens.y * lens.y;
    return -trace / (1.0 + abs(trace) + 0.25 * abs(detJ - 1.0 - trace));
}

vec2 waterCausticNormalTex(int idx, vec2 uv, float footprint)
{
    if (idx < 0) return vec2(0.0);
    const vec2 size = vec2(textureSize(uTextures[nonuniformEXT(uint(idx))], 0));
    // No negative material/upscaler mip bias: filter to the actual render footprint.
    const float lod = max(log2(max(footprint * max(size.x * 0.45, size.y * 0.9), 1.0)), 0.0);
    return textureLod(uTextures[nonuniformEXT(uint(idx))], uv, lod).xy * 2.0 - 1.0;
}
vec2 waterCausticDetail(vec2 xy, float footprint)
{
    // The water shader's middle normal layer, weight 0.4 * 0.5. Both EEP maps and
    // their day-cycle blend are retained; the swell supplies the large-scale lens.
    vec2 v = xy;
    v.x += (cos(v.x * 0.08) + sin(v.y * 0.02)) * 6.0;
    const vec2 uv = v * vec2(0.45, 0.9) + ssr.water[0].zw * (pc.u_cam_time.w * 0.5 * 0.13);
    const int ia = int(ssr.water[5].y), ib = int(ssr.water[5].z);
    const float blend = clamp(ssr.water[1].w, 0.0, 1.0);
    vec2 n = vec2(0.0);
    if (blend < 1.0) n += (1.0 - blend) * waterCausticNormalTex(ia, uv, footprint);
    if (blend > 0.0) n += blend * waterCausticNormalTex(ib, uv, footprint);
    return n * (0.2 * (seaOn() ? seaDetail() : 1.0)) * ssr.water[1].xy;
}

// Kept eye-relative for both desktop and layered stereo (deferInvVP's contract).
float waterCausticFootprint(vec3 relative)
{
    const vec2 extent = vec2(float(g_draw_push.width), float(g_draw_push.height));
    const vec2 ndc = CLEAN_FRAG_XY / extent * 2.0 - 1.0;
    const vec4 h = deferInvVP * vec4(ndc, 0.5, 1.0);
    const vec3 atNear = h.xyz / h.w;
    const float scale = linfo.cam_fwd.w > 0.5 ? 1.0 : dot(relative, atNear) / max(dot(atNear, atNear), 1e-8);
    const vec4 dx = deferInvVP[0] * (2.0 * scale / (extent.x * h.w));
    const vec4 dy = deferInvVP[1] * (2.0 * scale / (extent.y * h.w));
    return max(max(length(dx.xyz - atNear * dx.w), length(dy.xyz - atNear * dy.w)), 0.02);
}

float waterCausticGain(vec3 P, vec3 L)
{
    // Uniform off/opaque-water gate is before every wave, field and texture access.
    if (ssr.sea[6].z < 0.5 || L.z <= 0.02 || !specSunOn()
        || litDbg(LDBG_NO_SUN_SHADOW) || ssr.rt_mask.x < -1.5) return 1.0;
    // specSunOn is false when the host has no atlas/pipelines or shadows are off.
    // The final cascade / RT range fades to unshadowed; stop before that region.
    const float shadowDepth = dot(P - linfo.cam_pos.xyz, linfo.cam_fwd.xyz);
    if (shadowDepth >= 0.8 * ssr.shadow_clip.w) return 1.0;
    const float shadowFade = 1.0 - smoothstep(0.6 * ssr.shadow_clip.w, 0.8 * ssr.shadow_clip.w, shadowDepth);
    // This recording's minimum possible drawn height, not height sampled at the
    // displaced XY. All vertices and their convex morph/triangle combinations
    // are above it. Include receiver depth-unprojection/float tolerance.
    const float receiverError = 0.002 + 0.0000076293945 * (abs(P.z) + abs(pc.u_cam_time.z) + max(shadowDepth, 0.0) + 1.0);
    if (!(P.z + receiverError < ssr.sea[6].w)) return 1.0;
    // seaField also supplies neighbour-region height offsets (not the mean plane).
    const SeaTexel field = seaOn() ? seaField(P.xy) : seaOpenSea();
    const float plane = waterHeight() + field.dh;
    const float meanDepth = plane - P.z;
    if (meanDepth >= 24.0 || field.depth <= 0.0) return 1.0;
    const float incidence = smoothstep(0.02, 0.12, L.z);
    const vec3 ray = refract(-L, vec3(0.0, 0.0, 1.0), 0.75);
    const float cosT = max(-ray.z, 0.1);
    // The same extinction law as waterFogLL, but only on the incoming light path.
    // Existing fog handles the outgoing view path once, downstream of this gain.
    const float extinction = exp(-0.0343446 * max(ssr.water[4].w, 0.0) * max(meanDepth, 0.0) / cosT);
    if (extinction < 0.01) return 1.0;

    // World footprint at this receiver's depth. Uses this eye's projection and render
    // extent; works for perspective, orthographic and stereo without derivatives.
    const float footprint = waterCausticFootprint(P - pc.u_cam_time.xyz);
    const float footprintFade = 1.0 - smoothstep(0.12, 0.5, footprint);
    if (footprintFade <= 0.0) return 1.0;

    const float depth = meanDepth;
    if (depth <= 0.0 || depth >= 24.0) return 1.0;   // dry even in a trough below the mean
    const vec2 entry = P.xy - ray.xy * (depth / cosT);
    SeaSurf surface = seaFlatSurface();
    if (seaOn()) surface = seaSurface(entry, length(entry - ssr.cam_origin.xy), footprint);

    const float stepM = max(0.18, footprint * 2.0);
    const vec2 n = waterCausticDetail(entry, footprint);
    const vec2 nx = (waterCausticDetail(entry + vec2(stepM, 0.0), footprint) - n) / stepM;
    const vec2 ny = (waterCausticDetail(entry + vec2(0.0, stepM), footprint) - n) / stepM;
    // Normals lean opposite to the height gradient; symmetrize the detail derivative.
    const vec3 curvature = surface.curvature - vec3(nx.x, 0.5 * (nx.y + ny.x), ny.y);
    const float bend = 1.0 - 0.75 * L.z / cosT;
    const vec3 lens = curvature * (min(depth, 12.0) * bend);
    const float weight = shadowFade * incidence * extinction * footprintFade * smoothstep(0.0, 0.25, depth)
                       * (1.0 - smoothstep(8.0, 24.0, depth));
    return 1.0 + 0.65 * weight * waterCausticFocus(lens);
}
#endif
