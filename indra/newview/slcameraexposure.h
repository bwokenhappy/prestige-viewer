/**
 * @file slcameraexposure.h
 * @brief Camera mode (#1572): the photographic scales and the exposure model behind the
 *        camera overlay's dials -- 1/3-stop aperture, shutter and ISO scales, the P / Av / Tv
 *        / M exposure solve, focal length <-> field of view (35 mm equivalent) and the white
 *        balance Kelvin <-> renderer offset map. Pure math, no viewer state.
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

#ifndef SL_CAMERAEXPOSURE_H
#define SL_CAMERAEXPOSURE_H

#include "stdtypes.h"

#include <string>
#include <vector>

namespace SLCameraExposure
{
// The exposure mode dial.
enum Mode : U32
{
    MODE_PROGRAM = 0,    // P: aperture and shutter from the program line (program shift on the dial)
    MODE_APERTURE = 1,   // Av: the aperture is set, the shutter follows
    MODE_SHUTTER = 2,    // Tv: the shutter is set, the aperture follows
    MODE_MANUAL = 3,     // M: both set; the renderer's auto exposure is off
    MODE_COUNT = 4
};
const char* modeLabel(U32 mode);

// THE SCENE. The renderer meters with its own auto exposure (it never reports the scene's
// brightness to the CPU), so the camera assumes one fixed scene: EV 13 at ISO 100 (a bright,
// overcast day; f/8 at 1/125 s). Av / Tv / P solve the free value against it, which is how a
// real body behaves under constant light; the brightness itself stays the auto exposure's job.
constexpr F32 SCENE_EV100 = 13.f;

// 1/3-stop scales, in the order the dials step through them.
const std::vector<F32>& apertureStops();   // f/1.4 .. f/22 (marked values)
const std::vector<F32>& shutterStops();    // seconds, 1 s .. 1/4000 s (slow to fast)
const std::vector<F32>& isoStops();        // ISO 100 .. 12800

// The scale entry nearest @a value (compared in stops, so 1/3-stop neighbours are equidistant).
size_t nearestStop(const std::vector<F32>& stops, F32 value);
// @a value moved @a clicks entries along @a stops (snapped first), clamped to the ends.
F32 stepStop(const std::vector<F32>& stops, F32 value, S32 clicks);
// The nearest scale entry, as a value.
F32 snapStop(const std::vector<F32>& stops, F32 value);

// EV compensation in 1/3 steps, clamped to [lo, hi].
F32 stepThirds(F32 ev, S32 clicks, F32 lo, F32 hi);

// The camera's settings and what the solve made of them.
struct Settings
{
    U32 mode = MODE_PROGRAM;
    F32 fnumber = 8.f;      // N
    F32 shutter = 1.f / 125.f;   // seconds
    F32 iso = 100.f;
    F32 evComp = 0.f;       // stops (P / Av / Tv)
    F32 programShift = 0.f; // stops toward a wider aperture (P)
};
struct Result
{
    F32 fnumber = 8.f;
    F32 shutter = 1.f / 125.f;
    // The exposure the renderer should apply on top of its nominal one, in stops: the EV
    // compensation in P / Av / Tv (auto exposure on), the over / under exposure of the
    // manual settings against the scene in M (auto exposure off).
    F32 exposureStops = 0.f;
    bool autoExposure = true;
};
// Solve the mode: P / Av / Tv fill in the free value(s) on the 1/3-stop scales.
Result solve(const Settings& s);

// Labels as a camera shows them: "f/5.6", "1/125", "0.5\"", "2\"", "+1.3", "-0.7", "0.0".
std::string apertureLabel(F32 fnumber);
std::string shutterLabel(F32 seconds);
std::string evLabel(F32 stops);

// FOCAL LENGTH (35 mm full-frame equivalent, by the diagonal). The photo is the centred crop
// (crop_w x crop_h pixels) of a view whose vertical field of view is @a vfov radians and whose
// height is @a view_h pixels; a 35 mm frame's diagonal is 43.27 mm.
constexpr F32 FULL_FRAME_DIAGONAL_MM = 43.2666f;
F32 focalLengthForFov(F32 vfov, F32 view_h, F32 crop_w, F32 crop_h);
F32 fovForFocalLength(F32 focal_mm, F32 view_h, F32 crop_w, F32 crop_h);
// The zoom ring: @a focal_mm moved @a clicks steps of 1/6 of a doubling (about 12 %), rounded
// to whole millimetres.
F32 stepFocalLength(F32 focal_mm, S32 clicks);
// Prime lenses the focal length field cycles through.
const std::vector<F32>& primeFocalLengths();   // 24, 35, 50, 85, 135, 200 mm

// WHITE BALANCE. The renderer's white balance (RenderColorGradeWhiteBalanceCCT) is an offset in
// Kelvin applied as a gain: positive cools the image, negative warms it, 0 is the image as
// rendered. A camera's setting names the light it corrects for: 3200 K (tungsten) cools,
// 10000 K (deep shade) warms. The camera's "Daylight" (5500 K) is the image as rendered.
constexpr F32 WB_NEUTRAL_K = 5500.f;
constexpr F32 WB_MIN_K = 2500.f;
constexpr F32 WB_MAX_K = 10000.f;
F32 whiteBalanceOffsetForKelvin(F32 kelvin);
F32 kelvinForWhiteBalanceOffset(F32 offset);
// The named presets, by Kelvin; nullptr when @a kelvin is none of them.
const char* whiteBalancePresetName(F32 kelvin);
// The next / previous preset Kelvin from @a kelvin (Tungsten 3200, Fluorescent 4000,
// Daylight 5500, Cloudy 6500, Shade 7500), wrapping.
F32 nextWhiteBalancePreset(F32 kelvin, S32 dir);
// 100 K per click, clamped to [WB_MIN_K, WB_MAX_K].
F32 stepKelvin(F32 kelvin, S32 clicks);

// The aspect ratios the viewfinder offers. ASPECT_OUTPUT follows the snapshot output size.
enum Aspect : U32
{
    ASPECT_OUTPUT = 0,
    ASPECT_3_2 = 1,
    ASPECT_4_3 = 2,
    ASPECT_16_9 = 3,
    ASPECT_1_1 = 4,
    ASPECT_4_5 = 5,
    ASPECT_COUNT = 6
};
const char* aspectLabel(U32 aspect);
// width / height of a fixed aspect; 0 for ASPECT_OUTPUT.
F32 aspectRatio(U32 aspect);
// The output size for @a aspect from the snapshot size (base_w x base_h): the base itself for
// ASPECT_OUTPUT, else the base's long side kept and the other derived, clamped to @a max_side.
void outputSize(U32 aspect, S32 base_w, S32 base_h, S32 max_side, S32& out_w, S32& out_h);
// The centred crop of a view_w x view_h view that has the output's aspect (the rectangle the
// live snapshot keeps).
void cropRect(S32 view_w, S32 view_h, S32 out_w, S32 out_h, F32& crop_w, F32& crop_h);

// The viewfinder grid.
enum Grid : U32
{
    GRID_OFF = 0,
    GRID_THIRDS = 1,
    GRID_THIRDS_DIAGONALS = 2,
    GRID_GOLDEN = 3,
    GRID_CENTRE = 4,
    GRID_COUNT = 5
};
const char* gridLabel(U32 grid);
} // namespace SLCameraExposure

#endif // SL_CAMERAEXPOSURE_H
