/**
 * @file llcleanblendsplitvk.h
 * @brief Split transparency (RenderVKBlendSplit) for the Vulkan clean renderer's static alpha-blended surfaces:
 *        the per-layer forward lighting replaced by a translucency lighting volume, the front layer lit once
 *        per pixel.
 *
 * clean_bs_common.glsl has the stages and the data; per frame (outside any scope, before pass 2a):
 *   recordVolume   A  the translucency lighting volume (a fullscreen pass over a 2D layout of the froxel cells)
 *   beginFront / endFront
 *                  B  the static alpha lists of both water sides into the front surface (the renderer draws the
 *                     lists with frontPipe(): its buffers, counters, VS streams, draw sets)
 *   recordLight    C  the front layer lit once per pixel (fullscreen)
 * and inside pass 2, at the static lists' place: the renderer draws them with blendPipe() (stage D: every layer from
 * the volume, the front layer from C) -- or, under MBOIT, MBOIT's shade pass with blendMbPipe() -- and
 * recordSlice() for the volume slice view.
 *
 * PARAMS: a uniform block in the module's set, written in the command stream (writeParams: vkCmdUpdateBuffer,
 * outside any scope, before stage A) -- never by the host while a frame in flight may read it.
 *
 * MEMORY (bounded; freed after ~10 s unused, everything with the setting off): the volume W x H x D cells x
 * 8 B x 10 images (lighting ping-pong 2 x 4, the tail 2) -- resolution-independent; the front surface 40 B per render
 * pixel (F0 4 + F1 8 + F2 4 + F3 4 + F4 8 + depth 4 + the lit colour 8). Dedicated allocations: the G-buffer is NOT
 * dead in this window whenever one-frame-latency GI runs or DLSS Ray Reconstruction reads its guides (see
 * llcleanalphamboitvk.h). An allocation failure returns false (today's sorted path that frame), logged, retried in
 * 5 s.
 *
 * Never in stereo, snapshots, orthographic views, the transparent avatar layer, lit debug views / Highlight
 * Transparent (the renderer keeps the sorted path there, logged).
 */
#ifndef LL_LLCLEANBLENDSPLITVK_H
#define LL_LLCLEANBLENDSPLITVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"

#include <cstdint>

class LLCleanBlendSplitVK
{
public:
    struct Build
    {
        VkDeviceContext* ctx = nullptr;
        GraphicsPipelineDesc alphaDesc;     // the live LIT_ALPHA state (plain, mDrawLayout): stage D's / B's template
        bool rt = false;                    // build the ray-query twins (volume, front light)
        VkDescriptorSetLayout dslDraw0 = VK_NULL_HANDLE, dslDraw1 = VK_NULL_HANDLE;
        VkDescriptorSetLayout dslRt = VK_NULL_HANDLE;   // the RT consumer set (set 3 of the twins)
        VkDescriptorSetLayout dslMb = VK_NULL_HANDLE;   // MBOIT's set (set 2 of blendMbPipe; null: not built)
        uint32_t pushBytes = 16;
        const VkSpecializationInfo* spec = nullptr;      // the lit FS's live specialization
        uint64_t specKey = 0;
        bool generic = false;
        VkFormat hdrFormat = VK_FORMAT_UNDEFINED, glowFormat = VK_FORMAT_UNDEFINED;
        VkFormat reactFormat = VK_FORMAT_UNDEFINED, depthFormat = VK_FORMAT_UNDEFINED;
    };
    // The params block (clean_bs_common.glsl BsParamsBlock, std140).
    struct Params
    {
        float vp[16];
        float ivp[16];
        float pvp[16];
        float cam[4];
        float fwd[4];
        float grid[4];
        float pfwd[4];
        float knobs[4];
        uint32_t flags[4];
        uint32_t tile[4];
    };
    static_assert(sizeof(Params) == 304, "clean_bs_common.glsl BsParamsBlock");
    // clean_bs_common.glsl BSF_*
    static constexpr uint32_t kFlagFront = 1u, kFlagHistory = 2u, kFlagCascade = 4u, kFlagLocal = 8u, kFlagProbes = 16u,
                              kFlagTrace = 32u, kFlagPost = 64u;
    static constexpr uint32_t kDebugSlice = 2u;   // BSD_SLICE (the overlay pass)

    static constexpr VkFormat kVolFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kF0Format = VK_FORMAT_R8G8B8A8_SRGB;
    static constexpr VkFormat kF1Format = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kF2Format = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat kF3Format = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    static constexpr VkFormat kF4Format = VK_FORMAT_R32G32_UINT;
    static constexpr VkFormat kLitFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    LLCleanBlendSplitVK() = default;
    ~LLCleanBlendSplitVK() = default;   // destroy() is explicit
    LLCleanBlendSplitVK(const LLCleanBlendSplitVK&) = delete;
    LLCleanBlendSplitVK& operator=(const LLCleanBlendSplitVK&) = delete;

