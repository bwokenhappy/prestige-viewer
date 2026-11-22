// Fixture for THE BLEND LAYERS' OWN LIGHT (RenderVKBlendHitLighting), run on the shaders' OWN code:
// scripts/check/checks/blend_hit.py preprocesses every forward lit FS that carries the path (clean_draw.frag's
// ray-query variants: the pass-2 blend FS, its wire / WBOIT-accumulation twins, the plain lit FS that draws the
// cluster-LOD rigged alpha; clean_oit_resolve.frag -DOIT_LIT's ray-query twin) per variant (glslc -E, the
// variant's own -D flags from indra/vkclean/regen_world_spv.py), cuts the layer's light walk (rtHitLocalLights,
// with the forward hooks expanded: the froxel mask, the exact slots, the glare) and its GI (giForwardRefl,
// giFwdCache, giFwdOn, blendHitEnv) out of it, turns the GLSL into C++ and compiles it in as BH_FN_FILE. The shims
// below stand in for the light buffers, the BRDFs, the froxel cell, the shadow / occlusion rays and the radiance
// cache; the walk, its budget, its estimator and the GI fallback are the shader's.
//   SEALED   a glass layer in a closed room: every light reaching it lies outside (a point lamp, a record-less
//            projector, a ray-traced projector record, a shadow-map record), every shadow test blocked -> 0 for
//            every frame, from the froxel cell and from the cluster list. (The forward walk lit it with every point
//            light and the record-less projector unshadowed: the old path.)
//   OWN      the room's own lamp visible beside those -> the mean over frames is the own lamp alone.
//   EXACT    the shadowed face: the key light blocked, a fill light visible (two lights: both in the exact slots)
//            -> the fill light alone on EVERY frame (no estimator noise where the strongest lights dominate).
//   NOISE    a lamp + five dim lights (half blocked): the frame-to-frame RMS error below plain next-event
//            estimation with the same 4 rays (clean_hit_nee.h hnAdd, K = 4) -- the measured budget choice.
//   STABLE   1, 2, 4 and 8 lights in reach (random strengths, about half blocked, 64 layouts each): the frame-to-frame
//            RMS error of the layer's light / its unshadowed total under BH_NOISE_MAX, and one ray per light. (The old
//            2 exact + 2 picks: 5 % at 4 lights, 18 % at 8 -- the grain inside the lights' reach, changing per frame.)
//            12 / 16 / 32 lights: the mean over frames is the exact shadowed sum (no bias in the sampled tail); the
//            residual is printed (NOISE-TAIL).
//   BUDGET   at most BLEND_HIT_M + BLEND_HIT_K shadow rays per shaded layer, whatever the light count (16 lights).
//   GLARE    a legacy shiny layer under a lamp: LL's alpha glare from its local specular (coverage under highlights).
//   VISGLARE the glare follows the light's shadow ray: a shiny lamp blocked by a wall raises no coverage (the old walk
//            kept it: a blocked lamp left the layer opaque and unlit).
//   DISC     the dark circle (owner, the pool, 2026-11-08): a shiny legacy water layer under a point lamp whose
//            centre sits inside its own opaque 30 cm bulb prim. The layer's light rays are the shader's own
//            (rtrProjRaySetup / rtrProjRay with rtHitLightSide / rtHitOrigin, clean_rt_ray_origin.h) over a walk
//            that blocks at the bulb unless the own-fixture rule is on (rt_fix_on with the light's centre and the
//            frame's RenderVKReSTIRFixtureSize, the rule pass 15 and the GI hits apply). Across the lamp's whole reach:
//            the layer is lit with its unshadowed light and glare. (Before: no ray took the rule -- zero light and
//            the full glare at every point of the reach: a dark, nearly opaque disc.) And a lamp BELOW the layer
//            (the back-face self-shadow, no ray) adds neither light nor glare.
//   GI       a glass layer with no radiance-cache cell: its own occlusion ray blocked (a sealed room) -> zero
//            ambient and zero environment specular with the probes on or off, in every GI hit-ambient mode; the
//            ray escaped (outdoors) -> the probes' ambient (modes 1, 2) and radiance kept; mode 0 / 3 -> no probe
//            ambient; the cache's own cover -> the cache; an RT-traced forward reflection keeps its specular.
//            (#1499) FAR: the ray blocked only past the GI ray length (a cloud's underside, R toward the distant ground)
//            -> the probes' ambient in modes 1 / 2 as a GI ray's miss takes them, ramped from half the length (RAMP,
//            monotone), the environment still blocked; a wall within half the length stays SEALED; a translucent-only
//            blocker keeps its transmittance. MOSAIC: across the cache cover's edge the far-blocked ambient never dips
//            below both the cell and the probes (the light / dark patches). FAR / RAMP / MOSAIC fail on the old rule.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

