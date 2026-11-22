// Frame generation's moving labels (#1517 Task 7e), included by clean_display_output.frag.
// Name tags, chat bubbles and hover text are drawn once per real frame, sharp, into the slot's atlas
// (binding 2; premultiplied sRGB-encoded colour + coverage, as the UI layer). Each picture of a batch draws
// them between the world and the interface, each where it was at that picture's moment t (1: the real
// frame; k / (n + 1): generated frame k of n): its rect now, moved back by (1 - t) * its screen motion.
// The list (binding 3) mirrors llvkframegenlabels.h:
//   uint header[4] = {tilesX, tilesY, count, 0}
//   uint counts[tilesX * tilesY], idx[tilesX * tilesY * 16]   (64-pixel tiles, draw order)
//   GpuLabel labels[] at a 16-byte aligned offset: int rect[4], int atlas[2], float motion[2] (8 words)
layout(set=0,binding=3) readonly buffer Labels {
    uint header[4];
    uint data[];
} uLabels;

const uint kLabelTile = 64u;
const uint kLabelMaxPerTile = 16u;

// The labels listed for `pixel`'s tile: their count, and where their indices start in data[].
uint labelTile(ivec2 pixel, out uint idxBase) {
    idxBase = 0u;
    const uint tilesX = uLabels.header[0], tilesY = uLabels.header[1];
    if (pixel.x < 0 || pixel.y < 0) return 0u;
    const uint tx = uint(pixel.x) / kLabelTile, ty = uint(pixel.y) / kLabelTile;
    if (tx >= tilesX || ty >= tilesY) return 0u;
    const uint tile = ty * tilesX + tx;
    idxBase = tilesX * tilesY + tile * kLabelMaxPerTile;
    return min(uLabels.data[tile], kLabelMaxPerTile);
}

// Every label that reaches `pixel` at moment t, composited over `nits` (Rec.709 linear nits) in draw order,
// as the interface layer is (displayComposeUi at paper white).
// sdr (#1536): `nits` is the SDR world, sRGB-encoded, and each label blends as the SDR swapchain does
// (displayComposeUiSdr); otherwise in nits at paper white.
vec3 composeLabels(vec3 nits, ivec2 pixel, float t, float paperWhiteNits, bool sdr) {
    uint idxBase;
    const uint n = labelTile(pixel, idxBase);
    if (n == 0u) return nits;
    const uint tiles = uLabels.header[0] * uLabels.header[1];
    // labelsOffset (bytes, 16-aligned) less the 4-word header data[] starts after
    const uint labelBase = (((16u + tiles * 4u + tiles * kLabelMaxPerTile * 4u) + 15u) & ~15u) / 4u - 4u;
    for (uint i = 0u; i < n; ++i) {
        const uint b = labelBase + uLabels.data[idxBase + i] * 8u;
        const ivec4 rect = ivec4(int(uLabels.data[b]), int(uLabels.data[b + 1u]),
                                 int(uLabels.data[b + 2u]), int(uLabels.data[b + 3u]));
        const ivec2 atlas = ivec2(int(uLabels.data[b + 4u]), int(uLabels.data[b + 5u]));
        const vec2 motion = uintBitsToFloat(uvec2(uLabels.data[b + 6u], uLabels.data[b + 7u]));
        // drawn at rect - (1 - t) * motion: this pixel shows the label's texel at pixel + (1 - t) * motion
        const ivec2 src = pixel + ivec2(round((1.0 - t) * motion));
        if (all(greaterThanEqual(src, rect.xy)) && all(lessThan(src, rect.zw)))
        {
            const vec4 label = texelFetch(uLabelAtlas, atlas + (src - rect.xy), 0);
            nits = sdr ? displayComposeUiSdr(nits, label) : displayComposeUi(nits, label, paperWhiteNits);
        }
    }
    return nits;
}

// PQ (SMPTE ST 2084) back to absolute nits.
vec3 pqToNits(vec3 e) {
    const vec3 p = pow(clamp(e, 0.0, 1.0), vec3(32.0 / 2523.0));
    const vec3 num = max(p - 3424.0 / 4096.0, vec3(0.0));
    const vec3 den = 2413.0 / 128.0 - (2392.0 / 128.0) * p;
    return 10000.0 * pow(num / den, vec3(16384.0 / 2610.0));
}

// A generated frame (encoding 4): its world is Rec.2020 PQ; `ui` is the UI layer's texel (the slot's copy:
// premultiplied sRGB-encoded colour + coverage). Where the interface or a label reaches, the world is decoded
// to Rec.709 nits and composed exactly as the real frame is -- labels at moment t (labels: the 0x200 flag;
// the list is read only with it), then the interface, both with displayComposeUi at paper white, clamped to
// the peak -- and encoded back. Everywhere else the world texel is kept bit for bit.
vec3 composeOverEncoded(vec3 encoded, vec4 ui, bool labels, ivec2 pixel, float t, float paperWhiteNits,
                        float peakNits) {
    uint idxBase;
    const bool reach = labels && labelTile(pixel, idxBase) != 0u;
    if (!reach && all(equal(ui, vec4(0.0)))) return encoded;
    // Column-major Rec.2020 -> Rec.709 (the inverse of the encode's to2020), D65.
    const mat3 to709 = mat3( 1.660491, -0.124550, -0.018151,
                            -0.587641,  1.132900, -0.100579,
                            -0.072850, -0.008349,  1.118730);
    const mat3 to2020 = mat3(.627404, .069097, .016391,
                             .329283, .919540, .088013,
                             .043313, .011362, .895595);
    const float peak = clamp(peakNits, 80.0, 10000.0);
    vec3 nits = clamp(to709 * pqToNits(encoded), 0.0, peak);
    if (reach) nits = clamp(composeLabels(nits, pixel, t, paperWhiteNits, false), 0.0, peak);
    nits = clamp(displayComposeUi(nits, ui, paperWhiteNits), 0.0, peak);
    return clamp(pq(to2020 * nits), 0.0, 1.0);
}

// #1536 SDR frame generation: one picture of an SDR frame generation chain -- the world (sRGB-encoded,
// 8-bit: a generated one or the real one), the labels where they were at moment t, then the interface
// layer, all in that encoded space. Pixels with neither keep the world texel exactly.
vec3 composeOverSdr(vec3 encoded, vec4 ui, bool labels, ivec2 pixel, float t) {
    uint idxBase;
    const bool reach = labels && labelTile(pixel, idxBase) != 0u;
    if (!reach && all(equal(ui, vec4(0.0)))) return encoded;
    vec3 c = encoded;
    if (reach) c = composeLabels(c, pixel, t, 0.0, true);
    return displayComposeUiSdr(c, ui);
}
