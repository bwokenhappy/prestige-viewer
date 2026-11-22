/**
 * @file llcleandlssrrvk.h
 * @brief #870 DLSS Ray Reconstruction (joint denoise + upscale) and #962 DLSS Super Resolution
 *        (upscale / DLAA) for the Vulkan clean renderer: RenderVKUpscaler 7-11 (DLSS at a
 *        quality level; RenderVKDlssRayReconstruction picks the network), through the
 *        user-installed slip_nr shim (llvkneuralrecon.h).
 *
 * Want::kind is the network. RR runs the four steps below; SR (F_SR) runs 1, then
 * clean_sr_color.comp (the colour copy + the bias-current-colour mask, render size: no G-buffer
 * reads, no guides), then 3 with colour / depth / motion / the exposure image, and 4 only when
 * RenderVKDlssSRExposure pre-exposes. Switching kind is a Want change like any other: prestep
 * re-creates the targets (SR holds fewer) and the network feature; its first evaluate resets
 * history.
 *
 * It takes FSR 3.1's slot (after pass 2, where LLCleanFSR3VK::record runs) and keeps FSR's
 * Frame contract: render extent in, display extent out, jitter, reset, the target-generation
 * rule for its descriptor sets. Per frame (all on the graphics queue):
 *   1. INPUTS (clean_fsr_inputs.comp, FSR's own pass, its own pipeline + targets here): the
 *      completed motion (camera reprojection for sky / water / a frame whose pre-pass wrote no motion) and the
 *      water-substituted depth, plus the resolved exposure.
 *   2. GUIDES (clean_rr_guides.comp): linear diffuse / specular albedo, world normal +
 *      roughness, specular hit distance -- from the G-buffer only -- and the pre-exposed colour.
 *   3. EVALUATE: the network, recorded by the shim into the frame's command buffer (it may
 *      change pipeline / descriptor bindings: everything after re-binds, as after FSR).
 *   4. UNEXPOSE (clean_rr_unexpose.comp): the display-size output divided by the exposure.
 * The output is the display-size linear HDR scene the post chain reads (mHdr); our CAS / RCAS
 * sharpening runs after it in the final pass (RR does no sharpening).
 *
 * FRAME STRUCTURE (the pdnPrestep rule): nothing here is created, freed or re-bound while a
 * frame records. ready() only compares what the sets / feature were built for with what this
 * frame wants; a mismatch is recorded as a request by the renderer (the frame falls back to
 * FSR 3.1 / TAA) and prestep() -- syncFromStoresPrestep, before the next command buffer
 * begins -- waits for the device, (re)creates the targets, rewrites the sets and creates the
 * network feature in a one-shot submit. The constant buffer is a ring over the frames in
 * flight. Off, it holds nothing (release()).
 */
#ifndef LL_LLCLEANDLSSRRVK_H
#define LL_LLCLEANDLSSRRVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "llvkneuralrecon.h"

#include "llrrhistorypolicy.h"

#include <chrono>
#include <cstdint>
#include <string>