typedef uint32_t uint;
struct vec2 { float x, y; vec2(float a = 0.f, float b = 0.f) : x(a), y(b) {} vec2 xy() const { return *this; } };
struct vec3
{
    float x, y, z;
    vec3() : x(0.f), y(0.f), z(0.f) {}
    explicit vec3(float v) : x(v), y(v), z(v) {}
    vec3(float a, float b, float c) : x(a), y(b), z(c) {}
    vec3 xyz() const { return *this; }
    vec3& operator+=(vec3 o) { x += o.x; y += o.y; z += o.z; return *this; }
    vec3& operator-=(vec3 o) { x -= o.x; y -= o.y; z -= o.z; return *this; }
    vec3& operator*=(vec3 o) { x *= o.x; y *= o.y; z *= o.z; return *this; }
    vec3& operator*=(float s) { x *= s; y *= s; z *= s; return *this; }
};
static vec3 operator+(vec3 a, vec3 b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec3 operator-(vec3 a, vec3 b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec3 operator-(vec3 a) { return vec3(-a.x, -a.y, -a.z); }
static vec3 operator*(vec3 a, vec3 b) { return vec3(a.x * b.x, a.y * b.y, a.z * b.z); }
static vec3 operator*(vec3 a, float s) { return vec3(a.x * s, a.y * s, a.z * s); }
static vec3 operator*(float s, vec3 a) { return a * s; }
static vec3 operator/(vec3 a, float s) { return vec3(a.x / s, a.y / s, a.z / s); }
static vec3 operator/(vec3 a, vec3 b) { return vec3(a.x / b.x, a.y / b.y, a.z / b.z); }
struct vec4
{
    float x, y, z, w;
    vec4(float a = 0.f, float b = 0.f, float c = 0.f, float d = 0.f) : x(a), y(b), z(c), w(d) {}
    vec4(vec3 v, float d) : x(v.x), y(v.y), z(v.z), w(d) {}
    vec3 xyz() const { return vec3(x, y, z); }
    vec2 xy() const { return vec2(x, y); }
};
struct uvec4 { uint x, y, z, w; };
struct mat4 { float m[16]; };

static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float length(vec3 a) { return std::sqrt(dot(a, a)); }
static vec3 normalize(vec3 a) { return a / length(a); }
static float max(float a, float b) { return a > b ? a : b; }
static float min(float a, float b) { return a < b ? a : b; }
static uint max(uint a, uint b) { return a > b ? a : b; }
static uint min(uint a, uint b) { return a < b ? a : b; }
static vec3 max(vec3 a, vec3 b) { return vec3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)); }
static vec3 min(vec3 a, vec3 b) { return vec3(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)); }
static float clamp(float v, float a, float b) { return min(max(v, a), b); }
static vec3 clamp(vec3 v, vec3 a, vec3 b) { return min(max(v, a), b); }
static float mix(float a, float b, float t) { return a * (1.f - t) + b * t; }
static vec3 mix(vec3 a, vec3 b, float t) { return a * (1.f - t) + b * t; }
static uint floatBitsToUint(float f) { uint u; std::memcpy(&u, &f, 4); return u; }
static int bitCount(uint v) { return __builtin_popcount(v); }
static int findLSB(uint v) { return v ? __builtin_ctz(v) : -1; }
static float uintBitsToFloat(uint u) { float f; std::memcpy(&f, &u, 4); return f; }
static int floatBitsToInt(float f) { int i; std::memcpy(&i, &f, 4); return i; }
static float intBitsToFloat(int i) { float f; std::memcpy(&f, &i, 4); return f; }
using std::abs;

#include "clean_hit_nee.h"
#include "clean_rt_ray_origin.h"   // (rtHitLightSide / rtHitOrigin: the light rays' side and origin, as-is)

// ---- the light buffers ----
struct Light { vec4 pos_radius, color_falloff, proj_params, proj_origin, proj_axis; mat4 proj_mat; uvec4 tex; };
struct ProjShadow { vec4 params; };
struct LegacySpec { vec3 spec; float gloss, env; };
struct LInfo { vec4 cam_pos; uvec4 fwd; };
struct DrawView { vec4 u_cam_time; };   // (pc.u_cam_time: the draw view's, CLEAN_VIEW)
struct DrawPush { uint view; };
#define BH_LIGHTS 32
static Light lights[BH_LIGHTS];
static ProjShadow pshadow[4];
static uint cluster_count[1] = {0};
static uint cluster_index[64] = {0};
static LInfo linfo;
static DrawView draw_views[1];
static DrawPush g_draw_push;
static const uint kViewCount = 1u;
static const uint gl_ViewIndex = 0u;
static float g_vis[BH_LIGHTS]; // the true visibility of light i from the layer (the wall: 0)
static uint g_rays = 0;        // shadow rays the walk cast
static bool g_mask = true;     // the froxel cell is this view's (else the cluster list)
static bool g_bfs = true;      // RenderVKRTBackfaceSelfShadow (default on)

static uint clusterLen(uint v) { return v; }
static bool clusterWalkAll(uint) { return false; }   // (the fixture's lists never overflow CLUSTER_CAP)
static uint mainClusterFrag(vec3) { return 0u; }
static bool blendHitMask(vec3, uint* w)
{
    for (uint k = 0; k < 8; ++k) w[k] = 0u;
    if (!g_mask) return false;
    const uint n = uint(linfo.cam_pos.w);
    for (uint i = 0; i < n; ++i) w[i >> 5] |= 1u << (i & 31u);
    return true;
}
static int lightIndexOf(const Light& L)
{
    for (int i = 0; i < BH_LIGHTS; ++i)
        if (std::memcmp(&lights[i], &L, sizeof(Light)) == 0) return i;
    return -1;
}
static bool lightInRange(vec4 pr, vec3 P, vec3& lv, float& d2)
{
    lv = pr.xyz() - P;
    d2 = dot(lv, lv);
    return d2 < pr.w * pr.w;
}
static float lightReachAt(vec4 pr, vec4, float d2, float& dist)
{
    dist = std::sqrt(d2);
    return max(1.f - dist / pr.w, 0.f);
}
static void llPunctual(vec3 dc, vec3 sc, float rough, vec3 N, vec3 V, vec3 l, float& nl, vec3& df, vec3& sp)
{
    nl = max(dot(N, l), 0.f);
    const vec3 h = normalize(V + l);
    const float nh = max(dot(N, h), 0.f);
    const float a = max(rough * rough, 0.02f);
    df = dc * (1.f / 3.14159265f);
    sp = sc * (std::pow(nh, 2.f / (a * a)) * (2.f / (a * a) + 2.f) / 6.2831853f);
}
static float legacySpecular(vec3 N, vec3 V, vec3 l, float gloss, float& slit)
{
    slit = 1.f;
    return std::pow(max(dot(N, normalize(V + l)), 0.f), 8.f + 120.f * gloss);
}
static bool lightProjFrustum(mat4, vec3, vec2& ptc) { ptc = vec2(0.5f, 0.5f); return true; }
static vec3 projDiffuse(Light L, float, vec2) { return L.color_falloff.xyz(); }
static vec3 projAmbiance(Light L, float da, float, float, float, vec2) { return L.color_falloff.xyz() * (0.2f * da); }
static vec3 projEnvReflect(Light L, vec3, vec3, vec3, float) { return L.color_falloff.xyz() * 0.3f; }
static bool projAmbShadowed() { return true; }
static bool rtBackfaceSelfShadow() { return g_bfs; }
static vec3 rtProjShadowMapQ0(Light L, uint, vec3, vec3) { return vec3(g_vis[lightIndexOf(L)]); }
static float g_bh_glare = 0.f;

