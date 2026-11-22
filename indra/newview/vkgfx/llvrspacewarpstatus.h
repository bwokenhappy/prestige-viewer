/**
 * @file llvrspacewarpstatus.h
 * @brief #1594 VR space warp: what it is doing, in plain words (the VR settings menu, the wrist
 *        panel's header, the log). Pure text from numbers: no viewer state is read here, so the
 *        wording is checked by scripts/check/test_vr_spacewarp_status.cpp.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace llvrsw
{
struct StatusIn
{
    bool seen = false;           // a VR frame has been waited this run
    uint32_t mode = 1;           // SlipstreamVRSpaceWarp: 0 off, 1 auto, 2 always
    bool stereo = false;         // the session renders layered stereo (warp needs each eye's depth)
    bool timing = false;         // the headset's native frame period is known
    bool engaged = false;
    bool throttled = false;      // the runtime holds the app at a lower rate
    float shownHz = 0.f;         // frames the headset was given, per second (measured)
    float realHz = 0.f;          // frames whose world was rendered, per second (measured)
    float gpuMs = -1.f;          // the world's GPU time on a real frame; < 0 unknown
    float budgetMs = 0.f;        // the headset's native frame period
};

inline std::string num(float v, const char* fmt)
{
    char b[48];
    std::snprintf(b, sizeof b, fmt, (double)v);
    return b;
}

inline std::string rate(float hz)
{
    return std::to_string((int)std::lround(hz));
}

// One or two sentences for the settings menu.
inline std::string statusText(const StatusIn& s)
{
    if (s.mode == 0) return "Space warp: off.";
    if (!s.seen) return "Space warp: used in the headset only.";
    if (!s.stereo)
        return "Space warp: not available. It needs layered stereo, which is off in this VR session "
               "(it is chosen when the viewer starts).";
    if (!s.timing) return "Space warp: waiting for the headset's frame timing.";
    if (s.engaged)
    {
        std::string t = std::string("Space warp: on") + (s.mode == 1 ? " (automatic)" : "") + ", rendering " +
                        rate(s.realHz) + " of " + rate(s.shownHz) + " frames per second.";
        if (s.gpuMs > 0.f && s.budgetMs > 0.f)
            t += " The world takes " + num(s.gpuMs, "%.1f") + " ms; a headset frame is " + num(s.budgetMs, "%.1f") + " ms.";
        return t;
    }
    if (s.mode == 2) return "Space warp: always, starting.";
    std::string t = "Space warp: automatic, standing by";
    if (s.gpuMs > 0.f && s.budgetMs > 0.f)
        t += std::string(s.gpuMs > 0.95f * s.budgetMs ? " (the world takes " : " (headroom ok: the world takes ") +
             num(s.gpuMs, "%.1f") + " ms of a " + num(s.budgetMs, "%.1f") + " ms frame)";
    return t + ".";
}

// A few characters for the wrist panel's header; empty while warp is not working.
inline std::string shortText(const StatusIn& s)
{
    if (!s.engaged || s.mode == 0 || !s.stereo) return std::string();
    return "Warp " + rate(s.realHz) + " / " + rate(s.shownHz);
}
} // namespace llvrsw
