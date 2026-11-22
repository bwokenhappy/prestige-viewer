// Tests for indra/newview/vkgfx/llvkframegenlabels.h: how far a label moved since the last real frame,
// packing labels into the atlas, the atlas size, the tiles each label can reach, the in-between phase.
// Built and run by scripts/check/checks/framegen_labels.py.
#include "vkgfx/llvkframegenlabels.h"

#include <cstddef>
#include <cstdio>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using namespace llfgl;

static bool overlap(const Rect& a, const Rect& b)
{
    return a.x0 < b.x1 && b.x0 < a.x1 && a.y0 < b.y1 && b.y0 < a.y1;
}

int main()
{
    const Extent screen{2560, 1440};

    {   // history_first_sighting_steps
        History h;
        bool s = false;
        const Vec2 m = h.motion(7, {100, 100}, 10, screen, &s);
        CHECK(m.x == 0.f && m.y == 0.f);
        CHECK(s);
    }
    {   // history_moves
        History h;
        h.record(7, {100, 100}, 10);
        h.endFrame(10);
        bool s = true;
        const Vec2 m = h.motion(7, {110, 95}, 11, screen, &s);
        CHECK(m.x == 10.f && m.y == -5.f);
        CHECK(!s);
    }
    {   // history_stale_steps: last seen two frames ago
        History h;
        h.record(7, {100, 100}, 8);
        h.endFrame(8);
        bool s = false;
        const Vec2 m = h.motion(7, {110, 100}, 11, screen, &s);
        CHECK(m.x == 0.f && m.y == 0.f);
        CHECK(s);
    }
    {   // history_teleport_steps: more than a quarter of the larger side (640 px)
        History h;
        h.record(7, {0, 100}, 10);
        h.record(8, {0, 200}, 10);
        h.endFrame(10);
        bool s = false;
        Vec2 m = h.motion(7, {641, 100}, 11, screen, &s);
        CHECK(m.x == 0.f && m.y == 0.f && s);
        m = h.motion(8, {640, 200}, 11, screen, &s);
        CHECK(m.x == 640.f && m.y == 0.f && !s);
    }
    {   // history_forgets: an id not seen in a frame is gone after it
        History h;
        h.record(7, {100, 100}, 10);
        h.record(9, {300, 100}, 10);
        h.endFrame(10);
        h.record(9, {305, 100}, 11);
        h.endFrame(11);
        bool s = false;
        Vec2 m = h.motion(7, {100, 100}, 12, screen, &s);
        CHECK(s && m.x == 0.f);
        m = h.motion(9, {310, 100}, 12, screen, &s);
        CHECK(!s && m.x == 5.f);
        CHECK(h.size() == 1);
    }
    {   // packer_fills_and_refuses
        Packer p({1024, 512});
        int32_t x0, y0, x1, y1, x2, y2;
        CHECK(p.place(600, 200, x0, y0));
        CHECK(p.place(600, 200, x1, y1));   // a new shelf (600 + 1 + 600 > 1024)
        CHECK(!p.place(600, 200, x2, y2));  // 512 - 2 * 201 < 201
        const Rect a{x0, y0, x0 + 600, y0 + 200}, b{x1, y1, x1 + 600, y1 + 200};
        CHECK(!overlap(a, b));
        CHECK(y1 >= y0 + 201);              // 1 px gutter
        CHECK(x0 >= 0 && y0 >= 0 && b.x1 <= 1024 && b.y1 <= 512);
        int32_t x3, y3;
        CHECK(p.place(400, 50, x3, y3));    // fits beside on the first shelf
        const Rect c{x3, y3, x3 + 400, y3 + 50};
        CHECK(!overlap(a, c) && !overlap(b, c) && c.x1 <= 1024);
        CHECK(!p.place(2000, 10, x2, y2));  // wider than the atlas
        CHECK(!p.place(0, 10, x2, y2));     // empty
    }
    {   // atlas_grows_and_caps
        Extent e = nextAtlasSize(kAtlasStart, true, 0);
        CHECK(e.w == 1024 && e.h == 1024);
        e = nextAtlasSize(e, true, 0);
        CHECK(e.w == 2048 && e.h == 1024);
        e = nextAtlasSize(e, true, 0);
        CHECK(e.w == 2048 && e.h == 1024);
    }
    {   // atlas_shrinks_after_idle
        Extent e = nextAtlasSize({2048, 1024}, false, kIdleShrinkFrames - 1);
        CHECK(e.w == 2048 && e.h == 1024);
        e = nextAtlasSize({2048, 1024}, false, kIdleShrinkFrames);
        CHECK(e.w == 1024 && e.h == 1024);
        e = nextAtlasSize({1024, 1024}, false, kIdleShrinkFrames);
        CHECK(e.w == 1024 && e.h == 512);
        e = nextAtlasSize(kAtlasStart, false, kIdleShrinkFrames);
        CHECK(e.w == 1024 && e.h == 512);
        e = nextAtlasSize({2048, 1024}, true, kIdleShrinkFrames);   // overflow wins
        CHECK(e.w == 2048 && e.h == 1024);
        // smallerSize: the step down (or the start size itself)
        e = smallerSize({2048, 1024});
        CHECK(e.w == 1024 && e.h == 1024);
        e = smallerSize(kAtlasStart);
        CHECK(e.w == 1024 && e.h == 512);
    }
    {   // sweep_unions_and_clips
        Rect r = sweep({100, 100, 200, 140}, {30, -10}, screen);   // was at cur - motion = (70..170, 110..150)
        CHECK(r.x0 == 70 && r.y0 == 100 && r.x1 == 200 && r.y1 == 150);
        r = sweep({10, 10, 50, 50}, {40, 40}, screen);
        CHECK(r.x0 == 0 && r.y0 == 0 && r.x1 == 50 && r.y1 == 50);
        r = sweep({2540, 1430, 2600, 1460}, {0, 0}, screen);
        CHECK(r.x1 == 2560 && r.y1 == 1440);
        r = sweep({10, 10, 20, 20}, {0.4f, -0.6f}, screen);          // sub-pixel: rounded outward
        CHECK(r.x0 == 9 && r.y0 == 10 && r.x1 == 20 && r.y1 == 21);
    }
    {   // bin_orders_and_caps
        std::vector<Rect> s(17, Rect{2, 2, 10, 10});
        std::vector<bool> acc;
        Bins b = bin(s, screen, acc);
        CHECK(b.tilesX == 40 && b.tilesY == 23);
        CHECK(acc.size() == 17);
        for (int i = 0; i < 16; ++i) CHECK(acc[i]);
        CHECK(!acc[16]);
        CHECK(b.counts[0] == 16);
        for (uint32_t i = 0; i < 16; ++i) CHECK(b.idx[i] == i);   // draw order kept

        std::vector<Rect> two{Rect{60, 2, 70, 10}};               // spans tiles 0 and 1
        b = bin(two, screen, acc);
        CHECK(acc[0] && b.counts[0] == 1 && b.counts[1] == 1);
        CHECK(b.idx[0 * kMaxPerTile] == 0 && b.idx[1 * kMaxPerTile] == 0);

        std::vector<Rect> many;
        for (uint32_t i = 0; i < kMaxLabels + 1; ++i)
        {
            const int32_t x = int32_t((i % 40) * 64), y = int32_t((i / 40) * 64);
            many.push_back({x + 1, y + 1, x + 9, y + 9});
        }
        b = bin(many, screen, acc);
        for (uint32_t i = 0; i < kMaxLabels; ++i) CHECK(acc[i]);
        CHECK(!acc[kMaxLabels]);

        std::vector<Rect> off{Rect{3000, 10, 3100, 20}, Rect{5, 5, 5, 9}};   // off screen; empty
        b = bin(off, screen, acc);
        CHECK(!acc[0] && !acc[1]);
    }
    {   // phase_values
        CHECK(phase(1, 1) == 0.5f);
        CHECK(phase(3, 3) == 0.75f);
        CHECK(phase(1, 3) == 0.25f);
    }
    {   // gpu_layout
        CHECK(sizeof(GpuLabel) == 32);
        CHECK(sizeof(GpuHeader) == 16);
        CHECK(offsetof(GpuLabel, atlas) == 16);
        CHECK(offsetof(GpuLabel, motion) == 24);
        // buffer layout: header, counts[tiles], idx[tiles * kMaxPerTile] as uint32, labels
        const Extent e{2560, 1440};
        const uint32_t tiles = 40 * 23;
        CHECK(labelsOffset(e) == 16 + tiles * 4 + tiles * kMaxPerTile * 4);
        CHECK(labelsOffset(e) % 16 == 0);
        CHECK(bufferBytes(e) == labelsOffset(e) + kMaxLabels * 32);
    }
    if (failures) { std::printf("%d failure(s)\n", failures); return 1; }
    std::printf("all label rules hold\n");
    return 0;
}