// ---- the light ray (rtrProjRaySetup / rtrProjRay are the shader's, cut from the variant) over a mock scene walk ----
struct SsrBuf { vec4 foliage_rt; };
static SsrBuf ssr;
static vec3  rt_hit_ng(0.f, 0.f, 1.f);   // (clean_rt_light_ray.glsl's globals: blendHitLocal sets them per layer)
static float rt_hit_err = 0.f;
static bool  rt_fix_on = false;          // (clean_vis_walk.glsl's own-fixture state)
static bool  g_rtr_novis = false;        // (clean_rt_light_ray.glsl: pass 14's unshadowed walk; never set here)
static vec3  rt_fix_P;
static float rt_fix_max = 0.f;
static void rtFacingCullSet(vec4) {}
static void rtFoliageEnd() {}
static int   g_cur = -1;        // the light the walk's current ray goes to
static float g_bulb = 0.f;      // > 0: every light sits inside its OWN opaque bulb prim of this radius (m)
static float g_fix_cfg = 1.5f;  // the frame's RenderVKReSTIRFixtureSize (m)
static int   g_fix_bad = 0;     // rays that took the rule with the wrong light centre / size
// The scene's walk (clean_vis_walk.glsl rtVisTraceK): a light's own bulb blocks the ray it meets unless the
// own-fixture rule is on for that light (rtFixtureOwn: the instance's origin is the light's centre -- any size);
// everything else: the light's visibility g_vis.
static vec3 rtVisTraceK(vec3 o, vec3 l, float tmax, uint, uint, uint)
{
    const vec3 c = lights[g_cur].pos_radius.xyz();
    if (g_bulb > 0.f)
    {
        const vec3 oc = o - c;
        const float b = dot(oc, l), cc = dot(oc, oc) - g_bulb * g_bulb;
        const float disc = b * b - cc;
        const float t0 = -b - std::sqrt(max(disc, 0.f));
        const bool meets = disc >= 0.f && (cc < 0.f || (t0 >= 0.f && t0 <= tmax));
        if (meets)
        {
            const bool own = rt_fix_on && length(rt_fix_P - c) <= 0.02f;
            if (rt_fix_on && (!own || std::fabs(rt_fix_max - g_fix_cfg) > 1e-3f)) ++g_fix_bad;
            if (!own) return vec3(0.f);
        }
    }
    return vec3(g_vis[g_cur]);
}
#ifndef BH_HAS_FIXMAX
static float blendHitFixMax() { return 0.f; }   // (a tree without the forward fixture rule: never read)
#endif
static vec3 rtrProjRayReal(Light L, vec3 P, vec3 N);
static vec3 rtrProjRay(Light L, vec3 P, vec3 N) { ++g_rays; g_cur = lightIndexOf(L); return rtrProjRayReal(L, P, N); }

// ---- the GI side ----
struct SurfaceLit { uint model; vec3 N, Ng; };
struct SurfaceRefl { vec3 irr, spec, env, prad2; bool have; };
struct GiHeader { uvec4 cfg; vec4 prm; };
static GiHeader gi;
static vec4 g_gi_fwd_dbg;
static float g_gi_amb_occ = 1.f;
static bool g_rt_fwd_hit = false;
static bool g_gi_fwd_read = false;
static float g_gi_fwd_wf = 0.f;
static vec3 g_gi_fwd_E;
static float g_bh_occ = -1.f;   // (the layer's occlusion ray: lightSurface traces it before the reflections)
static float g_bh_occ_t = 1e30f; // (its nearest opaque blocker's distance; 1e30 = none)
static float g_wf = 0.f;        // the radiance cache's cover of the layer (giCacheForward)
static vec3 g_cacheE;
static bool blendHitOn() { return true; }
// Diagnostic controls off: this fixture tests the original exact hit/GI contract.
static constexpr unsigned BLC_GI = 4u;
static bool blendCostSkip(unsigned) { return false; }
static uint giCacheSlots() { return gi.cfg.z; }
static vec3 mainReceiverEye(vec3, vec3 cam) { return cam; }
static vec3 giCacheForward(vec3, vec3, vec3, float& w, bool& inSph) { w = g_wf; inSph = false; return g_cacheE; }
static float giLum(vec3 c) { return dot(c, vec3(0.2126f, 0.7152f, 0.0722f)); }
// (pass 14's resolved layer GI, clean_draw_common.glsl rtFwdGiIn: none here -- the fixture models the cache read itself)
static bool rtFwdGiIn(vec3, vec3& Ewf, float& wf) { Ewf = vec3(0.f); wf = 0.f; return false; }
static bool atmosClassic() { return false; }
static vec3 atmosLinearToSrgb(vec3 c) { return c; }

