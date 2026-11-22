/**
 * @file llcleanrtbudget.h
 * @brief The RT memory budget's arithmetic (LLCleanRTVK budgetBytes / updateHeadroomBudget /
 *        the over-budget test of recordBuild) and the renderer's RT memory fallback (shadow
 *        maps while RT's working buffers do not fit its budget, LLCleanRendererVK::
 *        selectLitVariant). Pure arithmetic (no Vulkan, no viewer state) so the regression
 *        fixture scripts/check/test_rt_budget.cpp drives exactly the code the viewer runs.
 *
 * WHY THE WORKING-SET FLOOR (2026-11-09, user report "once RT drops due to VRAM pressure it
 * never returns": Preferences showed 4.5 GB in use of a 10.8 GB budget, nothing reduced for
 * memory, and the projector shadow atlas allocated -- projectors on shadow maps). The headroom
 * rule shrinks RT's budget to the room VRAM has left; under pressure that room is ~0 and the
 * budget fell to its 384 MB floor. The over-budget test trips when the working buffers (TLAS,
 * scratch, instance / rig / terrain tables, staging, the cluster arenas: several hundred MB in
 * a busy region) exceed 70% of the budget, so every pressure episode also tripped RT's memory
 * fallback: ray-traced sun and projector shadows on shadow maps (allocating the projector
 * atlas, more VRAM, while memory was full) for a hold that doubled on every re-trip up to
 * 10 minutes and re-armed at its end while the budget was still held small (the budget grows
 * back only once the ladder is calm) -- RT came back 10-20 minutes after memory freed, or not
 * at all while episodes kept coming. The VRAM pressure ladder already owns degrading RT under
 * pressure (its STEP_RT rungs cap the budget at workingFloor, never under it, and its
 * projector / sun shadow rungs turn those off, all restored when it calms); the headroom rule
 * was a second, hidden controller that went under that floor. Now the automatic budget never
 * goes under workingFloor either, so over budget can only mean an explicit
 * RenderVKRTBlasBudgetMB too small for the working set; and the fallback lifts as soon as the
 * budget holds the working set it tripped on (after a short minimum), not only at the end of
 * its hold.
 */

#pragma once

#include <algorithm>
#include <cstdint>

namespace LLCleanRTBudget
{
    constexpr uint64_t kMiB = 1ull << 20;
    // The headroom rule's own floor (#1015): never under this, whatever the working set.
    constexpr uint64_t kHeadroomFloor = 384ull * kMiB;
    // Before the renderer has measured the device budget: 8 GB assumed.
    constexpr uint64_t kAssumedDevice = 8ull << 30;

    // The working buffers + a geometry share: working buffers at <= 60% of it, so the
    // over-budget test (enter > 70%, leave <= 60%) can never be tripped by a budget held at this
    // floor -- BLAS keep >= 40% (+32 MB). The pressure cap and the headroom rule both stop here.
    constexpr uint64_t workingFloor(uint64_t aux)
    {
        return std::max<uint64_t>(256ull * kMiB, aux / 6u * 10u + 32ull * kMiB);
    }

    // The headroom rule's target: what RT holds + the budget's free VRAM minus the reserve,
    // i.e. dev - (usage - held) - reserve, between the floors and the device budget.
    constexpr uint64_t headroomTarget(uint64_t dev, uint64_t usage, uint64_t held, uint64_t reserve, uint64_t aux)
    {
        const uint64_t other = usage > held ? usage - held : 0;   // everything that is not RT
        const uint64_t room = dev > other + reserve ? dev - other - reserve : 0;
        const uint64_t lo = std::max(kHeadroomFloor, workingFloor(aux));
        return std::clamp<uint64_t>(room, lo, std::max(lo, dev));
    }

    // Hysteresis (the measured usage moves every frame with texture streaming): grow past +5%
    // and only while the VRAM ladder is calm (RT above the ladder's recovery line must not stall
    // a recovery), shrink under -10% (a shrink evicts BLAS), else hold. cur 0 = not set yet.
    constexpr uint64_t headroomStep(uint64_t cur, uint64_t target, bool ladder_calm)
    {
        if ((cur == 0 || target > cur + cur / 20) && ladder_calm) return std::max(cur, target);
        if (cur != 0 && target + cur / 10 < cur) return target;
        return cur;
    }

    // True while the hysteresis holds the budget under a target it would grow to if the ladder
    // were calm (the readout / log say so: RT waits for the ladder, not for memory).
    constexpr bool headroomHeld(uint64_t cur, uint64_t target, bool ladder_calm)
    {
        return !ladder_calm && cur != 0 && target > cur + cur / 20;
    }

