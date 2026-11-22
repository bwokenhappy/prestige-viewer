// vkclean -- MOMENT-BASED ORDER-INDEPENDENT TRANSPARENCY (RenderVKAlphaMBOIT; LLCleanAlphaMBOITVK).
//
// After Muenstermann, Krumpen, Klein and Peters 2018, "Moment-Based Order-Independent Transparency"
// (Proc. ACM CGIT 1(1), article 7; I3D 2018), with the reconstruction of Peters and Klein 2015
// ("Moment Shadow Mapping") / Peters et al. 2017. Implemented here from the papers' equations; no
// code was taken from the authors' release.
//
// The static (non-avatar) alpha-blend lists of one water side take four passes:
//   1. MOMENTS (clean_draw.frag -DCLEAN_MB_MOMENTS): every fragment pass 2 would keep adds its
//      optical depth od = -ln(1 - a) and od x z^k (k = 1..m) on the WARPED depth z (mbWarp) into
//      additive float targets. No lighting: the base alpha only.
//   2. CUTOFF (clean_mb_cutoff.frag): per pixel the depth beyond which the CONSERVATIVE transmittance
//      (the reconstruction's upper bound, eta = 0: the canonical distribution puts no mass of the
//      query point in front of it) is below RenderVKMBOITCutoff, written as a depth buffer -- nearer
//      of it and the opaque depth pushed back by the #731 tolerance.
//   3. SHADE (clean_draw.frag -DCLEAN_MB_SHADE): the same fragments again, depth-tested against the
//      cutoff (early fragment tests: hidden layers never launch the lit FS). A survivor rebuilds
//      T_front at its OWN depth from the moments (eta 0.25, the paper's interpolation toward the
//      upper absorbance bound) and is lit at its own position, tiered by its visible weight a x T:
//      two passes, -DCLEAN_MB_TIER_FULL (the sorted path's lighting) and -DCLEAN_MB_TIER_CHEAP
//      (shared pixel visibility), each adding its share k of  colour x a x T,  a x T  and the glow
//      and multiplying (1 - a)^k into the revealage (the exact prod(1 - a) of the shaded layers).
//   4. RESOLVE (clean_mb_resolve.frag): colour = sum(c a T) / sum(a T) x (1 - revealage) over the
//      scene x revealage (the paper's eq. 2 renormalisation, with the coverage taken from pass 3's
//      exact product instead of exp(-b0): glare-lifted alpha and the cutoff's dropped tail stay
//      consistent with what was actually composited).
//
// MOMENT MODES (ssr-independent: the module's params block): 1 = 4 power moments (b1..b4 in an
// RGBA32F target, b0 + the debug fragment count in RG32F), 2 = 6 power moments (b1..b4 RGBA32F,
// b0 / b5 / b6 / count RGBA32F). BIAS (supplementary, Table 1, single precision): 4 power
// beta 5e-7 toward (0, 0.375, 0, 0.375); 6 power beta 5e-6 toward (0, 0.48, 0, 0.451, 0, 0.45).
// The 16-bit quantised storage of the paper (80 / 112 bits per pixel) is a follow-up.
#ifndef CLEAN_MBOIT_GLSL
#define CLEAN_MBOIT_GLSL

#ifdef MB_SET
// The module's set (LLCleanAlphaMBOITVK): 0 / 1 the moment targets, 2 the shade sums, 3 the glow
// sums, 4 the revealage (all GENERAL, read with texelFetch), 5 the params (written in the command
// stream per water side: vkCmdUpdateBuffer, never by the host while a frame may read it).
layout(set = MB_SET, binding = 0) uniform texture2D uMbMomA;
layout(set = MB_SET, binding = 1) uniform texture2D uMbMomB;
layout(set = MB_SET, binding = 2) uniform texture2D uMbSum;
layout(set = MB_SET, binding = 3) uniform texture2D uMbGlow;
layout(set = MB_SET, binding = 4) uniform texture2D uMbReveal;
layout(std140, set = MB_SET, binding = 5) uniform MbParamsBlock
{
    vec4  warp;    // x ln zmin, y 2 / (ln zmax - ln zmin), z zmin, w zmax (view depth, m)
    vec4  knobs;   // x cutoff epsilon (transmittance), y overestimation eta, z full-tier weight, w fade band
    uvec4 sizes;   // x moment target w | h << 16, y render w | h << 16, z 1 = half-resolution moments, w moment mode
    uvec4 flags;   // x debug bits (MBD_*), y 1 = the cutoff is on, z the projection is orthographic, w 1 = one shade pass (no tiers)
} mb;
#endif

// Debug bits (RenderVKMBOITDebug)
const uint MBD_TIER  = 1u;   // shade: each fragment painted by its tier (green full, yellow fading, blue cheap)
const uint MBD_TRANS = 2u;   // resolve: b0 / -ln(cutoff) in grey (white: the cutoff can drop layers), red where it did
const uint MBD_COUNT = 4u;   // resolve: the culled fraction (green none .. red all), brightness log2(layer count) (64 = full)
const uint MBD_NOCUT = 8u;   // cutoff pass writes the opaque depth only (A/B of the early-Z cut)

