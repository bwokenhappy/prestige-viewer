// #703 Tests for the new interface's window geometry (indra/newview/llnewuiwindowlayout.h): clamp on
// screen, snapping to screen and window edges, the saved-state string, z order, the pull-up grid.
#include "llnewuiwindowlayout.h"
#include "llnewuihistoryscroll.h"

#include <cstdio>

namespace
{
int gFailures = 0;
void expect(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++gFailures;
    }
}
bool near1(float a, float b) { return std::fabs(a - b) < 0.51f; }
} // namespace

int main()
{
    using namespace slwin;
    const Screen screen{ 0, 24, 1920, 1080 };

    // The bounded history scrollbar: newest at bottom, oldest at top. Pointer delta
    // is LLView Y-up; reaching either track endpoint cannot overscroll the source.
    {
        const auto newest = historyThumb(500, 0, 15.f);
        const auto oldest = historyThumb(500, 499, 15.f);
        expect(newest.height == 8.f && newest.top == 92.f, "history: minimum-size thumb at newest");
        expect(oldest.top == 0.f, "history: oldest at track top");
        expect(historyDragBack(0, 200.f, 200.f, newest, 500) == 499, "history: drag upward to oldest, bounded");
        expect(historyDragBack(499, -200.f, 200.f, oldest, 500) == 0, "history: drag downward to newest, bounded");
        expect(historyDragBack(123, 0.f, 200.f, newest, 500) == 123, "history: zero drag retains offset");
        expect(historyThumb(0, 0, 15.f).height == 0 && historyThumb(1, 0, .5f).height == 0, "history: empty and single never divide by zero");
        expect(historyThumb(10, 0, 15.f).height == 0, "history: no scrollbar when everything fits");
        expect(historyDragBack(9999, 10.f, 0.f, newest, 500) == 499, "history: zero-height track clamps invalid offset");
        for (size_t i = 1; i < 500; ++i)
            expect(historyThumb(500, i, 15.f).top <= historyThumb(500, i - 1, 15.f).top, "history: thumb position monotonic");
    }

    // Resize targets remain reachable above any bottom-row control, at non-zero origins.
    expect(resizeCorner({100,200,340,480},435,675), "resize: bottom right corner");
    expect(!resizeCorner({100,200,340,480},410,675), "resize: does not steal the neighbouring row");
    expect(!resizeCorner({100,200,340,480},445,675), "resize: outside window is never captured");
    {
        std::array<Rect,64> pieces{}; pieces[0]={0,0,100,100}; size_t count=1;
        expect(excludeOverlay(pieces,count,{25,25,50,50}), "embedded map subtraction bounded");
        float area=0;
        for(size_t i=0;i<count;++i) { area+=pieces[i].w*pieces[i].h;
            expect(pieces[i].right()<=25 || pieces[i].x>=75 || pieces[i].bottom()<=25 || pieces[i].y>=75,
                   "embedded map never paints through foreground"); }
        expect(count==4 && near1(area,7500), "embedded map preserves all exposed area");
        expect(excludeOverlay(pieces,count,{-10,-10,120,120}) && count==0, "fully obscured map draws nothing");
        pieces.fill({0,0,100,100}); count=64;
        expect(!excludeOverlay(pieces,count,{25,25,50,50}) && count==0, "fragment capacity fails closed");
    }

    // ---- clamp ----
    {
        Rect r = clampToScreen({ -50, -50, 400, 300 }, 200, 120, screen);
        expect(r.x == 0 && r.y == 24, "clamp: a window off the top-left comes back on screen (below the menu bar)");
        r = clampToScreen({ 1800, 1000, 400, 300 }, 200, 120, screen);
        expect(r.right() == 1920 && r.bottom() == 1080, "clamp: a window off the bottom-right comes back on screen");
        r = clampToScreen({ 10, 30, 50, 40 }, 200, 120, screen);
        expect(r.w == 200 && r.h == 120, "clamp: never smaller than the minimum");
        r = clampToScreen({ 0, 0, 5000, 5000 }, 200, 120, screen);
        expect(r.w == 1920 && r.h == 1056 && r.x == 0 && r.y == 24, "clamp: never larger than the screen");
        const Screen tiny{ 0, 0, 150, 100 };
        r = clampToScreen({ 0, 0, 400, 300 }, 200, 120, tiny);
        expect(r.w == 150 && r.h == 100, "clamp: the screen wins over the minimum when smaller (UI scale up, small window)");
        // A smaller screen (UI scale raised): the same saved rect lands inside it.
        const Screen small{ 0, 24, 1280, 720 };
        r = clampToScreen({ 1500, 900, 400, 300 }, 200, 120, small);
        expect(r.right() <= 1280 && r.bottom() <= 720, "clamp: a saved rect from a larger screen stays on the smaller one");
    }

    // ---- snapping to the screen ----
    {
        Rect r = snapMove({ 7, 400, 300, 200 }, {}, screen);
        expect(r.x == 0, "snap: left edge within 10 dp of the screen edge lands on it");
        r = snapMove({ 1925 - 300 - 8, 400, 300, 200 }, {}, screen);
        expect(near1(r.right(), 1920), "snap: right edge lands on the screen's right edge");
        r = snapMove({ 30, 400, 300, 200 }, {}, screen);
        expect(r.x == 30, "snap: no snap farther than 10 dp");
        r = snapMove({ 500, 30, 300, 200 }, {}, screen);
        expect(r.y == 24, "snap: top edge lands under the menu bar");
    }

    // ---- snapping to windows ----
    {
        const std::vector<Rect> others = { { 100, 100, 300, 400 } };
        Rect r = snapMove({ 405, 150, 200, 200 }, others, screen);
        expect(r.x == 400, "snap: left edge against another window's right edge");
        r = snapMove({ 106, 505, 200, 100 }, others, screen);
        expect(r.x == 100 && r.y == 500, "snap: under another window, left edges lined up");
        r = snapMove({ 405, 900, 200, 100 }, others, screen);
        expect(r.x == 405, "snap: a window far away on the other axis does not attract");
        r = snapResize({ 420, 100, 296, 200 }, { { 720, 100, 200, 200 } }, screen);
        expect(r.right() == 720, "snap: a resized right edge lands on the neighbour's left edge");
        r = snapResize({ 10, 30, 300, 1046 }, {}, screen);
        expect(near1(r.bottom(), 1080), "snap: a resized bottom edge lands on the screen's bottom");
    }

    // ---- cascade ----
    {
        const std::vector<Rect> open = { { 100, 100, 300, 300 }, { 128, 128, 300, 300 } };
        Rect r = cascade({ 100, 100, 300, 300 }, open, 28, screen);
        expect(r.x == 156 && r.y == 156, "cascade: a second instance opens down-right of the first ones");
    }

    // ---- persistence ----
    {
        std::vector<Saved> all = { { "chat", { 10, 40, 420, 380 }, true, false },
                                   { "inventory", { 900, 60, 360, 600 }, false, true },
                                   { "bad key", { 1, 1, 1, 1 }, true, false } };
        const std::string text = encode(all);
        expect(text.find("bad key") == std::string::npos, "persist: a key with a space is never written");
        std::vector<Saved> back = decode(text);
        expect(back.size() == 2, "persist: both valid windows read back");
        expect(back.size() == 2 && back[0].key == "chat" && back[0].rect == Rect{ 10, 40, 420, 380 } && back[0].open && !back[0].minimised,
               "persist: chat's rect and open state round-trip");
        expect(back.size() == 2 && back[1].key == "inventory" && !back[1].open && back[1].minimised,
               "persist: inventory's closed + minimised state round-trips");
        back = decode("chat 1 2 3;junk;people 5 6 300 200 1 0;people 7 8 310 210 0 0;inventory 0 0 -5 10 1 0");
        expect(back.size() == 1 && back[0].key == "people" && back[0].rect.x == 7 && !back[0].open,
               "persist: bad entries are skipped, the later entry for a key wins");
        upsert(back, { "people", { 1, 2, 300, 300 }, true, false });
        upsert(back, { "chat", { 3, 4, 300, 300 }, true, false });
        expect(back.size() == 2 && back[0].rect.x == 1 && back[1].key == "chat", "persist: upsert replaces or appends");
        expect(decode("").empty(), "persist: an empty setting is no windows");
    }

    // ---- z order ----
    {
        std::vector<int> order = { 1, 2, 3 };
        raise(order, 1);
        expect(order == std::vector<int>({ 2, 3, 1 }), "z: raising moves a window to the front (end)");
        raise(order, 7);
        expect(order.back() == 7 && order.size() == 4, "z: a new window goes to the front");
    }

    // ---- the pull-up tool grid ----
    {
        PullUp p;
        expect(!p.update(false, false, false, 0.0) && !p.open, "pull-up: closed while not hovered");
        expect(p.update(true, false, false, 0.1) && p.open, "pull-up: hovering the tab opens it");
        expect(!p.update(false, false, false, 0.2) && p.open && p.pending(), "pull-up: leaving starts the grace time");
        expect(!p.update(true, false, false, 0.35) && p.open && !p.pending(), "pull-up: coming back within the grace time keeps it open");
        p.update(false, false, false, 0.4);
        expect(!p.update(false, false, false, 0.6) && p.open, "pull-up: still open before 300 ms");
        expect(p.update(false, false, false, 0.75) && !p.open, "pull-up: closes after 300 ms away");
        expect(p.update(false, true, false, 1.0) && p.open, "pull-up: locked is open without hover");
        expect(!p.update(false, true, false, 5.0) && p.open, "pull-up: locked stays open");
        p.update(false, false, true, 6.0);
        expect(!p.update(false, false, true, 7.0) && p.open, "pull-up: a held tool (push to talk) keeps it open");
    }

    if (gFailures)
    {
        std::printf("%d failure(s)\n", gFailures);
        return 1;
    }
    std::printf("newui window layout: all tests pass\n");
    return 0;
}
