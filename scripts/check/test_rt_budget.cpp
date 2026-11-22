// Regression fixture for the RT memory budget and the RT memory fallback
// (indra/newview/vkgfx/llcleanrtbudget.h). Built and run by scripts/check/checks/rt_budget.py.
//
// 2026-11-09 user report "once RT drops due to VRAM pressure it never returns": Preferences showed
// 4.5 GB in use of a 10.8 GB budget (12 GB "Maximum video memory"), RT 682 MB, nothing reduced for
// memory, and the projector shadow atlas allocated (projectors on shadow maps). Under pressure the
// headroom rule shrank RT's budget to its 384 MB floor, under the working buffers, which tripped
// the over-budget fallback (shadow maps) whose hold doubled with every re-trip while the ladder was
// still recovering (the budget only grows back once the ladder is calm). Checks:
//   1. the automatic budget (headroom rule, pressure cap) never trips the over-budget test, for any
//      working set, usage and device budget, with and without a cluster RT share;
//   2. the user's numbers: under pressure the old floor trips it (the fixture sees the bug), the new
//      one does not; once the ladder is calm the budget grows back to the free VRAM at once, and it
//      is held (headroomHeld) while the ladder is not calm;
//   3. the old fallback policy, in the user's scenario (pressure, then a ladder recovering for minutes,
//      no other RT consumer), keeps RT on shadow maps for minutes after the ladder is calm; the new
//      budget never enters the fallback at all;
//   4. under an explicit budget that is too small, the fallback enters, does not flap (the hold),
//      and lifts within kMinHoldSecs once the budget holds the working set it tripped on.
#include "vkgfx/llcleanrtbudget.h"

#include <cstdio>
#include <cstdlib>

namespace
{
    using namespace LLCleanRTBudget;
    int g_fail = 0;
    void check(bool ok, const char* what)
    {
        if (!ok)
        {
            std::printf("FAIL: %s\n", what);
            ++g_fail;
        }
    }

    // LLVramBudgetVK::rtReserveFor (llvrambudgetvk.h; that header pulls in Vulkan, so the tier
    // formula is restated here -- the lines test checks the original).
    uint64_t reserveFor(uint64_t c)
    {
        uint64_t r = 512 * kMiB;
        if (c >= 4096 * kMiB) r = std::min<uint64_t>(1024 * kMiB, 512 * kMiB + (c - 4096 * kMiB));
        if (c >= 8192 * kMiB) r = std::min<uint64_t>(1536 * kMiB, 1024 * kMiB + (c - 8192 * kMiB));
        if (c >= 16384 * kMiB) r = std::min<uint64_t>(2048 * kMiB, 1536 * kMiB + (c - 16384 * kMiB));
        return std::max<uint64_t>(std::min<uint64_t>(r, c / 4u), c / 32u);
    }
    uint64_t rtReserveFor(uint64_t dev)
    {
        const uint64_t r = reserveFor(dev);
        const uint64_t h = r >= 512 * kMiB ? 128 * kMiB + (r - 512 * kMiB) / 12u : r / 4u;
        return r + 2u * h;
    }

    // The pre-2026-11-09 headroom target: floor 384 MB only.
    uint64_t oldTarget(uint64_t dev, uint64_t usage, uint64_t held, uint64_t reserve)
    {
        const uint64_t other = usage > held ? usage - held : 0;
        const uint64_t room = dev > other + reserve ? dev - other - reserve : 0;
        return std::clamp<uint64_t>(room, kHeadroomFloor, std::max(kHeadroomFloor, dev));
    }

    // The pre-2026-11-09 fallback: lifts only at the end of its hold.
    struct OldFallback
    {
        bool on = false;
        double overSince = -1.0, since = 0.0, lift = -1.0e18, hold = 60.0;
        void step(double now, bool over)
        {
            if (over) { if (overSince < 0.0) overSince = now; }
            else overSince = -1.0;
            if (!on && overSince >= 0.0 && now - overSince >= 3.0)
            {
                if (now - lift < 120.0) hold = std::min(hold * 2.0, 600.0);
                on = true;
                since = now;
            }
            else if (on && now - since >= hold && over) since = now;
            else if (on && now - since >= hold && !over) { on = false; lift = now; }
            if (!on && hold > 60.0 && now - lift > 900.0) hold = 60.0;
        }
    };

