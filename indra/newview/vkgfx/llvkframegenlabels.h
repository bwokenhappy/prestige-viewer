/**
 * @file llvkframegenlabels.h
 * @brief Frame generation's moving labels (#1517 Task 7e): the rules, no Vulkan.
 *
 *        The generator must never see interface pixels: it blends what it cannot match, so text it
 *        interpolates flickers and darkens. Labels anchored in the world (name tags, chat bubbles, hover
 *        text) are therefore drawn once per real frame, sharp, into a small per-slot atlas, and each
 *        picture of a batch -- the real frame and every generated one -- draws them between the world and
 *        the interface, each where it was at that picture's moment: a generated frame k of n sits at
 *        t = k / (n + 1) between the last real frame and this one, and a label that moved `motion`
 *        pixels on screen since then is drawn at cur - (1 - t) * motion. Its content and size are this
 *        real frame's (they change on real frames only).
 *
 *   History        screen positions by label id, one real frame back; when a label steps instead
 *   Packer         shelf packing of label rectangles into the atlas (1 px gutter)
 *   nextAtlasSize  grow on overflow up to the cap; shrink one step after a long fit in the smaller size
 *   sweep / bin    the screen rectangle a label covers for t in [0, 1], and the tiles that can see it
 *   GpuHeader / GpuLabel / labelsOffset / bufferBytes   the buffer the display shaders read
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

#ifndef LL_LLVKFRAMEGENLABELS_H
#define LL_LLVKFRAMEGENLABELS_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace llfgl
{
constexpr uint32_t kTile = 64;              // pixels per tile side
constexpr uint32_t kMaxPerTile = 16;        // labels one tile can list
constexpr uint32_t kMaxLabels = 512;        // atlas labels per frame
constexpr uint32_t kIdleShrinkFrames = 240; // real frames that fit the smaller atlas before it shrinks
constexpr float kTeleportFraction = 0.25f;  // of the screen's larger side: further, and the label steps

struct Extent { uint32_t w, h; };
struct Rect { int32_t x0, y0, x1, y1; };    // pixels, [x0, x1) x [y0, y1)
struct Vec2 { float x, y; };

constexpr Extent kAtlasStart{1024, 512};    // 2 MB in RGBA8
constexpr Extent kAtlasCap{2048, 1024};     // 8 MB in RGBA8

// Screen positions of labels by id (the object a label belongs to), from the last real frame that drew
// them into the atlas.
class History
{
public:
    // How far label `id` moved on screen since `frame - 1`. Zero, and *stepped set, when it was not drawn
    // in frame - 1 (new, or a gap: frames without labels in the atlas) or it moved further than a quarter
    // of the screen's larger side (a teleport, a region crossing, a camera cut): drawn where it is now.
    Vec2 motion(uint64_t id, Vec2 cur, uint64_t frame, Extent screen, bool* stepped) const
    {
        if (stepped) *stepped = true;
        const auto it = mLast.find(id);
        if (it == mLast.end() || frame == 0 || it->second.frame != frame - 1)
            return {0.f, 0.f};
        const Vec2 d{cur.x - it->second.pos.x, cur.y - it->second.pos.y};
        const float limit = kTeleportFraction * float(std::max(screen.w, screen.h));
        if (!(std::sqrt(d.x * d.x + d.y * d.y) <= limit))
            return {0.f, 0.f};
        if (stepped) *stepped = false;
        return d;
    }
    // Label `id` is at `cur` in `frame`.
    void record(uint64_t id, Vec2 cur, uint64_t frame) { mNext[id] = Entry{cur, frame}; }
    // `frame` is complete: only the labels recorded in it are remembered.
    void endFrame(uint64_t /*frame*/)
    {
        mLast.swap(mNext);
        mNext.clear();
    }
    size_t size() const { return mLast.size(); }
    void clear() { mLast.clear(); mNext.clear(); }

private:
    struct Entry { Vec2 pos; uint64_t frame; };
    std::unordered_map<uint64_t, Entry> mLast, mNext;
};

// Shelf packing: rows as high as their tallest rectangle, left to right, a 1 px gutter on every side
// between neighbours (no bilinear or rounding bleed between labels).
class Packer
{
public:
    explicit Packer(Extent atlas) : mAtlas(atlas) {}
    // False: it does not fit (nothing placed).
    bool place(int32_t w, int32_t h, int32_t& x, int32_t& y)
    {
        if (w <= 0 || h <= 0 || uint32_t(w) > mAtlas.w || uint32_t(h) > mAtlas.h)
            return false;
        int32_t sx = mX, sy = mY, sh = mShelfH;
        if (sx + w > int32_t(mAtlas.w))
        {   // a new shelf
            sy += sh + kGutter;
            sx = 0;
            sh = 0;
        }
        if (sy + h > int32_t(mAtlas.h))
            return false;   // (the shelf it would have opened stays unopened: a narrower one may still fit)
        x = sx;
        y = sy;
        mX = sx + w + kGutter;
        mY = sy;
        mShelfH = std::max(sh, h);
        return true;
    }

private:
    static constexpr int32_t kGutter = 1;
    Extent mAtlas;
    int32_t mX = 0, mY = 0, mShelfH = 0;
};

// One step down from `e` (the larger side halved), never below the start size.
inline Extent smallerSize(Extent e)
{
    Extent s = e;
    if (s.w >= s.h) s.w /= 2; else s.h /= 2;
    if (s.w < kAtlasStart.w || s.h < kAtlasStart.h)
        return kAtlasStart;
    return s;
}
// The atlas size for the next frame: a frame whose labels did not all fit grows it (the smaller side
// doubled) up to the cap; kIdleShrinkFrames frames in a row that fit smallerSize(cur) shrink it one step.
inline Extent nextAtlasSize(Extent cur, bool overflowed, uint32_t framesFittingSmaller)
{
    if (overflowed)
    {
        Extent g = cur;
        if (g.h < g.w) g.h *= 2; else g.w *= 2;
        return (g.w <= kAtlasCap.w && g.h <= kAtlasCap.h) ? g : cur;
    }
    if (framesFittingSmaller >= kIdleShrinkFrames)
        return smallerSize(cur);
    return cur;
}