// Smallest coverage a moments fragment adds; the absorbance clamp (od <= -ln(1 - 0.9999) = 9.2).
const float MB_MIN_A = 1.0 / 1024.0;
const float MB_MAX_A = 0.9999;
float mbOpticalDepth(float a) { return -log(1.0 - clamp(a, 0.0, MB_MAX_A)); }

// Warped depth (paper section 3.3): logarithmic over [zmin, zmax] onto [-1, 1].
float mbWarpWith(float zv, vec4 w) { return clamp((log(max(zv, w.z)) - w.x) * w.y - 1.0, -1.0, 1.0); }

float mbSafe(float d) { return abs(d) > 1e-12 ? d : (d < 0.0 ? -1e-12 : 1e-12); }

// ---- 4 POWER MOMENTS ----
// m = (b1, b2, b3, b4) / b0. Returns A / b0 in [0, 1]: the fraction of the absorbance in front of z,
// eta the weight of the query point itself (0: lower bound on the absorbance = UPPER bound on T).
// Hankel system H c = (1, z, z^2) by LDL^T, the quadratic c2 x^2 + c1 x + c0 gives the other two
// support points of the canonical distribution through z; the quadratic interpolating
// (z, eta), (x1, [x1 < z]), (x2, [x2 < z]) has expectation a0 + a1 m1 + a2 m2 under it.
float mbFraction4(vec4 m, float z, float eta)
{
    m = mix(m, vec4(0.0, 0.375, 0.0, 0.375), 5e-7);
    const float m1 = m.x, m2 = m.y, m3 = m.z, m4 = m.w;
    const float d1 = m2 - m1 * m1;
    const float l21 = (m3 - m1 * m2) / mbSafe(d1);
    const float d2 = m4 - m2 * m2 - l21 * l21 * d1;
    float y1 = z - m1;
    float y2 = z * z - m2 - l21 * y1;
    y1 /= mbSafe(d1);
    y2 /= mbSafe(d2);
    const float c2 = y2;
    const float c1 = y1 - l21 * c2;
    const float c0 = 1.0 - m1 * c1 - m2 * c2;
    float x1, x2;
    if (abs(c2) < 1e-12 * max(max(abs(c1), abs(c0)), 1e-30))
    {   // (degenerate: one finite root)
        x1 = abs(c1) > 1e-30 ? -c0 / c1 : 2.0;
        x2 = 1e6;
    }
    else
    {
        const float p = c1 / c2, q = c0 / c2;
        const float r = sqrt(max(0.25 * p * p - q, 0.0));
        x1 = -0.5 * p - r;
        x2 = -0.5 * p + r;
    }
    const float f1 = x1 < z ? 1.0 : 0.0, f2 = x2 < z ? 1.0 : 0.0;
    const float f01 = (f1 - eta) / mbSafe(x1 - z);
    const float f12 = (f2 - f1) / mbSafe(x2 - x1);
    const float f012 = (f12 - f01) / mbSafe(x2 - z);
    const float a2 = f012;
    const float a1 = f01 - f012 * (z + x1);
    const float a0 = eta - f01 * z + f012 * z * x1;
    const float F = a0 + a1 * m1 + a2 * m2;
    return (F >= 0.0) ? min(F, 1.0) : 0.0;   // (NaN -> 0: nothing in front)
}

