// Fixture for REDUCED-RESOLUTION GI ON SMALL RECEIVERS (scripts/check/checks/gi_stable_samples.py). Owner report
// 2026-11-09: at RenderVKGIScale 0.5 (worse at 0.25) avatar bodies past ~5 m get swimming noise; full-resolution GI
// does not.
//
// THE MODEL (one scanline through the scene, the shaders' rules per frame; a still camera, a render width of 1280 px
// over a 90 deg view -- DLSS Ultra Performance at 4K --, Halton(2) jitter over 128 phases as llcleanrenderervk.cpp
// makes it):
//   scene    a limb / torso (a cylinder of radius R at distance d, swaying 2 cm or still) 4 m in front of a wall; its
//            irradiance varies with its normal (side light + occlusion toward the grazing edge);
//   trace    GI texel m samples render pixel floor((m + 0.5 + J) r) (OLD: giRenderPixel's jittered lattice) or
//            floor((m + 0.5) r) (NEW: GI_F_STABLE_SAMPLES); raw = E x a 1-spp noise factor (mean 1);
//   history  the GI denoiser's temporal pass: the point's previous unjittered position (its motion vector) -> last
//            frame's texel coordinate q / r - 0.5 (dnSampleGridHistory is off for GI), bilinear taps on this surface
//            (depth 4 % / 3 cm, normal 0.85), else the 3-tap search (normal 0.8), the running mean capped at 32 frames;
//   combine  GI_F_LAG (the default): last frame's GI at the pixel's previous position, taps on this surface (depth
//            3 % / 5 cm, normal^8), none -> the radiance cache's cell.
// THE METRIC: per render pixel, the frame-to-frame change of what the combine returns after a DLSS-like accumulation
// (alpha 0.1: what survives the upscaler -- the low-frequency SWIM, not the per-frame noise it hides), relative to
// the pixel's irradiance, averaged (%) -- on the BODY, and on the BACKGROUND within 6 px of it (a fix must not move
// the swim onto the wall around the avatar). Three noise seeds / sway phases, 260 measured frames each.
// THE ASSERTIONS ("full" = full resolution, the owner's reference -- and what per-pixel samples on movers would give the
// body; old = the jittered lattice, new = GI_F_STABLE_SAMPLES):
//   SHAPE    at quarter extent the old rules swim on a small body: >= 1.5 x full (the report, reproduced);
//   QUARTER  the stable lattice: >= 40 % less than the old on a still limb (10 / 20 m) and a swaying one (10 m). A
//            swaying TORSO at quarter extent is NOT better (printed): the body slides under a fixed lattice, each
//            texel's one sample drifts across a normal that changes tens of degrees per texel and the combine's normal
//            weights flip together -- the jitter was dithering that. No lattice fixes that; per-pixel samples would;
//   HALF     (the default extent) MIXED in this model -- the old rules swim only 1.0-1.8 x full here, the stable lattice
//            helps two cases and hurts two (printed); bound only: <= 2 x full + 0.2 %. Hence RenderVKGIStableSamples
//            ships OFF, for the owner's A/B: the model is one scanline (no a-trous, no spatial reuse, no normal maps,
//            no limb in front of the torso) and does not reproduce a strong half-extent swim;
//   EDGE     the wall next to the body: the new rule <= 1.6 x the old + 0.02 %;
//   FULL     full resolution: unchanged (no lattice phase there);
//   MOVER    (RenderVKGIMoverSamples, clean_gi_movers.glsl, the jittered lattice elsewhere) the body's pixels take their
//            own sample + history along their motion + the combine's mover read: <= 1.5 x full + 0.15 % in EVERY
//            case -- half and quarter, still and swaying (measured: 0.32-0.41 % against full 0.31-0.54 %; the old rule
//            0.36-10.1 %) --, the wall around it no worse than the old + 0.02 % (the noise draws shift).
//            The model's mover sample is as noisy as a low-res texel's raw (1 spp); the shader's also resamples the
//            covering texels' reservoirs, so this is the conservative side.
// Candidates measured with this model and dropped: (1) inverting the previous jittered lattice in the reprojections
// (the local shadows' sampleGridHistory, for the GI denoiser, the reservoirs and the GI_F_LAG combine): "grid-only",
// no gain anywhere, slightly worse swaying; (2) a MOVER-PRIORITY pick (a texel whose cell holds an avatar pixel
// samples it): within the cell 1.37 -> 1.33 %, dilated by a pixel it fixed the body but tripled the swim on the wall.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <random>
#include <vector>
using std::floor;

