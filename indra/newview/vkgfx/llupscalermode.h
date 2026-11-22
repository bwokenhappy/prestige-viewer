/**
 * @file llupscalermode.h
 * @brief The upscaler mode the Vulkan renderer runs, and the one-time move of TAA users to it.
 *
 * FSR 3.1 (or DLSS where picked) is the only temporal anti-aliasing: the renderer's own TAA resolve is
 * gone. RenderVKUpscaler: 0 off (native, FXAA / SMAA per RenderFSAAType), 1 FSR Native AA, 2-5 FSR
 * Quality .. Ultra Performance, 6 FSR Custom (RenderVKRenderScale), 7 DLSS DLAA, 8-11 DLSS Quality ..
 * Ultra Performance (the matching FSR preset where DLSS cannot run). RenderFSAAType 3 was "TAA": it is
 * moved once at startup (migrate) and, arriving later (an old graphics preset), runs as FSR Native AA.
 *
 * The renderer reads RenderVKUpscaler only through effectiveMode (LLCleanRendererVK::upscalerMode).
 * Pure: standard headers only (host-tested, scripts/check/test_taa_removed.cpp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 * $/LicenseInfo$
 */
#ifndef LL_LLUPSCALERMODE_H
#define LL_LLUPSCALERMODE_H

#include <cstdint>

namespace llups
{
constexpr uint32_t kOff = 0, kFsrNativeAA = 1, kFsrCustom = 6, kDlaa = 7, kDlssLast = 11;
constexpr uint32_t kAaSmaa = 2, kAaTaaLegacy = 3;   // RenderFSAAType values

// The mode that runs: the setting, a legacy TAA value as FSR Native AA, and off when nothing can run it
// (FSR presets need FSR; DLSS modes need DLSS or FSR for their matching preset).
inline uint32_t effectiveMode(uint32_t upscaler, uint32_t aaType, bool fsrUsable, bool dlssAvailable)
{
    uint32_t m = upscaler > kDlssLast ? kOff : upscaler;
    if (m == kOff && aaType == kAaTaaLegacy) m = kFsrNativeAA;
    if (m >= kFsrNativeAA && m <= kFsrCustom && !fsrUsable) return kOff;
    if (m >= kDlaa && m <= kDlssLast && !fsrUsable && !dlssAvailable) return kOff;
    return m;
}

// This frame jitters and resolves temporally: an FSR preset FSR runs, or a DLSS mode whose DLSS runs this
// frame (rrPlan) or whose matching FSR preset can run. A DLSS mode with DLSS standing down (back-off, the
// VRAM ladder, a debug view) and FSR unusable renders as off: native, no jitter. (mode: effectiveMode's.)
inline bool temporalRuns(uint32_t mode, bool fsrUsable, bool dlssThisFrame)
{
    if (mode == kOff || mode > kDlssLast) return false;
    if (mode < kDlaa) return fsrUsable;
    return dlssThisFrame || fsrUsable;
}

// The FSR preset (1-6) a frame renders at when DLSS does not run it: the mode itself, a DLSS mode's
// matching tier (7 -> 1 ... 11 -> 5), or 0 (native) when FSR cannot run.
inline uint32_t fsrTier(uint32_t mode, bool fsrUsable)
{
    if (!fsrUsable || mode == kOff || mode > kDlssLast) return kOff;
    return mode >= kDlaa ? mode - (kDlaa - kFsrNativeAA) : mode;
}

// The post-process AA (RenderFSAAType: 1 FXAA, 2 SMAA) on a frame without a temporal resolve. A legacy 3
// (TAA) that could not run as FSR Native AA gets SMAA: never no anti-aliasing.
inline uint32_t postAaType(uint32_t aaType)
{
    if (aaType == kAaTaaLegacy) return kAaSmaa;
    return aaType <= kAaSmaa ? aaType : 0u;
}

struct Migration
{
    bool     changed;
    uint32_t upscaler;   // RenderVKUpscaler to write
    uint32_t aaType;     // RenderFSAAType to write
};

// A TAA user (RenderFSAAType 3, or the old RenderVKTAA switch) moves to DLAA where DLSS is available, else
// FSR Native AA; an upscaler already chosen is kept. Antialiasing 3 becomes SMAA (what turning the
// upscaler off then gives); FXAA / SMAA are kept. Anyone else: unchanged.
inline Migration migrate(uint32_t upscaler, uint32_t aaType, bool taaDebug, bool dlssAvailable)
{
    if (aaType != kAaTaaLegacy && !taaDebug) return Migration{false, upscaler, aaType};
    const uint32_t up = upscaler != kOff ? upscaler : (dlssAvailable ? kDlaa : kFsrNativeAA);
    return Migration{true, up, aaType == kAaTaaLegacy ? kAaSmaa : aaType};
}
} // namespace llups

#endif // LL_LLUPSCALERMODE_H
