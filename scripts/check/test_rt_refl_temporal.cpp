// Fixture for THE OPAQUE RT REFLECTIONS' TEMPORAL RULE (indra/vkclean/shaders/clean_rt_refl_temporal.h, included
// as-is; scripts/check/checks/rt_refl_temporal.py). Pass 13 resolves each mirror pixel from a gather that is mostly
// its own ray (a 0.6 px Gaussian; its neighbours weighed down further by the plane / lobe / hit-distance tests), and
// that ray's hit shading is noisy (its lights' shadow rays are sampled). The old rule clamped the history to the
// gather's mean +- (1.25 sigma + 2 %) and went back to this frame once the history sat 0.25 sigma outside: with a
// gather this narrow the box sits on this frame and the reset fires in most frames -- the reflection never converges
// and "dances". Each pixel here is simulated independently (a 3 x 3 gather of fresh noisy rays per frame, the
// shader's weights), against the OLD rule (reproduced below as it shipped) and rrtResolve:
//   MIRROR   a static mirror pixel (neighbours at half weight, and NARROW: neighbours ~0, the gather is the own ray):
//            after 40 frames the new rule's frame-to-frame RMS <= RRT_MAX_RATIO x the old rule's, its resets (blend
//            weight >= 0.5) in <= 5 % of the frames, its mean within 3 % of the truth -- grey light noise and COLOUR
//            (two lights of different colour: each channel swings while the luminance holds);
//   BOOT     a fresh own-ray-only history resets at most once in its first 8 frames (the variance's seed);
//   CHANGE   the reflected content changes 5 x (a light switched on in the mirror, then off): followed within 4 / 8
//            frames (the thresholds below; the light going OFF is the ghost case of a spread kept in absolute terms);
//   BIAS     a hit's single shadow ray (the lamp blocked ~0, another light scaled up): two-valued 0.02 / 0.12, 0.002
//            (and 0) + 10 % x 0.3, sparse 0.1 + 2 % x 3 -- the converged mean within 3 % of the truth at the RR cap
//            (4) and 8 (the log-symmetric clamp settled at the geometric mean: 0.83 / 0.92 of it);
//   STRAT    the hit's light picks (clean_hit_nee.h, the production helpers): the hashed draw (hnAdd, independent every
//            frame) against the STRATIFIED one (hnStratInit / hnAddS: the pixel's rotation + the frame along the
//            kernel's Kronecker step, K strata within the frame) -- EXACT (the picks over a uniform grid of xi hit
//            each light w / W, 3 to 64 candidates: the precision fallback included), UNBIASED (the raw mean), and the
//            reflection's error / crawl at the RR cap (4) for 2-16 lights, 1 / 2 / 4 rays: a mirror (the same lights
//            every frame), a rough lobe (the hit's shares move) and a re-drawn set (the order shuffled each frame);
//   FRESH    no history: this frame; then a running mean (length 2, 3, ... up to the cap) -- and the RR cap holds;
//   STILL    a noise-free reflection holds its value exactly.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <random>
#include <vector>
typedef uint32_t uint;