    // ---- 1. the automatic budget never trips the over-budget test ----
    void sweepAutoNeverOver()
    {
        bool ok = true;
        for (uint64_t dev : {2048 * kMiB, 4096 * kMiB, 8192 * kMiB, 12288 * kMiB, 24576 * kMiB})
            for (uint64_t aux = 0; aux <= 6144 * kMiB; aux += 16 * kMiB)
                for (uint64_t usage : {dev / 4, dev / 2, dev - dev / 8, dev, dev + dev / 8})
                    for (uint64_t cur : {uint64_t(0), kHeadroomFloor, dev / 2, dev})
                        for (int calm = 0; calm < 2; ++calm)
                            for (uint64_t cap : {uint64_t(0), 64 * kMiB, aux / 2})
                            {
                                const uint64_t held = aux + 256 * kMiB;
                                const uint64_t tgt = headroomTarget(dev, usage, held, rtReserveFor(dev), aux);
                                const uint64_t step = headroomStep(cur, tgt, calm != 0);
                                const uint64_t b = withPressureCap(autoBudget(step, dev, aux), cap, aux);
                                for (uint64_t cl : {uint64_t(0), aux / 3, aux})
                                {
                                    const uint64_t la = aux - cl, lb = b > cl ? b - cl : 0;
                                    if (overEnter(la, lb) || !overLeave(la, lb)) ok = false;
                                }
                            }
        check(ok, "the automatic RT budget (headroom rule + pressure cap) tripped the over-budget test");
    }

    // ---- 2. the user's numbers ----
    void userNumbers()
    {
        const uint64_t dev = 12288 * kMiB;            // 12 GB "Maximum video memory" (B 10.8 GB)
        const uint64_t reserve = rtReserveFor(dev);   // 1536 + 2 x 213 MB
        const uint64_t aux = 420 * kMiB;              // working buffers (of the 682 MB RT figure)
        const uint64_t held = 682 * kMiB;
        // Under pressure (usage at the budget): room ~200 MB.
        const uint64_t old_b = oldTarget(dev, 10800 * kMiB, held, reserve);
        check(old_b == kHeadroomFloor, "reproduction: the old headroom target is not its 384 MB floor under pressure");
        check(overEnter(aux, old_b), "reproduction: the old floor does not trip the over-budget test (fixture lost the bug)");
        uint64_t cur = 6000 * kMiB;
        for (int i = 0; i < 100; ++i)
            cur = headroomStep(cur, headroomTarget(dev, 10800 * kMiB, held, reserve, aux), false);
        const uint64_t b = autoBudget(cur, dev, aux);
        check(!overEnter(aux, b), "under pressure the new budget trips the over-budget test");
        check(b >= workingFloor(aux), "under pressure the new budget is under the working floor");
        // Calm again at 4.5 GB: one step grows it back to the free VRAM (~6.4 GB).
        const uint64_t tgt = headroomTarget(dev, 4608 * kMiB, held, reserve, aux);
        check(tgt >= 6000 * kMiB, "the calm target in the user's case is not ~6.4 GB");
        check(headroomHeld(cur, tgt, false), "growth held by a not-calm ladder is not reported as held");
        check(headroomStep(cur, tgt, false) == cur, "the budget grew while the ladder was not calm");
        check(headroomStep(cur, tgt, true) == tgt, "the budget did not grow back once the ladder was calm");
        check(!headroomHeld(headroomStep(cur, tgt, true), tgt, true), "a calm ladder still reports the budget held");
    }

