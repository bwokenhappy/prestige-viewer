// vkclean -- FULL-RESOLUTION GI ON MOVER PIXELS (RenderVKGIMoverSamples, an A/B, default off; llcleangivk.cpp
// recordGIMovers). Below full GI resolution a receiver a few GI texels wide -- a distant avatar -- swims: its GI
// comes from a handful of texels whose single samples, history tests and upsample taps change every frame
// (scripts/check/test_gi_stable_samples.cpp; the model's only variant that reaches full-resolution quality on a
// moving body is full-resolution samples on the body). Static surfaces stay at the reduced extent; MOVER pixels --
// the motion target's skinned class (avatars, animesh) and rigid surfaces whose motion vector departs from the
// camera's reprojection by > 0.5 px (moving prims, flexi) -- get their own GI sample, per render pixel:
//   LIST   (clean_deferred_light.comp -DCLEAN_GI_MOVER, push rtr.z < 0) one lane per render pixel: a mover appends
//          its pixel to the list, at most the CAP (RenderVKGIMoverBudget x the GI texel count); past it the pixel
//          counts as a cap hit and keeps the reduced-extent GI (no entry this frame: the consumers fall back);
//   TRACE  (push rtr.z = the cap; indirect, 64 entries per workgroup) per listed pixel: one fresh cosine ray from
//          the pixel's own point / normal (the GI trace's hit shading, giTrace), resampled with the reservoirs of
//          the GI texels covering it (this frame's, reconnected: giTap -- same surface only), one visibility ray
//          for a reused pick, the GI firefly clamp; then the pixel's own HISTORY along its motion vector (last
//          frame's entry at the reprojected pixel, the best depth match of the 3 x 3, at most GI_MOV_HIST frames);
//   READ   the combine (deferReflIn): a listed pixel's GI is its entry -- this frame's at the pixel, or under
//          GI_F_LAG last frame's at the camera reprojection (3 x 3 depth search: an avatar's own motion of up to a
//          pixel a frame) -- a depth-stopped 3 x 3 mean of the entries around it; no entry: the reduced-extent GI.
// THE BUFFER (one device-local SSBO, device address in the GI header's mov.xy and the trace push's rtr3.zw;
// uvec2 units): e[0] x the movers seen (atomic: listed = min(x, cap)), y the cap hits; e[1] 0; e[2] / e[3] the
// TRACE's indirect dispatch (x workgroups, 1, 1; byte 16); the list from e[4] (two pixel indices per uvec2); the
// HISTORY from e[giMovHistBase(cap)]: 2 parities x W x H entries (parity = the frame's & 1), one uvec2 each:
// x RGB9E5 irradiance / pi (the GI signal's unit), y half(view depth) | history length << 16 (5 bits) | the frame
// stamp << 21 (11 bits: an entry is valid only for the frame that wrote it). The host clears e[0..3] every frame
// (recordGIMovers). VRAM: 16 B per render pixel + 4 B per cap entry (2560 x 1440: 59 MB; 1280 x 720: 15 MB).
#ifndef CLEAN_GI_MOVERS_GLSL
#define CLEAN_GI_MOVERS_GLSL
#include "clean_probe_pack.glsl"   // packRGB9E5 / unpackRGB9E5
layout(buffer_reference, std430, buffer_reference_align = 8) buffer GiMoverBuf { uvec2 e[]; };
#define GI_MOV_HIST 31.0   // (the history length cap: the 5-bit field; the GI denoiser's RenderVKGIHistory default is 32)
#define GI_MOV_STAMP_MASK 2047u
uint giMovHistBase(uint cap) { return 4u + (cap + 1u) / 2u; }
uvec2 giMovPack(vec3 E, float vz, float len, uint stamp)
{
    const uint z = packHalf2x16(vec2(vz, 0.0)) & 0xFFFFu;
    return uvec2(packRGB9E5(max(E, vec3(0.0))), z | (uint(clamp(len, 0.0, 31.0)) << 16u) | ((stamp & GI_MOV_STAMP_MASK) << 21u));
}
void giMovUnpack(uvec2 w, out vec3 E, out float vz, out float len, out uint stamp)
{
    E = unpackRGB9E5(w.x);
    vz = unpackHalf2x16(w.y & 0xFFFFu).x;
    len = float((w.y >> 16u) & 31u);
    stamp = w.y >> 21u;
}

#ifdef GI_MOV_CONSUMER
// (the combine) the mover GI at render-pixel coordinate pxc (continuous, in the raster of the entries' frame) of a
// receiver at view depth vz: search -- the best depth match of the 3 x 3 around it (GI_F_LAG: last frame's entries at
// the camera reprojection), else the pixel itself; then the depth-stopped mean of the 3 x 3 around that entry. The
// header's mov word names the buffer, its extent, the history base and the stamp / parity the consumer reads.
bool giMoverFetch(vec2 pxc, float vz, bool search, out vec3 E)
{
    E = vec3(0.0);
    if (all(equal(gi.mov.xy, uvec2(0u)))) return false;
    const ivec2 R = ivec2(int(gi.mov.z & 0xFFFFu), int(gi.mov.z >> 16u));
    if (R.x != int(g_draw_push.width) || R.y != int(g_draw_push.height) || !(vz > 0.0)) return false;
    GiMoverBuf mb = GiMoverBuf(gi.mov.xy);
    const uint stamp = gi.mov.w >> 21u;
    const uint base = (gi.mov.w & 0x1FFFFFu) + (stamp & 1u) * uint(R.x * R.y);
    const ivec2 c = ivec2(floor(pxc));
    const float tol = max(0.03 * vz, 0.03);
    float best = 1.0;
    ivec2 bc = ivec2(-1);
    float bz = 0.0;
    const int rad = search ? 1 : 0;
    for (int dy = -rad; dy <= rad; ++dy)
        for (int dx = -rad; dx <= rad; ++dx)
        {
            const ivec2 q = c + ivec2(dx, dy);
            if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, R))) continue;
            vec3 Eq; float zq, lq; uint sq;
            giMovUnpack(mb.e[base + uint(q.y * R.x + q.x)], Eq, zq, lq, sq);
            if (sq != stamp || !(lq > 0.0)) continue;
            const float e = abs(zq - vz) / tol;
            if (e < best) { best = e; bc = q; bz = zq; }
        }
    if (bc.x < 0) return false;
    // the depth-stopped 3 x 3 mean around the matched entry (its own depth as the reference)
    const float gt = max(0.01 * bz, 0.02);
    vec3 sum = vec3(0.0);
    float ws = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            const ivec2 q = bc + ivec2(dx, dy);
            if (any(lessThan(q, ivec2(0))) || any(greaterThanEqual(q, R))) continue;
            vec3 Eq; float zq, lq; uint sq;
            giMovUnpack(mb.e[base + uint(q.y * R.x + q.x)], Eq, zq, lq, sq);
            if (sq != stamp || !(lq > 0.0) || abs(zq - bz) > gt) continue;
            const float w = (dx == 0 && dy == 0) ? 2.0 : 1.0;
            sum += w * Eq; ws += w;
        }
    if (!(ws > 0.0)) return false;
    E = sum / ws;
    return !any(isnan(E)) && !any(isinf(E));
}
#endif
#endif