static int g_fail = 0;
#define CHECK(c, ...) do { if (!(c)) { std::printf("FAIL: " __VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// clean_pdn_common.glsl pdnSampleGridHc, one axis (q: previous raster px incl. its jitter; gj: the texel phase)
static float gridHc(float q, float r, float gj)
{
    float b = floor(q / r - 0.5f - gj);
    float c0 = floor((b + 0.5f + gj) * r) + 0.5f;
    float c1 = floor((b + 1.5f + gj) * r) + 0.5f;
    if (q < c0) { b -= 1.f; c1 = c0; c0 = floor((b + 0.5f + gj) * r) + 0.5f; }
    else if (q >= c1) { b += 1.f; c0 = c1; c1 = floor((b + 1.5f + gj) * r) + 0.5f; }
    return b + std::min(std::max((q - c0) / std::max(c1 - c0, 1e-3f), 0.f), 1.f);
}
static float halton(int i, int b)
{
    float f = 1.f, r = 0.f;
    while (i > 0) { f /= (float)b; r += f * (float)(i % b); i /= b; }
    return r;
}

struct Surf { float z, nx; int id; float E; };
struct Scene
{
    float d, rad, k, wall, cx = 0.f, pcx = 0.f;
    int W;
    Scene(float d_, float r_, int W_) : d(d_), rad(r_), W(W_) { k = 2.f / (float)W; wall = d + 4.f; }   // (tan 45 deg)
    Surf hit(float u) const   // u: the unjittered render-px coordinate of the ray
    {
        const float t = (u - 0.5f * (float)W) * k;
        const float a = t * t + 1.f, b = -2.f * (t * cx + d), c = cx * cx + d * d - rad * rad;
        const float disc = b * b - 4.f * a * c;
        if (disc >= 0.f)
        {
            const float z = (-b - std::sqrt(disc)) / (2.f * a), x = t * z;
            const float nx = (x - cx) / rad, nz = (z - d) / rad;
            return {z, nx, 1, std::max(0.55f + 0.35f * nx - 0.25f * (1.f + nz), 0.02f)};
        }
        return {wall, 0.f, 0, 0.30f};
    }
};
struct Texel { float v = 0.f, len = 0.f, z = -1.f, nx = 0.f; };
static float cosN(float a, float b)
{
    return a * b + std::sqrt(std::max(1.f - a * a, 0.f)) * std::sqrt(std::max(1.f - b * b, 0.f));
}

