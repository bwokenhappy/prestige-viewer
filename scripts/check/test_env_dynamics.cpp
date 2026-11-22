// Fixtures for how bright an environment's light may drive the frame (indra/newview/slenvdynamics.h),
// run by scripts/check/checks/env_dynamics.py (compiled with the host C++ compiler, no viewer build).
//   DEFAULT    Second Life's default sky (LLSettingsSky::defaults() + the legacy haze defaults) at any
//              sun elevation: sunlit white below 1, so an explicit-zero ambiance default sky keeps
//              LL's clamp exactly and the bloom threshold is untouched.
//   CREATOR    a creator EEP with explicit zero probe ambiance and an orange sun at the stored
//              maximum (3): sunlit white ~11, the HDR path exactly, the bloom threshold at it.
//   BAND       the clamp weight: 1 at or below 1, 0 at or above 2, monotonic, quantised.
//   DYNAMICS   the environment dynamics' bloom weight / HDR headroom ends and clamping.
#include "slenvdynamics.h"

#include <cmath>
#include <cstdio>

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

// The packed block the frame builder writes (llappviewer.cpp; clean_atmos_common.glsl layout).
struct Sky
{
    float light[3], blue_density[3], ambient[3];
    float haze_density, density_mul, max_y, cloud_shadow, elevation_deg;
    bool classic;
};

static void pack(const Sky& s, float a[64])
{
    for (int i = 0; i < 64; ++i) a[i] = 0.f;
    for (int c = 0; c < 3; ++c)
    {
        a[1 * 4 + c] = s.blue_density[c];
        a[2 * 4 + c] = s.light[c];
        a[3 * 4 + c] = s.ambient[c];
    }
    a[1 * 4 + 3] = s.haze_density;
    a[2 * 4 + 3] = s.cloud_shadow;
    a[3 * 4 + 3] = s.max_y;
    a[4 * 4 + 2] = std::sin(s.elevation_deg * 3.14159265f / 180.f);
    a[4 * 4 + 3] = s.density_mul;
    a[8 * 4 + 3] = s.classic ? 1.f : 0.f;
    a[12 * 4 + 2] = 1.f;   // RenderHDRSkySunlightScale (LL and ours: 1.0)
    a[12 * 4 + 3] = 1.f;   // RenderSkyAmbientScale
}

static Sky defaultSky(float elev)
{
    return Sky{{0.7342f, 0.7815f, 0.8999f}, {0.2447f, 0.4487f, 0.7599f}, {0.25f, 0.25f, 0.25f},
               0.7f, 0.0001f, 1605.f, 0.2699f, elev, false};
}

// The editor's values x its display scales (llpaneleditsky.cpp: sun / ambient x3, blue x2, density
// multiplier x0.001): sun white at the maximum, blue density (0, .5, 1), haze density .41, density
// multiplier .42, max altitude 700, ambient (.33, .22, .29), sun elevation 28.93.
static Sky creatorSky(float sun)
{
    return Sky{{sun, sun, sun}, {0.f, 1.f, 2.f}, {0.99f, 0.66f, 0.87f}, 0.41f, 0.00042f, 700.f, 0.f, 28.93f, false};
}

static void testDefault()
{
    const float elevs[] = {90.f, 60.f, 30.f, 10.f, 3.f};
    for (float e : elevs)
    {
        float a[64];
        pack(defaultSky(e), a);
        const float w = slenv::sunlitWhiteFromPacked(a);
        CHECK(w > 0.f && w < 1.f, "default sky at %.0f deg: sunlit white %.3f should be in (0, 1)", e, w);
        CHECK(slenv::explicitZeroClampWeight(w) == 1.f, "default sky at %.0f deg: must keep LL's clamp exactly", e);
        float t, k;
        slenv::bloomThreshold(2.5f, 0.5f, w, t, k);
        CHECK(t == 2.5f && k == 0.5f, "default sky at %.0f deg: bloom threshold moved (%.3f / %.3f)", e, t, k);
    }
}

