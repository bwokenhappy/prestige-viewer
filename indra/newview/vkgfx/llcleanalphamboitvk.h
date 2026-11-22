/**
 * @file llcleanalphamboitvk.h
 * @brief Moment-based order-independent transparency (RenderVKAlphaMBOIT) for the Vulkan clean
 *        renderer's STATIC (non-avatar) alpha-blended surfaces.
 *
 * After Muenstermann, Krumpen, Klein and Peters 2018, "Moment-Based Order-Independent Transparency"
 * (Proc. ACM CGIT 1(1)), implemented from the paper (clean_mboit.glsl has the passes and the maths).
 * Per water side, outside the pass-2 scope:
 *   beginMoments / endMoments   pass 1: the static alpha list into the moment targets (additive, render
 *                               or half resolution; the scene depth is tested in the FS: #731 tolerance)
 *   recordCutoff                pass 2: fullscreen, the conservative cutoff depth (+ the opaque depth)
 *   beginShade / endShade       pass 3: the static alpha list again (twice: the FULL and CHEAP tier
 *                               modules, each adding its share), early-Z against the cutoff depth, lit
 *                               per fragment, into the colour / glow sums and the multiplicative revealage
 * and inside the pass-2 scope, at the static list's place in the blend order:
 *   recordResolve               the composite over the scene (+ glow, + the reactive mask)
 * The renderer draws the lists (its buffers, counters, VS streams, draw sets) with momentsPipe() /
 * shadeFullPipe() / shadeCheapPipe(); the world-only VS (CLEAN_VS_MB_WORLD) collapses every avatar face, which the renderer draws
 * on the sorted path (AVD_PREPASS_PUSH) after the resolve.
 *
 * PARAMS: a 64-byte uniform block in the module's set, written in the command stream (writeParams:
 * vkCmdUpdateBuffer, outside any scope, before the first pass of the frame) -- never by the host while
 * a frame in flight may read it.
 *
 * MEMORY (bounded, render size; freed after ~10 s unused and with the mode off): moments 24 B / moment px
 * (mode 1: RGBA32F + RG32F) or 32 B (mode 2: 2 x RGBA32F) at render or half resolution, + 4 B cutoff
 * depth + 18 B shade sums (2 x RGBA16F + R16F) per render px. Dedicated allocations: the G-buffer --
 * the obvious dead target -- is NOT dead in this window whenever one-frame-latency GI runs (its async
 * q_gi_lag segment samples G0..G3 until after pass 2) or DLSS Ray Reconstruction reads its guides after
 * the resolve; see agents/claude_mboit_report.md. An allocation failure returns false (the sorted path
 * that frame), logged, retried in 5 s.
 *
 * Never in stereo, snapshots, debug views / Highlight Transparent or orthographic views (the renderer
 * keeps the sorted path there).
 */
#ifndef LL_LLCLEANALPHAMBOITVK_H
#define LL_LLCLEANALPHAMBOITVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"

#include <cstdint>

class LLCleanAlphaMBOITVK
{
public:
    struct Build
    {
        VkDeviceContext* ctx = nullptr;
        GraphicsPipelineDesc statDesc[2];   // the live LIT_ALPHA state: [0] plain (mDrawLayout), [1] ray-query
        bool rt = false;                    // build the ray-query full-tier twin
        bool tiers = true;                  // build the CHEAP tier module (two shade passes); false: the full one alone
        uint32_t mode = 1;                  // 1 = 4 power moments, 2 = 6 power moments
        VkDescriptorSetLayout dslDraw0 = VK_NULL_HANDLE, dslDraw1 = VK_NULL_HANDLE;
        VkDescriptorSetLayout dslRt = VK_NULL_HANDLE;   // the RT consumer set (set 2 of the ray-query shade)
        uint32_t pushBytes = 16;
        const VkSpecializationInfo* spec = nullptr;      // the lit FS's live specialization
        uint64_t specKey = 0;
        bool generic = false;
        VkFormat hdrFormat = VK_FORMAT_UNDEFINED, glowFormat = VK_FORMAT_UNDEFINED;
        VkFormat reactFormat = VK_FORMAT_UNDEFINED, depthFormat = VK_FORMAT_UNDEFINED;
    };
    // The params block (clean_mboit.glsl MbParamsBlock, std140).
    struct Params
    {
        float warp[4];
        float knobs[4];
        uint32_t sizes[4];
        uint32_t flags[4];
    };
    static_assert(sizeof(Params) == 64, "clean_mboit.glsl MbParamsBlock");

    static constexpr VkFormat kMomAFormat   = VK_FORMAT_R32G32B32A32_SFLOAT;
    static constexpr VkFormat kMomB1Format  = VK_FORMAT_R32G32_SFLOAT;          // mode 1: b0, count
    static constexpr VkFormat kMomB2Format  = VK_FORMAT_R32G32B32A32_SFLOAT;    // mode 2: b0, b5, b6, count
    static constexpr VkFormat kSumFormat    = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kRevealFormat = VK_FORMAT_R16_SFLOAT;

    LLCleanAlphaMBOITVK() = default;
    ~LLCleanAlphaMBOITVK() = default;   // destroy() is explicit
    LLCleanAlphaMBOITVK(const LLCleanAlphaMBOITVK&) = delete;
    LLCleanAlphaMBOITVK& operator=(const LLCleanAlphaMBOITVK&) = delete;

