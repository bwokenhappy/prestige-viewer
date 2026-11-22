#pragma once
// Opt-in CPU recording state; no GPU query or cache behavior changes.
#include "llerror.h"
#include "llframetimer.h"
#include <algorithm>
#include <cstdint>

namespace LLGpuShadowState
{
// Call once on EVERY recordShadowRigCull exit path, main view only. `recorded`
// means a vkCmdDispatch was recorded, not proof that its submission executed.
// Counts cover all observed recordings; no frame-id de-duplication can hide a
// second recording of the same app frame. No allocation, GPU readback or query.
inline void observe(bool enabled, bool offscreen, uint64_t frameSerial,
                    bool sunRt, uint32_t projectorMode, uint32_t views,
                    bool recorded, uint32_t reason, uint32_t skinCount)
{
    if (!enabled || offscreen) return;
    struct Window
    {
        uint64_t firstFrame = 0, lastFrame = 0, recordings = 0;
        uint64_t dispatches = 0, noViews = 0, skipped = 0, rtSun = 0, rtProjectors = 0;
        uint32_t viewMin = UINT32_MAX, viewMax = 0;
        double nextLog = 0.0;
    };
    static Window w;
    if (!w.recordings) w.firstFrame = frameSerial;
    w.lastFrame = frameSerial;
    ++w.recordings;
    w.dispatches += recorded ? 1u : 0u;
    w.noViews += reason == 6u ? 1u : 0u;
    w.skipped += reason != 0u && reason != 6u ? 1u : 0u;
    w.rtSun += sunRt ? 1u : 0u;
    w.rtProjectors += projectorMode == 2u ? 1u : 0u;
    w.viewMin = std::min(w.viewMin, views);
    w.viewMax = std::max(w.viewMax, views);
    const double now = LLFrameTimer::getTotalSeconds();
    if (now < w.nextLog) return;
    LL_INFOS("VKGpu") << "shadow rig state: frame_first=" << w.firstFrame
        << " frame_last=" << w.lastFrame << " recordings=" << w.recordings
        << " dispatch_recordings=" << w.dispatches << " no_view_recordings=" << w.noViews
        << " other_skip_recordings=" << w.skipped << " rt_sun_recordings=" << w.rtSun
        << " rt_proj_recordings=" << w.rtProjectors << " nv_min=" << w.viewMin
        << " nv_max=" << w.viewMax << " last_nv=" << views << " last_reason=" << reason
        << " last_sun_rt=" << (sunRt ? 1u : 0u) << " last_proj_mode=" << projectorMode
        << " last_skin_count=" << skinCount << LL_ENDL;
    w = Window{};
    w.nextLog = now + 5.0;
}
} // namespace LLGpuShadowState