class LLCleanDlssRRVK
{
public:
    // What the sets name (the renderer's current targets). Compared by ready(); written by
    // prestep(). Layouts at record time: the G-buffer / scene / motion / react / ref /
    // no-temporal SHADER_READ_ONLY_OPTIMAL, depth DEPTH_READ_ONLY_OPTIMAL, exposure / RT
    // trace GENERAL (recordTaaInputBarriers + the lighting passes leave them so).
    struct Targets
    {
        VkImageView depth = VK_NULL_HANDLE, motion = VK_NULL_HANDLE, react = VK_NULL_HANDLE,
                    noTemporal = VK_NULL_HANDLE, scene = VK_NULL_HANDLE, ref = VK_NULL_HANDLE,
                    exposure = VK_NULL_HANDLE, out = VK_NULL_HANDLE;
        VkImageView gbuf[4] = {};
        VkImageView rtTrace = VK_NULL_HANDLE;      // VK_NULL_HANDLE: the placeholder
        VkBuffer    rtTiles = VK_NULL_HANDLE;
        // The GI trace's MOT image (LLCleanDenoiserVK::motView of the GI denoiser; VK_NULL_HANDLE: none -- the
        // placeholder): its w bit 8 marks a CHANGED radiance-cache cell (clean_gi_common.glsl GI_MOT_CHANGED)
        VkImageView giMot = VK_NULL_HANDLE;
        // The stochastic local-light denoiser's MOT image (LLCleanDenoiserVK::motView of the pass-15 denoiser;
        // VK_NULL_HANDLE: the placeholder): its w bit 20 marks a texel whose light really dropped
        // (clean_pdn_common.glsl PDN_MOT_LIGHT_DROP, clean_dn_temporal.comp THE DROP MARK)
        VkImageView pdnMot = VK_NULL_HANDLE;
        // Their identities (LLCleanDenoiserVK::motSerial; 0 with the placeholder). The denoisers re-create MOT in
        // their own prestep (no mTargetGen bump), and the new view can get the freed view's handle back: compared by
        // handle alone, ready() then kept the guide set naming the freed MOT (2026-11-21 DEVICE_LOST, READ_INVALID
        // in the freed 1920x1044 MOT after a 1920x1044 -> 3840x2088 projector denoiser re-create).
        uint64_t    giMotSerial = 0, pdnMotSerial = 0;
        VkSampler   nearest = VK_NULL_HANDLE;
        uint64_t    targetGen = 0;                 // LLCleanRendererVK::targetGen()
        uint64_t    rtEpoch = 0;                   // the RT reflection targets' re-creation count
        bool operator==(const Targets& o) const;
    };
    struct Want
    {
        LLVKNeuralRecon::FeatureKind kind = LLVKNeuralRecon::F_RR;   // the network (#962)
        uint32_t rw = 0, rh = 0, dw = 0, dh = 0;
        LLVKNeuralRecon::Quality q = LLVKNeuralRecon::Q_DLAA;
        VkImage  outImage = VK_NULL_HANDLE;        // display-size mHdr (STORAGE)
        VkFormat outFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        Targets  t;
    };
    struct Frame
    {
        Want      w;
        VkImage   sceneImage = VK_NULL_HANDLE;
        VkFormat  sceneFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
        const float* reproj = nullptr;      // 16: unjittered clip -> last frame's clip (camera)
        const float* waterPlane = nullptr;  // 4
        const float* view = nullptr;        // 16: world -> view (column-major)
        const float* proj = nullptr;        // 16: unjittered projection (column-major)
        const float* viewProjRel = nullptr; // 16: unjittered P x rotation-only V (column-major)
        float    jitterPx[2] = {0.f, 0.f};  // render pixels (sample q shows q + 0.5 - jitter)
        float    jitterNdc[2] = {0.f, 0.f};
        float    jitterSign = 1.f;          // RenderVKDlssJitterSign (the network's convention)
        bool     motionValid = false;
        float    staticExposure = 1.f;
        bool     dynamicExposure = false;
        bool     reset = false;
        uint32_t resetReasons = 0; // 1 temporal/cut, 2 skipped frame, 4 network change
        LLRRHistory::Policy policy{}; // semantics, never per-light/content state
        bool guideHealth = false;   // sample <=1 frame/second; read at fenced slot reuse
        bool     rtValid = false;           // the RT trace + tile list are this frame's
        bool     rtDense = false;           // ... with one ray per candidate pixel (the hit guide takes each own ray's t)
        uint32_t rtTilesPerRow = 0, rtTileCount = 0, rtHdrWords = 0;
        float    marchDist = 30.f;          // RenderVKDlssHitMarch (m)
        uint32_t marchSteps = 24;
        uint32_t slot = 0, slotCount = 2;
        // Milliseconds since the previous frame, shared by every view's instance (#1075 stereo:
        // one clock for both eyes); < 0: this instance's own clock (a single view, as before).
        float    frameTimeMs = -1.f;
        // The output IS the scene colour input (render = display): it is moved
        // SHADER_READ_ONLY -> GENERAL inside record(), after the guide pass read it.
        bool     outIsScene = false;
        // TRANSPARENCY OVERLAY (RenderVKDlssTransparencyLayer, a shim >= 1.1): the colour input is
        // the opaque scene (Want::t.ref = mRefract, copied this frame), pass 2's transparents
        // go in as the overlay the network upscales without denoising.
        bool     overlay = false;
        float    responsivity = 0.f;        // RenderVKDlssResponsivity (0: no mask handed over)
        // (the sealed-room fade) the GI MOT image (Targets::giMot) holds THIS frame's trace at giW x giH: pixels
        // whose radiance-cache cell is CHANGED get full responsivity (x the gain) -- the GI reaches RR raw, so
        // RR's own history is the only one left to cut there
        bool     giChanged = false;
        uint32_t giW = 0, giH = 0;
        // (the light-drop fade) the local-light denoiser's MOT image (Targets::pdnMot) holds THIS frame's drop marks
        // at pdnW x pdnH: pixels whose direct local light really dropped get full responsivity (x the gain) --
        // our denoiser let the old light go, RR's own history is the one left to cut there
        bool     lightDrop = false;
        uint32_t pdnW = 0, pdnH = 0;
        uint32_t debug = 0;                // RenderVKDlssDebug (clean_rr_debug.comp modes; 0 = off; RR only)
        // SR (#962): RenderVKDlssSRExposure 1 -- the colour is pre-exposed and the output divided
        // back (the RR path's handling); 0 -- the unexposed scene + the exposure image.
        bool     srPreExpose = false;
        // Frame generation (#1517): the main window's view (not a snapshot / offscreen / VR eye):
        // its camera goes to frame generation before the evaluate.
        bool     frameGenView = false;
    };
    // The inputs pass's depth (R32F reversed-Z device depth) and motion (RG16F, prev_uv - uv, UV
    // units, unjittered), render size, GENERAL: frame generation reads them after the evaluate.
    const GpuImage& inDepth() const { return mInDepth; }
    const GpuImage& inMotion() const { return mInMotion; }
    // (record(): the main view's camera to frame generation before its evaluate, in the renderer's own
    // terms -- llfg::GenCamera)
    void frameGenConstants(const Frame& f, const LLVKNeuralRecon::Eval& e) const;
    static constexpr VkImageUsageFlags kInputUsage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    LLCleanDlssRRVK() = default;
    ~LLCleanDlssRRVK() = default;   // destroy() is explicit
    LLCleanDlssRRVK(const LLCleanDlssRRVK&) = delete;
    LLCleanDlssRRVK& operator=(const LLCleanDlssRRVK&) = delete;

