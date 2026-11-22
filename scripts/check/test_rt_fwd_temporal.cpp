// Fixture for THE FORWARD RT REFLECTIONS' RESOLVE (indra/vkclean/shaders/clean_rt_fwd_temporal.h, included as-is;
// scripts/check/checks/rt_fwd_temporal.py). Pass 14 traces one mirror ray per 2 x 2 block through the jittered camera;
// on a normal-mapped layer (the pool's "water" prim) each frame's ray lands on another normal and reflects another
// part of the room. Before the resolve those rays went to the forward FS raw: grain that changes every frame.
// Here an 8 x 8 tile of such texels (one workgroup's), each frame's ray drawn from the reflection's distribution at
// that texel, resolved the way pass 14 does it (the same-layer 3 x 3 mean / deviation from the tile, the texel's own
// history, rftResolve):
//   STEADY    a static camera over a rippled layer reflecting a room with a bright window (a two-valued reflection +
//             a continuous part): after 40 frames the frame-to-frame RMS of the resolved texel <= RFT_MAX_RATIO x the
//             raw rays' (the old output), and its mean within 3 % of the raw mean (no darkening by the clamp).
//   CHANGE    the reflected content changes (a light switched on in the reflection): within 4 frames the resolved
//             texels are within 10 % of the new mean (the anti-lag; no ghost of the old reflection).
//   DISOCC    no history (a disocclusion, a camera cut): this frame's 3 x 3 mean with its own ray, no history used.
//   STILL     a noise-free reflection holds its value exactly.
//   CHANGE OFF  the reflected light switches OFF (a bright room with a window -> the dim room): within 10 % in 4 frames.
//   COHERENT  (the window flicker, 2026-11-09) region-wide noise -- every ray of the tile agrees, the value moves from
//             frame to frame (the hits' radiance-cache ambient; pass 14's layer GI): the resolved frame-to-frame RMS
//             <= RFT_MAX_RATIO x the raw, its mean within 3 %. Before: 60 % (5 % noise) to 95 % (20 %) came through.
//   COHERENT CHANGE  a real step (x 3, x 1.6, x 0.5) under 10 % region-wide noise: within 15 % after 4 frames.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <algorithm>
#include <random>
#include <vector>

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
using std::exp;
using std::log;
using std::sqrt;
#include "clean_rt_fwd_temporal.h"

#ifndef RFT_MAX_RATIO
#define RFT_MAX_RATIO 0.4
#endif
static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// One frame of pass 14's resolve over the tile: raw rays in, resolved out (hist: last frame's resolved, all valid;
// g_hv / g_len: their temporal variance and length).
static std::vector<vec3> g_hv(64);
static std::vector<float> g_len(64, 0.f);
static void resolveTile(const std::vector<vec3>& raw, std::vector<vec3>& hist, bool histOk)
{
    std::vector<vec3> out(64), hvo(64);
    std::vector<float> leno(64);
    for (int y = 0; y < 8; ++y)
        for (int x = 0; x < 8; ++x)
        {
            vec3 m1, m2;
            float n = 0.f;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int qx = x + dx, qy = y + dy;
                    if (qx < 0 || qy < 0 || qx > 7 || qy > 7) continue;
                    const vec3 s = raw[qy * 8 + qx];
                    m1 = m1 + s; m2 = m2 + s * s; n += 1.f;
                }
            const vec3 mu = m1 / n;
            const vec3 sg = sqrt3(m2 / n - mu * mu);
            float wHist;
            out[y * 8 + x] = max(rftResolve(raw[y * 8 + x], mu, sg, n, hist[y * 8 + x], g_hv[y * 8 + x], g_len[y * 8 + x],
                                            histOk, hvo[y * 8 + x], leno[y * 8 + x], wHist), vec3(0.f));
        }
    hist = out;
    g_hv = hvo;
    g_len = leno;
}
// The reflection a jittered block ray sees on a rippled layer: the room (0.2 .. 0.35) or, w.p. pw, the bright window.
static vec3 drawRefl(std::mt19937& rng, float room, float window, float pw)
{
    std::uniform_real_distribution<float> U(0.f, 1.f);
    if (U(rng) < pw) return vec3(window, window * 0.95f, window * 0.9f);
    const float r = room * (0.8f + 0.6f * U(rng));
    return vec3(r, r * 1.05f, r * 1.1f);
}

