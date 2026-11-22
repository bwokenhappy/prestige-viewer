/**
 * @file llvrspacewarpvk.h
 * @brief #1594 VR SPACE WARP, the GPU side: the history of the newest real frame and the warp
 *        pass that reprojects it to a frame's own eye poses (clean_vr_spacewarp.comp).
 *        The frame policy is LLVRSpaceWarp (llvrspacewarp.h); LLVulkanContext drives both.
 *
 * HISTORY (capture, at the tail of a real frame's graphics work, VR only while engaged): copies of
 * the world's final colour (the eye target, every eye layer), its depth (one layer per eye) and
 * its motion vectors (RGBA16F: xy = unjittered UV, current minus previous; z = previous clip w),
 * each at its own size, plus per eye the agent-space view / projection the frame was rendered
 * with, its camera-only reprojection to the previous real frame (the renderer's mTaaReprojV) and
 * the controllers' poses. Everything is on the one graphics queue in submission order, so one
 * history set is enough: a warp submitted before a real frame reads the older capture, the next
 * warp (queued after that frame) reads the new one. The copies cost about
 * 2 x 6.8 MP x (4 + 4 + 8) B ~ 0.22 GB of traffic per real frame at the Index's 2468x2740
 * (~0.15 ms on a 5090), only while engaged.
 *
 * WARP (per shown frame while engaged): one compute dispatch over both eyes (z = eye), into an
 * R8G8B8A8 storage image whose bytes are written in the eye swapchain's byte order (B and R
 * swapped for the B8G8R8A8 eyes), so LLXrSession copies it into the eyes bit for bit. Per pixel a
 * backward gather solved by fixed-point iteration (Bowles et al. 2012, "Iterative image warping"):
 *  - the source position of a target pixel is found by iterating "where would the source point
 *    under my current guess land": per eye a source clip -> target clip matrix (positional:
 *    translation, roll, the off-axis eye frusta) built on the CPU in double, applied to the
 *    source depth -- a static point moves exactly as the eyes did;
 *  - moving content: the motion vector minus the camera's own part (the source depth through the
 *    camera-only reprojection) is the object's screen motion per real-frame interval, carried
 *    forward by the fraction of that interval since the source was rendered;
 *  - the controllers (hand models and laser beams are world rows): a source point inside a
 *    controller's capsule moves rigidly with that controller's own pose change (its own matrix),
 *    so the hands and the beam follow the controllers at the full rate instead of the world's;
 *  - disocclusions: two iteration chains, seeded at the local depth and at the far plane; where
 *    both find a consistent source the nearer one is visible, where neither does the farther one
 *    fills the hole (the background side of the edge, stretched); the lens' hidden area and the
 *    image border are never a source when another candidate exists.
 *  The same matrices are built for both eyes from one head prediction, and both eyes use the same
 *  rules and the same fraction: the eyes always agree.
 *  Cost (ESTIMATE, 2468x2740 x 2 eyes = 13.5 MP): two chains x 3 iterations of (depth 4 B +
 *  motion 8 B, cache-coherent) + one bilinear colour fetch + a 4 B store; ~0.3 GB of traffic and
 *  ~0.5 kFLOP per pixel: about 0.3-0.6 ms on a 5090, measured by timestamps into the 10 s line.
 *
 * Every object is created on the first engagement (no allocation, pipeline or command pool
 * otherwise; nothing at all on the desktop) and released with the VR target. Allocation is soft:
 * out of memory refuses the warp (the frame renders the world as without space warp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#pragma once

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"   // PipelineJobSlot

#include <cstdint>
#include <functional>

class LLVRSpaceWarpVK
{
public:
    static constexpr uint32_t kEyes = 2;

    // One eye of one frame, agent space (column-major, Vulkan clip: Y down, reversed Z), unjittered.
    struct Eye
    {
        float view[16] = {};
        float proj[16] = {};
        float pos[3] = {};      // the eye (agent space)
    };
    // A controller at one frame: its grip pose (agent space; the rigid body the hand model and the
    // beam belong to) and its beam segment (from == to: no beam).
    struct Hand
    {
        bool valid = false;
        float pos[3] = {};
        float axis[3][3] = {{1.f, 0.f, 0.f}, {0.f, 1.f, 0.f}, {0.f, 0.f, 1.f}};   // the pose's x, y, z (agent space)
        float beamFrom[3] = {};
        float beamTo[3] = {};
        bool beam = false;
    };

    struct CaptureIn
    {
        // The world's final colour (every eye layer), in TRANSFER_SRC_OPTIMAL (left there).
        VkImage color = VK_NULL_HANDLE;
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;
        VkExtent2D colorExtent = {0, 0};
        uint32_t layers = 0;
        // Its depth (layer per eye) in depthLayout (restored).
        VkImage depth = VK_NULL_HANDLE;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        VkImageLayout depthLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkExtent2D depthExtent = {0, 0};
        // Its motion vectors (layer per eye) in motionLayout (restored); null / !motionValid: no
        // object motion is carried forward from this frame.
        VkImage motion = VK_NULL_HANDLE;
        VkFormat motionFormat = VK_FORMAT_UNDEFINED;
        VkImageLayout motionLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkExtent2D motionExtent = {0, 0};
        bool motionValid = false;
        Eye eyes[kEyes];
        float camPrev[kEyes][16] = {};   // this frame's unjittered clip -> the previous real frame's
        Hand hands[2];
    };

    struct WarpIn
    {
        Eye eyes[kEyes];          // this frame's eyes at their display pose (the warp's target)
        Hand hands[2];            // the controllers at this frame's display time
        float extrapolate = 0.f;  // motion-vector intervals since the source frame (object motion)
        uint32_t debug = 0;       // RenderVKVRSpaceWarpDebug (2: this is a warp-only frame, tinted)
        bool swapRB = true;       // the eyes are B8G8R8A8: write the bytes in that order
    };

    // Creates the pipeline on first use (in the background: false until it is built) and the history
    // images at the inputs' sizes; records the copies into `cmd` (the real frame's last graphics
    // command buffer). False: nothing recorded, the history is not valid.
    bool capture(VkDeviceContext& dev, VkCommandBuffer cmd, const CaptureIn& in);
    bool hasHistory() const { return mHistory; }
    void dropHistory() { mHistory = false; }
    // The eyes the history was rendered with (cut detection).
    const Eye& historyEye(uint32_t v) const { return mSrc.eyes[v < kEyes ? v : 0]; }

    // A frame's warp into its own command buffer, then `recordXr` (the eye copies of LLXrSession,
    // given the output: R8G8B8A8 bytes in the eyes' order, TRANSFER_SRC_OPTIMAL, eye v = layer v),
    // submitted to `queue` with its own fence. False: nothing was submitted (no history, no
    // pipeline yet, out of memory, a fence that did not come back).
    bool warpAndSubmit(VkDeviceContext& dev, VkQueue queue, const WarpIn& in,
                       const std::function<void(VkCommandBuffer, VkImage, VkExtent2D, uint32_t)>& recordXr);
    // The GPU time of the newest warp whose timestamps came back (ms; < 0: none new).
    float takeGpuMs();

    // Frees everything (the session stopped, the VR target went). Waits for the warp's own fences.
    void release(VkDeviceContext& dev);

private:
    bool ensurePipeline(VkDeviceContext& dev);
    bool ensureImages(VkDeviceContext& dev, const CaptureIn& in);
    bool ensureCommands(VkDeviceContext& dev);
    void writeSet(VkDeviceContext& dev);
    void destroyImages(VkDeviceContext& dev);

    // pipeline
    PipelineJobSlot mJob;
    VkPipeline mStage = VK_NULL_HANDLE;
    VkPipeline mPipe = VK_NULL_HANDLE;
    bool mPipeFailed = false;
    VkSampler mLinear = VK_NULL_HANDLE, mNearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkPipelineLayout mLayout = VK_NULL_HANDLE;
    VkDescPool mPool;
    VkDescriptorSet mSet = VK_NULL_HANDLE;
    bool mSetDirty = true;
    GpuBuffer mParams;

    // history (+ the warp's output)
    GpuImage mColor, mDepth, mMotion, mOut;   // 2D_ARRAY views (arrayView), one layer per eye
    bool mHistory = false;
    bool mImagesFailed = false;
    uint32_t mRetryAt = 0, mFrameNo = 0;
    CaptureIn mSrc;   // the history's per-frame data (no images)

    // own submission ring (never entangled with the swapchain's frame slots)
    static constexpr uint32_t kRing = 3;
    VkCommandPool mCmdPool = VK_NULL_HANDLE;
    VkCommandBuffer mCmd[kRing] = {};
    VkFence mFence[kRing] = {};
    bool mSubmitted[kRing] = {};
    uint32_t mRingPos = 0;
    // GPU timer: two timestamps per ring entry, read back without waiting
    VkQueryPool mQueries = VK_NULL_HANDLE;
    bool mQueryWritten[kRing] = {};
    float mGpuMs = -1.f;
    bool mLogged = false;
};