    // True when the pipelines are installed. NEVER BLOCKS: one background job per pipeline; the new set is
    // installed only when every job of the round finished (the frame keeps the sorted path until then).
    bool ensurePipelines(const Build& b);
    bool pipelinesPending() const;
    void cancelJobs();
    // The volume (W x H x D cells) and the front surface (render size). False: the sorted path this frame.
    bool ensureTargets(VkDeviceContext* ctx, uint32_t w, uint32_t h, uint32_t d, uint32_t rw, uint32_t rh);
    // The 2D layout of the volume pass: tiles per row and the viewport.
    uint32_t tilesPerRow() const { return mTilesX; }

    enum Pipe : uint32_t { P_VOLUME = 0, P_VOLUME_RT, P_FRONT, P_LIGHT, P_LIGHT_RT, P_BLEND, P_BLEND_MB, P_SLICE, P_COUNT };
    bool ready() const { return mReady; }
    bool rtBuilt() const { return mReady && mLive[P_VOLUME_RT] && mLive[P_LIGHT_RT]; }
    VkPipeline frontPipe() const { return mReady ? mLive[P_FRONT] : VK_NULL_HANDLE; }
    VkPipeline blendPipe() const { return mReady ? mLive[P_BLEND] : VK_NULL_HANDLE; }
    VkPipeline blendMbPipe() const { return mReady ? mLive[P_BLEND_MB] : VK_NULL_HANDLE; }

    // Once per frame, outside any scope, before stage A: flips the parity (this frame's volume images), then the
    // params update. history: the previous build is a valid history for this one (the caller's knowledge: grid,
    // range and camera continuity).
    void beginFrame(VkCommandBuffer cmd, const Params& p);
    // A: the draw sets 0 / 1 bound and the main view pushed by the caller; rtSet: the RT consumer set (the ray-query
    // twin) or null (plain).
    void recordVolume(VkCommandBuffer cmd, VkDescriptorSet rtSet);
    // B: clears the front surface and opens its scope (the caller draws the lists with frontPipe() and bindSet()).
    void beginFront(VkCommandBuffer cmd);
    void endFront(VkCommandBuffer cmd);
    // C: the draw sets 0 / 1 bound and the main view pushed by the caller.
    void recordLight(VkCommandBuffer cmd, VkDescriptorSet rtSet);
    // The module's set at 2 for blendPipe() / frontPipe(), at 3 (after MBOIT's) for blendMbPipe().
    void bindSet(VkCommandBuffer cmd) const;
    void bindSetMb(VkCommandBuffer cmd) const;
    // Inside pass 2: the volume slice view (RenderVKBlendSplitDebug 2).
    void recordSlice(VkCommandBuffer cmd);

    uint64_t volumeBytes() const;
    uint64_t frontBytes() const;
    uint32_t volW() const { return mVw; }
    uint32_t volH() const { return mVh; }
    uint32_t volD() const { return mVd; }
    void freeTargets();
    void destroy(VkDevice dev);

private:
    void destroyPipelines(VkDevice dev);
    void destroyStage(VkDevice dev);
    void writeSets();

    VkDeviceContext*      mCtx = nullptr;
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkDescPool            mPool;
    VkDescriptorSet       mDset[2] = {};
    VkPipelineLayout      mLayout = VK_NULL_HANDLE;     // {draw0, draw1, mDsl}
    VkPipelineLayout      mLayoutRt = VK_NULL_HANDLE;   // {draw0, draw1, mDsl, RT}
    VkPipelineLayout      mLayoutMb = VK_NULL_HANDLE;   // {draw0, draw1, MBOIT, mDsl}
    VkSampler             mSampler = VK_NULL_HANDLE;
    GpuBuffer             mParams;

    VkPipeline mLive[P_COUNT] = {};
    VkPipeline mStage[P_COUNT] = {};
    PipelineJobSlot mSlot[P_COUNT];
    double     mSince[P_COUNT] = {};
    bool       mInRound = false;
    double     mRoundSince = 0.0;
    bool       mReady = false;
    uint64_t   mBuiltKey = ~0ull;
    bool       mWantRt = false, mStageRt = false;
    bool       mWantMb = false;
    VkFormat   mBuiltFormats[4] = {};
    double     mRetryBuildAt = 0.0;

    // The volume: [p][0..3] the lighting (SH R, G, B, sun) of parity p, [2][0 / 1] the tail (offset, slope).
    GpuImage   mVol[2][4];
    GpuImage   mPost[2];
    // The front surface F0-F4, its depth, the lit colour.
    GpuImage   mFront[5];
    GpuImage   mFrontDepth;
    GpuImage   mLit;
    VkFormat   mDepthFormat = VK_FORMAT_UNDEFINED;
    uint32_t   mVw = 0, mVh = 0, mVd = 0, mTilesX = 1, mTilesY = 1, mW = 0, mH = 0;
    uint32_t   mParity = 0;
    bool       mFresh = true;
    double     mRetryAt = 0.0;
};

#endif