#include BH_FN_FILE

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static Light pointLight(vec3 p, vec3 c, float r)
{
    Light L{};
    L.pos_radius = vec4(p.x, p.y, p.z, r);
    L.color_falloff = vec4(c.x, c.y, c.z, 1.f);
    L.proj_origin = vec4(p.x, p.y, p.z, 0.f);
    return L;
}
static Light projector(vec3 p, vec3 c, float r, uint rec)
{
    Light L = pointLight(p, c, r);
    L.proj_origin.w = 1.f;
    L.proj_axis = vec4(0.f, -1.f, 0.f, 0.f);
    L.proj_params = vec4(1.f, r, 1.f, 0.5f);
    L.tex.z = rec;
    return L;
}
struct Recv { const char* name; bool pbr; vec3 base; float metal, rough; LegacySpec ls; };
static const Recv kRecv[] = {
    {"PBR glass", true, vec3(0.9f, 0.9f, 0.9f), 0.f, 0.05f, {vec3(0.f), 0.f, 0.f}},
    {"PBR blush on skin", true, vec3(0.8f, 0.4f, 0.4f), 0.f, 0.6f, {vec3(0.f), 0.f, 0.f}},
    {"legacy shiny decal", false, vec3(0.7f, 0.7f, 0.7f), 0.f, 1.f, {vec3(1.f, 1.f, 1.f), 0.9f, 0.6f}},
};
static vec3 shadeAt(const Recv& r, vec3 P, vec3 N = vec3(0.f, 1.f, 0.f), vec3 V = normalize(vec3(0.3f, 1.f, 0.2f)))
{
    g_bh_glare = 0.f;
    rt_hit_ng = vec3(0.f, 1.f, 0.f);   // (blendHitRayFrame: the layer's plane y = 0 on the side the camera sees)
    rt_hit_err = 0.f;
    return rtHitLocalLights(P, N, V, r.pbr, r.base, r.metal, r.rough, r.ls, 0u);
}
static vec3 shade(const Recv& r) { return shadeAt(r, vec3(0.f, 0.f, 0.f)); }
static void setFrame(uint f) { draw_views[0].u_cam_time.w = 1000.f + 0.016f * float(f); }
// the next room's lights, all behind the wall
static uint sealedRoom()
{
    std::memset(lights, 0, sizeof(lights));
    lights[0] = pointLight(vec3(2.f, 1.5f, 0.5f), vec3(1.f, 0.5f, 0.6f), 8.f);
    lights[1] = projector(vec3(-2.f, 2.f, 0.3f), vec3(0.6f, 0.6f, 0.3f), 8.f, 0u);
    lights[2] = projector(vec3(0.5f, 2.5f, -2.f), vec3(0.4f, 0.7f, 0.9f), 8.f, 1u);
    lights[3] = projector(vec3(-0.5f, 2.2f, 2.f), vec3(0.8f, 0.8f, 0.8f), 8.f, 2u);
    pshadow[0].params = vec4(0.f, 0.f, 2.f, 1.f);   // (ray traced, valid)
    pshadow[1].params = vec4(0.f, 0.f, 1.f, 1.f);   // (shadow map, valid)
    for (int i = 0; i < BH_LIGHTS; ++i) g_vis[i] = 0.f;
    cluster_count[0] = 4u;
    for (uint i = 0; i < 64; ++i) cluster_index[i] = i;
    linfo.cam_pos.w = 4.f;
    return 4u;
}
static void setLights(uint n) { linfo.cam_pos.w = float(n); cluster_count[0] = n; }
static float lum(vec3 c) { return hnLum(c); }
static float vmax3(vec3 v) { return max(max(std::fabs(v.x), std::fabs(v.y)), std::fabs(v.z)); }