// The screen rectangle a label covers for any t in [0, 1] (its rect now, and its rect moved back by
// `motion`), rounded outward and clipped to the screen. Empty (x0 >= x1 or y0 >= y1) when off screen.
inline Rect sweep(Rect cur, Vec2 motion, Extent screen)
{
    const float px0 = float(cur.x0) - motion.x, py0 = float(cur.y0) - motion.y;
    const float px1 = float(cur.x1) - motion.x, py1 = float(cur.y1) - motion.y;
    Rect r{std::min(cur.x0, int32_t(std::floor(px0))), std::min(cur.y0, int32_t(std::floor(py0))),
           std::max(cur.x1, int32_t(std::ceil(px1))), std::max(cur.y1, int32_t(std::ceil(py1)))};
    r.x0 = std::max(r.x0, 0);
    r.y0 = std::max(r.y0, 0);
    r.x1 = std::min(r.x1, int32_t(screen.w));
    r.y1 = std::min(r.y1, int32_t(screen.h));
    return r;
}

// Tiles of kTile pixels; per tile the count and up to kMaxPerTile label indices, in draw order.
struct Bins
{
    uint32_t tilesX = 0, tilesY = 0;
    std::vector<uint32_t> counts;   // tilesX * tilesY
    std::vector<uint32_t> idx;      // tilesX * tilesY * kMaxPerTile
};
inline uint32_t tilesFor(uint32_t pixels) { return (pixels + kTile - 1) / kTile; }

// Bins labels (their sweeps, in draw order). accepted[i]: label i is listed in every tile its sweep
// touches. Rejected (listed nowhere; the caller draws it with the interface, and it steps): an empty
// sweep (off screen), the kMaxLabels+1-th accepted label, or one that would make any tile list more than
// kMaxPerTile labels. A label's index is its place in `sweeps`.
inline Bins bin(const std::vector<Rect>& sweeps, Extent screen, std::vector<bool>& accepted)
{
    Bins b;
    b.tilesX = tilesFor(screen.w);
    b.tilesY = tilesFor(screen.h);
    b.counts.assign(size_t(b.tilesX) * b.tilesY, 0u);
    b.idx.assign(b.counts.size() * kMaxPerTile, 0u);
    accepted.assign(sweeps.size(), false);
    uint32_t n = 0;
    for (size_t i = 0; i < sweeps.size(); ++i)
    {
        Rect r = sweeps[i];
        r.x0 = std::max(r.x0, 0);
        r.y0 = std::max(r.y0, 0);
        r.x1 = std::min(r.x1, int32_t(screen.w));
        r.y1 = std::min(r.y1, int32_t(screen.h));
        if (r.x0 >= r.x1 || r.y0 >= r.y1 || n >= kMaxLabels)
            continue;
        const uint32_t tx0 = uint32_t(r.x0) / kTile, ty0 = uint32_t(r.y0) / kTile;
        const uint32_t tx1 = uint32_t(r.x1 - 1) / kTile, ty1 = uint32_t(r.y1 - 1) / kTile;
        bool fits = true;
        for (uint32_t ty = ty0; ty <= ty1 && fits; ++ty)
            for (uint32_t tx = tx0; tx <= tx1; ++tx)
                if (b.counts[size_t(ty) * b.tilesX + tx] >= kMaxPerTile) { fits = false; break; }
        if (!fits)
            continue;
        for (uint32_t ty = ty0; ty <= ty1; ++ty)
            for (uint32_t tx = tx0; tx <= tx1; ++tx)
            {
                const size_t t = size_t(ty) * b.tilesX + tx;
                b.idx[t * kMaxPerTile + b.counts[t]++] = uint32_t(i);
            }
        accepted[i] = true;
        ++n;
    }
    return b;
}

// Generated frame k (1..n) of a batch of n: its moment between the last real frame (0) and this one (1).
inline float phase(uint32_t k, uint32_t n) { return float(k) / float(n + 1); }

// The buffer the display shaders read (std430; mirrored in clean_display_labels.glsl):
//   GpuHeader | uint counts[tilesX * tilesY] | uint idx[tilesX * tilesY * kMaxPerTile] | GpuLabel labels[kMaxLabels]
// idx holds indices into labels[] (the accepted labels, renumbered in draw order by the writer).
struct GpuHeader { uint32_t tilesX, tilesY, count, pad; };
struct GpuLabel
{
    int32_t rect[4];   // this real frame's screen rect, [x0, x1) x [y0, y1)
    int32_t atlas[2];  // where rect's top-left texel sits in the atlas
    float motion[2];   // screen pixels moved since the last real frame
};
static_assert(sizeof(GpuHeader) == 16, "GpuHeader is 16 bytes (std430)");
static_assert(sizeof(GpuLabel) == 32, "GpuLabel is 32 bytes (std430)");

inline size_t labelsOffset(Extent screen)
{
    const size_t tiles = size_t(tilesFor(screen.w)) * tilesFor(screen.h);
    const size_t raw = sizeof(GpuHeader) + tiles * 4 + tiles * kMaxPerTile * 4;
    return (raw + 15) & ~size_t(15);
}
inline size_t bufferBytes(Extent screen) { return labelsOffset(screen) + size_t(kMaxLabels) * sizeof(GpuLabel); }
}

#endif // LL_LLVKFRAMEGENLABELS_H
