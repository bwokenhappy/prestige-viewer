// Hit-point ray origin fixture (scripts/check/checks/hit_ray_origin.py): the functions every shadow / visibility /
// occlusion ray leaving a ray-traced hit (GI hits, RT reflection hits), a forward blend layer or a reused GI sample
// takes its origin / end from -- indra/vkclean/shaders/clean_rt_ray_origin.h, included as-is:
//   rtHitLightSide + rtHitOrigin   the light ray's side and origin (rtrProjRaySetup, rtHitSunRay)
//   rtReuseSegEnd                  where the GI reuse's visibility ray toward a stored sample stops
// against the rules they replace:
//   OLD light ray   origin = offsetRayOrigin(P + n x 1 cm, n), n = the SHADING normal turned toward the light
//   OLD reuse end   tmax = dl - max(1 cm, 0.1 % dl)
// Scenes (world offset = the owner's region spot (244, 51, 22) unless said otherwise; geometry as float triangles):
//   EAR      a lamp straight above a hit (N . l = 1) with a 4 mm slab (an ear) 2 mm above it: truly shadowed.
//   ROOF     the eave's near-coincident layers: a shingle sheet over a trim / deck layer at gap g (0.5 .. 9 mm). GI
//            hits on the lower layer under the sheet's edge (reachable through the open eave), a lamp above the
//            roof: truly shadowed. Also the SUN at the same hits (its rule never took the 1 cm: the sun-only control).
//   REUSE    a reused GI sample on the lower layer seen from a receiver above the sheet: the segment crosses the
//            sheet g / cos before the sample -- truly blocked.
//   GRAZING  random region triangles, light directions down to cos 0.02 off the plane: the new origin lies strictly
//            on the leaving side and a float Moller-Trumbore re-test of its ray against the hit triangle and its
//            coplanar / folded edge neighbours never hits (self-intersection / acne).
//   LARGE    the same at a skybox altitude (|z| ~ 4000 m) and at the region's far corner (256, 256, ...).
//   REUSE-SELF  an unblocked reused sample: its visibility segment never meets the sample's own triangle (no
//            false rejection), down to cos 0.05, region and skybox coordinates.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>