struct vec3
{
    float x, y, z;
    vec3() : x(0.f), y(0.f), z(0.f) {}
    explicit vec3(float v) : x(v), y(v), z(v) {}
    vec3(float a, float b, float c) : x(a), y(b), z(c) {}
};
static vec3 operator+(vec3 a, vec3 b) { return vec3(a.x + b.x, a.y + b.y, a.z + b.z); }
static vec3 operator-(vec3 a, vec3 b) { return vec3(a.x - b.x, a.y - b.y, a.z - b.z); }
static vec3 operator*(vec3 a, float s) { return vec3(a.x * s, a.y * s, a.z * s); }
static vec3 operator*(vec3 a, vec3 b) { return vec3(a.x * b.x, a.y * b.y, a.z * b.z); }
static vec3 operator/(vec3 a, float s) { return vec3(a.x / s, a.y / s, a.z / s); }
static float dot(vec3 a, vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static float length(vec3 a) { return std::sqrt(dot(a, a)); }
static float min(float a, float b) { return a < b ? a : b; }
static float max(float a, float b) { return a > b ? a : b; }
static vec3 min(vec3 a, vec3 b) { return vec3(min(a.x, b.x), min(a.y, b.y), min(a.z, b.z)); }
static vec3 max(vec3 a, vec3 b) { return vec3(max(a.x, b.x), max(a.y, b.y), max(a.z, b.z)); }
static float clamp(float v, float a, float b) { return min(max(v, a), b); }
static vec3 clamp(vec3 v, vec3 a, vec3 b) { return min(max(v, a), b); }
static float mix(float a, float b, float t) { return a * (1.f - t) + b * t; }
static vec3 mix(vec3 a, vec3 b, float t) { return a * (1.f - t) + b * t; }
static vec3 sqrt3(vec3 v) { return vec3(std::sqrt(max(v.x, 0.f)), std::sqrt(max(v.y, 0.f)), std::sqrt(max(v.z, 0.f))); }
using std::abs;
using std::sqrt;
using std::log;
using std::exp;
#include "clean_rt_refl_temporal.h"
#include "clean_hit_nee.h"

#ifndef RRT_MAX_RATIO
#define RRT_MAX_RATIO 0.6
#endif
static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// THE OLD RULE (clean_deferred_light.comp rtrTemporal before this change; its caller's sigma scaling included)
static vec3 oldResolve(vec3 res, vec3 mu, vec3 sg, vec3 hist, bool histOk, float frames, float& aOut)
{
    aOut = 1.f;
    if (!histOk) return res;
    const vec3 sigma = sg * 1.25f + max(mu, res) * 0.02f + vec3(1e-4f);
    const vec3 hc = clamp(hist, min(mu, res) - sigma, max(mu, res) + sigma);
    const float off = length(hist - hc) / (length(sigma) + 1e-4f);
    aOut = mix(1.f / frames, 1.f, clamp(off - 0.25f, 0.f, 1.f));
    return mix(hc, res, aOut);
}

// One frame's gather at a mirror pixel: its own ray + the 8 neighbours at the 0.6 px Gaussian, each neighbour further
// weighed by kG (the plane / lobe / hit-distance tests). Every ray: the reflection's radiance t x the hit shading's
// sampled light (one of two lights per shadow ray: 0.6 or 1.4 x; COLOUR: a warm and a cool light of the same luminance
// -- a stable luminance, each channel swinging 3-10 x).
static float kG = 0.5f;
static bool g_colour = false;
static const vec3 kWarm(1.6f, 0.75f, 0.25f), kCool(0.25f, 1.1f, 4.0f);   // (luminance 0.82 / 1.13 ...)
static vec3 lightMul(bool second)
{
    if (!g_colour) return vec3(second ? 1.4f : 0.6f);
    // (each normalised to luminance 1)
    const vec3 c = second ? kCool : kWarm;
    return c / rrtLum(c);
}
static vec3 meanMul() { return (lightMul(false) + lightMul(true)) * 0.5f; }
struct Gather { vec3 mu, sg; float n; };
static Gather gather(std::mt19937& rng, vec3 t)
{
    std::uniform_real_distribution<float> U(0.f, 1.f);
    vec3 h, q;
    float w = 0.f, w2 = 0.f;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const float d2 = float(dx * dx + dy * dy);
            const float wb = (d2 == 0.f ? 1.f : kG) * std::exp(-d2 / (2.f * 0.6f * 0.6f));
            const vec3 r = t * lightMul(U(rng) < 0.5f);
            h = h + r * wb; q = q + r * r * wb; w += wb; w2 += wb * wb;
        }
    Gather g;
    g.mu = h / w;
    g.sg = sqrt3(q / w - g.mu * g.mu);
    g.n = w * w / w2;
    return g;
}