static void testWalk()
{
    const uint budget = BLEND_HIT_M + BLEND_HIT_K;
    for (int m = 0; m < 2; ++m)
    {
        g_mask = m == 0;
        const char* src = g_mask ? "froxel cell" : "cluster list";
        // SEALED
        for (const Recv& r : kRecv)
        {
            sealedRoom();
            g_rays = 0;
            for (uint f = 0; f < 2048; ++f)
            {
                setFrame(f);
                const vec3 e = shade(r);
                CHECK(e.x < 1e-5f && e.y < 1e-5f && e.z < 1e-5f, "SEALED %s (%s), frame %u: lit through the wall: %g %g %g",
                      r.name, src, f, e.x, e.y, e.z);
                if (g_fail) return;
            }
            CHECK(g_rays <= 2048u * budget, "%s: %u rays for 2048 layers (budget %u)", r.name, g_rays, budget);
            CHECK(g_rays > 0u, "%s (%s): no shadow ray at all -- the walk lights without testing", r.name, src);
        }
        // OWN: the room's own lamp visible beside them -> the mean is the own lamp alone
        for (const Recv& r : kRecv)
        {
            const uint n = sealedRoom();
            const Light own = pointLight(vec3(-0.6f, 2.f, -0.4f), vec3(0.3f, 0.28f, 0.25f), 6.f);
            std::memset(lights, 0, sizeof(lights));
            lights[0] = own; g_vis[0] = 1.f; setLights(1);
            setFrame(0);
            const vec3 want = shade(r);
            sealedRoom();
            lights[n] = own; g_vis[n] = 1.f; setLights(n + 1);
            vec3 acc;
            const uint frames = 60000;
            for (uint f = 0; f < frames; ++f) { setFrame(f); acc += shade(r); }
            const vec3 mean = acc * (1.f / float(frames));
            const float tol = 0.04f * max(max(want.x, want.y), want.z) + 1e-4f;
            CHECK(want.x > 1e-4f, "%s: the own lamp lights nothing (fixture sanity)", r.name);
            CHECK(std::fabs(mean.x - want.x) < tol && std::fabs(mean.y - want.y) < tol && std::fabs(mean.z - want.z) < tol,
                  "OWN %s (%s): mean %g %g %g, want the own lamp %g %g %g", r.name, src, mean.x, mean.y, mean.z, want.x, want.y, want.z);
        }
    }
    g_mask = true;
    // EXACT: the key light blocked, a fill light visible -> the fill alone, every frame
    for (const Recv& r : kRecv)
    {
        std::memset(lights, 0, sizeof(lights));
        lights[0] = pointLight(vec3(0.5f, 1.f, 0.2f), vec3(2.f, 1.9f, 1.7f), 6.f);    // key (blocked)
        lights[1] = pointLight(vec3(-0.4f, 1.2f, 0.6f), vec3(0.3f, 0.3f, 0.35f), 6.f);   // fill (visible)
        g_vis[0] = 0.f; g_vis[1] = 1.f;
        setLights(1);
        std::swap(lights[0], lights[1]); std::swap(g_vis[0], g_vis[1]);
        const vec3 want = shade(r);   // (the fill alone)
        std::swap(lights[0], lights[1]); std::swap(g_vis[0], g_vis[1]);
        setLights(2);
        for (uint f = 0; f < 512; ++f)
        {
            setFrame(f);
            const vec3 e = shade(r);
            CHECK(std::fabs(e.x - want.x) < 1e-5f + 1e-4f * want.x && std::fabs(e.z - want.z) < 1e-5f + 1e-4f * want.z,
                  "EXACT %s frame %u: %g, want the fill light %g (the strongest lights must not be sampled)", r.name, f, e.x, want.x);
            if (g_fail) return;
        }
    }
    // NOISE: a lamp + five dim lights, half of them blocked; the error over frames vs plain NEE with the same rays
    {
        const Recv& r = kRecv[0];
        std::memset(lights, 0, sizeof(lights));
        lights[0] = pointLight(vec3(0.3f, 1.4f, 0.1f), vec3(1.5f, 1.4f, 1.2f), 8.f);
        g_vis[0] = 1.f;
        for (int i = 1; i <= 5; ++i)
        {
            lights[i] = pointLight(vec3(-1.f + 0.4f * i, 1.6f, 0.8f - 0.3f * i), vec3(0.18f, 0.17f, 0.16f), 8.f);
            g_vis[i] = float(i & 1);
        }
        // each light's share at the layer, and the exact shadowed sum
        std::vector<vec3> share(6);
        Light all[BH_LIGHTS];
        std::memcpy(all, lights, sizeof(lights));
        float vsave[BH_LIGHTS];
        std::memcpy(vsave, g_vis, sizeof(g_vis));
        vec3 exact;
        for (int i = 0; i < 6; ++i)
        {
            std::memset(lights, 0, sizeof(lights));
            lights[0] = all[i]; g_vis[0] = 1.f; setLights(1);
            share[i] = shade(r);
            exact += share[i] * vsave[i];
        }
        std::memcpy(lights, all, sizeof(lights));
        std::memcpy(g_vis, vsave, sizeof(g_vis));
        setLights(6);
        double eWalk = 0.0, eNee = 0.0;
        const uint frames = 20000;
        for (uint f = 0; f < frames; ++f)
        {
            setFrame(f);
            const float a = lum(shade(r)) - lum(exact);
            eWalk += double(a) * a;
            float sc[HN_MAXK]; uint li[HN_MAXK]; vec3 sh[HN_MAXK];
            hnInit(sc, li, sh);
            float W = 0.f;
            for (uint i = 0; i < 6; ++i) W = hnAdd(W, 4u, hnHash(f * 2654435761u + 99u), i, i, share[i], sc, li, sh);
            vec3 s = hnFinish(W, 4u, sc, sh);
            for (uint j = 0; j < 4; ++j) if (sc[j] > 0.f) s -= sh[j] * (1.f - vsave[li[j]]);
            const float b = lum(s) - lum(exact);
            eNee += double(b) * b;
        }
        const double rw = std::sqrt(eWalk / frames), rn = std::sqrt(eNee / frames);
        std::printf("NOISE lamp + 5 dim: RMS error %.4f (%.1f%% of the light), plain NEE with 4 rays %.4f (%.1f%%)\n",
                    rw, 100.0 * rw / lum(exact), rn, 100.0 * rn / lum(exact));
        CHECK(rw < 0.6 * rn, "NOISE: the layer's walk (RMS %g) is not clearly below plain NEE with the same 4 rays (%g)", rw, rn);
    }
    // BUDGET: 16 lights, at most M + K rays per layer
    {
        std::memset(lights, 0, sizeof(lights));
        for (int i = 0; i < 16; ++i)
        {
            lights[i] = (i & 1) ? projector(vec3(-2.f + 0.25f * i, 2.f, 0.5f), vec3(0.2f + 0.03f * i), 7.f, (i % 4 == 1) ? 1u : 0u)
                                : pointLight(vec3(1.5f - 0.2f * i, 1.7f, -0.6f), vec3(0.1f + 0.05f * i), 7.f);
            g_vis[i] = float((i / 2) & 1);
        }
        pshadow[0].params = vec4(0.f, 0.f, 2.f, 1.f);
        setLights(16);
        for (uint f = 0; f < 256; ++f)
        {
            setFrame(f);
            g_rays = 0;
            shade(kRecv[2]);
            CHECK(g_rays <= BLEND_HIT_M + BLEND_HIT_K, "BUDGET: %u shadow rays for one layer (budget %u)", g_rays,
                  BLEND_HIT_M + BLEND_HIT_K);
            if (g_fail) return;
        }
    }
    // GLARE: a legacy shiny layer under a lamp raises its coverage
    {
        std::memset(lights, 0, sizeof(lights));
        lights[0] = pointLight(vec3(-0.6f, 2.f, -0.4f), vec3(1.f), 5.f);   // (along the mirror direction of V)
        g_vis[0] = 1.f;
        setLights(1);
        setFrame(3);
        shade(kRecv[2]);
        CHECK(g_bh_glare > 0.01f, "GLARE: a legacy shiny layer under a lamp has no alpha glare (%g)", g_bh_glare);
        // VISGLARE: the same lamp behind a wall -> no light and no coverage from it
        g_vis[0] = 0.f;
        const vec3 e = shade(kRecv[2]);
        CHECK(g_bh_glare < 1e-6f && vmax3(e) < 1e-6f,
              "VISGLARE: a blocked shiny lamp still raises the layer's coverage: glare %g, light %g (an unlit, opaque patch)",
              g_bh_glare, vmax3(e));
    }
}

