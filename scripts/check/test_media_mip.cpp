// Host test of indra/newview/vkgfx/llvkmediamip.h (#1025): the media mip lock K, when a media update rebuilds
// its mip chain, and which texels of each level a dirty rectangle changes. Run by
// scripts/check/checks/media_mip.py.
#include "llvkmediamip.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

static int failures = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++failures; } } while (0)

namespace
{
using namespace llvkmediamip;

bool approx(float a, float b) { return std::fabs(a - b) < 1e-4f; }

// One level as the GPU blit builds it from the level above: an exact 2:1 box when both sides are even (what a
// linear blit at scale 2 samples), any other deterministic resample otherwise (here: nearest of the scaled
// position) -- the property under test is only that a rectangle update leaves every level equal to a rebuild.
struct Level { uint32_t w, h; std::vector<float> t; float at(uint32_t x, uint32_t y) const { return t[y * w + x]; } };

float texel(const Level& s, uint32_t nw, uint32_t nh, uint32_t x, uint32_t y)
{
    if (s.w % 2u == 0u && s.h % 2u == 0u)
        return 0.25f * (s.at(2 * x, 2 * y) + s.at(2 * x + 1, 2 * y) + s.at(2 * x, 2 * y + 1) + s.at(2 * x + 1, 2 * y + 1));
    return s.at((uint32_t)((x + 0.5f) * s.w / nw), (uint32_t)((y + 0.5f) * s.h / nh));
}

std::vector<Level> rebuild(const Level& top)
{
    std::vector<Level> c{top};
    while (c.back().w > 1 || c.back().h > 1)
    {
        const Level& s = c.back();
        Level d{std::max(s.w / 2u, 1u), std::max(s.h / 2u, 1u), {}};
        d.t.resize((size_t)d.w * d.h);
        for (uint32_t y = 0; y < d.h; ++y)
            for (uint32_t x = 0; x < d.w; ++x) d.t[y * d.w + x] = texel(s, d.w, d.h, x, y);
        c.push_back(d);
    }
    return c;
}

// The renderer's RectOnly cascade (recordTexUpload): exact levels rebuild the rectangle, the rest whole levels.
void propagate(std::vector<Level>& c, Rect r)
{
    for (size_t i = 1; i < c.size(); ++i)
    {
        bool exact = false;
        r = nextLevel(c[i - 1].w, c[i - 1].h, r, exact);
        Level& d = c[i];
        if (exact)
        {
            CHECK(r.x + r.w <= d.w && r.y + r.h <= d.h);
            for (uint32_t y = r.y; y < r.y + r.h; ++y)
                for (uint32_t x = r.x; x < r.x + r.w; ++x) d.t[y * d.w + x] = texel(c[i - 1], d.w, d.h, x, y);
        }
        else
        {
            CHECK(r.x == 0 && r.y == 0 && r.w == d.w && r.h == d.h);
            for (uint32_t y = 0; y < d.h; ++y)
                for (uint32_t x = 0; x < d.w; ++x) d.t[y * d.w + x] = texel(c[i - 1], d.w, d.h, x, y);
        }
    }
}

void rectEqualsRebuild(uint32_t w, uint32_t h, uint32_t seed)
{
    std::srand(seed);
    Level top{w, h, std::vector<float>((size_t)w * h)};
    for (float& v : top.t) v = (float)(std::rand() % 1000);
    std::vector<Level> chain = rebuild(top);
    for (int round = 0; round < 6; ++round)
    {
        Rect r;
        r.x = (uint32_t)std::rand() % w; r.y = (uint32_t)std::rand() % h;
        r.w = 1u + (uint32_t)std::rand() % (w - r.x); r.h = 1u + (uint32_t)std::rand() % (h - r.y);
        for (uint32_t y = r.y; y < r.y + r.h; ++y)
            for (uint32_t x = r.x; x < r.x + r.w; ++x) chain[0].t[y * w + x] = (float)(std::rand() % 1000);
        propagate(chain, r);
        const std::vector<Level> want = rebuild(chain[0]);
        bool same = want.size() == chain.size();
        for (size_t i = 0; same && i < want.size(); ++i) same = want[i].t == chain[i].t;
        if (!same) std::printf("rect %ux%u+%u+%u of %ux%u\n", r.w, r.h, r.x, r.y, w, h);
        CHECK(same);
    }
}
} // namespace

int main()
{
    // K: off, clamped to 1..16 texels per pixel.
    CHECK(lockLevels(false, 4.f) == 0.f);
    CHECK(approx(lockLevels(true, 4.f), 2.f));
    CHECK(approx(lockLevels(true, 1.f), 0.f));
    CHECK(lockLevels(true, 0.5f) == 0.f);
    CHECK(approx(lockLevels(true, 100.f), 4.f));

    // The nearest face's level: a face as large as the image is level 0; a 16th of its texels is level 2.
    CHECK(approx(estimatedLevel(1920, 1080, 1920.f * 1080.f), 0.f));
    CHECK(approx(estimatedLevel(1920, 1080, 1920.f * 1080.f / 16.f), 2.f));
    CHECK(estimatedLevel(1920, 1080, 0.f) > 100.f);   // not seen yet: far (the chain is kept)

    // Lock off: every update rebuilds (the old behaviour).
    {
        State s; s.level = 0.f;
        for (uint32_t f = 1; f <= 8; ++f) CHECK(rebuildChain(0.f, f, s));
    }
    // A near media (well inside K = 2) rebuilds on every kNearInterval-th update only.
    {
        State s; s.level = 0.f;
        uint32_t rebuilt = 0;
        for (uint32_t f = 1; f <= 4 * kNearInterval; ++f) rebuilt += rebuildChain(2.f, f, s) ? 1u : 0u;
        CHECK(rebuilt == 4);
    }
    // Within the margin of K (lower mips may be sampled): every update.
    {
        State s; s.level = 2.f - kEstimateMargin + 0.25f;
        for (uint32_t f = 1; f <= 8; ++f) CHECK(rebuildChain(2.f, f, s));
    }
    // After skipped updates a dirty rectangle may not stop at its own texels.
    {
        State s; s.level = 0.f;
        CHECK(rectOnly(s));
        CHECK(!rebuildChain(2.f, 1, s));
        CHECK(!rectOnly(s));
    }
    // A paused media whose chain is behind catches up once, after kIdleCatchUpFrames.
    {
        State s; s.level = 0.f;
        CHECK(!rebuildChain(2.f, 100, s));
        CHECK(!catchUp(100 + kIdleCatchUpFrames - 1, s));
        CHECK(catchUp(100 + kIdleCatchUpFrames, s));
        CHECK(!catchUp(100 + 2 * kIdleCatchUpFrames, s));   // current again: nothing to do
    }
    // A current chain never catches up.
    {
        State s;
        CHECK(!catchUp(1000, s));
    }

    // Dirty rectangles: every level equals a full rebuild.
    rectEqualsRebuild(1920, 1080, 1);
    rectEqualsRebuild(1366, 768, 2);
    rectEqualsRebuild(1024, 1024, 3);
    rectEqualsRebuild(1280, 720, 4);
    rectEqualsRebuild(7, 1, 5);
    rectEqualsRebuild(2, 2, 6);
    // A whole-level rectangle is a whole-level blit.
    {
        bool exact = true;
        const Rect n = nextLevel(64, 64, Rect{0, 0, 64, 64}, exact);
        CHECK(!exact && n.w == 32 && n.h == 32);
    }

    if (failures) std::printf("%d failure(s)\n", failures);
    else std::printf("media mip rules: ok\n");
    return failures ? 1 : 0;
}
