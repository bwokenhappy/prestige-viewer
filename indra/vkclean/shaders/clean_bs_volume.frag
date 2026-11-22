#version 460
#extension GL_GOOGLE_include_directive : require
#extension GL_EXT_samplerless_texture_functions : require
#extension GL_EXT_nonuniform_qualifier : require
#define CLEAN_SKY_LUT   // (#819) skyColor() reads the sky-view LUT (draw set 0 binding 52) when valid
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
#extension GL_EXT_ray_query : require   // the ray-query twin: the sun ray + one picked local light's ray per cell
#endif
// vkclean -- SPLIT TRANSPARENCY, STAGE A: THE TRANSLUCENCY LIGHTING VOLUME (RenderVKBlendSplit; clean_bs_common.glsl
// has the grid and the cell contents; LLCleanBlendSplitVK records it).
//
// A fullscreen triangle over a 2D layout of the cells -- tile t = the grid's slice t, bs.tile.x tiles per row, each
// W x H -- with no attachments: every fragment is one cell and writes it with imageStore. A fragment shader, not a
// compute kernel, because it runs the forward lighting's own code unchanged (clean_draw_common.glsl in its
// CLEAN_COMPUTE_LIGHT form, as the WBOIT resolve does): the cascades / the traced sun, the cluster lists, the probe walk,
// the GI cache and the forward alpha tail (surfacePost). CLEAN_FRAG_XY = the cell centre's main-view pixel (the
// cluster lookups).
//
// PER CELL (its centre P on its view ray at its slice's centre depth):
//   * OPAQUE CULL: a cell whose near boundary lies beyond the FARTHEST opaque depth of its 3 x 3 cell neighbourhood
//     can hold no visible blend layer. The first such cell takes the values of the point just in front of that
//     surface (the trilinear reads of the layers right in front of it then see that surface's light, not the far
//     side of a wall); the ones behind it are written 0 and never read.
//   * SUN / MOON: the forward sunlit x visibility (sunShadowT with the light as the normal: the cascades, or one ray in
//     the ray-query twin; a traced frame without it: the clouds only) -> cell 3, the direction is global.
//   * LOCAL LIGHTS (the main view's cluster of P; the whole table when the cluster overflowed): each light's
//     irradiance at P (LL's attenuation x the cutoff fade, a projector's gobo texel inside its frustum) as a delta in
//     its direction (bsShDelta). Shadow-mapped projectors: one atlas tap. Ray-traced shadows (ray-query twin): every
//     light is added unshadowed, and ONE weighted reservoir pick (luminance) over the traced ones gets one shadow ray
//     whose OCCLUDED share is subtracted -- unbiased, noise-free where nothing blocks, the temporal blend converges it.
//   * AMBIENT: the probes' irradiance at +Z and -Z (sampleProbesEx; the sky ambient where no probe covers P), on GI
//     frames mixed with the radiance cache's (giCacheForward) as giForwardRefl mixes them -> a vertical L1 gradient.
//   * THE TAIL: lightSurface's haze, surfacePost, the froxel fog and the veil at P for colour 0 and 1 (affine:
//     offset, slope).
//   * TEMPORAL: the lighting cells (0-3) blend with the previous build at P's reprojected cell (bs.knobs.z the new
//     frame's weight; 1 = none). The tail is fresh every frame.

#include "clean_view.glsl"
#define CLEAN_DEBUG_CONST
const bool kDebugViews = false;
#define CLEAN_SPEC_SETTINGS       // the graphics-setting specialization constants (the live key's)
#define CLEAN_COMPUTE_LIGHT       // no vertex inputs (clean_draw_common.glsl) ...
#define CLEAN_COMPUTE_V_FLAGS 0u  // ... and the opaque rules (no blend-layer sharing / front records)
vec2 g_bs_xy = vec2(0.0);         // the cell centre's main-view pixel
#define CLEAN_FRAG_XY g_bs_xy
#include "clean_draw_common.glsl"
#define CLEAN_GI_FORWARD
#define GI_STATE_SET 0
#define GI_STATE_BINDING 53
#define GI_STATE_ACCESS readonly
#define GI_STATE_READONLY
#include "clean_gi_common.glsl"
#include "clean_surface_light.glsl"   // surfacePost and the haze globals (lightSurface itself is not called)
#include "clean_vol_apply.glsl"
#include "clean_weather_veil.glsl"
#define BS_WRITE_VOLUME
#include "clean_bs_common.glsl"