struct Swim { float body, edge; };
struct MovEntry { float v = 0.f, len = 0.f, z = -1.f; int stamp = -1; };
// stable: the fixed lattice; grid: the reprojections invert the previous lattice under the previous raster jitter
// mover: RenderVKGIMoverSamples (clean_gi_movers.glsl) -- the body's pixels (skinned: always movers) take their own
// sample, their own history along their motion (3-tap depth match, <= 31 frames: GI_MOV_HIST) and the combine's mover read (the
// camera reprojection, a 3-tap depth search, the depth-stopped 3-tap mean, centre x 2)
static Swim runSwim1(float d, float rad, float scale, bool stable, bool grid, float sway, uint32_t seed, bool mover = false)
{
    const int frames = 360, warm = 100;   // (260 measured frames)
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> U(0.f, 1.f);
    const int W = 1280;
    Scene sc(d, rad, W);
    const int D = std::max(1, (int)((float)W * scale));
    const float r = (float)W / (float)D;
    std::vector<Texel> hist;
    std::vector<MovEntry> mh(W);   // (the mover entries: last frame's)
    std::vector<float> ema(W, 0.f);
    std::vector<char> haveEma(W, 0);
    std::vector<int> emaId(W, -1);   // (the surface the accumulation is on: a pixel that changes surface restarts)
    float prevJ = 0.f;
    double sb = 0.0, se = 0.0;
    long nb = 0, ne = 0;
    for (int t = 0; t < frames; ++t)
    {
        sc.pcx = sc.cx;
        sc.cx = sway * std::sin((float)t * 0.05f + 2.1f * (float)(seed % 3u));
        const float J = halton(t % 128 + 1, 2) - 0.5f;
        // the texels' sample phase: this frame's / last frame's (giSampleJitter; prm7.z)
        const float Jg = (D < W && !stable) ? J : 0.f, pJg = (D < W && !stable) ? prevJ : 0.f;
        std::vector<Surf> ras(W);
        for (int i = 0; i < W; ++i) ras[i] = sc.hit((float)i + 0.5f - J);
        auto prevPos = [&](const Surf& s, float u) { return s.id == 1 ? u - (sc.cx - sc.pcx) / (sc.k * s.z) : u; };
        // ---- the trace + the GI denoiser's temporal pass ----
        std::vector<Texel> cur(D);
        for (int m = 0; m < D; ++m)
        {
            const int p = std::min(std::max((int)floor(((float)m + 0.5f + Jg) * r), 0), W - 1);
            const Surf s = ras[p];
            const float raw = s.E * (0.3f + 1.4f * U(rng));
            const float u = prevPos(s, (float)p + 0.5f - J);
            const float hc = grid ? gridHc(u + prevJ, r, pJg) : u / r - 0.5f;
            float val = raw, len = 0.f;
            if (!hist.empty())
            {
                const float b = floor(hc), f = hc - b;
                float acc = 0.f, wsum = 0.f, lsum = 0.f;
                for (int o = 0; o < 2; ++o)
                {
                    const int q = (int)b + o;
                    const float bw = o ? f : 1.f - f;
                    if (q < 0 || q >= D || bw <= 1e-4f) continue;
                    const Texel& h = hist[q];
                    if (std::fabs(h.z - s.z) > std::max(0.04f * s.z, 0.03f) || cosN(h.nx, s.nx) < 0.85f) continue;
                    acc += bw * h.v; lsum += bw * h.len; wsum += bw;
                }
                if (wsum < 1e-3f)
                {
                    const int c = (int)floor(hc + 0.5f);
                    float best = 1.f;
                    int bq = -1;
                    for (int q = c - 1; q <= c + 1; ++q)
                    {
                        if (q < 0 || q >= D || cosN(hist[q].nx, s.nx) < 0.8f) continue;
                        const float e = std::fabs(hist[q].z - s.z) / std::max(0.04f * s.z, 0.03f);
                        if (e < best) { best = e; bq = q; }
                    }
                    if (bq >= 0) { acc = hist[bq].v; lsum = hist[bq].len; wsum = 1.f; }
                }
                if (wsum >= 1e-3f)
                {
                    const float h = acc / wsum;
                    len = std::min(lsum / wsum, 31.f);
                    val = h + (raw - h) / (len + 1.f);
                }
            }
            cur[m] = {val, len + 1.f, s.z, s.nx};
        }
        // ---- (mover) the body's pixels: TRACE (their own sample + history along their motion) ----
        std::vector<MovEntry> mc(W);
        if (mover)
            for (int i = 0; i < W; ++i)
            {
                const Surf s = ras[i];
                if (s.id != 1) continue;
                const float raw = s.E * (0.3f + 1.4f * U(rng));
                const int c = (int)floor(prevPos(s, (float)i + 0.5f - J) + prevJ);
                float best = 1.f, hv = 0.f, hl = 0.f;
                for (int q = c - 1; q <= c + 1; ++q)
                {
                    if (q < 0 || q >= W || mh[q].stamp != t - 1 || !(mh[q].len > 0.f)) continue;
                    const float e = std::fabs(mh[q].z - s.z) / std::max(0.03f * s.z, 0.03f);
                    if (e < best) { best = e; hv = mh[q].v; hl = mh[q].len; }
                }
                const float len = hl > 0.f ? std::min(hl + 1.f, 31.f) : 1.f;
                mc[i] = {hl > 0.f ? hv + (raw - hv) / len : raw, len, s.z, t};
            }
        // ---- the combine (GI_F_LAG): last frame's GI (hist, before this frame's) at this pixel ----
        if (!hist.empty())
            for (int i = 0; i < W; ++i)
            {
                const Surf s = ras[i];
                bool nearBody = false;
                for (int k = -6; k <= 6 && !nearBody; ++k) nearBody = i + k >= 0 && i + k < W && ras[i + k].id == 1;
                if (!nearBody) continue;
                const float u = prevPos(s, (float)i + 0.5f - J);
                const float f0 = grid ? gridHc(u + prevJ, r, pJg) : u / r - 0.5f;
                const float b = floor(f0), f = f0 - b;
                float acc = 0.f, wsum = 0.f;
                for (int o = 0; o < 2; ++o)
                {
                    const int q = (int)b + o;
                    if (q < 0 || q >= D) continue;
                    const Texel& h = hist[q];
                    if (std::fabs(h.z - s.z) >= std::max(0.03f * s.z, 0.05f)) continue;
                    const float w = (o ? f : 1.f - f) * std::max(std::pow(std::fabs(cosN(h.nx, s.nx)), 8.f), 1e-3f);
                    acc += w * h.v; wsum += w;
                }
                float v = wsum > 1e-6f ? acc / wsum : 0.45f;   // (the radiance cache's cell)
                if (mover && s.id == 1)
                {
                    // the combine's mover read (giMoverFetch, GI_F_LAG: last frame's entries at the camera reprojection)
                    const int c = (int)floor((float)i + 0.5f - J + prevJ);
                    float best = 1.f, bz = 0.f;
                    int bc = -1;
                    for (int q = c - 1; q <= c + 1; ++q)
                    {
                        if (q < 0 || q >= W || mh[q].stamp != t - 1 || !(mh[q].len > 0.f)) continue;
                        const float e = std::fabs(mh[q].z - s.z) / std::max(0.03f * s.z, 0.03f);
                        if (e < best) { best = e; bc = q; bz = mh[q].z; }
                    }
                    if (bc >= 0)
                    {
                        float sum = 0.f, ws = 0.f;
                        for (int q = bc - 1; q <= bc + 1; ++q)
                        {
                            if (q < 0 || q >= W || mh[q].stamp != t - 1 || std::fabs(mh[q].z - bz) > std::max(0.01f * bz, 0.02f))
                                continue;
                            const float w = q == bc ? 2.f : 1.f;
                            sum += w * mh[q].v; ws += w;
                        }
                        if (ws > 0.f) v = sum / ws;
                    }
                }
                if (t < warm - 30) continue;
                if (!haveEma[i] || emaId[i] != s.id) { ema[i] = v; haveEma[i] = 1; emaId[i] = s.id; continue; }
                const float nv = ema[i] + 0.1f * (v - ema[i]);
                if (t >= warm)
                {
                    const float e = std::fabs(nv - ema[i]) / std::max(s.E, 0.05f);
                    if (s.id == 1) { sb += e; ++nb; } else { se += e; ++ne; }
                }
                ema[i] = nv;
            }
        hist = cur;
        mh.swap(mc);
        prevJ = J;
    }
    return {nb ? (float)(100.0 * sb / (double)nb) : 0.f, ne ? (float)(100.0 * se / (double)ne) : 0.f};
}

