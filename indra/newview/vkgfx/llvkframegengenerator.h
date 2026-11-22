/**
 * @file llvkframegengenerator.h
 * @brief Frame generation (#1544): what a generator is given for a rendered frame, in the renderer's own
 *        terms, and the interface every generator implements (LLVKFrameGenDlss, LLVKFrameGenFsr).
 *        Which one runs: llfg::chooseGenerator (RenderVKFrameGenGenerator), decided by LLVKFrameGen.
 *
 * Nothing above a generator names a vendor type: the render loop fills llfg::GenInputs, the present
 * thread hands it to LLVKFrameGen::evaluate, and each generator turns it into its own conventions.
 * The camera part (llfg::GenCamera) is written by the upscaler that made the frame's depth and motion
 * vectors, with ITS reset and the jitter sign it applied; no generator inherits another one's conversions.
 *
 * Threads: init / fits / prepare / imageBytes / release on the main thread only (prepare and release with
 * the present thread drained and the GPU idle); evaluate on the present thread only, and it only records.
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

#ifndef LL_LLVKFRAMEGENGENERATOR_H
#define LL_LLVKFRAMEGENGENERATOR_H

#include "llvkframegenpolicy.h"   // llfg::Generator, GeneratorCaps, Block
#include "vk_device.h"            // GpuImage

#include <cstdint>
#include <string>

namespace llfg
{

// What the motion vector image holds. One value: the inputs pass both upscalers run writes UV units,
// previous position minus current (prev_uv - uv), unjittered. A generator derives its own scale from it.
enum class MotionUnits : uint8_t { UvPrevMinusCur };

// An image a generator reads or writes (not owned).
struct GpuImageRef
{
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent = {0, 0};
};
inline GpuImageRef imageRef(const GpuImage& im) { return GpuImageRef{im.image, im.view, im.format, im.extent}; }

// The main view's camera of a rendered frame, as the renderer holds it: written by the upscaler that ran
// (LLVKFrameGen::mainViewConstants), taken once by the render loop (takeConstants).
struct GenCamera
{
    float proj[16] = {};            // the unjittered projection, column-major (glm's memory)
    float view[16] = {};            // world -> view, column-major
    float reproj[16] = {};          // unjittered clip -> the previous frame's clip, column-major
    float jitterPx[2] = {0.f, 0.f}; // render pixels, x right / y down (sample q shows q + 0.5 - jitter), as rendered
    float jitterSign = 1.f;         // the sign the upscaler applied to it for its own network (1: none)
    uint32_t renderW = 0, renderH = 0;   // the upscaler's render size: the depth / motion images' extent
    MotionUnits motion = MotionUnits::UvPrevMinusCur;
    bool depthInverted = true;      // reversed-Z device depth
    bool reset = false;             // the upscaler dropped its history for this frame (a cut, new targets, ...)
};

// One rendered frame's inputs for its generated frames.
struct GenInputs
{
    GpuImageRef color;              // the world without the interface, in the output's encoding
    GpuImageRef depth, motion;      // the upscaler's, render size (this slot's copies)
    GpuImageRef output;             // where the generated frame is written: the present thread's scratch image
    GenCamera camera;
    bool colorHdr = false;          // color is HDR10 (PQ, Rec.2020); else 8-bit sRGB-encoded SDR
    bool reset = false;             // the generator drops its history (LLVKFrameGen::frameReset; the present
                                    // thread adds its own broken history)
    float peakNits = 0.f;           // HDR10: the display's peak (0: not stated)
    float frameMs = 0.f;            // milliseconds since the rendered frame before (0: not stated)
    uint64_t serial = 0;            // LLVKFrameGen's frame counter of this rendered frame (0: not stated)
    uint32_t debug = 0;             // the generator's own debug setting, read on the main thread
                                    // (LLVKFrameGen::generatorDebug)
};

// (llvkframegenpolicy.h knows no Vulkan: its format numbers for llfg::scratchFormat are VkFormat's)
static_assert(kFormatR8G8B8A8Unorm == (uint32_t)VK_FORMAT_R8G8B8A8_UNORM && kFormatB8G8R8A8Unorm == (uint32_t)VK_FORMAT_B8G8R8A8_UNORM
                  && kFormatA2B10G10R10Unorm == (uint32_t)VK_FORMAT_A2B10G10R10_UNORM_PACK32,
              "llfg's format numbers must be VkFormat's");
// The scratch image's format for a generator and the world image's format (llfg::scratchFormat, as VkFormat).
inline VkFormat scratchFormat(Generator g, VkFormat hudlessFormat)
{
    return (VkFormat)scratchFormat(g, (uint32_t)hudlessFormat);
}

// What a generator's feature is made for; it is made again when fits() says this changed.
struct GenSize
{
    uint32_t displayW = 0, displayH = 0;   // color / output extent
    uint32_t renderW = 0, renderH = 0;     // depth / motion extent (0: not known yet)
    VkFormat colorFormat = VK_FORMAT_UNDEFINED;
    VkFormat scratchFormat = VK_FORMAT_UNDEFINED;   // the output image's
    bool colorHdr = false;                 // the frames are HDR10 (PQ, Rec.2020); else SDR -- #1600: FSR 4
                                           // frame generation is made for one or the other
    uint32_t slots = 0;                    // present slots (each has its own input images)
    // The images the generator will be handed with each rendered frame (GenInputs::color / depth / motion of
    // present slot k, and the one output), known when the feature is made: a generator that binds images
    // through descriptor sets writes them then, on the main thread, and never on the present thread. Not
    // stated (null views): a generator that needs them refuses. imagesSerial counts every time any of them was
    // made again -- a new image can get an old one's handle back, so the handles alone do not say "the same".
    GpuImageRef color[kMaxPresentSlots], depth[kMaxPresentSlots], motion[kMaxPresentSlots];
    GpuImageRef output;
    uint64_t imagesSerial = 0;
};

} // namespace llfg

class LLVKFrameGenGenerator
{
public:
    virtual ~LLVKFrameGenGenerator() = default;
    virtual llfg::Generator kind() const = 0;
    // Main thread, once at startup: whether it can run on this device (Block::None) and what it offers;
    // else its block, `detail` for the player's line and `log` for the log only.
    virtual llfg::Block init(VkDeviceContext& dev, llfg::GeneratorCaps& caps, std::string& detail, std::string& log) = 0;
    // Main thread: the feature made by the last prepare() serves this size.
    virtual bool fits(const llfg::GenSize& size) const = 0;
    // Main thread, the present thread drained and the GPU idle, after release() of an earlier feature: the
    // feature for this size. False: refused (err says why); release() still has to be called.
    virtual bool prepare(VkDeviceContext& dev, const llfg::GenSize& size, std::string& err) = 0;
    // The video memory its own images take for this size (admitted with the present slots' images).
    virtual uint64_t imageBytes(const llfg::GenSize& size) const = 0;
    // The present thread only: generated frame `index` (1-based) of `count` for the rendered frame in present
    // slot `slot`, into in.output, recorded into cmd. False: refused (err says why).
    virtual bool evaluate(VkCommandBuffer cmd, uint32_t slot, const llfg::GenInputs& in, uint32_t index, uint32_t count,
                          std::string& err) = 0;
    // Main thread, the present thread drained and the GPU idle: the feature and everything prepare() made
    // for a size.
    virtual void release(VkDeviceContext& dev) = 0;
    // Main thread, once at shutdown, after release() and with the GPU idle: what the generator keeps for the
    // whole session whatever the size (pipelines).
    virtual void shutdown(VkDeviceContext&) {}
};

#endif // LL_LLVKFRAMEGENGENERATOR_H
