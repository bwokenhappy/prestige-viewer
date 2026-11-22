/**
 * @file slcameraexposure.cpp
 * @brief Camera mode (#1572): photographic scales and the exposure model. See
 *        slcameraexposure.h.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "slcameraexposure.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace SLCameraExposure
{
namespace
{
template <typename T> T clampTo(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

F32 log2f_safe(F32 v) { return std::log2(v > 1e-9f ? v : 1e-9f); }

// White balance presets, warm to cool light.
struct WBPreset
{
    F32 kelvin;
    const char* name;
};
const WBPreset WB_PRESETS[] = {
    { 3200.f, "Tungsten" },
    { 4000.f, "Fluorescent" },
    { 5500.f, "Daylight" },
    { 6500.f, "Cloudy" },
    { 7500.f, "Shade" },
};
constexpr size_t WB_PRESET_COUNT = sizeof(WB_PRESETS) / sizeof(WB_PRESETS[0]);
} // namespace

const char* modeLabel(U32 mode)
{
    switch (mode)
    {
    case MODE_APERTURE: return "Av";
    case MODE_SHUTTER:  return "Tv";
    case MODE_MANUAL:   return "M";
    default:            return "P";
    }
}

const std::vector<F32>& apertureStops()
{
    static const std::vector<F32> stops = { 1.4f, 1.6f, 1.8f, 2.f,  2.2f, 2.5f, 2.8f, 3.2f, 3.5f, 4.f,  4.5f, 5.f, 5.6f,
                                            6.3f, 7.1f, 8.f,  9.f,  10.f, 11.f, 13.f, 14.f, 16.f, 18.f, 20.f, 22.f };
    return stops;
}

const std::vector<F32>& shutterStops()
{
    static const std::vector<F32> stops = {
        1.f,          0.8f,         0.6f,         0.5f,         0.4f,          0.3f,          1.f / 4.f,
        1.f / 5.f,    1.f / 6.f,    1.f / 8.f,    1.f / 10.f,   1.f / 13.f,    1.f / 15.f,    1.f / 20.f,
        1.f / 25.f,   1.f / 30.f,   1.f / 40.f,   1.f / 50.f,   1.f / 60.f,    1.f / 80.f,    1.f / 100.f,
        1.f / 125.f,  1.f / 160.f,  1.f / 200.f,  1.f / 250.f,  1.f / 320.f,   1.f / 400.f,   1.f / 500.f,
        1.f / 640.f,  1.f / 800.f,  1.f / 1000.f, 1.f / 1250.f, 1.f / 1600.f,  1.f / 2000.f,  1.f / 2500.f,
        1.f / 3200.f, 1.f / 4000.f
    };
    return stops;
}

const std::vector<F32>& isoStops()
{
    static const std::vector<F32> stops = { 100.f,  125.f,  160.f,  200.f,  250.f,  320.f,  400.f,  500.f,
                                            640.f,  800.f,  1000.f, 1250.f, 1600.f, 2000.f, 2500.f, 3200.f,
                                            4000.f, 5000.f, 6400.f, 8000.f, 10000.f, 12800.f };
    return stops;
}

size_t nearestStop(const std::vector<F32>& stops, F32 value)
{
    if (stops.empty()) return 0;
    const F32 lv = log2f_safe(value);
    size_t best = 0;
    F32 best_d = 1e30f;
    for (size_t i = 0; i < stops.size(); ++i)
    {
        const F32 d = std::fabs(log2f_safe(stops[i]) - lv);
        if (d < best_d)
        {
            best_d = d;
            best = i;
        }
    }
    return best;
}

F32 stepStop(const std::vector<F32>& stops, F32 value, S32 clicks)
{
    if (stops.empty()) return value;
    const S32 last = (S32)stops.size() - 1;
    const S32 i = clampTo((S32)nearestStop(stops, value) + clicks, 0, last);
    return stops[(size_t)i];
}

F32 snapStop(const std::vector<F32>& stops, F32 value)
{
    return stops.empty() ? value : stops[nearestStop(stops, value)];
}

F32 stepThirds(F32 ev, S32 clicks, F32 lo, F32 hi)
{
    const F32 thirds = std::round(ev * 3.f) + (F32)clicks;
    return clampTo(thirds / 3.f, lo, hi);
}

Result solve(const Settings& s)
{
    Result r;
    const F32 iso = s.iso > 0.f ? s.iso : 100.f;
    const F32 iso_stops = log2f_safe(iso / 100.f);
    const F32 n_lo = apertureStops().front(), n_hi = apertureStops().back();
    // The camera EV the scene needs at this ISO, with the compensation (+1 = one stop more light).
    const F32 ev_target = SCENE_EV100 + iso_stops - s.evComp;
    switch (s.mode)
    {
    case MODE_APERTURE:
    {
        r.fnumber = snapStop(apertureStops(), s.fnumber);
        r.shutter = snapStop(shutterStops(), r.fnumber * r.fnumber / std::exp2(ev_target));
        r.exposureStops = s.evComp;
        r.autoExposure = true;
        break;
    }
    case MODE_SHUTTER:
    {
        r.shutter = snapStop(shutterStops(), s.shutter);
        r.fnumber = snapStop(apertureStops(), clampTo(std::sqrt(r.shutter * std::exp2(ev_target)), n_lo, n_hi));
        r.exposureStops = s.evComp;
        r.autoExposure = true;
        break;
    }
    case MODE_MANUAL:
    {
        r.fnumber = snapStop(apertureStops(), s.fnumber);
        r.shutter = snapStop(shutterStops(), s.shutter);
        const F32 ev_set = log2f_safe(r.fnumber * r.fnumber / r.shutter);
        // More light than the scene needs (a lower EV than it asks for) is overexposure.
        r.exposureStops = (SCENE_EV100 + iso_stops) - ev_set;
        r.autoExposure = false;
        break;
    }
    default:   // MODE_PROGRAM
    {
        // The program line: half the EV above 3 goes to the aperture (EV 13 -> f/5.6 at 1/250),
        // and the program shift trades aperture for shutter.
        const F32 av_lo = log2f_safe(n_lo * n_lo), av_hi = log2f_safe(n_hi * n_hi);
        const F32 tv_lo = 0.f, tv_hi = log2f_safe(1.f / shutterStops().back());
        F32 av = clampTo((ev_target - 3.f) * 0.5f - s.programShift, av_lo, av_hi);
        const F32 tv = clampTo(ev_target - av, tv_lo, tv_hi);
        av = clampTo(ev_target - tv, av_lo, av_hi);
        r.fnumber = snapStop(apertureStops(), std::exp2(av * 0.5f));
        r.shutter = snapStop(shutterStops(), std::exp2(-tv));
        r.exposureStops = s.evComp;
        r.autoExposure = true;
        break;
    }
    }
    return r;
}

std::string apertureLabel(F32 fnumber)
{
    char buf[32];
    if (std::fabs(fnumber - std::round(fnumber)) < 0.05f)
        std::snprintf(buf, sizeof(buf), "f/%d", (int)std::round(fnumber));
    else
        std::snprintf(buf, sizeof(buf), "f/%.1f", fnumber);
    return buf;
}

std::string shutterLabel(F32 seconds)
{
    char buf[32];
    if (seconds >= 0.95f)
        std::snprintf(buf, sizeof(buf), "%d\"", (int)std::round(seconds));
    else if (seconds >= 0.29f)
        std::snprintf(buf, sizeof(buf), "%.1f\"", seconds);
    else
        std::snprintf(buf, sizeof(buf), "1/%d", (int)std::round(1.f / std::max(seconds, 1e-6f)));
    return buf;
}

std::string evLabel(F32 stops)
{
    const F32 rounded = std::round(stops * 10.f) / 10.f;
    if (std::fabs(rounded) < 0.05f) return "0.0";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%+.1f", rounded);
    return buf;
}

F32 focalLengthForFov(F32 vfov, F32 view_h, F32 crop_w, F32 crop_h)
{
    const F32 diag = std::sqrt(crop_w * crop_w + crop_h * crop_h);
    const F32 t = std::tan(clampTo(vfov, 0.001f, 3.1f) * 0.5f);
    if (diag <= 0.f || view_h <= 0.f || t <= 0.f) return 50.f;
    const F32 focal_px = view_h * 0.5f / t;
    return FULL_FRAME_DIAGONAL_MM * focal_px / diag;
}

F32 fovForFocalLength(F32 focal_mm, F32 view_h, F32 crop_w, F32 crop_h)
{
    const F32 diag = std::sqrt(crop_w * crop_w + crop_h * crop_h);
    if (diag <= 0.f || view_h <= 0.f || focal_mm <= 0.f) return 1.f;
    const F32 focal_px = focal_mm * diag / FULL_FRAME_DIAGONAL_MM;
    return 2.f * std::atan(view_h * 0.5f / focal_px);
}

F32 stepFocalLength(F32 focal_mm, S32 clicks)
{
    const F32 base = std::round(focal_mm);
    F32 next = std::round(focal_mm * std::exp2((F32)clicks / 6.f));
    // At short focal lengths a 12 % step rounds back to the same millimetre: move at least one.
    if (clicks > 0 && next <= base) next = base + 1.f;
    if (clicks < 0 && next >= base) next = base - 1.f;
    return clampTo(next, 8.f, 1200.f);
}

const std::vector<F32>& primeFocalLengths()
{
    static const std::vector<F32> primes = { 24.f, 35.f, 50.f, 85.f, 135.f, 200.f };
    return primes;
}

F32 whiteBalanceOffsetForKelvin(F32 kelvin)
{
    return clampTo(WB_NEUTRAL_K - kelvin, -5000.f, 5000.f);
}

F32 kelvinForWhiteBalanceOffset(F32 offset)
{
    return WB_NEUTRAL_K - offset;
}

const char* whiteBalancePresetName(F32 kelvin)
{
    for (const WBPreset& p : WB_PRESETS)
        if (std::fabs(p.kelvin - kelvin) < 1.f) return p.name;
    return nullptr;
}

F32 nextWhiteBalancePreset(F32 kelvin, S32 dir)
{
    if (dir >= 0)
    {
        for (const WBPreset& p : WB_PRESETS)
            if (p.kelvin > kelvin + 1.f) return p.kelvin;
        return WB_PRESETS[0].kelvin;
    }
    for (size_t i = WB_PRESET_COUNT; i-- > 0;)
        if (WB_PRESETS[i].kelvin < kelvin - 1.f) return WB_PRESETS[i].kelvin;
    return WB_PRESETS[WB_PRESET_COUNT - 1].kelvin;
}

F32 stepKelvin(F32 kelvin, S32 clicks)
{
    const F32 k = std::round(kelvin / 100.f) * 100.f + 100.f * (F32)clicks;
    return clampTo(k, WB_MIN_K, WB_MAX_K);
}

const char* aspectLabel(U32 aspect)
{
    switch (aspect)
    {
    case ASPECT_3_2:  return "3:2";
    case ASPECT_4_3:  return "4:3";
    case ASPECT_16_9: return "16:9";
    case ASPECT_1_1:  return "1:1";
    case ASPECT_4_5:  return "4:5";
    default:          return "Output";
    }
}

F32 aspectRatio(U32 aspect)
{
    switch (aspect)
    {
    case ASPECT_3_2:  return 3.f / 2.f;
    case ASPECT_4_3:  return 4.f / 3.f;
    case ASPECT_16_9: return 16.f / 9.f;
    case ASPECT_1_1:  return 1.f;
    case ASPECT_4_5:  return 4.f / 5.f;
    default:          return 0.f;
    }
}

void outputSize(U32 aspect, S32 base_w, S32 base_h, S32 max_side, S32& out_w, S32& out_h)
{
    base_w = std::max(base_w, 1);
    base_h = std::max(base_h, 1);
    const F32 ratio = aspectRatio(aspect);
    if (ratio <= 0.f)
    {
        out_w = base_w;
        out_h = base_h;
    }
    else
    {
        const S32 long_side = std::max(base_w, base_h);
        if (ratio >= 1.f)
        {
            out_w = long_side;
            out_h = std::max(1, (S32)std::lround((F32)long_side / ratio));
        }
        else
        {
            out_h = long_side;
            out_w = std::max(1, (S32)std::lround((F32)long_side * ratio));
        }
    }
    if (max_side > 0 && (out_w > max_side || out_h > max_side))
    {
        const F32 f = std::min((F32)max_side / (F32)out_w, (F32)max_side / (F32)out_h);
        out_w = std::max(1, (S32)((F32)out_w * f));
        out_h = std::max(1, (S32)((F32)out_h * f));
    }
}

void cropRect(S32 view_w, S32 view_h, S32 out_w, S32 out_h, F32& crop_w, F32& crop_h)
{
    if (view_w <= 0 || view_h <= 0 || out_w <= 0 || out_h <= 0)
    {
        crop_w = (F32)std::max(view_w, 0);
        crop_h = (F32)std::max(view_h, 0);
        return;
    }
    const F32 ratio = std::min((F32)view_w / (F32)out_w, (F32)view_h / (F32)out_h);
    crop_w = clampTo(ratio * (F32)out_w, 1.f, (F32)view_w);
    crop_h = clampTo(ratio * (F32)out_h, 1.f, (F32)view_h);
}

const char* gridLabel(U32 grid)
{
    switch (grid)
    {
    case GRID_THIRDS:           return "Thirds";
    case GRID_THIRDS_DIAGONALS: return "3x3 + diagonals";
    case GRID_GOLDEN:           return "Golden ratio";
    case GRID_CENTRE:           return "Centre";
    default:                    return "No grid";
    }
}
} // namespace SLCameraExposure