// (three noise seeds, the swaying cases at three phases of the sway: the mean)
static Swim runSwim(float d, float rad, float scale, bool stable, bool grid, float sway, bool mover = false)
{
    Swim a{0.f, 0.f};
    for (uint32_t k = 0; k < 3u; ++k)
    {
        const Swim s = runSwim1(d, rad, scale, stable, grid, sway, 1234u + 7919u * k, mover);
        a.body += s.body / 3.f; a.edge += s.edge / 3.f;
    }
    return a;
}

int main()
{
    struct Case { float d, rad, scale, sway; };
    const Case cases[] = {
        {10.f, 0.06f, 0.5f, 0.f},     // a still limb at 10 m, half extent
        {10.f, 0.06f, 0.5f, 0.02f},   // ... swaying
        {20.f, 0.15f, 0.5f, 0.f},     // a still torso at 20 m, half extent
        {20.f, 0.15f, 0.5f, 0.02f},   // ... swaying
        {10.f, 0.06f, 0.25f, 0.f},    // a still limb at 10 m, quarter extent
        {20.f, 0.06f, 0.25f, 0.f},    // ... at 20 m (one quarter texel across)
        {10.f, 0.06f, 0.25f, 0.02f},  // a swaying limb at 10 m, quarter extent (reported)
        {20.f, 0.15f, 0.25f, 0.02f},  // a swaying torso at 20 m, quarter extent (reported)
    };
    for (const Case& c : cases)
    {
        const Swim full = runSwim(c.d, c.rad, 1.f, false, false, c.sway);
        const Swim fullNew = runSwim(c.d, c.rad, 1.f, true, false, c.sway);
        const Swim oldS = runSwim(c.d, c.rad, c.scale, false, false, c.sway);
        const Swim gridOnly = runSwim(c.d, c.rad, c.scale, false, true, c.sway);
        const Swim newS = runSwim(c.d, c.rad, c.scale, true, false, c.sway);
        const Swim movS = runSwim(c.d, c.rad, c.scale, false, false, c.sway, true);
        std::printf("d %4.1f m R %.2f m scale %.2f sway %.2f m | body: full %.2f old %.2f stable %.2f mover %.2f (grid-only %.2f) "
                    "| wall edge: full %.2f old %.2f stable %.2f mover %.2f (%%)\n", c.d, c.rad, c.scale, c.sway,
                    full.body, oldS.body, newS.body, movS.body, gridOnly.body, full.edge, oldS.edge, newS.edge, movS.edge);
        // (MOVER, RenderVKGIMoverSamples) the body at full-resolution quality in every case, the wall untouched
        CHECK(movS.body <= 1.5f * full.body + 0.15f, "MOVER: the mover samples' body swim %.2f > 1.5 x full %.2f + 0.15",
              movS.body, full.body);
        CHECK(movS.edge <= oldS.edge + 0.02f, "MOVER: the wall around the body swims more (%.3f -> %.3f)",
              oldS.edge, movS.edge);
        const bool quarter = c.scale < 0.5f;
        if (quarter)
            CHECK(oldS.body >= 1.5f * full.body, "SHAPE: the old rules do not swim here (%.2f vs full %.2f): the model lost the report",
                  oldS.body, full.body);
        if (quarter && (c.sway == 0.f || c.rad < 0.1f))
            CHECK(newS.body <= 0.6f * oldS.body, "QUARTER: the stable lattice cuts the limb's swim by < 40 %% (%.2f -> %.2f)",
                  oldS.body, newS.body);
        if (!quarter)
            CHECK(newS.body <= 2.f * full.body + 0.2f, "HALF: the stable lattice's body swim %.2f > 2 x full %.2f + 0.2",
                  newS.body, full.body);
        CHECK(newS.edge <= 1.6f * oldS.edge + 0.02f, "EDGE: the wall around the body swims more (%.2f -> %.2f)", oldS.edge, newS.edge);
        CHECK(fullNew.body == full.body && fullNew.edge == full.edge, "FULL: full resolution changed (%.3f -> %.3f)",
              full.body, fullNew.body);
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("gi stable samples: the quarter-extent swim reproduced; stable lattice cuts it on limbs; mover samples reach full-resolution quality everywhere PASS\n");
    return 0;
}