    // The automatic budget (no RenderVKRTBlasBudgetMB): the headroom rule's value (0 = no
    // usage measured yet: the device budget itself), never under the working floor evaluated
    // LIVE (the working buffers can grow between two headroom updates, which the hysteresis
    // would otherwise hold under them).
    constexpr uint64_t autoBudget(uint64_t headroom, uint64_t dev, uint64_t aux)
    {
        const uint64_t b = headroom ? headroom : std::max<uint64_t>(dev ? dev : kAssumedDevice, kHeadroomFloor);
        return std::max(b, workingFloor(aux));
    }

    // The VRAM ladder's RT_BLAS_CAP (0 = none) on a budget: never under the working floor.
    constexpr uint64_t withPressureCap(uint64_t budget, uint64_t cap, uint64_t aux)
    {
        const uint64_t b = cap ? std::min(budget, std::max(cap, workingFloor(aux))) : budget;
        return std::max<uint64_t>(b, 64ull * kMiB);
    }

    // recordBuild's over-budget test on the legacy share (the cluster RT's arenas and buffers
    // are their own third of the budget and are taken off both sides): enter past 70%, leave at
    // or under 60%.
    constexpr bool overEnter(uint64_t legacy_aux, uint64_t legacy_budget) { return legacy_aux > legacy_budget / 10u * 7u; }
    constexpr bool overLeave(uint64_t legacy_aux, uint64_t legacy_budget) { return legacy_aux <= legacy_budget / 10u * 6u; }

    // ---- THE RT MEMORY FALLBACK (renderer, selectLitVariant) ----
    // Over budget for kEnterSecs -> ray-traced sun / projector shadows use the shadow maps
    // (temporary, settings untouched). It lifts:
    //  - as soon as the budget holds the working set it tripped on (<= 60%, the over-budget
    //    leave line) and RT is not over budget, after kMinHoldSecs -- the budget grew (the
    //    setting was raised) or the working set shrank;
    //  - else at the end of the hold when RT is no longer over budget; still over at the end ->
    //    a re-fit (the buffers released and re-created at the current size) and the hold
    //    re-armed. The hold doubles on a re-trip within kFlapSecs of a lift (up to kMaxHoldSecs),
    //    back to kBaseHoldSecs after kQuietSecs without one: an explicit budget that genuinely
    //    does not fit cannot flap RT on and off every few seconds.
    struct Fallback
    {
        static constexpr double kEnterSecs = 3.0;
        static constexpr double kMinHoldSecs = 5.0;
        static constexpr double kBaseHoldSecs = 60.0;
        static constexpr double kMaxHoldSecs = 600.0;
        static constexpr double kFlapSecs = 120.0;
        static constexpr double kQuietSecs = 900.0;
        enum class Event { None, Enter, Refit, LiftFits, LiftHold };   // (scoped: no macro clashes)

        bool     on = false;
        double   overSince = -1.0;   // over budget since (< 0: not over)
        double   since = 0.0;        // the fallback (or its re-armed hold) started
        double   lift = -1.0e18;     // the last lift
        double   hold = kBaseHoldSecs;
        uint64_t tripWorking = 0;    // the working buffers when it tripped (what must fit to lift early)

        static constexpr bool fits(uint64_t working, uint64_t budget) { return working <= budget / 10u * 6u; }

        // One evaluation (once per frame): over = RT's over-budget state, working = its working
        // buffers now, budget = its budget now.
        Event step(double now, bool over, uint64_t working, uint64_t budget)
        {
            Event ev = Event::None;
            if (over) { if (overSince < 0.0) overSince = now; }
            else overSince = -1.0;
            if (!on)
            {
                if (overSince >= 0.0 && now - overSince >= kEnterSecs)
                {
                    if (now - lift < kFlapSecs) hold = std::min(hold * 2.0, kMaxHoldSecs);
                    on = true;
                    since = now;
                    tripWorking = working;
                    ev = Event::Enter;
                }
            }
            else if (!over && now - since >= kMinHoldSecs && fits(tripWorking, budget))
            {
                on = false;
                lift = now;
                ev = Event::LiftFits;
            }
            else if (now - since >= hold)
            {
                if (over) { since = now; ev = Event::Refit; }
                else { on = false; lift = now; ev = Event::LiftHold; }
            }
            if (!on && hold > kBaseHoldSecs && now - lift > kQuietSecs) hold = kBaseHoldSecs;
            return ev;
        }
    };
}