    // In-frame, no side effects: the targets, sets and network feature match w.
    bool ready(const Want& w) const;
    // Outside any recording (syncFromStoresPrestep): make it match w. Waits for the device
    // when anything changes. false = it could not (err: why; the renderer backs off / falls
    // back to FSR).
    bool prestep(VkDeviceContext* ctx, const Want& w, uint32_t slotCount, std::string& err);
    // Outside any recording: free everything (waits for the device when anything is held).
    void release();
    bool holding() const { return mFeature || mColor.image || mDsl[0] || mCb.buffer; }
    // The network the current feature runs (valid while holding a feature).
    LLVKNeuralRecon::FeatureKind builtKind() const { return mBuilt.kind; }

    // Records the whole reconstruction (after ready() with f.w). The output (f.w.outImage)
    // then holds the display-size linear HDR scene: GENERAL, last written by a compute
    // storage write. false: the evaluate failed (err) -- the output is undefined this frame.
    bool record(VkCommandBuffer cmd, const Frame& f, std::string& err);

    void destroy();   // shutdown (waits for the device)
    // The last record() moved an outIsScene output to GENERAL (else it is still SHADER_READ_ONLY).
    bool outputMovedToGeneral() const { return mOutGeneral; }

private:
    enum Pass { P_INPUTS, P_GUIDES, P_UNEXPOSE, P_DEBUG, P_SRCOLOR, P_COUNT };
    bool ensurePipelines(VkDeviceContext* ctx);
    bool ensureResources(const Want& w);
    void destroyResources();
    bool ensureCb(uint32_t slotCount);
    void writeSets(const Want& w);
    void computeBarrier(VkCommandBuffer cmd);

    VkDeviceContext* mCtx = nullptr;
    VkDescriptorSetLayout mDsl[P_COUNT] = {};
    VkPipelineLayout mLayout[P_COUNT] = {};
    VkPipeline mPipe[P_COUNT] = {};
    VkDescPool mPool;
    VkDescriptorSet mSet[P_COUNT] = {};

    // Constant buffer ring: per frame slot {inputs params, guide params}.
    static constexpr uint32_t kCbInputs = 0, kCbGuides = 256, kCbSlotBytes = 1024;
    static constexpr uint32_t kHealthOffset = 512, kHealthWords = 21, kHealthMeta = 768;
    GpuBuffer mCb;
    uint32_t  mCbSlots = 0;

    // Render-size targets (GENERAL for good).
    GpuImage mInMotion, mInDepth, mInStub, mInExposure;   // the inputs pass (mInStub: 1x1, bindings 8 / 9)
    GpuImage mGuideDiff, mGuideSpec, mGuideNormal, mGuideHit, mColor, mTransp, mResp;
    GpuImage mRtTracePh;                                             // 1x1 placeholder
    GpuImage mGiMotPh;                                               // 1x1 placeholder (zero: nothing CHANGED)
    GpuBuffer mRtTilesPh;
    // What the targets above were made for (SR holds only the inputs pass's + mColor + mResp).
    uint32_t mResW = 0, mResH = 0;
    LLVKNeuralRecon::FeatureKind mResKind = LLVKNeuralRecon::F_RR;

    Want     mBuilt;                  // what the targets / sets / feature were made for
    bool     mBuiltValid = false;
    LLVKNeuralRecon::Feature* mFeature = nullptr;
    bool     mOutGeneral = false;
    LLRRHistory::Policy mPolicy{};
    bool mPolicyKnown = false, mInputsChanged = false;
    std::chrono::steady_clock::time_point mHealthTime{}, mResetLogTime{};
    uint32_t mHealthSerial = 0;
    LLRRHistory::ResetBatch mResetBatch{};
    bool     mFirstEval = true;       // reset on the first evaluate of a feature
    std::chrono::steady_clock::time_point mLastTime{};
    bool     mHaveTime = false;
};

#endif
