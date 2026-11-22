// Executes the production shader's scalar solve (in exposure_tail.inc), plus
// the production host gate. No Vulkan device, copied policy, or viewer needed.
#include "vkgfx/slinteriorexposure.h"
#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <limits>

using std::exp;
using std::exp2;
using std::isinf;
using std::isnan;
using std::log;
using std::log2;
float glslMin(float a, float b) { return std::min(a, b); }
float glslMax(float a, float b) { return std::max(a, b); }
float clamp(float x, float a, float b) { return glslMin(glslMax(x, a), b); }
float mix(float a, float b, float t) { return a * (1.f - t) + b * t; }
float smoothstep(float a, float b, float x)
{
    const float t = clamp((x - a) / (b - a), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
struct Four { float x, y, z, w; };
struct Push { Four params, params2, params3, params4; };

float solve(float average, float previous, const Push& pc)
{
    float s_lum[256] = {};
    s_lum[0] = average * 256.f;
#include "exposure_tail.inc"
}

void close(float actual, float expected, float tolerance = 2e-5f)
{
    if (!(std::isfinite(actual) && std::abs(actual - expected) <= tolerance))
    {
        std::fprintf(stderr, "exposure actual %.9g expected %.9g\n", actual, expected);
        std::abort();
    }
}

int main()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    Push p{{0.5f, 0.5f, 2.f, 0.12f}, {3.3f, 1.f/60.f, 1.f, 0.f}, {}, {}};
    // Synthetic gamma=1 range, known exact legacy targets. Low-key change is
    // NOT an alternative key/meter: off executes the original solve exactly.
    close(solve(0.05f, 1.f, p), 1.985f);
    p.params2.w = 1.f;
    close(solve(0.f, 1.f, p), 1.f);
    close(solve(0.05f, 1.f, p), 1.f);
    close(solve(0.125f, 1.f, p), 1.f);
    close(solve(0.1875f, 1.f, p), 1.39453125f);
    close(solve(0.25f, 1.f, p), 1.625f);
    close(solve(0.5f, 1.f, p), 0.5f);
    close(solve(8.f, 1.f, p), 0.5f); // emissive-dominated mean
    // Sampled small bright emitter amid dark surfaces: retains its influence.
    const float emitterMean = 0.05f * 0.99f + 10.f * 0.01f;
    const float dark = solve(0.05f, 1.f, p), emitter = solve(emitterMean, 1.f, p);
    assert(emitter > dark && emitter < 2.f);
    // Dense sweep: never increases any target, preserves downward adaptation
    // and daylight means >= half the coefficient, for representative EEP gammas.
    for (float gamma : {0.25f, 0.5f, 1.f, 4.f})
    {
        const float ceiling = 2.f * std::sqrt(gamma);
        p.params.y = 1.f / ceiling; p.params.z = ceiling;
        for (int i = 0; i <= 4000; ++i)
        {
            const float mean = i / 4000.f;
            p.params2.w = 0.f; const float old = solve(mean, 1.f, p);
            p.params2.w = 1.f; const float bounded = solve(mean, 1.f, p);
            assert(bounded >= p.params.y - 1e-6f && bounded <= old + 1e-6f);
            if (mean <= 0.125f) assert(bounded <= 1.f + 1e-6f);
            if (mean >= 0.25f || old <= 1.f) close(bounded, old);
        }
    }
    p.params.y = 0.5f; p.params.z = 2.f;
    // Both blend joins are continuous; show the deliberate middle-band hump.
    for (float join : {0.125f, 0.25f})
        assert(std::abs(solve(join-1e-6f, 1.f, p) - solve(join+1e-6f, 1.f, p)) < 1e-4f);
    assert(solve(0.23f, 1.f, p) > solve(0.25f, 1.f, p));
    // The host gate bypasses the actual setting selecting EEP ranges, even if
    // the range happens to equal the default; no guessing from numeric endpoints.
    close(slInteriorExposureWeight(false, false, false, false, 1.f), 0.f);
    close(slInteriorExposureWeight(true, true, false, false, 1.f), 0.f);
    close(slInteriorExposureWeight(true, false, true, false, 1.f), 0.f);
    close(slInteriorExposureWeight(true, false, false, true, 1.f), 0.f);
    for (float sun : {-1.f, -0.01f, 0.f, nan, inf})
        close(slInteriorExposureWeight(true, false, false, false, sun), 0.f);
    close(slInteriorExposureWeight(true, false, false, false, 0.034878235f), 0.5f);
    close(slInteriorExposureWeight(true, false, false, false, 0.06975647f), 1.f);
    float last = 0.f;
    for (int i = 0; i <= 1000; ++i)
    {
        const float weight = slInteriorExposureWeight(true, false, false, false, i*0.0001f);
        assert(weight >= last && weight <= 1.f && weight-last < 0.003f);
        p.params2.w = weight;
        close(solve(0.05f, 1.f, p), 1.985f - weight*0.985f);
        last = weight;
    }
    // Across the horizon, including physical-night eligibility changing, the
    // additional bound contributes zero continuously (the existing night
    // lighting/eye policy still controls the image on the other side).
    p.params2.w = slInteriorExposureWeight(true, false, false, false, 1e-6f);
    const float horizonDay = solve(0.05f, 1.f, p);
    p.params2.w = slInteriorExposureWeight(true, false, false, true, -1e-6f);
    close(solve(0.05f, 1.f, p), horizonDay);
    // Authored range with a minimum ABOVE unity must never acquire the unity
    // cap; toggling the A/B under that setting yields the original solve.
    p.params.y = 1.3f; p.params.z = 2.7f;
    p.params2.w = slInteriorExposureWeight(true, true, false, false, 1.f);
    close(solve(0.05f, 1.f, p), 2.686f);
    p.params.y = 0.5f; p.params.z = 2.f;
    // Physical night shader guard is independent of host gate, including a
    // future host error: its EV lift and interpolation remain identical.
    p.params4 = {0.75f, 4.f, 0.75f, 4.f};
    for (float mean : {0.f, 0.0001f, 0.01f, 0.125f, 0.5f})
    {
        p.params2.w = 0.f; const float old = solve(mean, 1.f, p);
        p.params2.w = 1.f; close(solve(mean, 1.f, p), old);
    }
    p.params4 = {};
    // Toggle ON keeps history, reducing it at the saved speed; 3.3 s leaves
    // exactly 12% of the starting error. OFF similarly restores original goal.
    p.params2 = {3.3f, 1.f/60.f, 0.f, 1.f};
    float history = 1.985f;
    for (int i = 0; i < 198; ++i) history = solve(0.05f, history, p);
    close(history, 1.f + 0.985f*0.12f);
    std::printf("dark toggle ON after 3.3 s: %.6f (target 1); ", history);
    for (int i = 0; i < 396; ++i) history = solve(0.05f, history, p);
    close(history, 1.f + 0.985f*0.001728f);
    p.params2.w = 0.f;
    const float start = history;
    for (int i = 0; i < 198; ++i) history = solve(0.05f, history, p);
    close(history, 1.985f+(start-1.985f)*0.12f);
    std::printf("OFF after 3.3 s: %.6f (target 1.985)\n", history);
    p.params2.w = 1.f;
    for (float bad : {nan, inf, 0.f, -1.f}) close(solve(0.05f, bad, p), 1.f);
    p.params2.y = nan; close(solve(0.05f, 1.8f, p), 1.8f);
    p.params2.y = 0.f; close(solve(0.05f, 1.8f, p), 1.8f);
    // Normal manual exposure is multiplied downstream, never forced to 1.
    p.params2.z = 1.f;
    close(2.f * solve(0.05f, 1.f, p), 2.f);
    std::puts("production exposure solve: dark/day/emissive, range, dawn/night, joins, live history, invalid history PASS");
}