// ---- STRAT: a reflection hit's local lights as rtHitLocalLights + its K shadow rays add them up (clean_hit_nee.h):
// n grey shares w_i (luminance w_i), visibility v_i (0: blocked); strat = the reflection kernels' RT_SHADE_STOCH_XI path
static const uint kXiStep = 2654435769u;   // (clean_deferred_light.comp RTR_XI_HIT_LIGHTS: the golden ratio x 2^32)
static float pixelXi(uint px, uint frame)   // (rtrPixelXi: the pixel's rotation + the frame's Kronecker step)
{
    return float((hnHash(px ^ 0x6A09E667u) + (frame & 0xFFFFu) * kXiStep) >> 8u) * (1.f / 16777216.f);
}
static void hitPicks(bool strat, uint K, int n, const float* w, float xi, uint seed, float* score, uint* light, vec3* share,
                     float& W)
{
    float u[HN_MAXK], amp[HN_MAXK];
    for (int j = 0; j < HN_MAXK; ++j) { score[j] = 0.f; light[j] = 0u; share[j] = vec3(0.f); }
    hnStratInit(xi, K, u, amp);
    W = 0.f;
    for (int i = 0; i < n; ++i)
        W = strat ? hnAddS(W, K, u, amp, seed, uint(i), uint(i), vec3(w[i]), score, light, share)
                  : hnAdd(W, K, seed, uint(i), uint(i), vec3(w[i]), score, light, share);
}
static float hitLights(bool strat, uint K, int n, const float* w, const float* v, float xi, uint seed)
{
    float score[HN_MAXK], W;
    uint light[HN_MAXK];
    vec3 share[HN_MAXK];
    hitPicks(strat, K, n, w, xi, seed, score, light, share, W);
    vec3 s = hnFinish(W, K, score, share);
    for (uint j = 0; j < K; ++j)
        if (score[j] > 0.f) s = s - share[j] * (1.f - v[light[j]]);   // (the ray: the blocked part removed)
    return s.y;
}
struct StratStat { double err, crawl, bias; };
// 256 pixels x 400 frames, resolved at the RR cap (own-ray gather): RMS error vs the truth and frame-to-frame RMS (the
// crawl), both relative to the truth; bias: the raw estimate's mean. mode 0 mirror, 1 rough lobe (each share x 0.7 ..
// 1.3 per frame; the truth at the mean), 2 re-drawn set (the candidates' order shuffled per frame). Same scenes for
// both draws (paired).
static StratStat stratRun(bool strat, int mode, int n, uint K)
{
    std::mt19937 g(4242u + 97u * uint(n) + uint(mode));
    std::uniform_real_distribution<float> U(0.f, 1.f);
    double se = 0.0, sc = 0.0, sb = 0.0, sTruth = 0.0;
    long cnt = 0;
    for (uint px = 0; px < 256u; ++px)
    {
        float w[64], v[64];
        double truth = 0.0, base = 0.0;
        for (int i = 0; i < n; ++i)
        {
            w[i] = std::exp(U(g) * std::log(10.f));   // (1 .. 10)
            v[i] = U(g) < 0.5f ? 1.f : 0.f;
            truth += w[i] * v[i];
            base += 0.5 * w[i];                       // (the hit's unsampled light: sun, ambient, emission)
        }
        truth += base;
        vec3 h, hv, prev;
        float len = 0.f;
        for (uint f = 0; f < 400u; ++f)
        {
            float wf[64], vf[64];
            int perm[64];
            for (int i = 0; i < n; ++i) perm[i] = i;
            if (mode == 2) std::shuffle(perm, perm + n, g);
            for (int i = 0; i < n; ++i)
            {
                wf[i] = w[perm[i]] * (mode == 1 ? 0.7f + 0.6f * U(g) : 1.f);
                vf[i] = v[perm[i]];
            }
            const uint seed = g();
            const float x = float(base) + hitLights(strat, K, n, wf, vf, pixelXi(px * 7919u + 13u, f), seed);
            const vec3 c(x);
            vec3 nhv;
            float nlen;
            const vec3 o = rrtResolve(c, c, vec3(1e-4f), 1.f, h, hv, len, f > 0, 4.f, nhv, nlen);
            if (f >= 40)
            {
                se += (o.y - truth) * (o.y - truth) / (truth * truth);
                sc += (o.y - prev.y) * (o.y - prev.y) / (truth * truth);
                sb += x; sTruth += truth;
                ++cnt;
            }
            prev = h = o; hv = nhv; len = nlen;
        }
    }
    return StratStat{std::sqrt(se / cnt), std::sqrt(sc / cnt), sb / sTruth - 1.0};
}

