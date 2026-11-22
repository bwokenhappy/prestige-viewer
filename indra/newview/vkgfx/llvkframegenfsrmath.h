/**
 * @file llvkframegenfsrmath.h
 * @brief FSR frame generation's host maths (#1544), pure: the constant buffers AMD's FSR 3.1 frame
 *        interpolation and optical flow shaders read and what goes into them, the sizes of the optical flow
 *        images, every pass's dispatch size, which of the ping-pong images a pass binds, the order of the
 *        passes, the reset rule and the memory the images take. No Vulkan, no viewer headers: tested on the
 *        host (scripts/check/test_framegen_fsr.cpp); LLVKFrameGenFsr records exactly what these say.
 *
 * Follows FidelityFX SDK v1.1.4 sdk/src/components/frameinterpolation/ffx_frameinterpolation.cpp ("FI.cpp":
 * ffxFrameInterpolationPrepare / ffxFrameInterpolationDispatch) and sdk/src/components/opticalflow/
 * ffx_opticalflow.cpp ("OF.cpp": dispatch) -- MIT, Copyright (C) 2024 Advanced Micro Devices, Inc.
 * (indra/vkclean/third_party/fidelityfx/LICENSE.txt). Where a value is ours and not the SDK's it says why.
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

#ifndef LL_LLVKFRAMEGENFSRMATH_H
#define LL_LLVKFRAMEGENFSRMATH_H

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>

// ---- shared with the FSR upscaler (LLCleanFSR3VK) ----
namespace llfsr
{

// The inverse of a 4x4 matrix (column-major, as the renderer holds its matrices). False: singular (out is
// untouched).
inline bool invert4(const float m[16], float out[16])
{
    double a[16], inv[16];
    for (int i = 0; i < 16; ++i) a[i] = (double)m[i];
    inv[0] = a[5] * a[10] * a[15] - a[5] * a[11] * a[14] - a[9] * a[6] * a[15] + a[9] * a[7] * a[14] + a[13] * a[6] * a[11] - a[13] * a[7] * a[10];
    inv[4] = -a[4] * a[10] * a[15] + a[4] * a[11] * a[14] + a[8] * a[6] * a[15] - a[8] * a[7] * a[14] - a[12] * a[6] * a[11] + a[12] * a[7] * a[10];
    inv[8] = a[4] * a[9] * a[15] - a[4] * a[11] * a[13] - a[8] * a[5] * a[15] + a[8] * a[7] * a[13] + a[12] * a[5] * a[11] - a[12] * a[7] * a[9];
    inv[12] = -a[4] * a[9] * a[14] + a[4] * a[10] * a[13] + a[8] * a[5] * a[14] - a[8] * a[6] * a[13] - a[12] * a[5] * a[10] + a[12] * a[6] * a[9];
    inv[1] = -a[1] * a[10] * a[15] + a[1] * a[11] * a[14] + a[9] * a[2] * a[15] - a[9] * a[3] * a[14] - a[13] * a[2] * a[11] + a[13] * a[3] * a[10];
    inv[5] = a[0] * a[10] * a[15] - a[0] * a[11] * a[14] - a[8] * a[2] * a[15] + a[8] * a[3] * a[14] + a[12] * a[2] * a[11] - a[12] * a[3] * a[10];
    inv[9] = -a[0] * a[9] * a[15] + a[0] * a[11] * a[13] + a[8] * a[1] * a[15] - a[8] * a[3] * a[13] - a[12] * a[1] * a[11] + a[12] * a[3] * a[9];
    inv[13] = a[0] * a[9] * a[14] - a[0] * a[10] * a[13] - a[8] * a[1] * a[14] + a[8] * a[2] * a[13] + a[12] * a[1] * a[10] - a[12] * a[2] * a[9];
    inv[2] = a[1] * a[6] * a[15] - a[1] * a[7] * a[14] - a[5] * a[2] * a[15] + a[5] * a[3] * a[14] + a[13] * a[2] * a[7] - a[13] * a[3] * a[6];
    inv[6] = -a[0] * a[6] * a[15] + a[0] * a[7] * a[14] + a[4] * a[2] * a[15] - a[4] * a[3] * a[14] - a[12] * a[2] * a[7] + a[12] * a[3] * a[6];
    inv[10] = a[0] * a[5] * a[15] - a[0] * a[7] * a[13] - a[4] * a[1] * a[15] + a[4] * a[3] * a[13] + a[12] * a[1] * a[7] - a[12] * a[3] * a[5];
    inv[14] = -a[0] * a[5] * a[14] + a[0] * a[6] * a[13] + a[4] * a[1] * a[14] - a[4] * a[2] * a[13] - a[12] * a[1] * a[6] + a[12] * a[2] * a[5];
    inv[3] = -a[1] * a[6] * a[11] + a[1] * a[7] * a[10] + a[5] * a[2] * a[11] - a[5] * a[3] * a[10] - a[9] * a[2] * a[7] + a[9] * a[3] * a[6];
    inv[7] = a[0] * a[6] * a[11] - a[0] * a[7] * a[10] - a[4] * a[2] * a[11] + a[4] * a[3] * a[10] + a[8] * a[2] * a[7] - a[8] * a[3] * a[6];
    inv[11] = -a[0] * a[5] * a[11] + a[0] * a[7] * a[9] + a[4] * a[1] * a[11] - a[4] * a[3] * a[9] - a[8] * a[1] * a[7] + a[8] * a[3] * a[5];
    inv[15] = a[0] * a[5] * a[10] - a[0] * a[6] * a[9] - a[4] * a[1] * a[10] + a[4] * a[2] * a[9] + a[8] * a[1] * a[6] - a[8] * a[2] * a[5];
    const double det = a[0] * inv[0] + a[1] * inv[4] + a[2] * inv[8] + a[3] * inv[12];
    if (!(std::fabs(det) > 1e-30)) return false;
    for (int i = 0; i < 16; ++i) out[i] = (float)(inv[i] / det);
    return true;
}

// The shaders' fDeviceToViewDepth, from the projection the frame was rendered with and its inverse (both
// column-major): out[0], out[1] turn device depth into positive view depth, Z = out[1] / (depth - out[0]);
// out[2] / out[3] are the tangents of half the horizontal / vertical field of view.
// From the inverse projection, view z = (a d + b) / (c d + e) (clean_taa.comp's `lin`): for a perspective
// a = 0, so out[0] = -e / c, out[1] = b / c (sign chosen so Z > 0). A ~0 out[0] (reversed-Z, infinite far)
// gets the SDK's FLT_EPSILON guard. The same values as the SDK's setupDeviceDepthToViewSpaceDepthParams for
// inverted depth, taken from the real projection instead of a near / far / field of view round trip.
inline void deviceToViewDepth(const float proj[16], const float invProj[16], float out[4])
{
    const double b = invProj[14], c = invProj[11], e = invProj[15];
    double F0 = -1e-7, F1 = 1.0;
    if (std::fabs(c) > 1e-20)
    {
        F0 = -e / c;
        F1 = b / c;
        if (std::fabs(F0) < (double)FLT_EPSILON) F0 = -(double)FLT_EPSILON;
        if (F1 / (0.5 - F0) < 0.0) F1 = -F1;
    }
    out[0] = (float)F0;
    out[1] = (float)F1;
    out[2] = std::fabs(proj[0]) > 1e-20f ? 1.f / std::fabs(proj[0]) : 1.f;
    out[3] = std::fabs(proj[5]) > 1e-20f ? 1.f / std::fabs(proj[5]) : 1.f;
}

// ffxSpdSetup (gpu/spd/ffx_spd.h, FFX_CPU) for the rect {0, 0, w, h}: the dispatch's thread groups, and the
// shader's work group offset, work group count and mip count. mips < 0: from the rect, at most 12.
struct SpdSetup
{
    uint32_t groups[2] = {1, 1};
    uint32_t offset[2] = {0, 0};
    uint32_t numWorkGroups = 1;
    uint32_t mips = 0;
};
inline SpdSetup spdSetup(uint32_t w, uint32_t h, int32_t mips = -1)
{
    SpdSetup s;
    w = std::max(w, 1u);
    h = std::max(h, 1u);
    s.groups[0] = (w - 1) / 64 + 1;
    s.groups[1] = (h - 1) / 64 + 1;
    s.numWorkGroups = s.groups[0] * s.groups[1];
    s.mips = mips >= 0 ? (uint32_t)mips : (uint32_t)std::min(std::floor(std::log2((float)std::max(w, h))), 12.f);
    return s;
}

} // namespace llfsr

// ---- frame generation ----
namespace llfsrfg
{

// ---- the constant buffers (std140; the GLSL callbacks' blocks, the SDK's host structs) ----

// cbFI (ffx_frameinterpolation_callbacks_glsl.h; the SDK's FrameInterpolationConstants).
struct FiConstants
{
    int32_t  renderSize[2];
    int32_t  displaySize[2];
    float    displaySizeRcp[2];
    float    cameraNear;
    float    cameraFar;
    int32_t  upscalerTargetSize[2];
    int32_t  mode;
    int32_t  reset;
    float    deviceToViewDepth[4];
    float    deltaTime;
    int32_t  hudLessAttachedFactor;
    int32_t  distortionFieldSize[2];
    float    opticalFlowScale[2];
    int32_t  opticalFlowBlockSize;
    uint32_t dispatchFlags;
    int32_t  maxRenderSize[2];
    int32_t  opticalFlowHalfResMode;
    int32_t  numInstances;
    int32_t  interpolationRectBase[2];
    int32_t  interpolationRectSize[2];
    float    debugBarColor[3];
    uint32_t backBufferTransferFunction;
    float    minMaxLuminance[2];
    float    tanHalfFov;
    int32_t  pad1;
    float    jitter[2];
    float    motionVectorScale[2];
};
static_assert(sizeof(FiConstants) == 176, "FiConstants must match cbFI (std140)");
static_assert(offsetof(FiConstants, deviceToViewDepth) == 48 && offsetof(FiConstants, opticalFlowScale) == 80
                  && offsetof(FiConstants, debugBarColor) == 128 && offsetof(FiConstants, backBufferTransferFunction) == 140
                  && offsetof(FiConstants, jitter) == 160 && offsetof(FiConstants, motionVectorScale) == 168,
              "FiConstants must match cbFI (std140)");

// cbInpaintingPyramid (both of frame interpolation's SPD passes).
struct PyramidConstants
{
    uint32_t mips;
    uint32_t numWorkGroups;
    uint32_t workGroupOffset[2];
};
static_assert(sizeof(PyramidConstants) == 16, "PyramidConstants must match cbInpaintingPyramid");

// cbOF (ffx_opticalflow_callbacks_glsl.h; the SDK's OpticalflowConstants).
struct OfConstants
{
    int32_t  inputLumaResolution[2];
    uint32_t pyramidLevel;
    uint32_t pyramidLevelCount;
    uint32_t frameIndex;
    uint32_t backbufferTransferFunction;
    float    minMaxLuminance[2];
};
static_assert(sizeof(OfConstants) == 32, "OfConstants must match cbOF (std140)");

// cbOF_SPD (the luminance pyramid).
struct OfSpdConstants
{
    uint32_t mips;
    uint32_t numWorkGroups;
    uint32_t workGroupOffset[2];
    uint32_t numWorkGroupOpticalFlowInputPyramid;
    uint32_t pad[3];
};
static_assert(sizeof(OfSpdConstants) == 32, "OfSpdConstants must match cbOF_SPD (std140)");

// Optical flow (OF.cpp dispatch): 7 pyramid levels, 8-pixel blocks, at the display size.
constexpr uint32_t kOfLevels = 7;
constexpr uint32_t kOfBlockSize = 8;
// The scene change detection's images: 3 x 3 histograms of 256 bins in one row; three values out.
constexpr uint32_t kScdHistogramsPerDim = 3;
constexpr uint32_t kScdHistogramShifts = 3;
constexpr uint32_t kScdHistogramWidth = 256 * kScdHistogramsPerDim * kScdHistogramsPerDim;
constexpr uint32_t kScdOutputWidth = 3;
// The inpainting pyramid's mip bindings in both SPD passes (the image has fewer mips: the last repeats).
constexpr uint32_t kPyramidMipBindings = 13;

// The constant ring: one region per present slot, of 256-byte sub-slots (bound whole as dynamic uniform
// buffers). A frame's blocks are all recorded into one command buffer, so each value that differs between
// two passes has its own sub-slot: both SPD blocks, and the optical flow block once per pyramid level (the
// passes that read no level use level 0's).
constexpr uint32_t kCbSubSlotBytes = 256;
constexpr uint32_t kCbFi = 0;
constexpr uint32_t kCbPyramidGame = 1 * kCbSubSlotBytes;     // the game vector field's pyramid: the render rect
constexpr uint32_t kCbPyramidOutput = 2 * kCbSubSlotBytes;   // the output's pyramid: the display rect
constexpr uint32_t kCbOfSpd = 3 * kCbSubSlotBytes;
constexpr uint32_t kCbOf = 4 * kCbSubSlotBytes;              // + level x kCbSubSlotBytes
constexpr uint32_t kCbSlotBytes = 12 * kCbSubSlotBytes;
static_assert(kCbOf + kOfLevels * kCbSubSlotBytes <= kCbSlotBytes, "a frame's blocks must fit a present slot's region");

struct Size { uint32_t w = 0, h = 0; };
struct Groups { uint32_t x = 1, y = 1, z = 1; };

// ---- sizes ----

// The optical flow vector images of a pyramid level: one vector per 8 x 8 block of the display, halved
// (rounding up) per level.
inline Size ofFlowSize(uint32_t displayW, uint32_t displayH, uint32_t level)
{
    Size s{(displayW + kOfBlockSize - 1) / kOfBlockSize, (displayH + kOfBlockSize - 1) / kOfBlockSize};
    for (uint32_t i = 0; i < level; ++i)
    {
        s.w = (s.w + 1) / 2;
        s.h = (s.h + 1) / 2;
    }
    s.w = std::max(s.w, 1u);
    s.h = std::max(s.h, 1u);
    return s;
}

// The luminance pyramid's level: the display size shifted down (never empty).
inline Size ofLumaSize(uint32_t displayW, uint32_t displayH, uint32_t level)
{
    return Size{std::max(displayW >> level, 1u), std::max(displayH >> level, 1u)};
}

// The inpainting pyramid: half the display size, the whole mip chain.
inline Size pyramidSize(uint32_t displayW, uint32_t displayH)
{
    return Size{std::max(displayW / 2, 1u), std::max(displayH / 2, 1u)};
}
inline uint32_t pyramidMips(uint32_t displayW, uint32_t displayH)
{
    const Size s = pyramidSize(displayW, displayH);
    uint32_t mips = 1;
    for (uint32_t m = std::max(s.w, s.h); m > 1; m >>= 1) ++mips;
    return mips;
}

// ---- the passes, in recording order ----

enum class Stage : uint8_t { Prepare, OpticalFlow, Interpolation };

enum class Step : uint8_t
{
    // A. prepare (ffxFrameInterpolationPrepare)
    ClearAll,                              // (ours, a reset only: every image and the counters, see passPlan)
    ClearReconstructedPrevDepth,
    ReconstructAndDilate,
    // B. optical flow (OF.cpp dispatch)
    OfPrepareLuma,
    OfLuminancePyramid,
    OfScdHistogram,
    OfScdDivergence,
    OfSearch,                              // per level, 6 down to 0
    OfFilter,                              // per level
    OfScale,                               // per level above 0: into the level below
    // C. interpolation (ffxFrameInterpolationDispatch)
    Setup,
    ClearReconstructedDepthInterpolated,
    ReconstructPrevDepth,
    GameVectorField,
    GameVectorPyramid,
    OpticalFlowVectorField,
    DisocclusionMask,
    Interpolate,
    InpaintingPyramid,
    Inpainting,
    CopyPreviousSource,
};

inline Stage stageOf(Step s)
{
    return s <= Step::ReconstructAndDilate ? Stage::Prepare : s <= Step::OfScale ? Stage::OpticalFlow : Stage::Interpolation;
}

// A compute dispatch (else a clear or a copy).
inline bool isCompute(Step s)
{
    return s != Step::ClearAll && s != Step::ClearReconstructedPrevDepth && s != Step::ClearReconstructedDepthInterpolated
        && s != Step::CopyPreviousSource;
}

struct PlanStep
{
    Step step = Step::ClearAll;
    uint32_t level = 0;   // the optical flow pyramid level (OfSearch / OfFilter / OfScale)
};
constexpr uint32_t kMaxPlanSteps = 40;
struct Plan
{
    PlanStep steps[kMaxPlanSteps];
    uint32_t count = 0;
};

// One rendered frame's passes, in order.
//  - A reset: the frame interpolation's preparation passes are left out (FI.cpp: bExecutePreparationPasses)
//    -- the interpolation pass then writes the current picture. Before anything, EVERY image of the generator
//    and its counters are cleared: the SDK clears the scene change detection's images and both luminance
//    pyramids (OF.cpp) and leaves the rest as created; here a reset is also what follows a frame whose
//    commands never ran, so it re-initialises all the GPU side carries.
//  - Optical flow runs whole either way (its frame index restarts on a reset).
inline Plan passPlan(bool reset)
{
    Plan p;
    const auto add = [&p](Step s, uint32_t level = 0) { p.steps[p.count].step = s; p.steps[p.count].level = level; ++p.count; };
    if (reset) add(Step::ClearAll);
    add(Step::ClearReconstructedPrevDepth);
    add(Step::ReconstructAndDilate);
    add(Step::OfPrepareLuma);
    add(Step::OfLuminancePyramid);
    add(Step::OfScdHistogram);
    add(Step::OfScdDivergence);
    for (uint32_t level = kOfLevels; level-- > 0;)
    {
        add(Step::OfSearch, level);
        add(Step::OfFilter, level);
        if (level > 0) add(Step::OfScale, level);
    }
    add(Step::Setup);
    if (!reset)
    {
        add(Step::ClearReconstructedDepthInterpolated);
        add(Step::ReconstructPrevDepth);
        add(Step::GameVectorField);
        add(Step::GameVectorPyramid);
        add(Step::OpticalFlowVectorField);
        add(Step::DisocclusionMask);
    }
    add(Step::Interpolate);
    add(Step::InpaintingPyramid);
    add(Step::Inpainting);
    add(Step::CopyPreviousSource);
    return p;
}

// The SPD set-up of an SPD pass: the luminance pyramid's is the display rect with FOUR mips, exactly as the
// SDK passes (OF.cpp; the pass writes six levels all the same); the game vector field's pyramid is the render
// rect, the output's the display rect.
inline llfsr::SpdSetup spdFor(Step s, uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH)
{
    switch (s)
    {
    case Step::OfLuminancePyramid: return llfsr::spdSetup(displayW, displayH, 4);
    case Step::GameVectorPyramid:  return llfsr::spdSetup(renderW, renderH);
    case Step::InpaintingPyramid:  return llfsr::spdSetup(displayW, displayH);
    default:                       return llfsr::SpdSetup{};
    }
}

// A compute pass's dispatch size (thread groups), as the SDK computes it; never zero.
inline Groups dispatchGroups(Step s, uint32_t level, uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH)
{
    Groups g;
    switch (s)
    {
    case Step::ReconstructAndDilate:
    case Step::Setup:
    case Step::ReconstructPrevDepth:
    case Step::GameVectorField:
    case Step::DisocclusionMask:
        g.x = (renderW + 7) / 8;
        g.y = (renderH + 7) / 8;
        break;
    case Step::Interpolate:
    case Step::Inpainting:
        g.x = (displayW + 7) / 8;
        g.y = (displayH + 7) / 8;
        break;
    case Step::OpticalFlowVectorField:
        // (FI.cpp: uint32_t(displaySize / float(opticalFlowBlockSize) + 7) / 8)
        g.x = (uint32_t)((float)displayW / (float)kOfBlockSize + 7.f) / 8;
        g.y = (uint32_t)((float)displayH / (float)kOfBlockSize + 7.f) / 8;
        break;
    case Step::OfPrepareLuma:
        // (16 x 16 threads, each 2 x 2 pixels)
        g.x = ((displayW + 1) / 2 + 15) / 16;
        g.y = ((displayH + 1) / 2 + 15) / 16;
        break;
    case Step::OfLuminancePyramid:
    case Step::GameVectorPyramid:
    case Step::InpaintingPyramid:
    {
        const llfsr::SpdSetup spd = spdFor(s, renderW, renderH, displayW, displayH);
        g.x = spd.groups[0];
        g.y = spd.groups[1];
        break;
    }
    case Step::OfScdHistogram:
        g.x = ((displayW / 4) / kScdHistogramsPerDim + 31) / 32;
        g.y = 16;
        g.z = kScdHistogramsPerDim * kScdHistogramsPerDim;
        break;
    case Step::OfScdDivergence:
        g.x = kScdHistogramsPerDim * kScdHistogramsPerDim;
        g.y = kScdHistogramShifts;
        break;
    case Step::OfSearch:
    {
        // (64 threads a group, each 4 pixels wide, 16 rows a group)
        const Size luma = ofLumaSize(displayW, displayH, level);
        g.x = ((luma.w + 3) / 4 * 16 + 63) / 64;
        g.y = (luma.h + 15) / 16;
        break;
    }
    case Step::OfFilter:
    {
        const Size flow = ofFlowSize(displayW, displayH, level);
        g.x = (flow.w + 15) / 16;
        g.y = (flow.h + 3) / 4;
        break;
    }
    case Step::OfScale:
    {
        // (into the level below: its size / 4)
        const Size next = ofFlowSize(displayW, displayH, level > 0 ? level - 1 : 0);
        g.x = (next.w + 3) / 4;
        g.y = (next.h + 3) / 4;
        break;
    }
    case Step::ClearAll:
    case Step::ClearReconstructedPrevDepth:
    case Step::ClearReconstructedDepthInterpolated:
    case Step::CopyPreviousSource:
        break;
    }
    g.x = std::max(g.x, 1u);
    g.y = std::max(g.y, 1u);
    g.z = std::max(g.z, 1u);
    return g;
}

// ---- the ping-pong images (OF.cpp: isOddFrame / isOddLevel) ----

// The luminance pyramids: the one this frame writes and the previous frame's (0: the SDK's "input 1").
struct OfInputs { uint32_t current = 0, previous = 1; };
inline OfInputs ofInputs(uint32_t parity)
{
    const uint32_t odd = parity & 1u;
    return OfInputs{odd, 1u - odd};
}

// The two sets of optical flow vector images at a level (0: the SDK's "optical flow 1"):
//   search   reads and writes A[level] (the level above scaled into it);
//   filter   A[level] -> B[level], or -> the shared result at level 0;
//   scale    (above level 0) B[level] -> B[level - 1], which is the level below's A.
struct OfFlow
{
    uint32_t a = 0, b = 1;
    bool filterToShared = false;
    bool scales = false;
};
inline OfFlow ofFlowTargets(uint32_t parity, uint32_t level)
{
    OfFlow f;
    f.a = ((parity & 1u) != (level & 1u)) ? 1u : 0u;
    f.b = 1u - f.a;
    f.filterToShared = level == 0;
    f.scales = level > 0;
    return f;
}

// ---- the reset rule ----

// The generator drops its history for this rendered frame: on its first use after its images were made, when
// the caller says so (LLVKFrameGen's reset rules, the present thread's broken history), and -- the SDK's own
// rule (FI.cpp: a frame id that decreased or skipped), kept as a second line -- when this frame's serial is
// not the one after the last frame it generated for. Serial 0: the caller states none.
inline bool resetFor(bool inReset, uint64_t serial, uint64_t lastSerial, bool first)
{
    return first || inReset || (serial != 0 && serial != lastSerial + 1);
}

// The optical flow's frame index (OF.cpp: 0 on a reset, else counting).
inline uint32_t ofFrameIndex(bool reset, uint32_t last)
{
    return reset ? 0u : last + 1u;
}

// ---- the constants ----

// fMotionVectorScale: what turns a stored motion vector into FSR's UV vector (LoadInputMotionVector), which
// points from a pixel to where it was in the previous frame (the game field pass reads the previous frame
// at uv + mv). The SDK computes it as its motionVectorScale parameter over the render size (FI.cpp). The
// inputs pass writes UV units, previous minus current, unjittered: in the SDK's terms its parameter is the
// render size, so the constant is 1. Derived here from the motion image's own units -- never from another
// generator's scale.
template <class Units>
inline void motionVectorScale(Units units, uint32_t renderW, uint32_t renderH, float out[2])
{
    out[0] = out[1] = 0.f;
    switch (units)
    {
    case Units::UvPrevMinusCur:
    {
        const float sdkScale[2] = {(float)renderW, (float)renderH};   // UV -> pixels of the motion image
        out[0] = renderW ? sdkScale[0] / (float)renderW : 1.f;
        out[1] = renderH ? sdkScale[1] / (float)renderH : 1.f;
        break;
    }
    }
}

// The transfer function of the world image (the SDK's FfxBackbufferTransferFunction): 1 PQ (HDR10), 0 sRGB.
inline uint32_t transferFunction(bool colorHdr) { return colorHdr ? 1u : 0u; }

// minMaxLuminance: PQ is decoded as value x 10000 / max; a display that states no peak, or an absurd one,
// stays inside what PQ carries. Not read for sRGB.
inline void minMaxLuminance(float peakNits, float out[2])
{
    out[0] = 0.f;
    out[1] = std::min(std::max(peakNits, 80.f), 10000.f);
}

// cbFI for a rendered frame. `In` is llfg::GenInputs (the test: a struct with the same members): the camera's
// projection, render size, raw jitter and motion units, the world image's extent, HDR and its peak, the
// frame time. The jitter is the one the frame was rendered with (render pixels; a sample at pixel q shows
// q + 0.5 - jitter, ComputeHrPosFromLrPos) -- the sign an upscaler applied for its own network is not used.
// The frame interpolation shaders' dispatch flags (ffx_frameinterpolation_common.h) a debug setting may set.
constexpr uint32_t kDispatchDebugTearLines = 1u << 0;        // FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_TEAR_LINES
constexpr uint32_t kDispatchDebugResetIndicators = 1u << 1;  // FFX_FRAMEINTERPOLATION_DISPATCH_DRAW_DEBUG_RESET_INDICATORS

template <class In>
inline FiConstants fillFiConstants(const In& in, bool reset)
{
    FiConstants k{};
    const int32_t rw = (int32_t)in.camera.renderW, rh = (int32_t)in.camera.renderH;
    const int32_t dw = (int32_t)in.color.extent.width, dh = (int32_t)in.color.extent.height;
    k.renderSize[0] = rw; k.renderSize[1] = rh;
    k.maxRenderSize[0] = rw; k.maxRenderSize[1] = rh;      // (the images are made at exactly this size)
    k.displaySize[0] = dw; k.displaySize[1] = dh;
    k.displaySizeRcp[0] = dw > 0 ? 1.f / (float)dw : 0.f;
    k.displaySizeRcp[1] = dh > 0 ? 1.f / (float)dh : 0.f;
    k.upscalerTargetSize[0] = dw; k.upscalerTargetSize[1] = dh;
    k.interpolationRectBase[0] = k.interpolationRectBase[1] = 0;
    k.interpolationRectSize[0] = dw; k.interpolationRectSize[1] = dh;
    k.mode = 0;
    k.reset = reset ? 1 : 0;
    float invProj[16] = {};
    (void)llfsr::invert4(in.camera.proj, invProj);         // (singular: zeros -> the guarded defaults)
    llfsr::deviceToViewDepth(in.camera.proj, invProj, k.deviceToViewDepth);
    // Near / far: view depth at device depth 1 / 0 (reversed-Z). No 3.1.4 shader reads them.
    {
        const double f0 = k.deviceToViewDepth[0], f1 = k.deviceToViewDepth[1];
        const double zNear = f1 / (1.0 - f0), zFar = f1 / (0.0 - f0);
        k.cameraNear = (float)zNear;
        k.cameraFar = (std::isfinite(zFar) && std::fabs(zFar) < (double)FLT_MAX) ? (float)std::fabs(zFar) : FLT_MAX;
    }
    k.deltaTime = in.frameMs;
    k.hudLessAttachedFactor = 0;                           // (one picture: the world without the interface)
    k.distortionFieldSize[0] = k.distortionFieldSize[1] = 1;   // (no distortion field: the 1 x 1 zero one)
    // The optical flow vectors are in display pixels (its resolution is the display size).
    k.opticalFlowScale[0] = k.displaySizeRcp[0];
    k.opticalFlowScale[1] = k.displaySizeRcp[1];
    k.opticalFlowBlockSize = (int32_t)kOfBlockSize;
    // (#1544 Task 9) RenderVKFrameGenFsrDebug (llfg::GenInputs::debug): bit 1 the SDK's tear lines (a green bar at the
    // left edge; the right one adds debugBarColor, left 0), bit 2 its reset indicators (red: reset, blue: a scene
    // change), drawn by the inpainting pass into the generated frame. The SDK's debug view (its bit 4) is a
    // pass of its own that is not built: never handed on.
    k.dispatchFlags = in.debug & (kDispatchDebugTearLines | kDispatchDebugResetIndicators);
    k.opticalFlowHalfResMode = 0;
    k.numInstances = 0;
    k.backBufferTransferFunction = transferFunction(in.colorHdr);
    minMaxLuminance(in.peakNits, k.minMaxLuminance);
    k.tanHalfFov = k.deviceToViewDepth[2];                 // (horizontal; FI.cpp: tan(atan(tan(fovY / 2) x aspect)))
    k.jitter[0] = in.camera.jitterPx[0];
    k.jitter[1] = in.camera.jitterPx[1];
    motionVectorScale(in.camera.motion, in.camera.renderW, in.camera.renderH, k.motionVectorScale);
    return k;
}

// cbInpaintingPyramid of one of frame interpolation's SPD passes.
inline PyramidConstants fillPyramidConstants(Step s, uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH)
{
    const llfsr::SpdSetup spd = spdFor(s, renderW, renderH, displayW, displayH);
    PyramidConstants k{};
    k.mips = spd.mips;
    k.numWorkGroups = spd.numWorkGroups;
    k.workGroupOffset[0] = spd.offset[0];
    k.workGroupOffset[1] = spd.offset[1];
    return k;
}

// cbOF for a pyramid level (the passes that read no level are given level 0's).
template <class In>
inline OfConstants fillOfConstants(const In& in, uint32_t level, uint32_t frameIndex)
{
    OfConstants k{};
    k.inputLumaResolution[0] = (int32_t)in.color.extent.width;
    k.inputLumaResolution[1] = (int32_t)in.color.extent.height;
    k.pyramidLevel = level;
    k.pyramidLevelCount = kOfLevels;
    k.frameIndex = frameIndex;
    k.backbufferTransferFunction = transferFunction(in.colorHdr);
    minMaxLuminance(in.peakNits, k.minMaxLuminance);
    return k;
}

// cbOF_SPD.
inline OfSpdConstants fillOfSpdConstants(uint32_t displayW, uint32_t displayH)
{
    const llfsr::SpdSetup spd = spdFor(Step::OfLuminancePyramid, 0, 0, displayW, displayH);
    OfSpdConstants k{};
    k.mips = spd.mips;
    k.numWorkGroups = spd.numWorkGroups;
    k.workGroupOffset[0] = spd.offset[0];
    k.workGroupOffset[1] = spd.offset[1];
    k.numWorkGroupOpticalFlowInputPyramid = spd.numWorkGroups;
    return k;
}

// ---- memory ----

// The bytes the generator's own images hold (texels x bytes per texel; a driver's padding is not in it):
// 34 bytes a render pixel, about 9.3 a display pixel (the previous picture in the world image's format,
// `colorBytesPerPixel`), the optical flow vectors and the scene change detection's rows. 0 when a size is.
inline uint64_t imageBytes(uint32_t renderW, uint32_t renderH, uint32_t displayW, uint32_t displayH, uint32_t colorBytesPerPixel)
{
    if (!renderW || !renderH || !displayW || !displayH) return 0;
    const uint64_t render = (uint64_t)renderW * renderH, display = (uint64_t)displayW * displayH;
    // dilated depth 4, dilated motion 4, two reconstructed depths 4 + 4, four vector fields 16, the mask 2
    uint64_t bytes = render * 34;
    bytes += display * colorBytesPerPixel;                 // the previous interpolation source
    {
        Size s = pyramidSize(displayW, displayH);          // RGBA16F, every mip
        for (uint32_t m = pyramidMips(displayW, displayH); m-- > 0;)
        {
            bytes += (uint64_t)s.w * s.h * 8;
            s.w = std::max(s.w / 2, 1u);
            s.h = std::max(s.h / 2, 1u);
        }
    }
    for (uint32_t level = 0; level < kOfLevels; ++level)
    {
        const Size luma = ofLumaSize(displayW, displayH, level), flow = ofFlowSize(displayW, displayH, level);
        bytes += 2 * (uint64_t)luma.w * luma.h;            // both luminance pyramids, R8
        bytes += 2 * (uint64_t)flow.w * flow.h * 4;        // both optical flow sets, RG16
    }
    {
        const Size flow = ofFlowSize(displayW, displayH, 0);
        bytes += (uint64_t)flow.w * flow.h * 4;            // the shared result
    }
    bytes += 2 * (uint64_t)kScdHistogramWidth * 4 + 2 * (uint64_t)kScdOutputWidth * 4;   // the scene change detection
    bytes += 2 + 4 + 8;                                    // the zero images and the counters
    return bytes;
}

} // namespace llfsrfg

#endif // LL_LLVKFRAMEGENFSRMATH_H
