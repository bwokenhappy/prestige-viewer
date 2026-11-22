// Tests for indra/newview/llvrwristgrid.h (the wrist panel's button grid) and
// indra/newview/vkgfx/llvrspacewarpstatus.h (space warp's plain-language status).
// Built and run by scripts/check/checks/vr_wrist_warp.py.
#include "llvrwristgrid.h"
#include "vkgfx/llvrspacewarpstatus.h"

#include <cstdio>
#include <string>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static bool has(const std::string& s, const char* needle)
{
    return s.find(needle) != std::string::npos;
}

int main()
{
    // ---- the grid: any number of buttons, never more than `columns` in a row, nothing lost
    for (int cols = 1; cols <= 6; ++cols)
    {
        for (int n = 0; n <= 12; ++n)
        {
            const std::vector<int> rows = llvrwrist::gridRows(n, cols);
            int sum = 0, lo = 99, hi = 0;
            for (int c : rows)
            {
                sum += c;
                CHECK(c >= 1 && c <= cols);
                lo = c < lo ? c : lo;
                hi = c > hi ? c : hi;
            }
            CHECK(sum == n);
            CHECK(n == 0 ? rows.empty() : (int)rows.size() == (n + cols - 1) / cols);   // as few rows as the limit allows
            CHECK(rows.empty() || hi - lo <= 1);                                          // evenly spread
        }
    }
    {
        const std::vector<int> six = llvrwrist::gridRows(6, 3);
        CHECK(six.size() == 2 && six[0] == 3 && six[1] == 3);
        const std::vector<int> seven = llvrwrist::gridRows(7, 3);
        CHECK(seven.size() == 3 && seven[0] == 3 && seven[1] == 2 && seven[2] == 2);
        const std::vector<int> five = llvrwrist::gridRows(5, 3);
        CHECK(five.size() == 2 && five[0] == 3 && five[1] == 2);
    }

    // ---- space warp status wording
    using llvrsw::StatusIn;
    {
        StatusIn s;
        s.mode = 0;
        CHECK(has(llvrsw::statusText(s), "off"));
        CHECK(llvrsw::shortText(s).empty());
    }
    {
        StatusIn s;   // never waited a VR frame
        CHECK(has(llvrsw::statusText(s), "headset only"));
    }
    {
        StatusIn s;   // the owner's case: always, in a mono session
        s.seen = true;
        s.mode = 2;
        s.stereo = false;
        s.timing = true;
        const std::string t = llvrsw::statusText(s);
        CHECK(has(t, "not available"));
        CHECK(has(t, "layered stereo"));
        CHECK(llvrsw::shortText(s).empty());
    }
    {
        StatusIn s;
        s.seen = s.stereo = s.timing = true;
        s.mode = 2;
        s.engaged = true;
        s.realHz = 44.6f;
        s.shownHz = 90.2f;
        s.gpuMs = 14.3f;
        s.budgetMs = 8.33f;
        const std::string t = llvrsw::statusText(s);
        CHECK(has(t, "on, rendering 45 of 90 frames per second"));
        CHECK(has(t, "14.3 ms"));
        CHECK(llvrsw::shortText(s) == "Warp 45 / 90");
        s.mode = 1;
        CHECK(has(llvrsw::statusText(s), "on (automatic)"));
    }
    {
        StatusIn s;
        s.seen = s.stereo = s.timing = true;
        s.mode = 1;
        s.gpuMs = 5.f;
        s.budgetMs = 8.33f;
        const std::string t = llvrsw::statusText(s);
        CHECK(has(t, "standing by"));
        CHECK(has(t, "headroom ok"));
        CHECK(llvrsw::shortText(s).empty());
        s.gpuMs = 9.f;
        CHECK(!has(llvrsw::statusText(s), "headroom ok"));
    }
    {
        StatusIn s;
        s.seen = s.stereo = true;
        s.mode = 1;
        CHECK(has(llvrsw::statusText(s), "waiting"));
    }

    if (failures) return 1;
    std::printf("ok\n");
    return 0;
}