// ---- BUSY CELL: the light grid in front of the hit's K rays (clean_regir_build.comp population 0, clean_regir.glsl
// regirHitLights, rtHitLocalLights): a cell reached by n > R lights stores R stratified samples of its CDF (q = p_cell /
// W, one xi per cell and frame), the hit picks N of them by RIS with its own target p_hit (weight p_hit / q, the pick's
// estimate x wsum / (R N p_hit), duplicates merged), and its K rays sample those picks' shares. lvl 0: every draw hashed
// (the tree before the stratified picks); 1: the grid picks (hnStratTake on the pixel's sqrt 2 - 1 point) and the K
// picks (hnAddS, the golden ratio) stratified, the cell's xi hashed; 2: + the cell's xi along regirCellXi (sqrt 3 - 1).
static const uint kRegirR = 8u, kRegirN = 2u;   // (RenderVKLightGridReservoirs / Picks defaults)
static float cellXi(uint cell, uint frame)   // (clean_regir.glsl regirCellXi)
{
    return float((hnHash(cell ^ 0x3C6EF372u) + (frame & 0xFFFFu) * 3144134278u) >> 8u) * (1.f / 16777216.f);
}
static float gridXi(uint px, uint frame)   // (rtrPixelXi(0xBB67AE85u, RTR_XI_GRID_PICKS))
{
    return float((hnHash(px ^ 0xBB67AE85u) + (frame & 0xFFFFu) * 1779033704u) >> 8u) * (1.f / 16777216.f);
}
static float busyHit(int lvl, uint K, int n, const float* pc, const float* ph, const float* f, const float* v, uint px,
                     uint frame, std::mt19937& g)
{
    std::uniform_real_distribution<float> U(0.f, 1.f);
    // the build: R stratified samples of p_cell
    float W = 0.f;
    for (int i = 0; i < n; ++i) W += pc[i];
    const float xi = lvl >= 2 ? cellXi(px * 31u + 5u, frame) : U(g);
    int sl[kRegirR];
    float sinv[kRegirR];
    uint r = 0;
    float cum = 0.f;
    for (int i = 0; i < n && r < kRegirR; ++i)
    {
        cum += pc[i];
        while (r < kRegirR && (float(r) + xi) * (W / float(kRegirR)) < cum) { sl[r] = i; sinv[r] = W / pc[i]; ++r; }
    }
    for (; r < kRegirR; ++r) { sl[r] = n - 1; sinv[r] = W / pc[n - 1]; }
    // the hit: N RIS picks
    float su[HN_MAXK], samp[HN_MAXK];
    hnStratInit(gridXi(px, frame), kRegirN, su, samp);
    float wsum = 0.f;
    int sel[kRegirN] = {0, 0};
    float selP[kRegirN] = {0.f, 0.f};
    for (uint j = 0; j < kRegirR; ++j)
    {
        const float p = ph[sl[j]], w = p * sinv[j];
        wsum += w;
        const float pt = w / wsum;
        for (uint k = 0; k < kRegirN; ++k)
        {
            const bool take = (lvl >= 1 && samp[k] < HN_STRAT_AMP) ? hnStratTake(su[k], samp[k], pt) : U(g) * wsum < w;
            if (take) { sel[k] = sl[j]; selP[k] = p; }
        }
    }
    const float scale = wsum / float(kRegirR * kRegirN);
    float cw[kRegirN], cv[kRegirN];
    int cl[kRegirN], nc = 0;
    for (uint k = 0; k < kRegirN; ++k)
    {
        int m = -1;
        for (int q = 0; q < nc; ++q) if (cl[q] == sel[k]) m = q;
        const float wk = scale / selP[k];
        if (m >= 0) { cw[m] += wk * f[sel[k]]; continue; }
        cl[nc] = sel[k]; cw[nc] = wk * f[sel[k]]; cv[nc] = v[sel[k]]; ++nc;
    }
    // the K rays among the picks (the walk's candidates: lw x the share)
    return hitLights(lvl >= 1, K, nc, cw, cv, pixelXi(px * 7919u + 13u, frame), g());
}
static StratStat busyRun(int lvl, int n, uint K)
{
    std::mt19937 gs(777u + 13u * uint(n)), g(31337u + uint(n));   // (the scenes: the same at every level; the draws)
    std::uniform_real_distribution<float> U(0.f, 1.f);
    double se = 0.0, sc = 0.0, sb = 0.0, sTruth = 0.0;
    long cnt = 0;
    for (uint px = 0; px < 256u; ++px)
    {
        float f[64], v[64], pc[64], ph[64];
        double truth = 0.0, base = 0.0;
        for (int i = 0; i < n; ++i)
        {
            f[i] = std::exp(U(gs) * std::log(10.f));
            v[i] = U(gs) < 0.5f ? 1.f : 0.f;
            pc[i] = f[i] * (0.3f + 1.4f * U(gs));   // (the cell's target: the light's reach into the box, not at the hit)
            ph[i] = f[i] * (0.6f + 0.8f * U(gs));   // (the hit's: no BRDF lobe, no shadow)
            truth += f[i] * v[i];
            base += 0.5 * f[i];
        }
        truth += base;
        vec3 h, hv, prev;
        float len = 0.f;
        for (uint fr = 0; fr < 400u; ++fr)
        {
            const float x = float(base) + busyHit(lvl, K, n, pc, ph, f, v, px, fr, g);
            const vec3 c(x);
            vec3 nhv;
            float nlen;
            const vec3 o = rrtResolve(c, c, vec3(1e-4f), 1.f, h, hv, len, fr > 0, 4.f, nhv, nlen);
            if (fr >= 40)
            {
                se += (o.y - truth) * (o.y - truth) / (truth * truth);
                sc += (o.y - prev.y) * (o.y - prev.y) / (truth * truth);
                sb += x; sTruth += truth;
                ++cnt;
            }
            prev = h = o; hv = nhv; len = nlen;
        }
    }
    return StratStat{std::sqrt(se / cnt), std::sqrt(sc / cnt), sb / sTruth - 1.0};
}