// STABLE / TAIL: n lights in reach of the layer, random strengths, about half blocked; the error of the layer's light
// over frames vs the exact shadowed sum, relative to the unshadowed total (the noise the eye sees on the glass).
#ifndef BH_NOISE_MAX
#define BH_NOISE_MAX 1e-4   // (relative RMS: float rounding only -- no estimator noise)
#endif
static void layout(std::mt19937& rng, uint n)
{
    std::uniform_real_distribution<float> U(0.f, 1.f);
    std::memset(lights, 0, sizeof(lights));
    for (uint i = 0; i < n; ++i)
    {
        const float c = 0.1f + 0.9f * U(rng);
        lights[i] = pointLight(vec3(-1.5f + 3.f * U(rng), 0.8f + 1.7f * U(rng), -1.5f + 3.f * U(rng)),
                               vec3(c, c * (0.8f + 0.2f * U(rng)), c * (0.7f + 0.3f * U(rng))), 8.f);
        g_vis[i] = (rng() & 1u) ? 1.f : 0.f;
    }
    setLights(n);
}
// the exact shadowed sum and the unshadowed total of the layout (each light shaded alone)
static void truth(const Recv& r, uint n, vec3& exact, vec3& total)
{
    Light all[BH_LIGHTS];
    float vis[BH_LIGHTS];
    std::memcpy(all, lights, sizeof(lights));
    std::memcpy(vis, g_vis, sizeof(g_vis));
    exact = vec3(); total = vec3();
    for (uint i = 0; i < n; ++i)
    {
        std::memset(lights, 0, sizeof(lights));
        lights[0] = all[i]; g_vis[0] = 1.f; setLights(1);
        const vec3 s = shade(r);
        total += s;
        exact += s * vis[i];
    }
    std::memcpy(lights, all, sizeof(lights));
    std::memcpy(g_vis, vis, sizeof(g_vis));
    setLights(n);
}
static void testStable()
{
    const int fail0 = g_fail;
    const uint ns[] = {1u, 2u, 4u, 8u};
    for (const Recv& r : kRecv)
        for (uint n : ns)
        {
            std::mt19937 rng(77u + n);
            double worst = 0.0;
            for (int lay = 0; lay < 64; ++lay)
            {
                layout(rng, n);
                vec3 exact, total;
                truth(r, n, exact, total);
                double e2 = 0.0;
                const uint frames = 256;
                for (uint f = 0; f < frames; ++f)
                {
                    setFrame(f * 7u + lay);
                    g_rays = 0;
                    const double d = double(lum(shade(r))) - double(lum(exact));
                    e2 += d * d;
                    CHECK(g_rays <= n, "STABLE %s, %u lights: %u shadow rays (one per light)", r.name, n, g_rays);
                }
                worst = std::max(worst, std::sqrt(e2 / frames) / std::max(double(lum(total)), 1e-9));
            }
            std::printf("NOISE-STABLE %s, %u lights: worst RMS %.3f%% of the light\n", r.name, n, 100.0 * worst);
            CHECK(worst < BH_NOISE_MAX, "STABLE %s, %u lights: the layer's light changes from frame to frame: RMS %.2f%% of "
                  "its light (limit %.3f%%) -- the grain inside the lights' reach", r.name, n, 100.0 * worst, 100.0 * BH_NOISE_MAX);
            if (g_fail > fail0) return;
        }
    // TAIL: past the budget the sampled tail stays unbiased; its residual is reported
    const uint nt[] = {12u, 16u, 32u};
    const Recv& r = kRecv[0];
    for (uint n : nt)
    {
        std::mt19937 rng(991u + n);
        double rms = 0.0;
        const int lays = 8;
        for (int lay = 0; lay < lays; ++lay)
        {
            layout(rng, n);
            vec3 exact, total;
            truth(r, n, exact, total);
            double e2 = 0.0, m = 0.0;
            const uint frames = 20000;
            for (uint f = 0; f < frames; ++f)
            {
                setFrame(f * 3u + lay);
                g_rays = 0;
                const double v = lum(shade(r));
                m += v;
                e2 += (v - lum(exact)) * (v - lum(exact));
                CHECK(g_rays <= BLEND_HIT_M + BLEND_HIT_K, "TAIL %u lights: %u rays", n, g_rays);
            }
            m /= frames;
            CHECK(std::fabs(m - lum(exact)) < 0.02 * lum(total) + 1e-6, "TAIL %u lights: biased: mean %g, exact %g (total %g)", n,
                  m, double(lum(exact)), double(lum(total)));
            rms += std::sqrt(e2 / frames) / std::max(double(lum(total)), 1e-9);
            if (g_fail > fail0) return;
        }
        std::printf("NOISE-TAIL %u lights: mean RMS %.2f%% of the light (6 exact + 2 picks)\n", n, 100.0 * rms / lays);
    }
}