    // ---- 3. the user's scenario over time ----
    // Pressure 0..400 s (usage at the budget), the ladder recovering until 600 s (usage 4.5 GB,
    // not calm), calm after. The only RT consumers are the shadows (no GI / reflections): in the
    // fallback RT idles and is released after 15 s. Returns when RT traced again for good after
    // the ladder became calm (< 0: it never fell back at all; 1e9: not back by the end).
    double scenario(bool new_policy)
    {
        const uint64_t dev = 12288 * kMiB, reserve = rtReserveFor(dev);
        const double dt = 1.0 / 30.0, t_press_end = 400.0, t_calm = 600.0, t_end = 3000.0;
        Fallback fb;
        OldFallback ofb;
        uint64_t cur = 6000 * kMiB;   // (a calm session before the pressure: RT at the free VRAM)
        bool over = false, released = true, ever = false;
        double idle_since = -1.0, back_at = 1.0e9;
        for (double t = 0.0; t < t_end; t += dt)
        {
            const uint64_t usage = (t < t_press_end ? 10800 : 4608) * kMiB;
            const bool calm = t >= t_calm;
            const bool fb_on = new_policy ? fb.on : ofb.on;
            const bool building = !fb_on;
            uint64_t aux = 0;
            if (building)
            {
                released = false;
                idle_since = -1.0;
                aux = 420 * kMiB;
            }
            else
            {
                if (idle_since < 0.0) idle_since = t;
                if (!released && t - idle_since > 15.0) { released = true; over = false; }
                aux = released ? 0 : 420 * kMiB;
            }
            const uint64_t held = aux ? aux + 262 * kMiB : 0;
            if (building || new_policy)   // (the old rule updated the headroom only while building)
            {
                const uint64_t tgt = new_policy ? headroomTarget(dev, usage, held, reserve, aux)
                                                : oldTarget(dev, usage, held, reserve);
                cur = headroomStep(cur, tgt, calm);
            }
            const uint64_t budget = new_policy ? autoBudget(cur, dev, aux) : (cur ? cur : dev);
            if (building)
            {
                if (!over && overEnter(aux, budget)) over = true;
                else if (over && overLeave(aux, budget)) over = false;
            }
            if (new_policy) fb.step(t, over, aux, budget);
            else ofb.step(t, over);
            const bool on_now = new_policy ? fb.on : ofb.on;
            ever = ever || on_now;
            if (t >= t_calm && !on_now && back_at > 1.0e8) back_at = t;
            if (on_now) back_at = 1.0e9;
        }
        if (!ever) return -1.0;
        return back_at > 1.0e8 ? 1.0e9 : back_at - t_calm;
    }

    void scenarios()
    {
        const double old_delay = scenario(false);
        const double new_delay = scenario(true);
        std::printf("user scenario: old policy back %.0f s after the ladder calmed; new policy %s\n", old_delay,
                    new_delay < 0.0 ? "never fell back" : "fell back");
        check(old_delay > 120.0, "reproduction: the old policy came back within 2 min of calm (fixture lost the bug)");
        check(new_delay < 0.0, "the new automatic budget entered the RT memory fallback");
    }

    // ---- 4. an explicit budget too small for the working set ----
    void explicitBudget()
    {
        Fallback fb;
        const double dt = 1.0 / 30.0;
        uint64_t budget = 512 * kMiB;
        const uint64_t working = 400 * kMiB;   // > 70% of 512
        double t = 1000.0;
        int enters = 0, lifts = 0;
        // Over budget while building; released (not over, nothing held) once in the fallback 15 s.
        double idle_since = -1.0;
        for (; t < 1000.0 + 300.0; t += dt)
        {
            const bool building = !fb.on;
            bool over = false;
            uint64_t w = working;
            if (building) { idle_since = -1.0; over = overEnter(w, budget); }
            else
            {
                if (idle_since < 0.0) idle_since = t;
                if (t - idle_since > 15.0) { w = 0; over = false; }
                else over = true;
            }
            const Fallback::Event ev = fb.step(t, over, w, budget);
            if (ev == Fallback::Event::Enter) ++enters;
            if (ev == Fallback::Event::LiftFits || ev == Fallback::Event::LiftHold) ++lifts;
            check(ev != Fallback::Event::LiftFits, "lifted early although the budget does not hold the working set");
        }
        // 300 s: enter at 3 s, lift at 63 s, re-enter at 66 s (hold 120), lift at 186, re-enter (240)
        check(enters >= 2 && enters <= 4, "an explicit too-small budget flapped (or never fell back)");
        check(lifts >= 1 && lifts <= enters, "the fallback never lifted at a hold end (or lifted more often than it entered)");
        check(fb.hold > Fallback::kBaseHoldSecs, "re-trips did not lengthen the hold");
        // The user raises RenderVKRTBlasBudgetMB: lifted within kMinHoldSecs of the fallback start (or at once).
        if (!fb.on) { for (int i = 0; i < 200 && !fb.on; ++i) { t += dt; fb.step(t, true, working, budget); } }
        check(fb.on, "setup: the fallback is not on");
        budget = 2048 * kMiB;
        const double raised = t;
        double lifted = -1.0;
        for (; t < raised + 60.0; t += dt)
        {
            const Fallback::Event ev = fb.step(t, false, 0, budget);
            if (ev == Fallback::Event::LiftFits || ev == Fallback::Event::LiftHold) { lifted = t; break; }
        }
        check(lifted >= 0.0 && lifted - raised <= Fallback::kMinHoldSecs + 0.1,
              "the fallback did not lift within kMinHoldSecs once the budget held the working set");
    }
}

int main()
{
    sweepAutoNeverOver();
    userNumbers();
    scenarios();
    explicitBudget();
    if (g_fail)
    {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("RT budget fixtures passed\n");
    return 0;
}