// The scene's (opaque) depth, as the blend FS's tolerance test reads it (draw set 0 binding 40; reversed-Z, 0 = sky).
layout(set = 0, binding = 40) uniform sampler2DArray uBsSceneDepth;

// The view depth of the farthest opaque surface over the 3 x 3 cells around this one (lo / hi: this cell's footprint,
// render px; 1e30 = the sky in it). The NEIGHBOURS count: stage D reads the volume trilinearly, so a layer in a
// neighbouring column reads this cell too -- it may be culled only when no such reader can exist.
float bsFootprintFar(vec2 lo, vec2 hi)
{
    const ivec2 res = textureSize(uBsSceneDepth, 0).xy;
    const vec2 cs = hi - lo;
    float zmin = 1.0;
    for (int k = 0; k < 13; ++k)
    {
        // (this cell's 4 corners + centre, then the 8 neighbours' centres)
        vec2 f;
        if (k < 4) f = vec2(k & 1, k >> 1) * 0.9 + 0.05;
        else if (k == 4) f = vec2(0.5);
        else { const int n = k - 5 + (k - 5 >= 4 ? 1 : 0); f = vec2(n % 3 - 1, n / 3 - 1) + 0.5; }
        const ivec2 p = clamp(ivec2(lo + f * cs), ivec2(0), res - 1);
        zmin = min(zmin, texelFetch(uBsSceneDepth, ivec3(p, 0), 0).r);
    }
    if (!(zmin > 0.0)) return 1e30;
    // (the farthest tap's view depth: its pixel's ray through the inverse view-projection)
    const vec2 uv = (lo + hi) * 0.5 / vec2(res);
    const vec4 h = bs.ivp * vec4(uv * 2.0 - 1.0, zmin, 1.0);
    return abs(h.w) > 1e-12 ? dot(h.xyz / h.w, bs.fwd.xyz) : 1e30;
}

// The tail at P for colour c (clean_draw.frag main's order for an alpha-blend lit face).
vec3 bsTail(vec3 c, vec3 P)
{
    const vec3 cam = pc.u_cam_time.xyz;
    g_haze_ok = false;
    if (!cmOn() && !litDbg(LDBG_NO_HAZE))
    {
        vec3 sunlit, amblit, add, att;
        atmosLightTerms(P - cam, vec3(0.0, 0.0, 1.0), sunlit, amblit, add, att);
        c = atmosOrWaterFog(c, P, cam, add, att);
        g_haze_add = add; g_haze_atten = att; g_haze_ok = true;
    }
    g_medium_inline = true;
    // (noFade: the draw-distance fade's target -- the sky and the sea's horizon colour along the ray -- was ~330k
    //  SPIR-V words of this module; the deep layers in the last stretch of the draw distance keep their own colour.
    //  The front layer -- stage C -- fades exactly.)
    c = surfacePost(SURF_LEGACY, true, c, P);
    if (!cmOn() && volOn()) c = volFogApply(c, P);
    c = weatherVeilApply(c, P);
    return c;
}

#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
// One weighted reservoir step on a single uniform number (clean_vol_light.comp volPickStep).
bool bsPickStep(inout float u, float p)
{
    if (u < p) { u = clamp(u / max(p, 1e-30), 0.0, 0.99999994); return true; }
    u = clamp((u - p) / max(1.0 - p, 1e-30), 0.0, 0.99999994);
    return false;
}
// An opaque shadow ray from P toward light li (a point: to its prim's sphere; a projector: to its near plane).
float bsLightRay(uint li, vec3 P)
{
    const Light L = lights[li];
    if (L.proj_origin.w < 0.5)
    {
        const vec3 C = L.pos_radius.xyz;
        const float nearClip = min(clamp(L.proj_axis.x, 0.0, 0.5 * L.pos_radius.w), RS_POINT_NEAR_MAX);
        const vec3 toL = C - P;
        const float len = length(toL);
        const float tmax = len - nearClip - 0.002;
        if (!(tmax > 0.0)) return 1.0;
        return traceShadowRayOpaque(P, toL / len, 0.0, tmax, RT_MASK_SHADOW & ~RT_MASK_BLEND);
    }
    const vec3 O = L.proj_origin.xyz, A = L.proj_axis.xyz;
    const float nearP = L.tex.z != 0u ? pshadow[L.tex.z - 1u].params.y : 0.0;
    const vec3 toL = O - P;
    const float len = length(toL);
    if (len <= 1e-4) return 1.0;
    const vec3 dir = toL / len;
    const float da = dot(dir, A), dp = dot(P - O, A);
    float tmax = (da < -1e-6) ? (nearP - dp) / da : len;
    tmax = min(tmax, len) - 0.002;
    if (!(tmax > 0.0)) return 1.0;
    return traceShadowRayOpaque(P, dir, 0.0, tmax, RT_MASK_SHADOW & ~RT_MASK_BLEND);
}
// The cell's rotated Weyl number of this frame.
float bsSeq(ivec3 c, uint frame)
{
    const uint h = (uint(c.x) * 73856093u) ^ (uint(c.y) * 19349663u) ^ (uint(c.z) * 83492791u);
    return fract(float(h & 0xFFFFu) * (1.0 / 65536.0) + float(frame) * 0.61803399);
}
#endif