static void testCreator()
{
    float a[64];
    pack(creatorSky(3.f), a);
    const float w = slenv::sunlitWhiteFromPacked(a);
    // sunlit (linear) = srgb_to_linear(3 x exp(-light_atten / sin(28.93))) = (11.08, 2.68, 0.67)
    CHECK(w > 10.5f && w < 12.f, "creator sky (sun 3): sunlit white %.3f, expected ~11.1", w);
    CHECK(slenv::explicitZeroClampWeight(w) == 0.f, "creator sky (sun 3): must take the HDR path exactly");
    float t, k;
    slenv::bloomThreshold(2.5f, 0.5f, w, t, k);
    CHECK(t == w && std::fabs(k - 0.5f * w / 2.5f) < 1e-5f, "creator sky: bloom threshold %.3f knee %.3f", t, k);
    // A sunlit yellow PBR face (albedo sRGB .85 / .65 / .25, N.L .8): red reaches ~5.7 before
    // the clamp -- below the raised threshold, so it extracts nothing.
    const float red = 0.8f * (slenv::srgbToLinear(0.85f) * 0.96f / 3.14159265f) * 3.f * 11.08f;
    CHECK(red > 4.f && red < t, "creator sky: sunlit yellow red %.3f should sit below the threshold %.3f", red, t);
    pack(creatorSky(0.8f), a);
    const float w1 = slenv::sunlitWhiteFromPacked(a);
    CHECK(w1 < 1.f && slenv::explicitZeroClampWeight(w1) == 1.f,
          "the same sky with a sun of 0.8: sunlit white %.3f must keep LL's clamp", w1);
    // classic recombination stays sRGB-space based (rows 2 / 3 unlinearised)
    Sky c = creatorSky(1.f);
    c.classic = true;
    pack(c, a);
    const float wc = slenv::sunlitWhiteFromPacked(a);
    CHECK(wc > 0.f && std::isfinite(wc), "classic sunlit white %.3f", wc);
}

static void testBand()
{
    CHECK(slenv::explicitZeroClampWeight(0.f) == 1.f, "0 -> clamp");
    CHECK(slenv::explicitZeroClampWeight(1.f) == 1.f, "1 -> clamp");
    CHECK(slenv::explicitZeroClampWeight(2.f) == 0.f, "2 -> HDR path");
    CHECK(slenv::explicitZeroClampWeight(50.f) == 0.f, "50 -> HDR path");
    CHECK(slenv::explicitZeroClampWeight(NAN) == 1.f, "NaN -> clamp (as LL)");
    float prev = 1.f;
    for (int i = 0; i <= 100; ++i)
    {
        const float x = 0.5f + 2.f * (float)i / 100.f;
        const float w = slenv::explicitZeroClampWeight(x);
        CHECK(w <= prev + 1e-6f, "the weight must not rise with the light (%.3f at %.3f)", w, x);
        CHECK(std::fabs(w * slenv::kWeightSteps - std::round(w * slenv::kWeightSteps)) < 1e-4f, "not quantised: %.5f", w);
        prev = w;
    }
}

static void testDynamics()
{
    CHECK(slenv::bloomLuminanceWeight(0.f) == 0.f, "dynamics 0: no luminance bloom (LL's glow only)");
    CHECK(slenv::bloomLuminanceWeight(1.f) == 1.f, "dynamics 1: full luminance bloom");
    CHECK(slenv::bloomLuminanceWeight(0.5f) == 0.5f, "dynamics 0.5: half");
    CHECK(slenv::bloomLuminanceWeight(-3.f) == 0.f && slenv::bloomLuminanceWeight(7.f) == 1.f, "clamped");
    CHECK(slenv::bloomLuminanceWeight(NAN) == 1.f, "NaN: the full look");
    CHECK(slenv::headroomRatio(2.857f, 0.f) == 1.f, "dynamics 0: no headroom past paper white");
    CHECK(std::fabs(slenv::headroomRatio(2.857f, 1.f) - 2.857f) < 1e-6f, "dynamics 1: the display's headroom");
    CHECK(slenv::headroomRatio(0.5f, 1.f) == 1.f, "never below paper white");
}

int main()
{
    testDefault();
    testCreator();
    testBand();
    testDynamics();
    if (g_fail) std::printf("%d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}
