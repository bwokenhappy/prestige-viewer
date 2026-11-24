/**
 * @file prestigecameralayout.h
 * @brief Camera mode's overlay: where its parts go so that none covers another, pure geometry in
 *        dp from the window's top-left (slcameraview.cpp reads the boxes and applies the result;
 *        the layout test drives the same functions). Standard headers only.
 *
 * The top-right chips (Controls' chips, then the shot chips) wrap into rows no wider than the
 * room the status pills, the bar (at the top edge) and the strip leave; the light panel, the Studio
 * rail and the Filters panel stack under them; the hint row wraps clear of the side panels; the
 * notices at the top go under the chips when they would run into them; the tilt arc shrinks to
 * stay clear of the side panels.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Prestige Viewer Source Code
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */

#ifndef PRESTIGE_CAMERALAYOUT_H
#define PRESTIGE_CAMERALAYOUT_H

#include <algorithm>
#include <cmath>
#include <vector>

namespace pcamlayout
{
struct Rect
{
    float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool  empty() const { return w <= 0.5f || h <= 0.5f; }
    bool  overlaps(const Rect& o) const
    {
        return !empty() && !o.empty() && x < o.right() - 0.5f && o.x < right() - 0.5f && y < o.bottom() - 0.5f && o.y < bottom() - 0.5f;
    }
};

constexpr float kMargin = 24.f; // from the window's edges
constexpr float kGap    = 8.f;  // between neighbours

// Rows of items `widths` wide, `gap` apart, no row wider than `maxW` (flex-wrap: wrap): how many.
inline int rowCount(const std::vector<float>& widths, float gap, float maxW)
{
    int   rows = 0;
    float used = 0.f;
    for (float w : widths)
    {
        if (rows == 0 || used + gap + w > maxW + 0.5f)
        {
            ++rows;
            used = w;
        }
        else
            used += gap + w;
    }
    return rows;
}

inline float sum(const std::vector<float>& widths, float gap)
{
    float s = 0.f;
    for (size_t i = 0; i < widths.size(); ++i)
        s += widths[i] + (i ? gap : 0.f);
    return s;
}

// ---- the chips at the top right ----
struct TopIn
{
    float W = 0.f;
    std::vector<float> chips, shots; // the chips' and the shot chips' widths (none: not shown)
    float rowH        = 28.f;        // a chip's height
    float statusLeft  = 68.f;        // the status pills start here (right of the ?)
    float statusTop   = 36.f;
    std::vector<float> status;       // the status pills' widths
    bool  barTop      = false;       // the bar (and the strip, hints) at the top edge
    Rect  bar, strip;                // at the top edge: what the chips stay right of
};
struct TopOut
{
    float maxW = 0.f;         // the chips' and the shot chips' rows: at most this wide
    bool  shotsBeside = true; // the shot chips left of the chips (one row), else under them
    float shotsRight = kMargin, shotsTop = kMargin;
    float bottom = kMargin;   // under the chips (the right column starts below)
    float statusMaxW = 0.f;   // the status pills wrap at this width
    int   statusRows = 0;     // into this many rows
};

inline TopOut placeTop(const TopIn& in)
{
    TopOut      o;
    const float chipsW = sum(in.chips, kGap), shotsW = sum(in.shots, kGap), statusW = sum(in.status, 6.f);
    const float all    = chipsW + (in.shots.empty() ? 0.f : kGap + shotsW);
    auto        bottomFor = [&](float maxW, bool& beside)
    {
        const int   rows    = in.chips.empty() ? 0 : rowCount(in.chips, kGap, maxW);
        const float chipsB  = rows ? kMargin + (float)rows * in.rowH + (float)(rows - 1) * kGap : kMargin;
        beside              = rows <= 1 && all <= maxW + 0.5f;
        if (in.shots.empty() || beside)
            return std::max(chipsB, in.shots.empty() ? chipsB : kMargin + in.rowH);
        const int srows = rowCount(in.shots, kGap, maxW);
        return (rows ? chipsB + kGap : kMargin) + (float)srows * in.rowH + (float)(srows - 1) * kGap;
    };
    float maxW = 0.f;
    if (in.barTop && !in.bar.empty())
    {
        // Right of the bar; rows that reach down beside the strip stay right of it too.
        maxW = in.W - kMargin - (in.bar.right() + 12.f);
        bool beside = true;
        if (!in.strip.empty() && bottomFor(maxW, beside) > in.strip.y - kGap)
            maxW = std::min(maxW, in.W - kMargin - (in.strip.right() + 12.f));
        // The status pills left of the bar; rows that reach down beside the strip stay left of it.
        o.statusMaxW = in.bar.x - 16.f - in.statusLeft;
        const int rows = in.status.empty() ? 0 : rowCount(in.status, 6.f, std::max(o.statusMaxW, 120.f));
        if (!in.strip.empty() && rows > 0 && in.statusTop + (float)rows * 24.f + (float)(rows - 1) * 6.f > in.strip.y - kGap)
            o.statusMaxW = std::min(o.statusMaxW, in.strip.x - 16.f - in.statusLeft);
    }
    else
    {
        // Right of the status pills; they keep their row unless the chips would get less than
        // 45% of the window, then they wrap.
        maxW         = std::max(in.W - kMargin - 16.f - (in.statusLeft + statusW), std::min(all, in.W * 0.45f));
        o.statusMaxW = in.W - kMargin - maxW - 16.f - in.statusLeft;
    }
    // At least the widest chip; never past the window.
    float widest = 0.f;
    for (float w : in.chips)
        widest = std::max(widest, w);
    for (float w : in.shots)
        widest = std::max(widest, w);
    o.maxW       = std::min(std::max(maxW, widest), in.W - 2.f * kMargin);
    o.statusMaxW = std::max(o.statusMaxW, 120.f);
    o.statusRows = in.status.empty() ? 0 : rowCount(in.status, 6.f, o.statusMaxW);
    o.bottom     = bottomFor(o.maxW, o.shotsBeside);
    if (in.shots.empty())
        return o;
    if (o.shotsBeside)
    {
        o.shotsRight = kMargin + chipsW + kGap;
        o.shotsTop   = kMargin;
    }
    else
    {
        const int rows = in.chips.empty() ? 0 : rowCount(in.chips, kGap, o.maxW);
        o.shotsRight   = kMargin;
        o.shotsTop     = rows ? kMargin + (float)rows * (in.rowH + kGap) : kMargin;
    }
    return o;
}

// ---- a side panel's room ----
// The free stretches of [y0, y1] down a column spanning [x0, x1] between `obstacles`, top first.
struct Gap
{
    float y0 = 0.f, y1 = 0.f;
    float h() const { return y1 - y0; }
};
inline std::vector<Gap> freeGaps(float x0, float x1, float y0, float y1, const std::vector<Rect>& obstacles)
{
    std::vector<Gap> blocked;
    for (const Rect& r : obstacles)
        if (!r.empty() && r.right() > x0 + 0.5f && r.x < x1 - 0.5f && r.bottom() + kGap > y0 && r.y - kGap < y1)
            blocked.push_back({ r.y - kGap, r.bottom() + kGap });
    std::sort(blocked.begin(), blocked.end(), [](const Gap& a, const Gap& b) { return a.y0 < b.y0; });
    std::vector<Gap> gaps;
    float            at = y0;
    for (const Gap& b : blocked)
    {
        if (b.y0 > at)
            gaps.push_back({ at, std::min(b.y0, y1) });
        at = std::max(at, b.y1);
        if (at >= y1)
            break;
    }
    if (at < y1)
        gaps.push_back({ at, y1 });
    return gaps;
}

// The tallest free stretch of [top, bottom] for a panel spanning [x0, x1] (top and bottom: in and out).
inline void sideRoom(float x0, float x1, const std::vector<Rect>& obstacles, float& top, float& bottom)
{
    Gap best = { top, top };
    for (const Gap& g : freeGaps(x0, x1, top, bottom, obstacles))
        if (g.h() > best.h() + 0.5f)
            best = g;
    top    = best.y0;
    bottom = best.y1;
}

// The first free stretch from the top at least `h` tall (y: its top); false when there is none.
inline bool firstFit(float x0, float x1, float top, float bottom, float h, const std::vector<Rect>& obstacles, float& y)
{
    for (const Gap& g : freeGaps(x0, x1, top, bottom, obstacles))
        if (g.h() >= h)
        {
            y = g.y0;
            return true;
        }
    return false;
}

// ---- the hint row: centred on `cx`; how wide it may be beside `obstacles` ----
// `lineH`: one line's height; `edgeY`: its fixed edge (its bottom, or its top when `down`).
inline float hintMaxWidth(float cx, float natural, float lineH, float edgeY, bool down, const std::vector<Rect>& obstacles,
                          float minW = 240.f)
{
    float maxW = natural;
    for (int pass = 0; pass < 3; ++pass)
    {
        const int   lines = std::max(1, (int)std::ceil(natural / std::max(maxW, 1.f) - 0.01f));
        const float h     = (float)lines * lineH;
        const Rect  band  = { 0.f, down ? edgeY : edgeY - h, 1.f, h };
        float       room  = natural;
        for (const Rect& r : obstacles)
        {
            if (r.empty() || r.bottom() <= band.y + 0.5f || r.y >= band.bottom() - 0.5f)
                continue;
            if (r.right() <= cx)
                room = std::min(room, 2.f * (cx - r.right()) - 24.f);
            else if (r.x >= cx)
                room = std::min(room, 2.f * (r.x - cx) - 24.f);
            // (one across the centre cannot be avoided by wrapping)
        }
        const float next = std::max(minW, room);
        if (std::fabs(next - maxW) < 0.5f)
            break;
        maxW = next;
    }
    return maxW;
}

// ---- the tilt arc: the largest radius from `r0` down to `rMin` whose band (and the knob's label
// beside it) clears `obstacles`; -1 when none does ----
inline Rect tiltBand(float cx, float cy, float r, float lim)
{
    const float bl = cx + r * std::cos(lim) - 14.f, bt = cy - r * std::sin(lim) - 12.f;
    return { bl, bt, cx + r + 16.f - bl, 2.f * (r * std::sin(lim) + 12.f) };
}
inline float tiltRadius(float cx, float cy, float r0, float rMin, float lim, const std::vector<Rect>& obstacles, float labelReach = 80.f)
{
    for (float r = r0; r >= rMin; r -= 2.f)
    {
        Rect b = tiltBand(cx, cy, r, lim);
        Rect label = { cx + r, cy - 10.f, labelReach, 20.f }; // the knob's "Tilt 0°" beside it
        bool clear = true;
        for (const Rect& o : obstacles)
            clear = clear && !b.overlaps(o) && !label.overlaps(o);
        if (clear)
            return r;
    }
    return -1.f;
}

// ---- the notices at the top (saved, toast, picking banner): how far down they go so that none
// runs into what stands at the top (the chips, the status pills) ----
inline float noticeShift(const std::vector<Rect>& pills, const std::vector<Rect>& obstacles)
{
    float shift = 0.f;
    for (const Rect& p : pills)
        for (const Rect& r : obstacles)
            if (!p.empty() && !r.empty() && r.x < p.right() && p.x < r.right() && r.y < p.bottom() + shift && r.bottom() + kGap > p.y + shift)
                shift = std::max(shift, r.bottom() + kGap - p.y);
    return shift;
}
} // namespace pcamlayout

#endif // PRESTIGE_CAMERALAYOUT_H