// The local lights at P as three SH channels (+ the picked traced light's estimate, ray-query twin).
void bsLocal(vec3 P, ivec3 cell, inout vec4 shR, inout vec4 shG, inout vec4 shB)
{
    const uint count = uint(linfo.cam_pos.w);
    const uint c = mainClusterProject(P);
    const uint craw = cluster_count[c];
    const bool walkAll = clusterWalkAll(craw);
    const uint n = walkAll ? min(count, uint(lights.length())) : clusterLen(craw);
    const uint nrec = uint(pshadow.length());
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
    const bool trace = (bs.flags.y & BSF_TRACE) != 0u;
    float u = bsSeq(cell, bs.flags.z);
    float sumW = 0.0;
    uint pick = 0xFFFFFFFFu;
    vec3 pickE = vec3(0.0), pickL = vec3(0.0);
#endif
    for (uint i = 0u; i < n; ++i)
    {
        const uint li = walkAll ? i : cluster_index[c * CLUSTER_CAP + i];
        if (li >= count) continue;
        const vec4 cf = lights[li].color_falloff;
        vec3 lv;
        float dist;
        const float atten = lightReach(lights[li].pos_radius, cf, P, lv, dist);
        if (atten <= 0.0) continue;
        vec3 l = lv / max(dist, 1e-4);
        vec3 E;
        bool traced = false;
        if (lights[li].proj_origin.w < 0.5)
        {
            E = cf.rgb * atten;
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
            // (points: the ray-traced shadow the forward pass gives them only through pass 15 / the mask)
            traced = trace && lights[li].proj_axis.y < 0.5;
#endif
        }
        else
        {
            const Light L = lights[li];
            vec2 ptc;
            if (!lightProjFrustum(L.proj_mat, P, ptc)) continue;
            l = normalize(L.proj_origin.xyz - P);
            E = projDiffuse(L, -dot(lv, L.proj_axis.xyz), ptc.xy) * atten;
            if (specProjOn() && L.tex.z != 0u && L.tex.z - 1u < nrec && !(pshadow[L.tex.z - 1u].params.w < 0.5))
            {
                if (pshadow[L.tex.z - 1u].params.z > 1.5)
                {
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
                    traced = trace;
#endif
                }
                else E *= projShadowMap(L, L.tex.z - 1u, P, l);
            }
        }
        if (any(isnan(E)) || any(isinf(E))) continue;
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
        if (traced)
        {   // (unshadowed below, every frame; the pick estimates only what is OCCLUDED: no noise where nothing blocks)
            const float w = dot(E, vec3(0.2126, 0.7152, 0.0722));
            if (w > 0.0)
            {
                sumW += w;
                if (bsPickStep(u, w / sumW)) { pick = li; pickE = E / w; pickL = l; }
            }
        }
#endif
        shR += bsShDelta(E.r, l); shG += bsShDelta(E.g, l); shB += bsShDelta(E.b, l);
    }
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
    if (pick != 0xFFFFFFFFu)
    {
        // sum_i E_i (1 - v_i) ~ E_k / w_k x sum w x (1 - v_k): the occluded share, taken off the unshadowed sum
        const vec3 E = pickE * (sumW * (1.0 - bsLightRay(pick, P)));
        shR -= bsShDelta(E.r, pickL); shG -= bsShDelta(E.g, pickL); shB -= bsShDelta(E.b, pickL);
    }
#endif
}