// DISC: the dark circle. A point lamp inside its own opaque bulb prim over a shiny legacy water layer; the camera at
// (0, 3, -6). Every layer point in the lamp's reach: lit with the unshadowed light and glare.
static void testDisc()
{
    const Recv& water = kRecv[2];
    const vec3 cam(0.f, 3.f, -6.f);
    std::memset(lights, 0, sizeof(lights));
    lights[0] = pointLight(vec3(0.f, 2.5f, 0.f), vec3(1.f, 0.9f, 0.75f), 6.f);
    lights[0].proj_axis.x = 0.15f;   // (its prim's bounding radius: the ray stops RS_POINT_NEAR_MAX short of the centre)
    g_vis[0] = 1.f;
    setLights(1);
    setFrame(5);
    int pts = 0, lost = 0, glareKept = 0;
    float maxGlare = 0.f;
    for (int iz = -10; iz <= 10; ++iz)
        for (int ix = -10; ix <= 10; ++ix)
        {
            const vec3 P(0.55f * ix, 0.f, 0.55f * iz);
            if (length(lights[0].pos_radius.xyz() - P) >= 5.9f) continue;
            const vec3 V = normalize(cam - P);
            g_bulb = 0.f;
            const vec3 want = shadeAt(water, P, vec3(0.f, 1.f, 0.f), V);
            const float wantG = g_bh_glare;
            g_bulb = 0.15f;
            g_fix_bad = 0;
            const vec3 got = shadeAt(water, P, vec3(0.f, 1.f, 0.f), V);
            const float gotG = g_bh_glare;
            g_bulb = 0.f;
            ++pts;
            maxGlare = max(maxGlare, wantG);
            if (vmax3(got - want) > 1e-4f + 1e-3f * vmax3(want)) ++lost;
            if (lum(got) < 0.05f * lum(want) && gotG > 0.5f * wantG && wantG > 1e-3f) ++glareKept;
            CHECK(g_fix_bad == 0, "DISC: a ray took the own-fixture rule with the wrong light centre / size limit");
        }
    std::printf("DISC: %d layer points in the lamp's reach, %d lose its light, %d of them keep its glare (unlit and opaque)\n",
                pts, lost, glareKept);
    CHECK(pts > 200 && maxGlare > 0.05f, "DISC: fixture sanity (%d points, glare %g)", pts, maxGlare);
    CHECK(lost == 0, "DISC: the lamp inside its own bulb prim lights %d of %d layer points in its reach with less than its "
          "unshadowed light (%d keep its glare: the dark disc) -- the layer's light rays skip the own-fixture rule",
          lost, pts, glareKept);
    // a lamp BELOW the layer (its shading normal tilted toward it by a normal map): its own surface shadows it under the
    // back-face self-shadow -- no ray, and neither its light nor its glare
    {
        std::memset(lights, 0, sizeof(lights));
        lights[0] = pointLight(vec3(3.f, -0.5f, 0.f), vec3(1.f), 6.f);
        g_vis[0] = 1.f;
        setLights(1);
        const vec3 N = normalize(vec3(0.8f, 0.6f, 0.f));
        const vec3 l = normalize(lights[0].pos_radius.xyz());
        const vec3 V = normalize(N * (2.f * dot(N, l)) - l);   // (the mirror direction: the glare's peak)
        g_bfs = true;
        const vec3 e = shadeAt(water, vec3(0.f), N, V);
        CHECK(vmax3(e) < 1e-6f && g_bh_glare < 1e-6f, "BELOW: a lamp under the layer lights it %g / raises its coverage %g",
              vmax3(e), g_bh_glare);
    }
}

