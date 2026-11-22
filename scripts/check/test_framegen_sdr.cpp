// Tests for #1536 SDR frame generation's composite: vkdisplay::composeLayerSdr
// (indra/newview/vkgfx/vk_display_hdr.h), the C++ mirror of displayComposeUiSdr in
// indra/vkclean/shaders/clean_display_common.glsl. On an SDR frame generation chain every picture --
// the real one and each generated one -- is the world (8-bit, sRGB-encoded) with the moving labels and
// then the interface layer (premultiplied sRGB-encoded colour, coverage in alpha) over it, in that
// encoded space: the SDR swapchain's own blend, with nothing decoded or re-encoded.
// Built and run by scripts/check/checks/framegen_sdr.py.
#include "vkgfx/vk_display_hdr.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using Rgb = std::array<float, 3>;
using Rgba = std::array<float, 4>;

// A UNORM8 store and load: what an 8-bit image keeps of a value.
static int q(float v) { return (int)std::lround(std::fmin(std::fmax(v, 0.f), 1.f) * 255.f); }
static float un(int v) { return (float)v / 255.f; }
static Rgb qRgb(const Rgb& c) { return {un(q(c[0])), un(q(c[1])), un(q(c[2]))}; }
static Rgba qRgba(const Rgba& c) { return {un(q(c[0])), un(q(c[1])), un(q(c[2])), un(q(c[3]))}; }

// A premultiplied draw (colour already times its coverage) blended straight onto an 8-bit target,
// colour ONE / ONE_MINUS_SRC_ALPHA, alpha ONE / ONE_MINUS_SRC_ALPHA: the SDR UI pipelines' blend.
static Rgb overRgb(const Rgba& s, const Rgb& d)
{
    return qRgb({s[0] + d[0] * (1.f - s[3]), s[1] + d[1] * (1.f - s[3]), s[2] + d[2] * (1.f - s[3])});
}
static Rgba overRgba(const Rgba& s, const Rgba& d)
{
    return qRgba({s[0] + d[0] * (1.f - s[3]), s[1] + d[1] * (1.f - s[3]), s[2] + d[2] * (1.f - s[3]),
                  s[3] + d[3] * (1.f - s[3])});
}
static int maxStep(const Rgb& a, const Rgb& b)
{
    int m = 0;
    for (int i = 0; i < 3; ++i) m = std::max(m, std::abs(q(a[i]) - q(b[i])));
    return m;
}

int main()
{
    using vkdisplay::composeLayerSdr;

    // 1. No label and no interface (layer 0): every 8-bit world value comes back unchanged.
    for (int v = 0; v < 256; ++v)
    {
        const Rgb out = composeLayerSdr({un(v), un(255 - v), un(v / 2)}, {0.f, 0.f, 0.f, 0.f});
        CHECK(q(out[0]) == v && q(out[1]) == 255 - v && q(out[2]) == v / 2);
    }

    // 2. An opaque interface texel: the layer's colour, the world gone.
    {
        const Rgb out = composeLayerSdr({.9f, .1f, .5f}, {.2f, .4f, .6f, 1.f});
        CHECK(std::fabs(out[0] - .2f) < 1e-6f && std::fabs(out[1] - .4f) < 1e-6f && std::fabs(out[2] - .6f) < 1e-6f);
    }

    // 3. Additive (coverage 0, colour > 0): world + colour, clamped at white as UNORM is.
    {
        const Rgb out = composeLayerSdr({.5f, .9f, 0.f}, {.25f, .25f, .25f, 0.f});
        CHECK(std::fabs(out[0] - .75f) < 1e-6f && out[1] == 1.f && std::fabs(out[2] - .25f) < 1e-6f);
    }
    // (coverage outside 0..1 never inverts or amplifies the world)
    {
        const Rgb out = composeLayerSdr({.5f, .5f, .5f}, {0.f, 0.f, 0.f, 1.5f});
        CHECK(out[0] == 0.f);
        const Rgb out2 = composeLayerSdr({.5f, .5f, .5f}, {0.f, 0.f, 0.f, -1.f});
        CHECK(std::fabs(out2[0] - .5f) < 1e-6f);
    }

    // 4. Against drawing straight onto the 8-bit world (SDR without frame generation): one draw into the
    //    empty layer, then the composite, is the same picture -- exactly when the draw's colour and coverage
    //    are 8-bit values, within one step otherwise (the layer stores them in 8 bits first); two stacked
    //    translucent draws differ by at most two steps per channel (each draw's rounding lands in the layer
    //    instead of on the world: 2 is the worst case of a 300 000-case random sweep too).
    {
        int oneOnGrid = 0, oneOff = 0, two = 0;
        const float covers[] = {0.f, .05f, .2f, .37f, .5f, .7f, .93f, 1.f};
        for (int w = 0; w < 256; w += 5)
            for (float a : covers)
                for (float c : {0.f, .1f, .33f, .6f, .99f})
                {
                    const Rgb world = {un(w), un(255 - w), un((w * 7) % 256)};
                    // on the 8-bit grid: premultiplied colour and coverage as 8-bit values
                    const Rgba g = {un(q(c * a)), un(q(c * a * .5f)), un(q(c * a * .25f)), un(q(a))};
                    const Rgb direct = overRgb(g, world);
                    const Rgba layer = overRgba(g, {0.f, 0.f, 0.f, 0.f});
                    oneOnGrid = std::max(oneOnGrid, maxStep(qRgb(composeLayerSdr(world, layer)), direct));
                    // off the grid: what a shader writes
                    const Rgba s = {c * a, c * a * .5f, c * a * .25f, a};
                    oneOff = std::max(oneOff, maxStep(qRgb(composeLayerSdr(world, overRgba(s, {0.f, 0.f, 0.f, 0.f}))),
                                                      overRgb(s, world)));
                    for (float a2 : covers)
                    {
                        const Rgba s2 = {.8f * a2, .3f * a2, .55f * a2, a2};
                        const Rgb direct2 = overRgb(s2, overRgb(s, world));
                        const Rgba layer2 = overRgba(s2, overRgba(s, {0.f, 0.f, 0.f, 0.f}));
                        two = std::max(two, maxStep(qRgb(composeLayerSdr(world, layer2)), direct2));
                    }
                }
        CHECK(oneOnGrid == 0);
        CHECK(oneOff <= 1);
        CHECK(two <= 2);
        if (oneOnGrid || oneOff > 1 || two > 2)
            std::printf("steps: one draw on the grid %d, off it %d, two stacked %d\n", oneOnGrid, oneOff, two);
    }

    // 5. Labels, then the interface, in that order (a later layer covers an earlier one).
    {
        const Rgb world = {.2f, .2f, .2f};
        const Rgba label1 = {.6f, 0.f, 0.f, .6f}, label2 = {0.f, .5f, 0.f, .5f}, ui = {0.f, 0.f, .3f, .3f};
        const Rgb ordered = composeLayerSdr(composeLayerSdr(composeLayerSdr(world, label1), label2), ui);
        const Rgb swapped = composeLayerSdr(composeLayerSdr(composeLayerSdr(world, label2), label1), ui);
        const Rgb uiFirst = composeLayerSdr(composeLayerSdr(composeLayerSdr(world, ui), label1), label2);
        CHECK(maxStep(ordered, swapped) > 0);
        CHECK(maxStep(ordered, uiFirst) > 0);
        // label 2 (later) is on top: its green survives in full, label 1's red is attenuated by it
        CHECK(std::fabs(ordered[1] - ((.5f + (.2f * .4f) * .5f) * .7f)) < 1e-5f);
    }

    if (failures) std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
