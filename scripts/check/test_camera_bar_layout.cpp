// Tests for indra/newview/slcamerabarlayout.h: camera mode's quick-shot bar (#1572). The shutter sits
// exactly on the window's centre line, the two sides are mirrored in width, labels shrink one type step
// before they truncate, and the hint row shows for 8 s.
// Built and run by scripts/check/checks/camera_bar_layout.py.
#include "slcamerabarlayout.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static bool near(float a, float b, float eps = 0.5f) { return std::fabs(a - b) <= eps; }

static void shutterCentredAtAnyWidth()
{
    for (float w : { 1280.f, 1920.f, 2561.f })
        for (float dp : { 1.f, 1.5f })
        {
            const slcambar::Bar b = slcambar::layoutBar(w, dp);
            CHECK(near(b.shutter.x + b.shutter.w / 2, w / 2));
            CHECK(near(b.left + b.width / 2, w / 2));
        }
}

static void sidesMirrored()
{
    for (float dp : { 1.f, 1.5f })
    {
        const slcambar::Bar b = slcambar::layoutBar(1920.f, dp);
        CHECK(near(b.leave.w, 104 * dp) && near(b.controls.w, 104 * dp));
        CHECK(near(b.saveTo.w, 168 * dp) && near(b.size.w, 168 * dp));
        CHECK(near(b.shutter.w, 46 * dp));
        CHECK(near(b.shutter.x - (b.saveTo.x + b.saveTo.w), b.size.x - (b.shutter.x + b.shutter.w)));
        CHECK(near(b.shutter.x - (b.saveTo.x + b.saveTo.w), 12 * dp));
        CHECK(near(b.leave.x - b.left, 6 * dp));
        CHECK(near(b.left + b.width - (b.controls.x + b.controls.w), 6 * dp));
        CHECK(near(b.saveTo.x, b.leave.x + b.leave.w) && near(b.controls.x, b.size.x + b.size.w));
    }
}

static void barWidth()
{
    for (float dp : { 1.f, 1.5f })
        CHECK(near(slcambar::layoutBar(1920.f, dp).width, (2 * 6 + 2 * 104 + 2 * 168 + 46 + 2 * 12) * dp));
}

static void labelStepShrinksBeforeTruncating()
{
    CHECK(slcambar::labelStep(160, 168) == 0);
    CHECK(slcambar::labelStep(168, 168) == 0);
    CHECK(180.f * 11 / 13 <= 168.f);
    CHECK(slcambar::labelStep(180, 168) == 1);
    CHECK(slcambar::labelStep(260, 168) == 2);
}

// The bar's labels are 15 dp and shrink to 13 dp: the step is judged at those sizes.
static void labelStepCustomSizes()
{
    CHECK(slcambar::labelStep(150, 160, 15, 13) == 0);
    CHECK(slcambar::labelStep(170, 160, 15, 13) == 1);  // 170 * 13 / 15 = 147
    CHECK(slcambar::labelStep(200, 160, 15, 13) == 2);  // 173 still too wide
}

static void hintEightSeconds()
{
    CHECK(slcambar::hintVisible(0.0, 0.0));
    CHECK(slcambar::hintVisible(7.9, 0.0));
    CHECK(!slcambar::hintVisible(8.0, 0.0));
    CHECK(!slcambar::hintVisible(-0.1, 0.0));
    CHECK(slcambar::hintVisible(105.0, 100.0));
}

int main()
{
    shutterCentredAtAnyWidth();
    sidesMirrored();
    barWidth();
    labelStepShrinksBeforeTruncating();
    labelStepCustomSizes();
    hintEightSeconds();
    if (failures)
        std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