static SurfaceRefl probesOn()  { SurfaceRefl r{vec3(0.30f, 0.32f, 0.36f), vec3(0.20f, 0.22f, 0.25f), vec3(0.15f), vec3(0.f), true}; return r; }
static SurfaceRefl probesOff() { SurfaceRefl r{vec3(0.45f, 0.50f, 0.60f), vec3(0.80f, 0.85f, 0.95f), vec3(0.70f), vec3(0.f), false}; return r; }
static float vmax(vec3 v) { return max(max(std::fabs(v.x), std::fabs(v.y)), std::fabs(v.z)); }
static bool same(vec3 a, vec3 b) { return vmax(a - b) < 1e-6f; }
// The layer's GI: lightSurface's order -- its occlusion ray (vis), the environment through blendHitEnv, then giForwardRefl
// vis: the occlusion ray's visibility; blockT: its nearest opaque blocker (m; 1e30 = none). The GI ray length is 8 m.
static const float kGiRayLen = 8.f;
static SurfaceRefl glass(SurfaceRefl in, uint amb, bool cache, float vis, float blockT = 1e30f)
{
    gi.cfg = uvec4{1u | (cache ? 16u : 0u) | (1u << 8), 1u, cache ? 4096u : 0u, 0u};
    gi.prm = vec4(1.f, 0.f, kGiRayLen, float(amb));
    ssr.foliage_rt = vec4(128.f, 0.f, 0.f, 0.f);   // (a GI frame)
    linfo.cam_pos = vec4(0.f, 0.f, 0.f, 0.f);
    SurfaceLit s; s.model = SURF_PBR; s.N = vec3(-1.f, 0.f, 0.f); s.Ng = s.N;
    g_gi_fwd_read = false;
    g_bh_occ = vis;
    g_bh_occ_t = blockT;
    const vec3 P(2.f, 0.f, 0.f), R(-1.f, 0.f, 0.f);
    SurfaceRefl r = in;
    r.spec = blendHitEnv(s, P, R, r.spec);
    r.env = blendHitEnv(s, P, R, r.env);
    return giForwardRefl(r, s, P);
}
static void testGi()
{
    const char* const kProbe[2] = {"probes on", "probes off"};
    g_wf = 0.f;
    for (int p = 0; p < 2; ++p)
        for (uint amb = 0; amb < 4; ++amb)
            for (int cache = 0; cache < 2; ++cache)
            {
                const SurfaceRefl in = p == 0 ? probesOn() : probesOff();
                // SEALED: the occlusion ray blocked, no cache cell -- by an unrecorded blocker, and by a room's wall
                // 3 m away and right at half the GI ray length (the ramp's start)
                const float sealedT[3] = {1e30f, 3.f, 0.5f * kGiRayLen};
                for (float t : sealedT)
                {
                    const SurfaceRefl o = glass(in, amb, cache != 0, 0.f, t);
                    CHECK(vmax(o.irr) < 1e-6f && vmax(o.spec) < 1e-6f && vmax(o.env) < 1e-6f,
                          "SEALED %s, hit-ambient %u, cache %d, wall at %g m: the glass keeps ambient %g / specular %g / env %g",
                          kProbe[p], amb, cache, t, o.irr.x, o.spec.x, o.env.x);
                }
                SurfaceRefl o;
                // (#1499) FAR: blocked by geometry past the GI ray length (a cloud's underside, R toward the distant
                // ground) -- the far field the probes captured, as a GI ray's miss: the probes' ambient in modes 1 / 2
                // (none in 0 / 3); the environment (specular) keeps the plain ray: blocked, no cell -> 0
                {
                    const bool probes = amb == 1u || amb == 2u;
                    o = glass(in, amb, cache != 0, 0.f, 3.f * kGiRayLen);
                    CHECK(same(o.irr, probes ? in.irr : vec3(0.f)), "FAR %s, hit-ambient %u, cache %d: ambient %g, want %g "
                          "(a blocker past the GI ray length is the probes' far field)", kProbe[p], amb, cache, o.irr.x,
                          probes ? in.irr.x : 0.f);
                    CHECK(vmax(o.spec) < 1e-6f && vmax(o.env) < 1e-6f, "FAR %s, hit-ambient %u: the blocked environment "
                          "changed: specular %g / env %g (the specular keeps the plain ray)", kProbe[p], amb, o.spec.x, o.env.x);
                    // the ramp: 3/4 of the length -> half the probes, no contour; monotone over the ramp
                    o = glass(in, amb, cache != 0, 0.f, 0.75f * kGiRayLen);
                    CHECK(same(o.irr, probes ? in.irr * 0.5f : vec3(0.f)), "RAMP %s, hit-ambient %u: ambient %g, want %g",
                          kProbe[p], amb, o.irr.x, probes ? 0.5f * in.irr.x : 0.f);
                    float prev = -1.f;
                    for (int i = 0; i <= 64; ++i)
                    {
                        const float t = kGiRayLen * (0.25f + 1.0f * float(i) / 64.f);
                        const float a = glass(in, amb, cache != 0, 0.f, t).irr.y;
                        CHECK(a >= prev - 1e-6f, "RAMP %s, hit-ambient %u: the ambient falls with the blocker's distance "
                              "(%g at %g m after %g)", kProbe[p], amb, a, t, prev);
                        prev = a;
                    }
                    // a translucent blocker only (no opaque blocker recorded): its transmittance, unchanged
                    o = glass(in, amb, cache != 0, 0.4f);
                    CHECK(same(o.irr, probes ? in.irr * 0.4f : vec3(0.f)), "TRANSLUCENT %s, hit-ambient %u: ambient %g, want %g",
                          kProbe[p], amb, o.irr.x, probes ? 0.4f * in.irr.x : 0.f);
                }
                // OUTDOORS: the ray escaped -- the probes' ambient in modes 1 / 2 (none in 0 / 3), the radiance kept
                o = glass(in, amb, cache != 0, 1.f);
                const bool probes = amb == 1u || amb == 2u;
                CHECK(same(o.irr, probes ? in.irr : vec3(0.f)), "OUTDOORS %s, hit-ambient %u: ambient %g, want %g", kProbe[p], amb,
                      o.irr.x, probes ? in.irr.x : 0.f);
                CHECK(same(o.spec, in.spec) && same(o.env, in.env), "OUTDOORS %s, hit-ambient %u: the sky radiance changed %g -> %g",
                      kProbe[p], amb, in.spec.x, o.spec.x);
            }
    // THE CACHE'S OWN COVER: its value, also for a blocked environment (the cache at the layer)
    {
        g_wf = 1.f;
        g_cacheE = vec3(0.05f, 0.04f, 0.03f);
        const SurfaceRefl o = glass(probesOn(), 2u, true, 0.f);
        CHECK(same(o.irr, g_cacheE), "CACHE: ambient %g, want the cell's %g", o.irr.x, g_cacheE.x);
        CHECK(same(o.spec, g_cacheE), "CACHE: a blocked environment %g, want the cell's %g", o.spec.x, g_cacheE.x);
        g_wf = 0.f;
    }
    // (#1499) THE MOSAIC: a cloud's underside facing distant ground (blocked past the GI ray length), across the edge of
    // the cache's cover (wf 1 -> 0 over a cell, giCacheForward's trilinear weight): the ambient runs from the cell's
    // value to the probes' without a dip below both -- before, the uncovered share was 0 (probes x the blocked ray):
    // light cells next to black ones. A sealed room's wall (near) still fades to 0 off the cover.
    {
        const SurfaceRefl in = probesOn();
        g_cacheE = in.irr * 0.8f;
        const float lo = min(giLum(g_cacheE), giLum(in.irr));
        float worst = 1e30f;
        for (int i = 0; i <= 32; ++i)
        {
            g_wf = float(i) / 32.f;
            worst = min(worst, giLum(glass(in, 2u, true, 0.f, 3.f * kGiRayLen).irr));
        }
        CHECK(worst > lo - 1e-6f, "MOSAIC: across the cache's cover edge the ambient dips to %g (the cell %g, the probes %g): "
              "uncovered patches go dark next to covered ones", worst, giLum(g_cacheE), giLum(in.irr));
        g_wf = 0.f;
        CHECK(vmax(glass(in, 2u, true, 0.f, 2.f).irr) < 1e-6f, "MOSAIC: a sealed room's glass off the cache's cover is lit");
        g_cacheE = vec3();
    }
#ifdef BH_RT_FWD
    // RT FORWARD REFLECTION traced at this layer: its specular stays (its rays are occluded hits already)
    {
        g_rt_fwd_hit = true;
        const SurfaceRefl in = probesOn();
        const SurfaceRefl o = glass(in, 2u, true, 0.f);
        CHECK(same(o.spec, in.spec), "RT FWD: the traced specular was occluded again: %g of %g", o.spec.x, in.spec.x);
        g_rt_fwd_hit = false;
    }
#endif
}

int main()
{
    // the light info's fwd.w: bit 0 the layers' own light, bits 8.. the own-fixture size limit (mm)
    linfo.fwd.w = 1u | (uint(std::lround(g_fix_cfg * 1000.f)) << 8);
    testWalk();
    testDisc();
    testStable();
    testGi();
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