int main()
{
    std::mt19937 rng(4242u);
    // STEADY: a large bright window (30 % of the rays) and a small lamp's glint (5 %, 5x brighter)
    const float kPw[2] = {0.3f, 0.05f}, kWin[2] = {2.0f, 5.0f};
    for (int sc = 0; sc < 2; ++sc)
    {
        const float pw = kPw[sc], win = kWin[sc];
        std::vector<vec3> hist(64), raw(64);
        std::fill(g_hv.begin(), g_hv.end(), vec3(0.f));
        std::fill(g_len.begin(), g_len.end(), 0.f);
        double eRaw = 0.0, eRes = 0.0, mRaw = 0.0, mRes = 0.0;
        int cnt = 0;
        const int frames = 400;
        vec3 prevRaw[64], prevRes[64];
        for (int f = 0; f < frames; ++f)
        {
            for (int i = 0; i < 64; ++i) raw[i] = drawRefl(rng, 0.25f, win, pw);
            resolveTile(raw, hist, f > 0);
            if (f >= 40)
                for (int i = 0; i < 64; ++i)
                {
                    const double a = raw[i].y - prevRaw[i].y, b = hist[i].y - prevRes[i].y;
                    eRaw += a * a; eRes += b * b;
                    mRaw += raw[i].y; mRes += hist[i].y;
                    ++cnt;
                }
            for (int i = 0; i < 64; ++i) { prevRaw[i] = raw[i]; prevRes[i] = hist[i]; }
        }
        const double rr = std::sqrt(eRaw / cnt), rs = std::sqrt(eRes / cnt);
        mRaw /= cnt; mRes /= cnt;
        std::printf("STEADY (window %.0f%% of the rays, %.0fx): frame-to-frame RMS raw %.4f -> resolved %.4f (x %.2f); "
                    "mean %.4f -> %.4f\n", 100.0 * pw, win / 0.25, rr, rs, rs / rr, mRaw, mRes);
        CHECK(rs <= RFT_MAX_RATIO * rr, "STEADY: the resolved reflection still changes from frame to frame: RMS %.4f vs "
              "the raw rays' %.4f (x %.2f, limit %.2f)", rs, rr, rs / rr, (double)RFT_MAX_RATIO);
        CHECK(std::fabs(mRes - mRaw) <= 0.03 * mRaw, "STEADY: the resolve biases the reflection: mean %.4f vs %.4f", mRes, mRaw);
    }
    // CHANGE: the reflected content switches; the resolve follows within 4 frames
    {
        std::vector<vec3> hist(64), raw(64);
        for (int f = 0; f < 60; ++f)
        {
            for (int i = 0; i < 64; ++i) raw[i] = drawRefl(rng, 0.25f, 2.0f, 0.0f);
            resolveTile(raw, hist, f > 0);
        }
        double worst = 0.0;
        for (int f = 0; f < 4; ++f)
        {
            for (int i = 0; i < 64; ++i) raw[i] = drawRefl(rng, 1.5f, 2.0f, 0.0f);
            resolveTile(raw, hist, true);
        }
        double m = 0.0;
        for (int i = 0; i < 64; ++i) m += hist[i].y;
        m /= 64.0;
        const double want = 1.5 * 1.05 * 1.1;   // (the new reflection's mean)
        worst = std::fabs(m / want - 1.0);
        std::printf("CHANGE: after 4 frames the resolved tile is %.1f %% off the new reflection\n", 100.0 * worst);
        CHECK(worst <= 0.10, "CHANGE: the old reflection lingers (%.1f %% off after 4 frames): a ghost", 100.0 * worst);
    }
    // CHANGE OFF: the reflected light switches OFF (bright room with a window -> the dim room): no ghost of the light
    // after 4 frames (the opaque resolve's 2026-11-09 finding: an absolute spread let a switched-off light linger)
    {
        std::vector<vec3> hist(64), raw(64);
        for (int f = 0; f < 60; ++f)
        {
            for (int i = 0; i < 64; ++i) raw[i] = drawRefl(rng, 1.5f, 6.0f, 0.3f);
            resolveTile(raw, hist, f > 0);
        }
        for (int f = 0; f < 4; ++f)
        {
            for (int i = 0; i < 64; ++i) raw[i] = drawRefl(rng, 0.25f, 2.0f, 0.0f);
            resolveTile(raw, hist, true);
        }
        double m = 0.0;
        for (int i = 0; i < 64; ++i) m += hist[i].y;
        m /= 64.0;
        const double want = 0.25 * 1.05 * 1.1;
        const double off = std::fabs(m / want - 1.0);
        std::printf("CHANGE OFF: after 4 frames the resolved tile is %.1f %% off the dimmed reflection\n", 100.0 * off);
        CHECK(off <= 0.10, "CHANGE OFF: the switched-off light lingers (%.1f %% off after 4 frames): a ghost", 100.0 * off);
    }
    // DISOCC: no history -- this frame's spatial step only
    {
        std::vector<vec3> hist(64, vec3(50.f)), raw(64);
        for (int i = 0; i < 64; ++i) raw[i] = drawRefl(rng, 0.25f, 2.0f, 0.3f);
        resolveTile(raw, hist, false);
        bool used = false;
        for (int i = 0; i < 64; ++i) used = used || hist[i].y > 10.f;
        CHECK(!used, "DISOCC: a texel without a valid history took the stale history");
    }
    // STILL: a noise-free reflection holds exactly
    {
        std::vector<vec3> hist(64), raw(64, vec3(0.3f, 0.4f, 0.5f));
        for (int f = 0; f < 50; ++f) resolveTile(raw, hist, f > 0);
        float d = 0.f;
        for (int i = 0; i < 64; ++i) d = max(d, length(hist[i] - raw[i]));
        CHECK(d < 1e-5f, "STILL: a steady reflection drifts by %g", d);
    }
    // COHERENT: the whole tile moves together from frame to frame (the hits' radiance-cache ambient: one cell spans the
    // tile, so all 9 rays of every 3 x 3 agree -- sigma 0). Before the fix the moment collapsed on each anti-lag reset
    // and the resolve passed 60 % (5 % noise) to 95 % (20 %) of the frame-to-frame change.
    std::normal_distribution<float> G(0.f, 1.f);
    const float kAmp[3] = {0.05f, 0.1f, 0.2f};
    for (int sc = 0; sc < 3; ++sc)
    {
        std::vector<vec3> hist(64), raw(64);
        std::fill(g_hv.begin(), g_hv.end(), vec3(0.f));
        std::fill(g_len.begin(), g_len.end(), 0.f);
        double eRaw = 0.0, eRes = 0.0, mRaw = 0.0, mRes = 0.0;
        int cnt = 0;
        float prevRaw = 0.f, prevRes = 0.f;
        for (int f = 0; f < 400; ++f)
        {
            const float v = 0.5f * max(1.f + kAmp[sc] * G(rng), 0.f);
            for (int i = 0; i < 64; ++i) raw[i] = vec3(v, v * 0.9f, v * 0.8f);
            resolveTile(raw, hist, f > 0);
            const float r = hist[27].x;   // (an inner texel; the tile is uniform -- the channel v sits in)
            if (f >= 40)
            {
                eRaw += (v - prevRaw) * (v - prevRaw);
                eRes += (r - prevRes) * (r - prevRes);
                mRaw += v; mRes += r;
                ++cnt;
            }
            prevRaw = v; prevRes = r;
        }
        const double rr = std::sqrt(eRaw / cnt), rs = std::sqrt(eRes / cnt);
        mRaw /= cnt; mRes /= cnt;
        std::printf("COHERENT (%.0f%% region-wide noise): frame-to-frame RMS raw %.4f -> resolved %.4f (x %.2f); "
                    "mean %.4f -> %.4f\n", 100.0 * kAmp[sc], rr, rs, rs / rr, mRaw, mRes);
        CHECK(rs <= RFT_MAX_RATIO * rr, "COHERENT %.0f%%: region-wide noise passes the resolve: RMS %.4f vs the raw "
              "%.4f (x %.2f, limit %.2f) -- the blob flicker on glass", 100.0 * kAmp[sc], rs, rr, rs / rr,
              (double)RFT_MAX_RATIO);
        CHECK(std::fabs(mRes - mRaw) <= 0.03 * mRaw, "COHERENT %.0f%%: the resolve biases the value: mean %.4f vs %.4f",
              100.0 * kAmp[sc], mRes, mRaw);
    }
    // COHERENT CHANGE: under 10 % region-wide noise the value really steps (x 3, x 1.6, x 0.5): followed within 4 frames
    const float kStep[3] = {3.0f, 1.6f, 0.5f};
    for (int sc = 0; sc < 3; ++sc)
    {
        std::vector<vec3> hist(64), raw(64);
        for (int f = 0; f < 64; ++f)
        {
            const float v = 0.5f * (1.f + 0.1f * G(rng));
            for (int i = 0; i < 64; ++i) raw[i] = vec3(v);
            resolveTile(raw, hist, f > 0);
        }
        float worst = 0.f;
        for (int trial = 0; trial < 1; ++trial)
        {
            for (int f = 0; f < 4; ++f)
            {
                const float v = 0.5f * kStep[sc] * (1.f + 0.1f * G(rng));
                for (int i = 0; i < 64; ++i) raw[i] = vec3(v);
                resolveTile(raw, hist, true);
            }
            worst = max(worst, std::fabs(hist[27].y / (0.5f * kStep[sc]) - 1.f));
        }
        std::printf("COHERENT CHANGE (x %.1f under 10%% noise): after 4 frames %.1f %% off the new value\n", kStep[sc],
                    100.0 * worst);
        CHECK(worst <= 0.15f, "COHERENT CHANGE x %.1f: the old value lingers (%.1f %% off after 4 frames): a ghost",
              kStep[sc], 100.0 * worst);
    }
    if (g_fail) { std::printf("%d failure(s)\n", g_fail); return 1; }
    std::printf("ok\n");
    return 0;
}