    // True when the set for b.mode is installed (background compile on first use / key change).
    // NEVER BLOCKS: one background job per pipeline (a log line at each start and finish, with its wall time);
    // the new set is installed only when every job of the round finished.
    bool ensurePipelines(const Build& b);
    bool pipelinesPending() const;
    // Stop the running jobs from starting more compiles (a compile already in the driver finishes).
    void cancelJobs();
    // half: the moments at half resolution. False: the sorted path this frame.
    bool ensureTargets(VkDeviceContext* ctx, uint32_t mode, uint32_t rw, uint32_t rh, bool half);
    enum Pipe : uint32_t { P_MOMENTS = 0, P_SHADE_FULL, P_SHADE_FULL_RT, P_SHADE_CHEAP, P_CUTOFF, P_RESOLVE, P_COUNT };
    VkPipeline momentsPipe() const { return mReady ? mLive[P_MOMENTS] : VK_NULL_HANDLE; }
    // The FULL tier pass (rt: its ray-query twin when built) and the CHEAP tier pass (always plain).
    VkPipeline shadeFullPipe(bool rt) const
    { return !mReady ? VK_NULL_HANDLE : (rt && mLive[P_SHADE_FULL_RT]) ? mLive[P_SHADE_FULL_RT] : mLive[P_SHADE_FULL]; }
    VkPipeline shadeCheapPipe() const { return mReady ? mLive[P_SHADE_CHEAP] : VK_NULL_HANDLE; }
    // The set was built for (and the frame may use) b.tiers: two shade passes.
    bool tiersBuilt() const { return mReady && mBuiltTiers && mLive[P_SHADE_CHEAP] != VK_NULL_HANDLE; }
    bool shadeRtBuilt() const { return mReady && mLive[P_SHADE_FULL_RT] != VK_NULL_HANDLE; }
    // The module's set layout (split transparency's MBOIT shade pipeline takes it at set 2).
    VkDescriptorSetLayout setLayout() const { return mDsl; }
    bool ready() const { return mReady; }
    uint32_t momentW() const { return mMw; }
    uint32_t momentH() const { return mMh; }

    // Outside any scope, once per frame before the first pass.
    void writeParams(VkCommandBuffer cmd, const Params& p);
    // Destroys everything. Never while a job runs except at shutdown (it cancels, then waits for the compiles
    // already in the driver): the renderer parks the module until pipelinesPending() is false.
    // Binds the module's set (plain layout: set 2; rtLayout: set 3 after the RT set).
    void bindSet(VkCommandBuffer cmd, bool rtLayout) const;
    void beginMoments(VkCommandBuffer cmd);   // sets viewport / scissor to the moment size
    void endMoments(VkCommandBuffer cmd);
    void recordCutoff(VkCommandBuffer cmd);   // the draw sets 0 / 1 and the main view push are the caller's
    void beginShade(VkCommandBuffer cmd);     // viewport / scissor: the caller's (render size)
    void endShade(VkCommandBuffer cmd);
    void recordResolve(VkCommandBuffer cmd);  // inside the pass-2 scope; rebinds nothing else

    uint64_t targetBytes() const;
    uint32_t mode() const { return mMode; }
    bool half() const { return mHalf; }
    void freeTargets();
    void destroy(VkDevice dev);

private:
    void destroyPipelines(VkDevice dev);
    void destroyStage(VkDevice dev);
    void writeSet();

    VkDeviceContext*      mCtx = nullptr;
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;   // 0-4 the targets (sampled), 5 the params
    VkDescPool            mPool;
    VkDescriptorSet       mDset = VK_NULL_HANDLE;
    VkPipelineLayout      mLayout = VK_NULL_HANDLE;     // {draw0, draw1, mDsl}
    VkPipelineLayout      mLayoutRt = VK_NULL_HANDLE;   // {draw0, draw1, RT, mDsl}
    GpuBuffer             mParams;

    VkPipeline mLive[P_COUNT] = {};
    VkPipeline mStage[P_COUNT] = {};      // written by the background jobs; installed when the round is done
    PipelineJobSlot mSlot[P_COUNT];
    double     mSince[P_COUNT] = {};
    bool       mInRound = false;          // a round of jobs was started and not yet installed / dropped
    double     mRoundSince = 0.0;
    bool       mReady = false;
    uint64_t   mBuiltKey = ~0ull;
    uint32_t   mBuiltMode = 0, mWantMode = 0, mStageMode = 0;
    bool       mBuiltRt = false, mWantRt = false, mStageRt = false;
    bool       mBuiltTiers = false, mWantTiers = false, mStageTiers = false;
    VkFormat   mBuiltFormats[4] = {};
    double     mRetryBuildAt = 0.0;

    // [0] moments A, [1] moments B, [2] colour sum, [3] glow sum, [4] revealage; mDepth the cutoff depth.
    GpuImage   mTarget[5];
    GpuImage   mDepth;
    VkFormat   mDepthFormat = VK_FORMAT_UNDEFINED;
    uint32_t   mMode = 0, mW = 0, mH = 0, mMw = 0, mMh = 0;
    bool       mHalf = false;
    bool       mFresh = true;     // new targets: the first barriers come from UNDEFINED
    double     mRetryAt = 0.0;
};

#endif
