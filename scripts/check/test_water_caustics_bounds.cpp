#include "vkgfx/llwatercausticsbounds.h"
#include "slseamath.h"
#include "llcleanqvertex.h"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <vector>

static void need(bool value, const char* message)
{
    if (!value) { std::fprintf(stderr, "%s\n", message); std::exit(1); }
}
struct Row
{
    float model_matrix[16] = {}, aabb_min[4] = {}, aabb_max[4] = {};
    uint32_t lod_group_id = 0, flags = 2050;
    float spin_axis[4] = {};
};
static Row plane(float z)
{
    Row r;
    r.model_matrix[0] = r.model_matrix[5] = r.model_matrix[10] = r.model_matrix[15] = 1.f;
    r.model_matrix[14] = z;
    r.aabb_min[2] = -8.f; r.aabb_max[2] = 8.f; // live producer's deliberately padded box.
    return r;
}
int main(int argc, char**)
{
    using Bound = LLWaterCausticsBounds;
    float sea[28] = {}, waves[64] = {};
    sea[0] = 2.f; sea[7] = .4f;
    waves[0] = waves[5] = 1.57079632679f; waves[2] = .2f; waves[4] = .6f;
    // A drawn grid vertex u=1: h=20, world x=1-Q A sin(k u). The old
    // receiver=seaSurface(worldXY) rule obtains a different, higher height.
    const float u = 1.f, actualZ = 20.f + waves[2] * std::cos(waves[0] * u);
    const float x = u - waves[4] * waves[2] * std::sin(waves[0] * u);
    const float dryZ = actualZ + .01f;
    const float oldDepth = 20.f - dryZ + waves[2] * std::cos(waves[0] * x);
    need(dryZ > actualZ && oldDepth > 0.f, "old-world-XY counterexample disappeared");
    if (argc > 1) { need(oldDepth <= 0.f, "BEFORE: dry receiver above drawn Gerstner vertex was accepted"); }
    const float lower = Bound::waveLower(20.f, 0.f, sea, waves);
    need(!(dryZ < lower), "AFTER: dry receiver escaped the conservative certificate");
    need(19.5f < lower, "ordinary half-metre receiver was needlessly disabled");

    Bound b;
    // GPU packed halves: depth=2, shore=1, fetch=4, dh=-1.0009765625.
    uint32_t packed[8] = {0x3c004000u, 0xbc014400u, 0x3c004000u, 0x40004400u,
                          0x3c004000u, 0xc4004400u, 0x3c004000u, 0x00004400u};
    need(b.publishedField(packed, 4, 1, 0) == -4.f, "half minimum must include every GPU texel");
    need(Bound::half(0xbc01u) == -1.0009765625f && Bound::half(1u) == 0x1p-24f,
         "half quantization/subnormal mismatch");
    // Changing unpublished CPU bytes without a publication does not replace the
    // selected GPU copy. A new slot/version does, including upward height changes.
    uint32_t next[8]; std::memcpy(next, packed, sizeof(next)); next[5] = 0x40004400u;
    need(b.publishedField(next, 4, 1, 0) == -4.f, "unpublished bytes replaced the certificate");
    need(b.publishedField(next, 4, 2, 1) == -1.0009765625f, "new publication retained stale minimum");
    for (uint32_t bad : {0x7c00u, 0xfc00u, 0x7e00u})
        for (int channel = 0; channel < 4; ++channel)
        {
            uint32_t invalid[8]; std::memcpy(invalid, packed, sizeof(invalid));
            const int word = channel / 2, shift = (channel & 1) * 16;
            invalid[word] = (invalid[word] & ~(0xffffu << shift)) | (bad << shift);
            Bound invalidBound;
            need(invalidBound.publishedField(invalid, 4, 1, 0) == Bound::reject, "nonfinite field did not fail closed");
        }
    need(b.publishedField(nullptr, 4, 99, 0) == Bound::reject, "null selected field did not fail closed");
    const float mixedLower = Bound::waveLower(20.f, -4.f, sea, waves);
    // Convex bilinear/edge fades, arbitrary bounded wave/LOD amplitudes, positive
    // breaker lift, then the exact convex structure of grid morph/triangle mixing.
    for (int i = 0; i <= 1024; ++i)
    {
        const float t = float(i) / 1024.f;
        const float dh = (-4.f * (1.f-t) + 2.f * t) * t;
        const float a = waves[2] * (1.f + .6f * t) * (1.f-t);
        const float z[3] = {20.f + dh + a * std::cos(7.f*t),
                            20.f -4.f - a + t, 20.f + a + 2.f*t};
        for (float v : z) need(v >= mixedLower, "vertex fell below lower envelope");
        const float morph = z[0]*(1.f-t) + .5f*(z[1]+z[2])*t;
        const float triangle = .2f*morph + .3f*z[1] + .5f*z[2];
        need(morph >= mixedLower && triangle >= mixedLower, "convex morph/raster envelope failed");
    }
    float invalidWave[64]; std::memcpy(invalidWave, waves, sizeof(waves));
    uint32_t nan = 0x7fc00000u; std::memcpy(invalidWave + 2, &nan, sizeof(nan));
    need(Bound::waveLower(20.f, 0.f, sea, invalidWave) == Bound::reject, "nonfinite wave accepted");
    sea[9] = -1.f;
    need(Bound::waveLower(20.f, 0.f, sea, waves) == Bound::reject, "negative breaker lift accepted");
    sea[9] = 0.f;

    // Exercise the existing production CPU twin of the shader, not a replacement
    // wave formula. Low/high have the same displacement and differ in grid density.
    slsea::Waves model;
    slsea::Params params;
    params.hs = sea[7]; params.surf = 1.f; params.runup = 1.5f;
    params.dir[0] = -1.f;
    for (int j = 0; j < 8; ++j)
    {
        model.k[j] = .1f * (j + 1); model.kvec[j][0] = model.k[j];
        model.amp[j] = .01f * (j + 1); model.phase[j] = .3f * j;
        waves[8*j] = model.k[j]; waves[8*j+2] = model.amp[j]; waves[8*j+5] = model.k[j];
    }
    sea[8] = params.surf; sea[9] = params.runup;
    for (int mode : {1, 2})
    {
        sea[0] = float(mode);
        const float certificate = Bound::waveLower(20.f, -4.f, sea, waves);
        for (int i = 0; i < 4096; ++i)
        {
            slsea::Texel f;
            const float t = float(i) / 4095.f;
            f.depth = -2.f + 102.f*t; f.shore = -15.f + 55.f*t;
            f.fetch = 65504.f*t; f.dh = -4.f + 6.f*t; f.shoreN[0] = 1.f;
            params.swashPhase = t;
            float height, breaking, swash;
            slsea::eval(model, params, f, 31.f*t, -23.f*t, height, breaking, swash);
            need(20.f + f.dh + height >= certificate, "production sea twin fell below certificate");
            // Depth-unprojection tolerance rejects even a receiver just above a
            // trough or crest; the certificate never needs horizontal inversion.
            need(!(20.f + f.dh + height + .0001f < certificate), "near-crest/trough dry point accepted");
        }
    }

    float center, step;
    LLCleanQV::axisLattice(0.0, 0.0, center, step);
    need(center + LLCleanQV::latticeIndex(0.f, center, step) * step == 0.f,
         "flat water's quantized local z changed");

    std::array<Row, 3> rows = {plane(20.f), plane(8.f), plane(30.f)};
    std::vector<std::pair<uint32_t,uint32_t>> runs;
    b.publishPlanes(true, true, true, rows.data(), rows.size(), runs, 0.f);
    need(b.planeZ < 8.f && b.planeZ > 7.99f, "mixed flat planes/padded AABB bound incorrect");
    rows[1] = plane(40.f); runs = {{1,2}};
    b.publishPlanes(true, false, false, rows.data(), rows.size(), runs, 0.f);
    need(!b.planesPublished && b.planeZ == Bound::reject, "refused GPU publication used pending CPU rows");
    b.publishPlanes(true, true, true, rows.data(), rows.size(), runs, 0.f);
    need(b.planeZ < 20.f && b.planeZ > 19.99f, "full retry failed to replace old low plane");
    rows[0].lod_group_id = ~0u; runs = {{0,1}};
    b.publishPlanes(true, true, false, rows.data(), rows.size(), runs, 0.f);
    need(b.planeZ < 30.f && b.planeZ > 29.99f, "deleted minimum plane remained stale");
    b.publishPlanes(true, true, false, rows.data(), rows.size(), runs, 5.f);
    need(b.planeZ < 25.f && b.planeZ > 24.99f, "agent/store vertical rebase lost");
    rows[2].model_matrix[2] = .1f; runs = {{2,3}};
    b.publishPlanes(true, true, false, rows.data(), rows.size(), runs, 5.f);
    need(b.planeZ == Bound::reject, "unsupported tilted water did not fail closed");
    b.publishPlanes(false, false, true, (const Row*)nullptr, 1000000, runs, 0.f);
    need(!b.planesPublished, "OFF touched/retained plane inputs");
    std::puts("AFTER PASS: dry counterexample, published half field, invalid inputs, mixed levels, convex grid, flat publication/retry/rebase");
}
