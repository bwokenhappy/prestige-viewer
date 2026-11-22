/**
 * @file llvkframegendlss.cpp
 * @brief Frame generation's DLSS generator: see llvkframegendlss.h.
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
#include "linden_common.h"

#include "llvkframegendlss.h"

#include "llerror.h"

#include <glm/glm.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <cmath>
#include <cstring>
#include <utility>

llfg::Block LLVKFrameGenDlss::init(VkDeviceContext& dev, llfg::GeneratorCaps& caps, std::string& detail, std::string& log)
{
    caps = llfg::GeneratorCaps{};
    LLVKNeuralRecon& nr = LLVKNeuralRecon::instance();
    // The add-on's own block (llfg::addonBlock): switched off (RenderVKDlssShim 0: never loaded -- the reason says
    // so, not "install it"), else not loaded (the loader's reason goes to the log: the player's line stays
    // short), else too old, else unable to run (NGX did not start or the GPU / driver runs no DLSS feature: its
    // reason).
    const llfg::Block addon = llfg::addonBlock(nr.loaded(), nr.switchedOff(), nr.fgHasEntryPoints(), nr.available());
    if (addon != llfg::Block::None)
    {
        if (addon == llfg::Block::Unsupported) detail = nr.reason();
        else if (addon == llfg::Block::NoAddon) log = nr.reason();
        return addon;
    }
    // (Task 11b fix 1) A 1.3.0 / 1.3.1 add-on runs frame generation but predates 1.3.2's fixes: accepted, and said.
    if (llfg::addonPredatesFixes(nr.libraryVersion().c_str()))
        LL_INFOS("Vulkan") << "Frame generation: the DLSS add-on is " << nr.libraryVersion()
                           << "; rebuild the add-on for the latest fixes (it is still used)" << LL_ENDL;
    // The GPU's own answer (llfg::capsBlock): each cause the player can fix has its own text -- NVIDIA's
    // frame generation library missing, Windows' GPU scheduling off (an add-on from 1.3.2 says which), the
    // add-on unable to report -- and a driver that is too old is named. Anything else: the general text.
    LLVKNeuralRecon::FgCaps fc;
    std::string capsError;
    const bool reported = nr.fgCaps(fc, &capsError);
    const llfg::Block gpu = llfg::capsBlock(reported, fc.available, fc.maxGenerated, fc.why);
    if (gpu != llfg::Block::None)
    {
        if (gpu == llfg::Block::Unsupported && fc.needsNewerDriver)
            detail = "needs a newer driver (min " + std::to_string(fc.minDriverMajor) + "." + std::to_string(fc.minDriverMinor) + ")";
        else if (gpu == llfg::Block::NoFgLibrary)
#if LL_WINDOWS
            detail = "nvngx_dlssg.dll";
#else
            detail = "libnvidia-ngx-dlssg.so";
#endif
        log = gpu == llfg::Block::NoFgLibrary ? "looked in " + nr.featureDir() : capsError;
        return gpu;
    }
    caps.maxGenerated = fc.maxGenerated;
    caps.driverPacing = true;          // (present metering, where the device has it)
    caps.reflex = dev.lowLatency2();
    return llfg::Block::None;
}

bool LLVKFrameGenDlss::prepare(VkDeviceContext& dev, const llfg::GenSize& size, std::string& err)
{
    LLVKNeuralRecon& nr = LLVKNeuralRecon::instance();
    int cr = -1;
    dev.submitImmediate([&](VkCommandBuffer c) { cr = nr.fgCreate(c, size.displayW, size.displayH, size.colorFormat, err); });
    mMade = true;   // (released at shutdown even after a refusal: the add-on may hold a part)
    mW = size.displayW;
    mH = size.displayH;
    mFormat = size.colorFormat;
    mReady = cr == 0;
    if (!mReady && err.empty()) err = std::string("result ") + std::to_string(cr);
    return mReady;
}

void LLVKFrameGenDlss::release(VkDeviceContext&)
{
    if (mMade) LLVKNeuralRecon::instance().fgRelease();
    mMade = mReady = false;
    mW = mH = 0;
    mFormat = VK_FORMAT_UNDEFINED;
}

bool LLVKFrameGenDlss::evaluate(VkCommandBuffer cmd, uint32_t, const llfg::GenInputs& in, uint32_t index, uint32_t count,
                                std::string& err)
{
    // (the present thread: nothing of this object is read -- the feature is the add-on's)
    const int er = LLVKNeuralRecon::instance().fgEvaluate(cmd, toEval(in), index, count, err);
    if (er == 0) return true;
    if (err.empty()) err = std::string("result ") + std::to_string(er);
    return false;
}

LLVKNeuralRecon::FgEval LLVKFrameGenDlss::toEval(const llfg::GenInputs& in)
{
    const llfg::GenCamera& c = in.camera;
    LLVKNeuralRecon::FgEval e;
    e.color = in.color.image;
    e.colorView = in.color.view;
    e.colorFormat = in.color.format;
    e.w = in.color.extent.width;
    e.h = in.color.extent.height;
    e.depth = in.depth.image; e.depthView = in.depth.view; e.depthFormat = in.depth.format;
    e.depthW = in.depth.extent.width; e.depthH = in.depth.extent.height;
    e.motion = in.motion.image; e.motionView = in.motion.view; e.motionFormat = in.motion.format;
    e.motionW = in.motion.extent.width; e.motionH = in.motion.extent.height;
    e.output = in.output.image;
    e.outputView = in.output.view;
    const glm::mat4 view = glm::make_mat4(c.view);
    const glm::mat4 proj = glm::make_mat4(c.proj);
    const glm::mat4 reproj = glm::make_mat4(c.reproj);
    const glm::mat4 invProj = glm::inverse(proj);
    const glm::mat4 invReproj = glm::inverse(reproj);
    std::memcpy(e.viewToClip, glm::value_ptr(proj), sizeof(e.viewToClip));
    std::memcpy(e.clipToView, glm::value_ptr(invProj), sizeof(e.clipToView));
    std::memcpy(e.clipToPrevClip, glm::value_ptr(reproj), sizeof(e.clipToPrevClip));
    std::memcpy(e.prevClipToClip, glm::value_ptr(invReproj), sizeof(e.prevClipToClip));
    const glm::mat4 toWorld = glm::inverse(view);
    for (int i = 0; i < 3; ++i)
    {
        e.camRight[i] = toWorld[0][i];
        e.camUp[i] = toWorld[1][i];
        e.camFwd[i] = -toWorld[2][i];   // (view space looks down -Z)
        e.camPos[i] = toWorld[3][i];
    }
    e.jitter[0] = c.jitterPx[0] * c.jitterSign;
    e.jitter[1] = c.jitterPx[1] * c.jitterSign;
    // The inputs pass writes UV units (prev_uv - uv): x the render size = pixels of the motion resource,
    // current -> previous. (A scale of 1 made a whole-screen camera step look like a fraction of a pixel:
    // the generator saw a still scene and blended the two real frames.)
    switch (c.motion)
    {
    case llfg::MotionUnits::UvPrevMinusCur:
        e.mvScale[0] = (float)c.renderW;
        e.mvScale[1] = (float)c.renderH;
        break;
    }
    // Reversed-Z perspective: depth = (A z + B) / -z with A = n / (f - n), B = n f / (f - n).
    const float a = proj[2][2], b = proj[3][2];
    e.zNear = (1.f + a) != 0.f ? std::fabs(b / (1.f + a)) : 0.1f;
    e.zFar = a > 1e-7f ? std::fabs(b / a) : 1.0e6f;
    e.fovY = proj[1][1] != 0.f ? 2.f * std::atan(1.f / std::fabs(proj[1][1])) : 1.f;
    e.aspect = proj[0][0] != 0.f ? std::fabs(proj[1][1] / proj[0][0]) : 1.f;
    e.depthInverted = c.depthInverted;
    e.colorHdr = in.colorHdr;
    e.reset = in.reset;
    // In-world A/B of the conventions (RenderVKFrameGenConventionDebug, not persisted; 0 = the SDK guide's, the
    // default): bit 0 transposes the four matrices (the layout before the fix), bit 1 gives the motion vectors
    // a scale of 1 (UV units read as pixels: the doubled-scene bug).
    if (in.debug & 1u)
    {
        float* const mats[4] = {e.viewToClip, e.clipToView, e.clipToPrevClip, e.prevClipToClip};
        for (float* m : mats)
            for (int r = 0; r < 4; ++r)
                for (int k = r + 1; k < 4; ++k) std::swap(m[r * 4 + k], m[k * 4 + r]);
    }
    if (in.debug & 2u)
    {
        e.mvScale[0] = 1.f;
        e.mvScale[1] = 1.f;
    }
    return e;
}