// The ambient irradiance pair (+Z, -Z) at P: the probes (or the sky ambient), GI-mixed as giForwardRefl.
void bsAmbient(vec3 P, out vec3 Eu, out vec3 Ed)
{
    const vec3 cam = pc.u_cam_time.xyz;
    vec3 su, au, ad, sd, x0, x1;
    atmosLightTerms(P - cam, vec3(0.0, 0.0, 1.0), su, au, x0, x1);
    atmosLightTerms(P - cam, vec3(0.0, 0.0, -1.0), sd, ad, x0, x1);
    Eu = au; Ed = ad;
    if ((bs.flags.y & BSF_PROBES) != 0u)
    {
        vec3 prad, prad2, pirr;
        if (sampleProbesEx(P, vec3(0.0, 0.0, 1.0), vec3(0.0, 0.0, 1.0), 0.0, 0.0, au, false, false, true, prad, prad2, pirr))
            Eu = pirr;
        if (sampleProbesEx(P, vec3(0.0, 0.0, -1.0), vec3(0.0, 0.0, -1.0), 0.0, 0.0, ad, false, false, true, prad, prad2, pirr))
            Ed = pirr;
    }
    if (giFrameOn() && (gi.cfg.x & GI_F_ON) != 0u && (gi.cfg.x & GI_F_CACHE) != 0u && giCacheSlots() != 0u)
    {
        const float floorA = clamp(gi.prm.y, 0.0, 1.0);
        for (int s = 0; s < 2; ++s)
        {
            float wf;
            bool inSph;
            const vec3 N = s == 0 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 0.0, -1.0);
            vec3 E = giCacheForward(P, N, linfo.cam_pos.xyz, wf, inSph);
            if (!(wf > 0.0)) continue;
            E = max(E, vec3(0.0)) * max(gi.prm.x, 0.0);
            const vec3 Eg = atmosClassic() ? atmosLinearToSrgb(E) : E;
            const vec3 probe = s == 0 ? Eu : Ed;
            const vec3 r = max(mix(probe, Eg, wf), probe * floorA);
            if (s == 0) Eu = r; else Ed = r;
        }
    }
}