typedef unsigned int uint;
struct vec3
{
    float x, y, z;
    vec3() : x(0.f), y(0.f), z(0.f) {}
    vec3(float a, float b, float c) : x(a), y(b), z(c) {}
};
static vec3 operator+(vec3 a, vec3 b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec3 operator-(vec3 a, vec3 b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec3 operator-(vec3 a) { return vec3(-a.x, -a.y, -a.z); }
static vec3 operator*(vec3 a, float s) { return vec3(a.x * s, a.y * s, a.z * s); }
static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static vec3 cross(vec3 a, vec3 b) { return vec3(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float length(vec3 a) { return std::sqrt(dot(a, a)); }
static vec3 normalize(vec3 a) { return a * (1.f / length(a)); }
static uint floatBitsToUint(float f) { uint u; std::memcpy(&u, &f, 4); return u; }
static float uintBitsToFloat(uint u) { float f; std::memcpy(&f, &u, 4); return f; }
static int floatBitsToInt(float f) { int i; std::memcpy(&i, &f, 4); return i; }
static float intBitsToFloat(int i) { float f; std::memcpy(&f, &i, 4); return f; }
using std::abs;
using std::max;

#include "clean_rt_ray_origin.h"

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// ---- double-precision geometry (the truth / the occluder tests) ----
struct D3 { double x, y, z; };
static D3 d3(vec3 v) { return D3{v.x, v.y, v.z}; }
static D3 operator-(D3 a, D3 b) { return D3{a.x - b.x, a.y - b.y, a.z - b.z}; }
static double dotd(D3 a, D3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static D3 crossd(D3 a, D3 b) { return D3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
struct Tri { vec3 a, b, c; };
// segment o + d t, t in (0, tmax): does it cross triangle T (both sides)? (double Moller-Trumbore)
static bool segHitsD(D3 o, D3 d, double tmax, const Tri& T)
{
    const D3 a = d3(T.a), e1 = d3(T.b) - a, e2 = d3(T.c) - a;
    const D3 p = crossd(d, e2);
    const double det = dotd(e1, p);
    if (std::fabs(det) < 1e-30) return false;
    const double inv = 1.0 / det;
    const D3 s = o - a;
    const double u = dotd(s, p) * inv;
    if (u < 0.0 || u > 1.0) return false;
    const D3 q = crossd(s, e1);
    const double v = dotd(d, q) * inv;
    if (v < 0.0 || u + v > 1.0) return false;
    const double t = dotd(e2, q) * inv;
    return t > 0.0 && t < tmax;
}
// the same in FLOAT (the acne proxy: the traversal's own arithmetic is float)
static bool rayHitsF(vec3 o, vec3 d, float tmax, const Tri& T)
{
    const vec3 e1 = T.b - T.a, e2 = T.c - T.a;
    const vec3 p = cross(d, e2);
    const float det = dot(e1, p);
    if (std::fabs(det) < 1e-30f) return false;
    const float inv = 1.f / det;
    const vec3 s = o - T.a;
    const float u = dot(s, p) * inv;
    if (u < 0.f || u > 1.f) return false;
    const vec3 q = cross(s, e1);
    const float v = dot(d, q) * inv;
    if (v < 0.f || u + v > 1.f) return false;
    const float t = dot(e2, q) * inv;
    return t > 0.f && t < tmax;
}
struct Scene { Tri t[16]; int n = 0; };
// an axis-aligned rectangle z = zc over [x0, x1] x [y0, y1] (two triangles), offset by W
static void addRectZ(Scene& s, vec3 W, float x0, float x1, float y0, float y1, float zc)
{
    const vec3 p00 = W + vec3(x0, y0, zc), p10 = W + vec3(x1, y0, zc), p11 = W + vec3(x1, y1, zc), p01 = W + vec3(x0, y1, zc);
    s.t[s.n++] = Tri{p00, p10, p11};
    s.t[s.n++] = Tri{p00, p11, p01};
}
static bool blocked(const Scene& s, vec3 o, vec3 to)
{
    const D3 a = d3(o), b = d3(to), d = b - a;
    for (int i = 0; i < s.n; ++i) if (segHitsD(a, d, 1.0, s.t[i])) return true;
    return false;
}
static bool blockedDir(const Scene& s, vec3 o, vec3 l, float tmax)
{
    for (int i = 0; i < s.n; ++i) if (segHitsD(d3(o), d3(l), tmax, s.t[i])) return true;
    return false;
}

// ---- the two light-ray rules at a hit P (geometric normal Ng on the arrival side, shading normal N) ----
// visibility of point light Lp: 1 lit, 0 shadowed
static int oldLight(const Scene& s, vec3 P, vec3 N, vec3 Lp)
{
    const vec3 l = normalize(Lp - P);
    const vec3 n = dot(N, l) < 0.f ? -N : N;                    // (the old rtrProjRaySetup)
    const vec3 o = offsetRayOrigin(P + n * 0.01f, n);
    return blocked(s, o, Lp) ? 0 : 1;
}
static int newLight(const Scene& s, vec3 P, vec3 Ng, vec3 Lp, bool bfs = true)
{
    const vec3 l = normalize(Lp - P);
    vec3 n;
    if (!rtHitLightSide(Ng, l, bfs, n)) return 0;               // (rtrProjRaySetup: no ray, visibility 0)
    const vec3 o = rtHitOrigin(P, n, 0.f);
    return blocked(s, o, Lp) ? 0 : 1;
}
// the sun (rtHitSunRay: the same side rule + origin; directional, 512 m)
static int newSun(const Scene& s, vec3 P, vec3 Ng, vec3 L)
{
    vec3 n;
    if (!rtHitLightSide(Ng, L, true, n)) return 0;
    return blockedDir(s, rtHitOrigin(P, n, 0.f), L, 512.f) ? 0 : 1;
}
static int oldSun(const Scene& s, vec3 P, vec3 Ng, vec3 L)
{
    // (the sun's rule before this change: offsetRayOrigin(P + n x sun_rt.z (0), n) -- the same arithmetic)
    if (!(dot(Ng, L) > 0.f)) return 0;
    return blockedDir(s, offsetRayOrigin(P, Ng), L, 512.f) ? 0 : 1;
}

static uint32_t g_rng = 0x2468ace1u;
static float rnd() { g_rng = g_rng * 1664525u + 1013904223u; return float(g_rng >> 8) * (1.f / 16777216.f); }
static vec3 rndDir() { for (;;) { vec3 v(2.f * rnd() - 1.f, 2.f * rnd() - 1.f, 2.f * rnd() - 1.f); const float l = length(v); if (l > 0.05f && l <= 1.f) return v * (1.f / l); } }
// a unit vector at cosine c from n (random azimuth)
static vec3 atCos(vec3 n, float c)
{
    vec3 t = normalize(cross(n, std::fabs(n.z) < 0.9f ? vec3(0.f, 0.f, 1.f) : vec3(1.f, 0.f, 0.f)));
    const vec3 b = cross(n, t);
    const float ph = 6.2831853f * rnd(), sn = std::sqrt(std::max(1.f - c * c, 0.f));
    return normalize(n * c + t * (sn * std::cos(ph)) + b * (sn * std::sin(ph)));
}

static int g_acneSide = 0, g_acneOwn = 0, g_acneNb = 0;
// GRAZING / LARGE: random triangles of size ~sz around W; returns the number of self-hits
static int acneRun(vec3 W, float span, float sz, int iters, const float* coss, int ncos, float* worstOfs)
{
    int self = 0;
    for (int it = 0; it < iters; ++it)
    {
        const vec3 c = W + vec3((2.f * rnd() - 1.f) * span, (2.f * rnd() - 1.f) * span, (2.f * rnd() - 1.f) * span * 0.1f);
        const Tri T{c + rndDir() * sz, c + rndDir() * sz, c + rndDir() * sz};
        const vec3 e1 = T.b - T.a, e2 = T.c - T.a;
        const vec3 ngr = cross(e1, e2);
        if (length(ngr) < 1e-4f * sz * sz) continue;
        const vec3 ng0 = normalize(ngr);
        const vec3 Ng = rnd() < 0.5f ? ng0 : -ng0;            // the arrival side
        // the edge neighbours across edge a-b: coplanar, and folded 5 deg AWAY from the arrival side (a convex
        // edge: no ray leaving the hit's side may meet it -- a concave fold is real occlusion, not acne)
        const vec3 m = (T.a + T.b) * 0.5f;
        const vec3 away = normalize(m - T.c);
        const Tri Nc{T.b, T.a, m + away * sz};
        const float f = 0.0872665f;
        const Tri Nf{T.b, T.a, m + (away * std::cos(f) - Ng * std::sin(f)) * sz};
        // the hit: the shader's barycentric interpolation (clean_rt_surface.glsl s.P), in float
        float u = rnd(), v = rnd();
        if (u + v > 1.f) { u = 1.f - u; v = 1.f - v; }
        const float bx = 1.f - u - v;
        const vec3 P = T.a * bx + T.b * u + T.c * v;
        for (int k = 0; k < ncos; ++k)
        {
            const vec3 l = atCos(Ng, coss[k]);                // leaving at cosine coss[k] off the plane
            vec3 n;
            if (!rtHitLightSide(Ng, l, true, n)) { CHECK(false, "acne fixture: a light on the arrival side counted as turned away"); continue; }
            const vec3 o = rtHitOrigin(P, n, 0.f);
            // strictly on the leaving side of the (float) triangle's plane, in double
            const D3 nd = crossd(d3(T.b) - d3(T.a), d3(T.c) - d3(T.a));
            const double sd = dotd(d3(o) - d3(T.a), nd) / std::sqrt(dotd(nd, nd)) * (dot(Ng, ng0) > 0.f ? 1.0 : -1.0);
            if (!(sd > 0.0)) { ++self; ++g_acneSide; }
            *worstOfs = std::max(*worstOfs, float(sd));
            if (rayHitsF(o, l, 1e4f, T)) { ++self; ++g_acneOwn; }
            if (rayHitsF(o, l, 1e4f, Nc) || rayHitsF(o, l, 1e4f, Nf)) { ++self; ++g_acneNb; }
        }
    }
    return self;
}

int main()
{
    const vec3 W(244.f, 51.f, 22.f);   // the owner's spot in Cloudsdale (camera ~ (244, 51.5, 22.9))
    const vec3 up(0.f, 0.f, 1.f);
    // ---- EAR: a lamp straight above (N . l = 1), a 4 mm slab 2 mm above the hit ----
    {
        Scene s;
        addRectZ(s, W, -0.05f, 0.05f, -0.05f, 0.05f, 0.002f);
        addRectZ(s, W, -0.05f, 0.05f, -0.05f, 0.05f, 0.006f);
        const vec3 P = W + vec3(0.003f, -0.002f, 0.f), lamp = W + vec3(0.003f, -0.002f, 0.5f);
        CHECK(blocked(s, P, lamp), "fixture sanity: the ear does not hide the lamp");
        CHECK(oldLight(s, P, up, lamp) == 1, "fixture sanity: the old 1 cm origin no longer leaks the ear (it must, it is the bug)");
        CHECK(newLight(s, P, up, lamp) == 0, "EAR: the lamp lights the hit through a 4 mm ear 2 mm above it");
    }
    // ---- ROOF: shingles over a trim / deck layer at gap g; hits on the lower layer under the sheet's edge ----
    const float gaps[] = {0.0005f, 0.002f, 0.005f, 0.009f};
    int oldLeaks = 0, newLeaks = 0, sunOld = 0, sunNew = 0, roofHits = 0;
    for (float g : gaps)
    {
        Scene s;
        addRectZ(s, W, -1.f, 0.f, -1.f, 1.f, 0.f);         // the shingle sheet, its eave edge at x = 0
        addRectZ(s, W, -1.f, 0.1f, -1.f, 1.f, -g);         // the trim / deck layer, 10 cm past the edge
        const vec3 lamp = W + vec3(-0.3f, 0.1f, 0.6f);     // a lamp above the roof
        const vec3 sun = normalize(vec3(-0.4f, 0.2f, 1.f));
        for (int i = 0; i < 40; ++i)
        {
            // under the sheet within a few gaps of its edge: what a GI ray entering the eave's opening meets
            const vec3 P = W + vec3(-0.0005f - 0.001f * float(i), -0.2f + 0.01f * float(i), -g);
            ++roofHits;
            CHECK(blocked(s, P, lamp), "fixture sanity: the sheet does not hide the lamp from (%g) at g %g", P.x - W.x, g);
            oldLeaks += oldLight(s, P, up, lamp);
            const int nl = newLight(s, P, up, lamp);
            newLeaks += nl;
            CHECK(nl == 0, "ROOF g %g mm: a lamp above the shingles lights the layer under them at x %g mm", g * 1e3f, (P.x - W.x) * 1e3f);
            sunOld += oldSun(s, P, up, sun);
            sunNew += newSun(s, P, up, sun);
        }
        // and the open part of the lower layer (past the edge) keeps the lamp (no over-shadowing)
        const vec3 Q = W + vec3(0.06f, 0.f, -g);
        CHECK(newLight(s, Q, up, lamp) == (blocked(s, Q, lamp) ? 0 : 1), "ROOF g %g mm: the uncovered layer's lamp visibility is wrong", g * 1e3f);
    }
    CHECK(oldLeaks == roofHits, "fixture sanity: the old 1 cm origin leaked %d of %d roof hits (every gap < 1 cm should)", oldLeaks, roofHits);
    CHECK(newLeaks == 0, "ROOF: %d of %d hits under the shingles lit by the lamp above", newLeaks, roofHits);
    CHECK(sunOld == 0 && sunNew == 0, "ROOF: the sun leaks (old %d, new %d) -- its rule never took the 1 cm", sunOld, sunNew);
    // a turned-away hit (normal map toward a lamp below its plane): the old flip went through the sheet
    {
        Scene s;
        addRectZ(s, W, -1.f, 1.f, -1.f, 1.f, 0.f);         // the sheet the hit is on
        addRectZ(s, W, -1.f, 1.f, -1.f, 1.f, -0.004f);     // a layer 4 mm under it
        const vec3 P = W + vec3(0.f, 0.f, 0.f), lamp = W + vec3(0.5f, 0.f, -0.3f);
        CHECK(newLight(s, P, up, lamp, true) == 0, "BACK: a lamp behind the hit's plane lights it (bfs)");
    }
    // ---- REUSE: a sample on the lower layer seen from above the sheet (the segment crosses it g / cos early) ----
    const float rgaps[] = {0.001f, 0.003f, 0.005f, 0.009f};
    int rOld = 0, rNew = 0, rN = 0;
    for (float g : rgaps)
    {
        Scene s;
        addRectZ(s, W, -1.f, 0.f, -1.f, 1.f, 0.f);
        addRectZ(s, W, -1.f, 0.1f, -1.f, 1.f, -g);
        for (int i = 0; i < 24; ++i)
        {
            const vec3 xs = W + vec3(-0.02f - 0.002f * float(i), 0.f, -g);      // under the sheet
            const vec3 o = W + vec3(0.25f, 0.03f * float(i % 5), 0.15f + 0.02f * float(i % 7));   // a receiver above
            const vec3 v = xs - o;
            const float dl = length(v);
            const vec3 dv = v * (1.f / dl);
            const float cosEnd = std::fabs(dot(up, dv));
            CHECK(blocked(s, o, xs), "fixture sanity: the sheet does not hide the sample");
            const float tOld = std::max(dl - std::max(0.01f, dl * 1e-3f), 0.f);
            const float tNew = rtReuseSegEnd(xs, dl, cosEnd);
            const bool lOld = !blockedDir(s, o, dv, tOld), lNew = !blockedDir(s, o, dv, tNew);
            rOld += lOld; rNew += lNew; ++rN;
            // the new end: every layer more than RT_REUSE_END_CLEAR in front of the sample is tested
            if (g > RT_REUSE_END_CLEAR * 1.05f)
                CHECK(!lNew, "REUSE g %g mm: the sample under the sheet reconnects through it (cos %g)", g * 1e3f, cosEnd);
        }
    }
    CHECK(rOld > rN / 2, "fixture sanity: the old 1 cm end no longer leaks the reuse (%d of %d)", rOld, rN);
    // ---- GRAZING / LARGE: acne ----
    const float coss[] = {0.02f, 0.05f, 0.1f, 0.3f, 0.7f, 1.f};
    float wReg = 0.f, wFar = 0.f, wSky = 0.f;
    int selfReg = acneRun(W, 2.f, 0.3f, 20000, coss, 6, &wReg);
    int selfFar = acneRun(vec3(255.f, 255.f, 40.f), 1.f, 0.05f, 20000, coss, 6, &wFar);
    int selfSky = acneRun(vec3(250.f, 250.f, 4000.f), 5.f, 1.f, 20000, coss, 6, &wSky);
    if (selfReg || selfFar || selfSky) std::printf("acne: %d off-side origins, %d own-triangle hits, %d neighbour hits\n", g_acneSide, g_acneOwn, g_acneNb);
    CHECK(selfReg == 0, "GRAZING (region): %d self-intersections", selfReg);
    CHECK(selfFar == 0, "LARGE (region corner): %d self-intersections", selfFar);
    CHECK(selfSky == 0, "LARGE (skybox 4000 m): %d self-intersections", selfSky);
    CHECK(wReg < 0.0005f && wFar < 0.0005f, "the region offset grew past 0.5 mm (%g, %g m)", wReg, wFar);
    CHECK(wSky < 0.006f, "the skybox offset grew past 6 mm (%g m)", wSky);
    // ---- REUSE-SELF: an unblocked sample's own triangle never stops its visibility ray ----
    int falseRej = 0, nSelf = 0;
    const vec3 bases[] = {W, vec3(255.f, 255.f, 40.f), vec3(250.f, 250.f, 4000.f)};
    for (const vec3& B : bases)
        for (int it = 0; it < 20000; ++it)
        {
            const vec3 c = B + vec3(2.f * rnd() - 1.f, 2.f * rnd() - 1.f, 0.2f * rnd());
            const Tri T{c + rndDir() * 0.5f, c + rndDir() * 0.5f, c + rndDir() * 0.5f};
            const vec3 ngr = cross(T.b - T.a, T.c - T.a);
            if (length(ngr) < 0.01f) continue;
            const vec3 ns = normalize(ngr);
            float u = rnd(), v = rnd();
            if (u + v > 1.f) { u = 1.f - u; v = 1.f - v; }
            const vec3 xs = T.a * (1.f - u - v) + T.b * u + T.c * v;
            const float cs[] = {0.05f, 0.1f, 0.4f, 1.f};
            const vec3 side = rnd() < 0.5f ? ns : -ns;
            const vec3 dirToRecv = atCos(side, cs[it & 3]);
            const vec3 o = xs + dirToRecv * (0.3f + 20.f * rnd());
            const vec3 vv = xs - o;
            const float dl = length(vv);
            const vec3 dv = vv * (1.f / dl);
            const float tm = rtReuseSegEnd(xs, dl, std::fabs(dot(ns, dv)));
            ++nSelf;
            if (rayHitsF(o, dv, tm, T)) ++falseRej;
        }
    CHECK(falseRej == 0, "REUSE-SELF: %d of %d unblocked samples rejected by their own triangle", falseRej, nSelf);
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok (ear: old lit, new shadowed; roof %d hits under 0.5-9 mm gaps: old %d lit, new 0, sun old/new 0; "
                "reuse old %d / new %d of %d leak (new: none past %g mm); acne 0 over region / corner / skybox, worst offset "
                "%.3g / %.3g / %.3g mm; reuse self-rejections 0 of %d)\n",
                roofHits, oldLeaks, rOld, rNew, rN, RT_REUSE_END_CLEAR * 1e3f, wReg * 1e3f, wFar * 1e3f, wSky * 1e3f, nSelf);
    return 0;
}
