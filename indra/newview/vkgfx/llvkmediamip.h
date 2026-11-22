/**
 * @file llvkmediamip.h
 * @brief Mip rules for media on prims (#1025; no Vulkan, no viewer types).
 *
 *        A media face (video, web page) samples its base colour at mip 0 while the texel
 *        footprint is small (the shader's MEDIA MIP LOCK: clean_frame_info.glsl mediaLodBias,
 *        K mips = log2 of the texels per display pixel it keeps sharp). Its lower mips are then
 *        read only by faces farther away, by the probes and the mirror -- yet every media frame
 *        used to rebuild the whole chain (10 blits + 20 barriers for 1080p). This header holds:
 *          - the lock K from the settings;
 *          - whether an in-place media update rebuilds the chain: always when the lock is off,
 *            when the media's nearest face may sample below mip 0 (its LL virtual size), or
 *            on every kNearInterval-th update (the probes / mirror / farther faces, and a
 *            stale virtual size); a paused media catches up after kIdleCatchUpFrames;
 *          - which texels of each level a dirty-rectangle update changes (exact 2:1 levels
 *            take the rectangle, an odd level and everything below it the whole level).
 *        scripts/check/test_media_mip.cpp tests them; scripts/check/checks/media_mip.py runs
 *        that and checks the shaders and the renderer decide through this header.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef LL_LLVKMEDIAMIP_H
#define LL_LLVKMEDIAMIP_H

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace llvkmediamip
{
// A media whose nearest face samples only mip 0 still rebuilds its chain on every this-many-th
// update (at 30 fps video: ~8 times a second), for the faces / views the estimate does not see.
constexpr uint32_t kNearInterval = 4;
// A media that skipped its chain and then got no new frame for this many renderer frames (paused,
// a static page) rebuilds it once, so nothing keeps showing an older frame in its lower mips.
constexpr uint32_t kIdleCatchUpFrames = 30;
// The virtual size is an area estimate of the NEAREST face (oblique faces, partial overlap):
// the chain is kept while the estimated level comes within this many mips of the lock.
constexpr float kEstimateMargin = 1.f;
// The texels per display pixel the lock may be set to (2^0 .. 2^4; the shader bias stays within 2K = 8
// mips, inside every desktop maxSamplerLodBias).
constexpr float kMinTexels = 1.f, kMaxTexels = 16.f;

// The shader's K (frame info display_output.z): log2 of the texels per display pixel that still
// sample mip 0. 0 = the lock off (the old full-chain trilinear sample).
inline float lockLevels(bool on, float texelsPerPixel)
{
    if (!on || !(texelsPerPixel >= kMinTexels)) return 0.f;
    return std::log2(std::min(texelsPerPixel, kMaxTexels));
}

// The level an area-matched sample of a w x h image takes on a face that needs `virtualSize`
// texels (LLFace::getVirtualSize: on-screen pixels per UV area, display resolution). No size yet
// (0, unknown) reads as far: the chain is kept.
inline float estimatedLevel(uint32_t w, uint32_t h, float virtualSize)
{
    if (!(virtualSize > 0.f) || !w || !h) return 1e9f;
    return 0.5f * std::log2((float)w * (float)h / virtualSize);
}

// Per media slot.
struct State
{
    uint32_t skipped = 0;      // updates since the chain was last rebuilt (0: the chain is current)
    uint32_t lastUpdate = 0;   // the renderer frame (mMediaFrame) of the last update
    float level = 1e9f;        // the nearest face's estimatedLevel, refreshed before each update
};

// An in-place update (level 0 refreshed in a resident image, levels 1.. SHADER_READ_ONLY):
// rebuild the chain this time (state.level: this update's estimate)? Updates `state`.
inline bool rebuildChain(float lock, uint32_t frame, State& state)
{
    state.lastUpdate = frame;
    if (lock <= 0.f || state.level + kEstimateMargin > lock || state.skipped + 1u >= kNearInterval)
    {
        state.skipped = 0;
        return true;
    }
    ++state.skipped;
    return false;
}
// A media without a new frame this pass: rebuild its stale chain now (no copy)?
inline bool catchUp(uint32_t frame, State& state)
{
    if (!state.skipped || frame - state.lastUpdate < kIdleCatchUpFrames) return false;
    state.skipped = 0;
    state.lastUpdate = frame;
    return true;
}
// A dirty rectangle may stop at its own texels down the chain only while the chain was current
// before this update; after skipped rebuilds the whole chain is behind.
inline bool rectOnly(const State& state) { return state.skipped == 0; }

// ---- the changed texels of each level ----
struct Rect { uint32_t x = 0, y = 0, w = 0, h = 0; };
// Level i - 1 is mw x mh and `r` its changed texels (inside it). Returns level i's changed
// texels and whether the blit is an exact 2:1 of the rectangle (src = 2 x dst); otherwise the
// whole level is blitted (an odd side: the full-level blit's scale is not 2, so a partial blit
// would not match it). The caller blits src [2x, 2(x+w)) -> dst [x, x+w) when exact.
inline Rect nextLevel(uint32_t mw, uint32_t mh, const Rect& r, bool& exact)
{
    const uint32_t nw = std::max(mw / 2u, 1u), nh = std::max(mh / 2u, 1u);
    exact = mw % 2u == 0u && mh % 2u == 0u && r.w && r.h;
    if (!exact) return Rect{0u, 0u, nw, nh};
    Rect n;
    n.x = r.x / 2u;
    n.y = r.y / 2u;
    n.w = std::min((r.x + r.w + 1u) / 2u, nw) - n.x;
    n.h = std::min((r.y + r.h + 1u) / 2u, nh) - n.y;
    // the whole level after all: one blit of the full level matches it exactly too
    if (n.x == 0u && n.y == 0u && n.w == nw && n.h == nh) exact = false;
    return n;
}
} // namespace llvkmediamip

#endif // LL_LLVKMEDIAMIP_H
