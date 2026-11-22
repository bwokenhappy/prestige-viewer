// #703 The new interface's floating windows: their geometry, std only (no viewer, no RmlUi), so the
// window framework (llnewuiwindow.cpp) and its tests (scripts/check/test_newui_window_layout.cpp)
// use the same code.
//
// Units: UI units (dp), the window's top-left origin, y down (RmlUi's), so a rect follows the UI
// scale and the display scale. Rules:
//  - clamp: a window is never larger than the screen and never off it (all of it on screen);
//  - snap: while moved, an edge within kSnapDp of a screen edge or of another window's edge (the
//    facing edge, or the same edge to line them up) lands on it; while resized, the moving
//    right / bottom edge snaps the same way;
//  - persistence: one string for every window, "key x y w h open minimised;..." (a key has no
//    spaces or ';'), read back tolerantly (a bad entry is skipped, never fatal).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace slwin
{
constexpr float kSnapDp = 10.f;

struct Rect
{
    float x = 0, y = 0, w = 0, h = 0;
    float right() const { return x + w; }
    float bottom() const { return y + h; }
    bool  operator==(const Rect& o) const { return x == o.x && y == o.y && w == o.w && h == o.h; }
    bool  operator!=(const Rect& o) const { return !(*this == o); }
};

// The screen area windows may use (UI units): the whole window, less the menu bar on top.
struct Screen
{
    float left = 0, top = 0, right = 0, bottom = 0;
    float width() const { return right - left; }
    float height() const { return bottom - top; }
};

// Reachable resizing even if an overlapping row is the DOM hover target.
inline bool resizeCorner(const Rect& r, float x, float y)
{
    return x >= r.right()-20.f && x <= r.right() && y >= r.bottom()-20.f && y <= r.bottom();
}

// Subtract opaque window coverage from a classic embedded view. Fixed stack capacity;
// on overflow, suppress the overlay rather than drawing through a foreground window.
inline bool excludeOverlay(std::array<Rect,64>& pieces, size_t& count, const Rect& cover)
{
    std::array<Rect,64> next{};
    size_t used = 0;
    const auto add = [&](Rect r) { if (r.w <= 0 || r.h <= 0) return true;
        if (used == next.size()) return false;
        next[used++] = r; return true; };
    for (size_t i = 0; i < count; ++i)
    {
        const Rect a = pieces[i];
        const float l = std::max(a.x, cover.x), t = std::max(a.y, cover.y);
        const float r = std::min(a.right(), cover.right()), b = std::min(a.bottom(), cover.bottom());
        if (l >= r || t >= b) { if (!add(a)) { count=0; return false; } continue; }
        if (!add({a.x,a.y,a.w,t-a.y}) || !add({a.x,b,a.w,a.bottom()-b})
            || !add({a.x,t,l-a.x,b-t}) || !add({r,t,a.right()-r,b-t})) { count=0; return false; }
    }
    pieces = next; count = used; return true;
}

// Smaller than the screen, at least the minimum (the screen wins when it is smaller still), and
// all of it on the screen.
inline Rect clampToScreen(Rect r, float minW, float minH, const Screen& s)
{
    r.w = std::max(r.w, minW);
    r.h = std::max(r.h, minH);
    r.w = std::min(r.w, std::max(1.f, s.width()));
    r.h = std::min(r.h, std::max(1.f, s.height()));
    r.x = std::clamp(r.x, s.left, s.right - r.w);
    r.y = std::clamp(r.y, s.top, s.bottom - r.h);
    r.x = std::round(r.x);
    r.y = std::round(r.y);
    r.w = std::round(r.w);
    r.h = std::round(r.h);
    return r;
}

namespace detail
{
// The best (nearest within kSnapDp) shift that puts `edge` on one of `targets`; 0 when none.
inline float bestShift(float edge, const std::vector<float>& targets, float& best)
{
    float shift = 0.f;
    for (float t : targets)
    {
        const float d = t - edge;
        if (std::fabs(d) <= kSnapDp && std::fabs(d) < best)
        {
            best  = std::fabs(d);
            shift = d;
        }
    }
    return shift;
}
// Another window is near enough on the other axis for its edges to count (overlapping, or
// within the snap distance).
inline bool spans(float a0, float a1, float b0, float b1) { return a0 <= b1 + kSnapDp && b0 <= a1 + kSnapDp; }
} // namespace detail

// A window being moved to `r`: its edges snap to the screen's and the other windows' edges.
inline Rect snapMove(Rect r, const std::vector<Rect>& others, const Screen& s)
{
    std::vector<float> xs = { s.left, s.right }, ys = { s.top, s.bottom };
    std::vector<float> xsR = xs, ysB = ys; // targets for the right / bottom edges
    for (const Rect& o : others)
    {
        if (detail::spans(r.y, r.bottom(), o.y, o.bottom()))
        {
            xs.push_back(o.right()); // left edge against its right edge
            xs.push_back(o.x);       // left edges lined up
            xsR.push_back(o.x);      // right edge against its left edge
            xsR.push_back(o.right());
        }
        if (detail::spans(r.x, r.right(), o.x, o.right()))
        {
            ys.push_back(o.bottom());
            ys.push_back(o.y);
            ysB.push_back(o.y);
            ysB.push_back(o.bottom());
        }
    }
    float bestX = kSnapDp + 1.f, bestY = kSnapDp + 1.f;
    float dx  = detail::bestShift(r.x, xs, bestX);
    float dxR = detail::bestShift(r.right(), xsR, bestX);
    if (dxR != 0.f)
        dx = dxR;
    float dy  = detail::bestShift(r.y, ys, bestY);
    float dyB = detail::bestShift(r.bottom(), ysB, bestY);
    if (dyB != 0.f)
        dy = dyB;
    r.x += dx;
    r.y += dy;
    return r;
}

// A window being resized (its right / bottom edge moving) to `r`: that edge snaps.
inline Rect snapResize(Rect r, const std::vector<Rect>& others, const Screen& s)
{
    std::vector<float> xs = { s.right }, ys = { s.bottom };
    for (const Rect& o : others)
    {
        if (detail::spans(r.y, r.bottom(), o.y, o.bottom()))
        {
            xs.push_back(o.x);
            xs.push_back(o.right());
        }
        if (detail::spans(r.x, r.right(), o.x, o.right()))
        {
            ys.push_back(o.y);
            ys.push_back(o.bottom());
        }
    }
    float bx = kSnapDp + 1.f, by = kSnapDp + 1.f;
    r.w += detail::bestShift(r.right(), xs, bx);
    r.h += detail::bestShift(r.bottom(), ys, by);
    return r;
}

// Where a new window goes when it has no saved place: its default, moved down-right by one title
// height per window already open there (cascade), within the screen.
inline Rect cascade(Rect r, const std::vector<Rect>& open, float step, const Screen& s)
{
    for (int i = 0; i < 16; ++i)
    {
        const bool taken = std::any_of(open.begin(), open.end(),
                                       [&](const Rect& o) { return std::fabs(o.x - r.x) < 2.f && std::fabs(o.y - r.y) < 2.f; });
        if (!taken || r.right() + step > s.right || r.bottom() + step > s.bottom)
            break;
        r.x += step;
        r.y += step;
    }
    return r;
}

// One window's saved state.
struct Saved
{
    std::string key;
    Rect        rect;
    bool        open = false, minimised = false;
};

inline bool validKey(const std::string& key)
{
    return !key.empty() && key.size() <= 64 && key.find_first_of(" ;\t\n") == std::string::npos;
}

inline std::string encode(const std::vector<Saved>& all)
{
    std::string out;
    char        buf[160];
    for (const Saved& s : all)
    {
        if (!validKey(s.key))
            continue;
        std::snprintf(buf, sizeof(buf), " %d %d %d %d %d %d;", int(std::lround(s.rect.x)), int(std::lround(s.rect.y)),
                      int(std::lround(s.rect.w)), int(std::lround(s.rect.h)), s.open ? 1 : 0, s.minimised ? 1 : 0);
        out += s.key;
        out += buf;
    }
    return out;
}

inline std::vector<Saved> decode(const std::string& text)
{
    std::vector<Saved> out;
    size_t             from = 0;
    while (from < text.size() && out.size() < 256)
    {
        size_t end = text.find(';', from);
        if (end == std::string::npos)
            end = text.size();
        const std::string entry = text.substr(from, end - from);
        from                    = end + 1;
        char key[65]            = {};
        int  x, y, w, h, open, mini;
        if (std::sscanf(entry.c_str(), " %64s %d %d %d %d %d %d", key, &x, &y, &w, &h, &open, &mini) != 7)
            continue;
        if (w <= 0 || h <= 0 || w > 100000 || h > 100000 || std::abs(x) > 100000 || std::abs(y) > 100000)
            continue;
        Saved s;
        s.key       = key;
        s.rect      = { float(x), float(y), float(w), float(h) };
        s.open      = open != 0;
        s.minimised = mini != 0;
        if (!validKey(s.key))
            continue;
        // a later entry for the same key wins
        auto same = std::find_if(out.begin(), out.end(), [&](const Saved& o) { return o.key == s.key; });
        if (same != out.end())
            *same = s;
        else
            out.push_back(s);
    }
    return out;
}

// Replaces (or adds) one window's entry.
inline void upsert(std::vector<Saved>& all, const Saved& s)
{
    auto it = std::find_if(all.begin(), all.end(), [&](const Saved& o) { return o.key == s.key; });
    if (it != all.end())
        *it = s;
    else
        all.push_back(s);
}

// Z order: front last. Raising moves `id` to the end; unknown ids are added there.
inline void raise(std::vector<int>& order, int id)
{
    order.erase(std::remove(order.begin(), order.end(), id), order.end());
    order.push_back(id);
}

// The pull-up tool grid (llnewuitools.cpp): open while hovered (the tab or the grid), locked open,
// or held (push to talk); after the pointer leaves, it stays open for the grace time.
struct PullUp
{
    double graceSec   = 0.3;
    double leftAt     = -1.0; // when the pointer left (or -1: over it)
    bool   open       = false;

    // `over`: the pointer is over the tab or the grid. True when `open` changed.
    bool update(bool over, bool locked, bool held, double now)
    {
        const bool before = open;
        if (locked || held || over)
        {
            open   = true;
            leftAt = -1.0;
        }
        else if (open)
        {
            if (leftAt < 0.0)
                leftAt = now;
            else if (now - leftAt >= graceSec)
            {
                open   = false;
                leftAt = -1.0;
            }
        }
        return open != before;
    }
    // Still counting down: the caller keeps refreshing until it has run.
    bool pending() const { return open && leftAt >= 0.0; }
};
} // namespace slwin