void main()
{
    const uvec2 fp = uvec2(gl_FragCoord.xy);
    const uint W = uint(bs.grid.x + 0.5), H = uint(bs.grid.y + 0.5), D = uint(bs.grid.z + 0.5);
    const uint k = (fp.y / H) * max(bs.tile.x, 1u) + fp.x / W;
    if (k >= D || fp.x / W >= max(bs.tile.x, 1u)) return;
    const ivec3 cell = ivec3(int(fp.x % W), int(fp.y % H), int(k));
    const vec2 rs = vec2(float(bs.flags.w & 0xFFFFu), float(bs.flags.w >> 16u));
    const vec2 uv = (vec2(cell.xy) + 0.5) / vec2(W, H);
    g_bs_xy = uv * rs;
    // the cell's ray (camera-relative) and its slice's depths
    const vec4 h = bs.ivp * vec4(uv * 2.0 - 1.0, 0.5, 1.0);
    vec3 d = abs(h.w) > 1e-12 ? h.xyz / h.w : bs.fwd.xyz;
    d = dot(d, d) > 1e-20 ? normalize(d) : bs.fwd.xyz;
    const float df = max(dot(d, bs.fwd.xyz), 1e-4);
    const float zNear = bsSliceDepth(float(k) / float(D));
    const float zMid = bsSliceDepth((float(k) + 0.5) / float(D));
    float vz = zMid;
    // OPAQUE CULL (header)
    {
        const vec2 lo = vec2(cell.xy) / vec2(W, H) * rs, hi = (vec2(cell.xy) + 1.0) / vec2(W, H) * rs;
        const float zo = bsFootprintFar(lo, hi);
        if (zNear > zo)
        {
            const float zPrev = k > 0u ? bsSliceDepth((float(k) - 1.0) / float(D)) : 0.0;
            if (k > 0u && zPrev > zo)
            {   // (two slices behind: never read)
                imageStore(uBsOutShR, cell, vec4(0.0)); imageStore(uBsOutShG, cell, vec4(0.0));
                imageStore(uBsOutShB, cell, vec4(0.0)); imageStore(uBsOutSun, cell, vec4(0.0));
                imageStore(uBsOutPostO, cell, vec4(0.0)); imageStore(uBsOutPostS, cell, vec4(1.0));
                return;
            }
            vz = max(zo - 0.02, 0.0);   // (the first one behind: the point just in front of the surface)
        }
    }
    const vec3 P = bs.cam.xyz + d * (vz / df);

    // SUN / MOON
    const vec3 L = atmosLightDir();
    vec3 sunlit, amb0, add0, att0;
    atmosLightTerms(P - pc.u_cam_time.xyz, L, sunlit, amb0, add0, att0);
    if (atmosClassic()) sunlit *= 1.35;
    float vis = 1.0;
    vec3 tint = vec3(1.0);
    if (L.z > -0.2)
    {
#ifdef CLEAN_PROJ_SHADOW_RAYQUERY
        vis = sunShadowT(P, L, tint);
#else
        if (!specSunTraced()) vis = sunShadowT(P, L, tint);
        else vis = cloudShadowMap(P);   // (a traced frame without this twin: unshadowed under the clouds)
#endif
    }
    vec4 sun = vec4(sunlit * tint * vis, vis);

    // LOCAL LIGHTS + AMBIENT
    vec4 shR = vec4(0.0), shG = vec4(0.0), shB = vec4(0.0);
    if ((bs.flags.y & BSF_LOCAL) != 0u)
        bsLocal(P, cell, shR, shG, shB);
    {
        vec3 Eu, Ed;
        bsAmbient(P, Eu, Ed);
        shR += bsShVertical(Eu.r, Ed.r); shG += bsShVertical(Eu.g, Ed.g); shB += bsShVertical(Eu.b, Ed.b);
    }

    // TEMPORAL (header)
    const float a = clamp(bs.knobs.z, 0.0, 1.0);
    if (a < 1.0 && (bs.flags.y & BSF_HISTORY) != 0u)
    {
        const vec3 rel = P - bs.cam.xyz;
        const vec4 pc4 = bs.pvp * vec4(rel, 1.0);
        const float pvz = dot(rel, bs.pfwd.xyz) + bs.pfwd.w;
        if (pc4.w > 1e-4 && pvz > 0.0)
        {
            const vec2 puv = pc4.xy / pc4.w * 0.5 + 0.5;
            const float pw = log(max(pvz, bs.cam.w) / bs.cam.w) / max(bs.fwd.w, 1e-4);
            if (all(greaterThanEqual(puv, vec2(0.0))) && all(lessThanEqual(puv, vec2(1.0))) && pw <= 1.0)
            {
                const vec3 tc = vec3(puv, clamp(pw, 0.0, 1.0));
                const vec4 hR = textureLod(uBsHistR, tc, 0.0), hG = textureLod(uBsHistG, tc, 0.0);
                const vec4 hB = textureLod(uBsHistB, tc, 0.0), hS = textureLod(uBsHistSun, tc, 0.0);
                if (!(any(isnan(hR)) || any(isnan(hG)) || any(isnan(hB)) || any(isnan(hS))))
                {
                    shR = mix(hR, shR, a); shG = mix(hG, shG, a); shB = mix(hB, shB, a); sun = mix(hS, sun, a);
                }
            }
        }
    }
    const vec4 lim = vec4(60000.0);
    imageStore(uBsOutShR, cell, clamp(shR, -lim, lim));
    imageStore(uBsOutShG, cell, clamp(shG, -lim, lim));
    imageStore(uBsOutShB, cell, clamp(shB, -lim, lim));
    imageStore(uBsOutSun, cell, clamp(sun, vec4(0.0), lim));

    // THE TAIL (offset, slope)
    vec3 O = vec3(0.0), S = vec3(1.0);
    if ((bs.flags.y & BSF_POST) != 0u)
    {
        // (ONE call site in a loop the compiler keeps: the tail inlined twice was ~650k SPIR-V words)
        vec3 t[2];
        [[dont_unroll]] for (int i = 0; i < 2; ++i) t[i] = bsTail(vec3(float(i)), P);
        O = t[0];
        S = t[1] - t[0];
    }
    imageStore(uBsOutPostO, cell, vec4(clamp(O, vec3(-60000.0), vec3(60000.0)), 1.0));
    imageStore(uBsOutPostS, cell, vec4(clamp(S, vec3(-60000.0), vec3(60000.0)), 1.0));
}