int main()
{
    std::mt19937 rng(1906u);
    const vec3 truth(0.30f, 0.33f, 0.36f);
    const float kFrames = 8.f;      // (RTR_HIST_SMOOTH)
    const float kFramesOld = 5.f;   // (the old RTR_HIST_SMOOTH)
    // MIRROR: the old rule against the new, same rays; the neighbours at half weight, then NARROW (a bumpy mirror whose
    // lobe / plane tests leave the neighbours ~nothing: the gather is the own ray, its spread ~0); grey light noise, then
    // COLOUR (two lights of different colour at the hit: a per-channel swing a luminance variance would not see)
    const float kGs[2] = {0.5f, 0.02f};
    for (int mi = 0; mi < 4; ++mi)
    {
        const int gi = mi & 1;
        kG = kGs[gi];
        g_colour = mi >= 2;
        const char* name = mi == 0 ? "wide" : mi == 1 ? "narrow" : mi == 2 ? "wide, colour" : "narrow, colour";
        const int frames = 600;
        vec3 hOld, hNew, pOld, pNew, hv;
        float len = 0.f;
        double eOld = 0.0, eNew = 0.0, mNew = 0.0, mOld = 0.0;
        int resetOld = 0, resetNew = 0, cnt = 0;
        for (int f = 0; f < frames; ++f)
        {
            const Gather g = gather(rng, truth);
            float aOld;
            const vec3 o = oldResolve(g.mu, g.mu, g.sg, hOld, f > 0, kFramesOld, aOld);
            vec3 nhv;
            float nlen;
            const vec3 n = rrtResolve(g.mu, g.mu, g.sg + vec3(1e-4f), g.n, hNew, hv, len, f > 0, kFrames, nhv, nlen);
            const float aNew = 1.f / nlen;
            if (f >= 40)
            {
                const double a = length(o - pOld), b = length(n - pNew);
                eOld += a * a; eNew += b * b; mNew += n.y; mOld += o.y;
                resetOld += aOld >= 0.5f; resetNew += aNew >= 0.5f;
                ++cnt;
            }
            pOld = hOld = o; pNew = hNew = n; hv = nhv; len = nlen;
        }
        const double ro = std::sqrt(eOld / cnt), rn = std::sqrt(eNew / cnt);
        mNew /= cnt;
        mOld /= cnt;
        const double want = truth.y * meanMul().y;
        std::printf("MIRROR (%s, n_eff %.2f): frame-to-frame RMS old %.4f -> new %.4f (x %.2f); resets old %.0f %% -> new %.0f %%; "
                    "mean old %.4f new %.4f (truth %.4f)\n", name, gather(rng, truth).n, ro, rn, rn / ro, 100.0 * resetOld / cnt,
                    100.0 * resetNew / cnt, mOld, mNew, want);
        CHECK(rn <= RRT_MAX_RATIO * ro, "MIRROR (%s): the reflection still dances: RMS %.4f vs the old rule's %.4f (x %.2f, limit %.2f)",
              name, rn, ro, rn / ro, (double)RRT_MAX_RATIO);
        CHECK(resetNew <= 0.05 * cnt, "MIRROR (%s): the anti-lag fires on noise in %.0f %% of the frames", name, 100.0 * resetNew / cnt);
        // (3 %: the log-symmetric box pulled this two-valued +-40 % noise ~3 % toward its geometric mean; the linear box
        //  below the anti-lag does not -- BIAS below)
        CHECK(std::fabs(mNew - want) <= 0.03 * want, "MIRROR (%s): the rule biases the reflection: %.4f vs %.4f", name, mNew, want);
    }
    g_colour = false;
    // BOOT: a fresh history (a disocclusion at a moving edge, a teleport) on an own-ray-only gather -- its spread shows
    // no noise, so the variance starts from nothing: resets in its first 8 frames (each one shows a single frame's
    // sample), averaged over 256 pixels
    {
        kG = kGs[1];
        int resets = 0;
        for (int p = 0; p < 256; ++p)
        {
            vec3 h, hv;
            float len = 0.f;
            for (int f = 0; f < 9; ++f)
            {
                const Gather g = gather(rng, truth);
                vec3 nhv;
                float nlen;
                h = rrtResolve(g.mu, g.mu, g.sg + vec3(1e-4f), g.n, h, hv, len, f > 0, kFrames, nhv, nlen);
                // (a reset: the blend at least twice the running mean's -- the length at most half of hlen + 1)
                if (f > 0 && nlen <= 0.5f * min(len + 1.f, kFrames)) ++resets;
                hv = nhv; len = nlen;
            }
        }
        const double perPx = resets / 256.0;
        std::printf("BOOT (narrow): %.2f resets in a fresh history's first 8 frames\n", perPx);
        CHECK(perPx <= 1.0, "BOOT: a fresh history resets %.2f times in its first 8 frames (the dance at every "
              "disocclusion edge)", perPx);
    }
    // CHANGE: the reflected content switches (a light in the mirror on: 5 x; then off again): within 5 % after 4 frames
    // (wide) / 15 % (narrow: one ray per frame with +-40 % noise -- a single frame cannot tell a 5 x change from the
    // noise with certainty, the box carries most of it), within 8 % after 8 frames either way. (The mean over 64
    // pixels: one pixel's 4th frame is itself a noisy estimate.)
    for (int ci = 0; ci < 4; ++ci)
    {
        const int dir = ci & 1;
        kG = kGs[ci >> 1];
        const vec3 from = dir == 0 ? truth : truth * 5.f, to = dir == 0 ? truth * 5.f : truth;
        double m4 = 0.0, m8 = 0.0;
        for (int p = 0; p < 64; ++p)
        {
            vec3 h, hv;
            float len = 0.f;
            for (int f = 0; f < 68; ++f)
            {
                const Gather g = gather(rng, f < 60 ? from : to);
                vec3 nhv;
                float nlen;
                h = rrtResolve(g.mu, g.mu, g.sg + vec3(1e-4f), g.n, h, hv, len, f > 0, kFrames, nhv, nlen);
                hv = nhv; len = nlen;
                if (f == 63) m4 += h.y;
            }
            m8 += h.y;
        }
        const double off4 = std::fabs(m4 / 64.0 / to.y - 1.0), off8 = std::fabs(m8 / 64.0 / to.y - 1.0);
        const char* what = dir == 0 ? "light on" : "light off";
        const char* gw = (ci >> 1) ? "narrow" : "wide";
        std::printf("CHANGE (%s, %s): %.1f %% off the new reflection after 4 frames, %.1f %% after 8\n", what, gw,
                    100.0 * off4, 100.0 * off8);
        CHECK(off4 <= ((ci >> 1) ? 0.15 : 0.05), "CHANGE (%s, %s): the old reflection lingers (%.1f %% off after 4 frames): "
              "a ghost", what, gw, 100.0 * off4);
        CHECK(off8 <= 0.08, "CHANGE (%s, %s): the old reflection lingers (%.1f %% off after 8 frames): a ghost", what, gw,
              100.0 * off8);
    }
    // BIAS: the hit's shadow ray as the reflection's noise (own-ray-only gather): one shadow ray per hit picks a light
    // -- the blocked lamp gives ~0, another a scaled-up value -- a two-valued signal whose low value is a FACTOR below
    // the mean. The log-symmetric box clipped the history on every low frame and almost never on a high one: the mean
    // converged to the geometric mean (0.02 / 0.12 at p 0.5: 0.83 of the truth; 0.002 + 10 % x 0.3: 0.92) -- shadows
    // in reflections 3-17 % too dark. Converged mean within 3 % of the samples' own mean, at the RR cap (4) and the
    // mirror's (8). SPARSE: a rare spike (0.1 + 2 % x 3): one frame cannot tell it from a light switched on, so it
    // passes for a frame and is taken back the next -- unbiased, but it shows (the frame-to-frame RMS printed).
    // 2 RAYS (two shadow rays per hit, random): printed, not checked (the log anti-lag still trips on the rarer
    // all-dark frame: see the header).
    {
        struct Case { const char* name; float base, spike, p; int rays; };
        const Case cs[5] = {{"two-valued 0.02 / 0.12", 0.02f, 0.10f, 0.5f, 1}, {"0.002 + 10 % x 0.3", 0.002f, 0.3f, 0.1f, 1},
                            {"0 + 10 % x 0.3", 0.0f, 0.3f, 0.1f, 1}, {"sparse 0.1 + 2 % x 3", 0.1f, 3.0f, 0.02f, 1},
                            {"two-valued, 2 rays", 0.02f, 0.10f, 0.5f, 2}};
        std::uniform_real_distribution<float> U(0.f, 1.f);
        for (int ci = 0; ci < 5; ++ci)
            for (int cap = 4; cap <= 8; cap += 4)
            {
                const Case& c = cs[ci];
                double sIn = 0.0, sOut = 0.0, d2 = 0.0;
                long cnt = 0;
                for (int p = 0; p < 64; ++p)
                {
                    vec3 h, hv, prev;
                    float len = 0.f;
                    for (int f = 0; f < 3000; ++f)
                    {
                        float s = 0.f;
                        for (int r = 0; r < c.rays; ++r) s += c.base + (U(rng) < c.p ? c.spike : 0.f);
                        const vec3 x(s / float(c.rays));
                        vec3 nhv;
                        float nlen;
                        h = rrtResolve(x, x, vec3(1e-4f), 1.f, h, hv, len, f > 0, float(cap), nhv, nlen);
                        hv = nhv; len = nlen;
                        if (f >= 100)
                        {
                            sIn += x.y; sOut += h.y; d2 += (h.y - prev.y) * (h.y - prev.y);
                            ++cnt;
                        }
                        prev = h;
                    }
                }
                const double mIn = sIn / cnt, ratio = sOut / cnt / mIn;
                std::printf("BIAS (%s, cap %d): converged mean %.3f x the truth; frame-to-frame RMS %.3f x the mean\n", c.name,
                            cap, ratio, std::sqrt(d2 / cnt) / mIn);
                if (c.rays == 1)
                    CHECK(std::fabs(ratio - 1.0) <= 0.03, "BIAS (%s, cap %d): the resolve biases a shadowed reflection: %.3f x "
                          "the truth (the log-symmetric clamp: the geometric mean)", c.name, cap, ratio);
            }
    }
    // STRAT, EXACT: over a uniform grid of the frame's xi the stratified picks take each light w / W -- every reservoir
    // (K strata), 3 / 8 candidates on the stratified path, 64 (most reservoirs pass HN_STRAT_AMP and finish on the hash)
    for (int n : {3, 8, 64})
        for (uint K = 1; K <= 4; ++K)
        {
            float w[64];
            double Wt = 0.0;
            for (int i = 0; i < n; ++i) { w[i] = 1.f + float((i * 37) % 11) + (i == n / 2 ? 40.f : 0.f); Wt += w[i]; }
            std::vector<double> hits(n, 0.0);
            const uint G = 65536u;
            for (uint gi = 0; gi < G; ++gi)
            {
                float score[HN_MAXK], W;
                uint light[HN_MAXK];
                vec3 share[HN_MAXK];
                hitPicks(true, K, n, w, (float(gi) + 0.5f) / float(G), hnHash(gi * 2654435761u + 7u), score, light, share, W);
                for (uint j = 0; j < K; ++j)
                    if (score[j] > 0.f) hits[light[j]] += 1.0;
            }
            // (the stratified grid is exact to a few / G: 2e-4; the hash fallback adds its binomial noise: 5 sigma)
            double worst = 0.0;
            for (int i = 0; i < n; ++i)
            {
                const double p = w[i] / Wt, N = double(G) * K;
                const double tol = n <= 8 ? 2e-4 : 5.0 * std::sqrt(p * (1.0 - p) / N) + 1e-4;
                worst = std::max(worst, std::fabs(hits[i] / N - p) / tol);
            }
            CHECK(worst <= 1.0, "STRAT EXACT (%d lights, K %u): a light's pick rate is %.2f x the tolerance off its w / W", n,
                  K, worst);
        }
    // STRAT: the hashed draw against the stratified one -- the reflection's error and crawl at the RR cap
    {
        const char* mn[3] = {"mirror", "rough lobe", "re-drawn set"};
        for (int mode = 0; mode < 3; ++mode)
            for (int n : {2, 4, 8, 16})
            {
                std::printf("STRAT (%s, %d lights): err / crawl at cap 4 --", mn[mode], n);
                for (uint K = 1; K <= 4; K *= 2)
                {
                    const StratStat a = stratRun(false, mode, n, K), b = stratRun(true, mode, n, K);
                    std::printf(" %u ray%s %.3f / %.3f -> %.3f / %.3f;", K, K > 1 ? "s" : "", a.err, a.crawl, b.err, b.crawl);
                    CHECK(std::fabs(b.bias) <= 0.01, "STRAT (%s, %d lights, %u rays): the stratified picks are biased: the raw "
                          "mean %.4f x the truth", mn[mode], n, K, 1.0 + b.bias);
                    // never worse than the hashed draw (2 % slack: a re-drawn set leaves little to stratify) ...
                    CHECK(b.err <= 1.02 * a.err, "STRAT (%s, %d lights, %u rays): stratified %.3f vs hashed %.3f", mn[mode], n,
                          K, b.err, a.err);
                    // ... and well under it where a pixel's lights hold still and fit the strata
                    if (mode < 2 && n <= 4 && K >= 2)
                        CHECK(b.err <= 0.8 * a.err, "STRAT (%s, %d lights, %u rays): stratified %.3f, not 20 %% under the "
                              "hashed %.3f", mn[mode], n, K, b.err, a.err);
                }
                std::printf("\n");
            }
    }
    // BUSY CELL: the grid's busy cells (n > R = 8 lights) -- every draw hashed / the hit's draws stratified / + the cell's
    for (int n : {16, 32})
    {
        std::printf("BUSY CELL (%d lights, R %u, N %u): err / crawl at cap 4 --", n, kRegirR, kRegirN);
        for (uint K = 1; K <= 4; K *= 2)
        {
            const StratStat a = busyRun(0, n, K), b = busyRun(1, n, K), c = busyRun(2, n, K);
            std::printf(" %u ray%s %.3f / %.3f -> %.3f / %.3f -> %.3f / %.3f;", K, K > 1 ? "s" : "", a.err, a.crawl, b.err,
                        b.crawl, c.err, c.crawl);
            CHECK(std::fabs(c.bias) <= 0.01 && std::fabs(b.bias) <= 0.01, "BUSY CELL (%d lights, %u rays): biased: the raw "
                  "mean %.4f / %.4f x the truth", n, K, 1.0 + b.bias, 1.0 + c.bias);
            CHECK(c.err <= 1.02 * b.err && b.err <= 1.02 * a.err, "BUSY CELL (%d lights, %u rays): a stratified level is "
                  "noisier: %.3f -> %.3f -> %.3f", n, K, a.err, b.err, c.err);
        }
        std::printf("\n");
    }
    // FRESH: no history -> this frame; then a running mean up to the cap (and the RR cap)
    {
        const vec3 c(0.2f, 0.3f, 0.4f);
        vec3 v;
        float len;
        const vec3 r = rrtResolve(c, c, vec3(1e-4f), 1.f, vec3(50.f), vec3(0.5f), 8.f, false, kFrames, v, len);
        CHECK(length(r - c) < 1e-6f && len == 1.f, "FRESH: a pixel without a valid history took the stale one");
        for (int cap = 4; cap <= 8; cap += 4)
        {
            vec3 h = r, hv = v;
            float hl = len;
            float maxLen = 0.f;
            for (int f = 0; f < 20; ++f)
            {
                vec3 nhv;
                float nlen;
                h = rrtResolve(c, c, vec3(1e-4f), 1.f, h, hv, hl, true, float(cap), nhv, nlen);
                if (f < cap - 1) CHECK(std::fabs(nlen - float(f + 2)) < 1e-3f, "FRESH: frame %d's length %.2f, want %d", f, nlen, f + 2);
                hv = nhv; hl = nlen; maxLen = max(maxLen, nlen);
            }
            CHECK(std::fabs(maxLen - float(cap)) < 1e-3f, "FRESH: the history grew to %.2f frames past its cap %d", maxLen, cap);
        }
    }
    // STILL: a noise-free reflection holds exactly
    {
        const vec3 c(0.3f, 0.4f, 0.5f);
        vec3 h, hv;
        float len = 0.f;
        for (int f = 0; f < 50; ++f)
        {
            vec3 nhv;
            float nlen;
            h = rrtResolve(c, c, vec3(1e-4f), 1.f, h, hv, len, f > 0, kFrames, nhv, nlen);
            hv = nhv; len = nlen;
        }
        CHECK(length(h - c) < 1e-5f, "STILL: a steady reflection drifts by %g", length(h - c));
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
