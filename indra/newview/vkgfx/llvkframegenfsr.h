/**
 * @file llvkframegenfsr.h
 * @brief Frame generation's FSR generator (#1544): AMD FSR 3.1 frame interpolation and optical flow, run by
 *        the viewer's own host code behind LLVKFrameGenGenerator.
 *
 * The passes are the SDK's own GLSL (indra/vkclean/third_party/fidelityfx, MIT; vendored with its LICENSE
 * and a README naming the version, the commit and the local patch), compiled with the renderer's other
 * shaders into vk_fsr3_shaders_spv.h. This class replaces the SDK's host side (ffx_frameinterpolation.cpp,
 * ffx_opticalflow.cpp and the ffx Vulkan backend; no FidelityFX runtime, swapchain or pacing threads): it
 * owns the images, the descriptor sets, the constant ring and the pass sequence, as LLCleanFSR3VK does for
 * the upscaler. What it computes on the host is llvkframegenfsrmath.h (tested on the host).
 *
 * One evaluate() records, for one rendered frame, into the present thread's command buffer:
 *   A. prepare        reconstruct and dilate the depth and motion vectors (render size)
 *   B. optical flow   luminance pyramid of the world image, scene change detection, a block search from the
 *                     coarsest of 7 levels down (display size / 8)
 *   C. interpolation  the game and optical flow vector fields, the disocclusion mask, the half-way frame into
 *                     the output image, its inpainting; then the world image is kept as the previous picture
 * All compute, all images in GENERAL, one memory barrier between passes. One generated frame per rendered
 * frame (2x).
 *
 * Threads (single owner, no lock): init / prepare / release / shutdown on the main thread only -- prepare and
 * release with the present thread drained and the GPU idle; evaluate on the present thread only. Every
 * object is made, and every descriptor set written, in prepare: evaluate only records commands and writes
 * the constant ring's memory. What evaluate advances (the frame parity, the frame index, the last serial)
 * assumes the recorded commands run; when they do not, the present thread's broken history makes the next
 * frame a reset, and a reset re-initialises everything the GPU side carries.
 *
 * #1600 FSR 4: with RenderVKFrameGenFsr4 on (hidden, not saved, off: untested -- it needs an RX 9000 GPU)
 * and AMD's runtime offering a 4.x frame generation for this GPU (llcleanffxvk.h), evaluate() hands the
 * frame to FSR Frame Generation 4.0.2 instead of the passes above, dispatched by hand into our own present
 * chain. The FSR 3.1 images stay made beside it: a refusal generates that same frame with 3.1, and FSR 4 is
 * not tried again until the sizes change.
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

#ifndef LL_LLVKFRAMEGENFSR_H
#define LL_LLVKFRAMEGENFSR_H

#include "llvkframegengenerator.h"
#include "llvkframegenfsrmath.h"
#include "llcleanffxvk.h"   // #1600 (names no SDK type)
#include "vk_descpool.h"

class LLVKFrameGenFsr final : public LLVKFrameGenGenerator
{
public:
    llfg::Generator kind() const override { return llfg::Generator::Fsr; }
    // What the device must offer (the storage and filterable formats of the passes, subgroup operations in
    // compute for the optical flow search, compute on the present queue's family): Block::Unsupported with
    // the missing thing as the detail, else the caps -- one generated frame, the viewer's pacer.
    llfg::Block init(VkDeviceContext& dev, llfg::GeneratorCaps& caps, std::string& detail, std::string& log) override;
    // Made for the display and render sizes, both formats, the slot count and the very images it was told.
    bool fits(const llfg::GenSize& size) const override;
    // Pipelines (the first time), images, the constant ring, and every descriptor set.
    bool prepare(VkDeviceContext& dev, const llfg::GenSize& size, std::string& err) override;
    // llfsrfg::imageBytes; with the render size not known yet, for the largest it can be (the display size).
    uint64_t imageBytes(const llfg::GenSize& size) const override;
    bool evaluate(VkCommandBuffer cmd, uint32_t slot, const llfg::GenInputs& in, uint32_t index, uint32_t count,
                  std::string& err) override;
    void release(VkDeviceContext& dev) override;
    void shutdown(VkDeviceContext& dev) override;

    // One set of bindings per pass (a pipeline layout each).
    enum Pass : uint32_t
    {
        P_RECONSTRUCT_DILATE, P_OF_PREPARE_LUMA, P_OF_LUMA_PYRAMID, P_OF_SCD_HISTOGRAM, P_OF_SCD_DIVERGENCE,
        P_OF_SEARCH, P_OF_FILTER, P_OF_SCALE, P_SETUP, P_RECONSTRUCT_PREV_DEPTH, P_GAME_VECTOR_FIELD,
        P_GAME_VECTOR_PYRAMID, P_OF_VECTOR_FIELD, P_DISOCCLUSION_MASK, P_INTERPOLATE, P_INPAINTING_PYRAMID,
        P_INPAINTING, P_COUNT
    };

private:
    static constexpr uint32_t kSlots = llfg::kMaxPresentSlots;
    static constexpr uint32_t kLevels = llfsrfg::kOfLevels;
    static constexpr uint32_t kVariants = 2;   // the output image's format: 0 rgb10_a2 (HDR10), 1 rgba8 (SDR)

    // What one evaluate records with (the present thread's; nothing of it outlives the call).
    struct Frame
    {
        uint32_t slot = 0;
        uint32_t parity = 0;
        bool reset = false;
        uint32_t cbOffset = 0;        // this slot's region of the constant ring
        VkImage color = VK_NULL_HANDLE;
    };

    // ---- main thread (prepare / release / shutdown) ----
    bool ensurePipelines(VkDeviceContext& dev, std::string& err);
    bool makeResources(VkDeviceContext& dev, const llfg::GenSize& size, std::string& err);
    bool allocateSets(VkDeviceContext& dev, const llfg::GenSize& size, std::string& err);
    void writeSets(VkDeviceContext& dev, const llfg::GenSize& size);
    void destroyResources(VkDeviceContext& dev);
    void destroyPipelines(VkDeviceContext& dev);

    // ---- present thread (evaluate) ----
    void stageConstants(const Frame& f, const llfg::GenInputs& in);
    void recordPrepare(VkCommandBuffer cmd, const Frame& f, const llfsrfg::Plan& plan);
    void recordOpticalFlow(VkCommandBuffer cmd, const Frame& f, const llfsrfg::Plan& plan);
    void recordInterpolation(VkCommandBuffer cmd, const Frame& f, const llfsrfg::Plan& plan);
    void recordStep(VkCommandBuffer cmd, const Frame& f, const llfsrfg::PlanStep& step);
    void recordClearAll(VkCommandBuffer cmd);
    void barrier(VkCommandBuffer cmd) const;
    void dispatch(VkCommandBuffer cmd, const Frame& f, Pass pass, uint32_t level, const llfsrfg::Groups& groups) const;
    VkDescriptorSet setOf(Pass pass, uint32_t slot, uint32_t parity, uint32_t level) const;

    // ---- kept for the session (ensurePipelines; shutdown) ----
    VkSampler mLinearClamp = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDsl[P_COUNT] = {};
    VkPipelineLayout mLayout[P_COUNT] = {};
    VkPipeline mPipe[P_COUNT][kVariants] = {};   // [pass][output variant] (one pipeline for a pass without the output)
    bool mPipelines = false;
    bool mWaveOps = false;          // the SPD passes use subgroup quad operations
    bool mFullSubgroups = false;    // the optical flow search is made with full subgroups required

    // ---- made for a size (prepare; release) ----
    bool mReady = false;
    llfg::GenSize mSize;            // what the feature was made for (fits)
    int32_t mVariant = -1;          // the output variant of mSize.scratchFormat
    VkDescPool mPool;
    VkDescriptorSet mSet[P_COUNT][kSlots][2][kLevels] = {};   // [pass][present slot][frame parity][level], as the pass needs
    GpuBuffer mCb;                  // the constant ring: host-visible, one region per present slot
    GpuBuffer mCounters;            // 2 x uint32: the SPD counter, frames since the last reset
    GpuImage mDilatedDepth, mDilatedMotion, mReconPrevDepth, mReconDepthInterpolated;   // render size
    GpuImage mGameFieldX, mGameFieldY, mOfFieldX, mOfFieldY, mDisocclusion;             // render size
    GpuImage mPyramid;              // half the display size, every mip
    VkImageView mPyramidMip[llfsrfg::kPyramidMipBindings] = {};
    uint32_t mPyramidMips = 0;
    GpuImage mPrevSource;           // the previous rendered frame's world image (its format)
    GpuImage mDistortion, mZeroUint;                 // 1 x 1, zero: no distortion field, no optical flow confidence
    GpuImage mOfInput[2][kLevels];  // the luminance pyramids (ping-pong by frame parity)
    GpuImage mOfFlow[2][kLevels];   // the optical flow vectors (ping-pong by frame and level parity)
    GpuImage mOfResult;             // the optical flow at level 0
    GpuImage mScdOutput, mScdHistogram, mScdPrevHistogram, mScdTemp;   // scene change detection

    // ---- the present thread's (evaluate), reset by prepare ----
    bool mFirst = true;             // nothing recorded since the images were made
    uint32_t mParity = 0;
    uint32_t mOfFrameIndex = 0;
    uint64_t mLastSerial = 0;

    // ---- #1600 FSR 4 (llcleanffxvk.h) ----
    // Main thread: whether FSR 4 is wanted for this size (the setting, and no refusal for this size yet).
    bool wantFsr4(const llfg::GenSize& size) const;
    bool evaluateFsr4(VkCommandBuffer cmd, const llfg::GenInputs& in, std::string& err);
    llffx::FrameGen mFsr4;          // made / freed by prepare and release (main, drained); evaluate dispatches it
    // The present thread's between two prepares: evaluate uses FSR 4 (set by prepare; cleared by evaluate when
    // a dispatch is refused -- FSR 3.1 until the next prepare). fits never reads them: it runs beside evaluate.
    bool mFsr4Now = false;
    bool mFsr4LastRan = false;      // the last evaluate generated with FSR 4 (3.1's history is stale after it)
    // No 4.x frame generation here, or a refused context, for these sizes: FSR 3.1 until they change. Main
    // thread only (prepare writes, fits reads).
    bool mFsr4Refused = false;
    uint32_t mFsr4RefusedDisplay[2] = {0, 0}, mFsr4RefusedRender[2] = {0, 0};
    VkFormat mFsr4RefusedFormat = VK_FORMAT_UNDEFINED;
    bool mFsr4RefusedHdr = false;
};

#endif // LL_LLVKFRAMEGENFSR_H