// ---- 6 POWER MOMENTS ----
// m = (b1..b4) / b0, n = (b5, b6) / b0. Hankel 4 x 4 by LDL^T, the cubic's three real roots
// (trigonometric form of the depressed cubic), the cubic interpolant by divided differences.
vec4 mbPolyMulRoot(vec4 p, float r) { return vec4(-r * p.x, p.x - r * p.y, p.y - r * p.z, p.z - r * p.w); }
float mbFraction6(vec4 m, vec2 n, float z, float eta)
{
    m = mix(m, vec4(0.0, 0.48, 0.0, 0.451), 5e-6);
    n = mix(n, vec2(0.0, 0.45), 5e-6);
    const float b1 = m.x, b2 = m.y, b3 = m.z, b4 = m.w, b5 = n.x, b6 = n.y;
    // H = [[1 b1 b2 b3] [b1 b2 b3 b4] [b2 b3 b4 b5] [b3 b4 b5 b6]] = L D L^T
    const float l10 = b1, l20 = b2, l30 = b3;
    const float d1 = b2 - b1 * b1;
    const float id1 = 1.0 / mbSafe(d1);
    const float l21 = (b3 - l20 * l10) * id1;
    const float l31 = (b4 - l30 * l10) * id1;
    const float d2 = b4 - l20 * l20 - l21 * l21 * d1;
    const float id2 = 1.0 / mbSafe(d2);
    const float l32 = (b5 - l30 * l20 - l31 * l21 * d1) * id2;
    const float d3 = b6 - l30 * l30 - l31 * l31 * d1 - l32 * l32 * d2;
    // L y = (1, z, z^2, z^3); y /= D; L^T c = y
    float y0 = 1.0;
    float y1 = z - l10 * y0;
    float y2 = z * z - l20 * y0 - l21 * y1;
    float y3 = z * z * z - l30 * y0 - l31 * y1 - l32 * y2;
    y1 *= id1; y2 *= id2; y3 /= mbSafe(d3);
    const float c3 = y3;
    const float c2 = y2 - l32 * c3;
    const float c1 = y1 - l21 * c2 - l31 * c3;
    const float c0 = y0 - l10 * c1 - l20 * c2 - l30 * c3;
    // roots of c3 x^3 + c2 x^2 + c1 x + c0
    vec3 x;
    if (abs(c3) < 1e-12 * max(max(abs(c2), abs(c1)), max(abs(c0), 1e-30)))
    {
        const float p = c1 / mbSafe(c2), q = c0 / mbSafe(c2);
        const float r = sqrt(max(0.25 * p * p - q, 0.0));
        x = vec3(-0.5 * p - r, -0.5 * p + r, 1e6);
    }
    else
    {
        const float a = c2 / c3, b = c1 / c3, c = c0 / c3;
        const float p = b - a * a / 3.0;
        const float q = 2.0 * a * a * a / 27.0 - a * b / 3.0 + c;
        const float pn = min(p, -1e-20);   // (three real roots: p < 0; the bias keeps it there)
        const float s = 2.0 * sqrt(-pn / 3.0);
        const float arg = clamp(3.0 * q / (pn * s), -1.0, 1.0);   // = (3q / 2p) sqrt(-3 / p)
        const float th = acos(arg) / 3.0;
        x = s * vec3(cos(th), cos(th - 2.0943951), cos(th - 4.1887902)) - a / 3.0;
    }
    const float f1 = x.x < z ? 1.0 : 0.0, f2 = x.y < z ? 1.0 : 0.0, f3 = x.z < z ? 1.0 : 0.0;
    // divided differences on the nodes (z, x1, x2, x3)
    const float d01 = (f1 - eta) / mbSafe(x.x - z);
    const float d12 = (f2 - f1) / mbSafe(x.y - x.x);
    const float d23 = (f3 - f2) / mbSafe(x.z - x.y);
    const float d012 = (d12 - d01) / mbSafe(x.y - z);
    const float d123 = (d23 - d12) / mbSafe(x.z - x.x);
    const float d0123 = (d123 - d012) / mbSafe(x.z - z);
    // Newton form -> monomials: ((d0123 (x - x2) + d012) (x - x1) + d01) (x - z) + eta
    // (coefficients lowest degree first)
    vec4 P = vec4(d0123, 0.0, 0.0, 0.0);
    P = mbPolyMulRoot(P, x.y); P.x += d012;
    P = mbPolyMulRoot(P, x.x); P.x += d01;
    P = mbPolyMulRoot(P, z);   P.x += eta;
    const float F = P.x + P.y * b1 + P.z * b2 + P.w * b3;
    return (F >= 0.0) ? min(F, 1.0) : 0.0;
}

// The fraction of b0 in front of warped depth z (mode 1: momB unused but b0 in momB.x elsewhere).
float mbFraction(uint mode, vec4 momA, vec4 momB, float z, float eta)
{
    const float b0 = momB.x;
    if (!(b0 > 1e-7)) return 0.0;
    const float ib = 1.0 / b0;
    return mode == 2u ? mbFraction6(momA * ib, momB.yz * ib, z, eta) : mbFraction4(momA * ib, z, eta);
}
// T in front of z (eta: 0 = the conservative upper bound).
float mbFrontT(uint mode, vec4 momA, vec4 momB, float z, float eta)
{
    return exp(-momB.x * mbFraction(mode, momA, momB, z, eta));
}

// CUTOFF: the smallest warped depth (in [-1, 1]) at which the upper-bound transmittance is <= eps,
// or 2.0 when none (the bound is monotone in z). Bisection; the returned depth is the FAR end of
// the final bracket, so every depth in front of it has an upper bound above eps.
float mbCutoff(uint mode, vec4 momA, vec4 momB, float eps)
{
    const float b0 = momB.x;
    if (!(b0 > 1e-7)) return 2.0;
    const float need = -log(max(eps, 1e-30)) / b0;   // the fraction of b0 that must lie in front
    if (need >= 1.0) return 2.0;                      // (even everything in front keeps T above eps)
    if (mbFraction(mode, momA, momB, 1.0, 0.0) < need) return 2.0;
    float lo = -1.0, hi = 1.0;
    for (int i = 0; i < 14; ++i)
    {
        const float mid = 0.5 * (lo + hi);
        if (mbFraction(mode, momA, momB, mid, 0.0) >= need) hi = mid; else lo = mid;
    }
    return hi;
}

#endif   // CLEAN_MBOIT_GLSL
