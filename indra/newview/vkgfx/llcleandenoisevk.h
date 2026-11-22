/**
 * @file llcleandenoisevk.h
 * @brief Diffuse / specular lighting denoiser (#85 phase 2): SVGF / ReLAX-style temporal
 *        accumulation + variance-guided a-trous for noisy, DEMODULATED lighting signals.
 *
 * A general denoiser for any pass that estimates lighting stochastically (the first one: the
 * ray-traced projectors' stochastic sampler, RenderVKProjStochastic; next: ReSTIR DI #869, RT
 * reflections, GI). The contract is clean_pdn_common.glsl; in short:
 *
 *   PRODUCER (any compute pass, its own layout) writes, per denoiser texel, at the extent
 *   ensure() allocated (the render extent x 1 or 1/2, sampling render pixel
 *   (m + 0.5 + J) x render / denoiser -- the RT mask's jittered convention):
 *     IN   (inView, rgba16f x 2 layers): demodulated diffuse (rgb, a = confidence) and
 *          specular (rgb, a = hit distance m) -- divided by pdnDiffFactor / pdnSpecFactor;
 *     GEOM (geomView, rgba32ui x 2 layers): layer parity() -- view depth, normal + specular
 *          roughness, depth gradient, the signal reference W (pdnPackMeta);
 *     MOT  (motView, rgba32ui): the camera-only previous positions (surface, virtual image),
 *          the expected previous view depth.
 *   DENOISER (recordTemporal, then recordSpatial): history validation on GEOM + motion vectors,
 *   neighbourhood clamp, W anti-lag, separate diffuse / specular histories (specular length and
 *   kernel follow roughness and hit distance), N a-trous iterations.
 *   CONSUMER reads the FINAL image (finalIsIn(): IN after an odd iteration count, else OUT;
 *   outView / inView), rgb demodulated, diffuse alpha = view depth for a depth-aware upsample,
 *   and multiplies by the same factors with its own full-resolution material.
 *
 * SUN / MOON CHANNEL (#195 / #85 merge, RenderVKRTShadowMerge; ensure(.., sun = true)): a third
 * signal for the merged RT shadow pass -- the sun / moon VISIBILITY (rgb = visibility x the
 * translucent tint, as the RT shadow mask's sun) in IN / OUT layer 2 and its own history in
 * HIST layers 2 + parity (clean_pdn_common.glsl SUN CHANNEL): its own history length, penumbra
 * radius, moving-caster countdown, fast mean and variance, exact reprojection, a penumbra-gated
 * a-trous (contact shadows stay crisp), never cut by the projector W anti-lag. Allocated only
 * while the merge wants it (+16 + 32 B per texel); without it every image has exactly the
 * projector-only layout and the passes take exactly the projector-only paths.
 *
 * SPLIT CHANNEL (#869, RenderVKProjStochasticSplitK > 0; ensure(.., split = true)): the K dominant
 * lights' OWN visibility per texel, keyed by light (clean_pdn_common.glsl SPLIT CHANNEL) -- the
 * temporal pass follows each light's history by its id, the a-trous filters each light over the
 * taps that carry it; layers 0 / 1 then hold the REST's ratio. Present <=> GEOM has 5 layers.
 *
 * EXACT SPECULAR (local radiance only): one split slot identifies the strongest glossy
 * specular light; the split IN/OUT layer instead carries its RAW demodulated RGB.
 * It passes through both denoiser stages without history/spatial filtering. Main specular
 * is the residual excluding that light; its temporal/spatial taps require matching IDs.
 * Diffuse and its proposal/PDF are unchanged. No new images beyond the budgeted split set.
 *
 * VRAM (VKMEM_DENOISE, the RT group of the breakdown): 144 B per denoiser texel (192 with the
 * sun channel; + 80 B with the split channel, + 96 B without the sun's -- the split ages 16-bit in
 * SPLITAGE) -- ~127 MB at 2560x1440 half extent, ~506 MB full; 4K: ~285 / ~1139 MB. The GI instance
 * (THE NARROW AGE: AGE r32ui) 120 B. THE LEAN HISTORY (HistMode) takes 32 B per texel off
 * per channel DLSS RR takes raw (the GI's colour history; the merged sun's). Soft
 * allocations (#697): a refusal leaves the 1x1 placeholders and the caller falls back to the
 * exact path. Every image is bounded by the extent; nothing grows.
 * GPU: the caller's sub-marks (c_dn_tmp / c_dn_sp).
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * Slipstream Viewer Source Code
 * Copyright (C) 2026, Slipstream Viewer contributors
 * $/LicenseInfo$
 */
#ifndef LL_LLCLEANDENOISEVK_H
#define LL_LLCLEANDENOISEVK_H

#include "vk_device.h"
#include "vk_descpool.h"

#include <cstdint>
#include <string>

class LLCleanRendererVK;

class LLCleanDenoiserVK
{
public:
    // The per-frame knobs (clean_dn_common.glsl DnPush). History/young/mover settings
    // retain 60 Hz reference-duration units; authoritative GPU ages are milliseconds.
    // Ray counts, iteration counts, and the sampling frame index remain actual counts.
    struct Params
    {
        uint32_t atrous = 3;          // a-trous iterations (1..5)
        float maxHistDiffuse = 32.f; // reference duration (32 * 1000/60 ms)
        float maxHistSpecular = 24.f; // reference duration, rough lobes
        float minHistSpecular = 4.f;  // reference duration, a mirror
        float clampGamma = 1.5f;      // neighbourhood clamp width (sigmas)
        float lumPhi = 4.f;           // a-trous luminance edge-stop (sigmas)
        float depthPhi = 1.f;         // a-trous depth edge-stop
        float normalPow = 64.f;       // a-trous diffuse normal power
        float wCut = 0.25f;           // relative change of W that cuts the history (0 = off)
        uint32_t debug = 0;           // clean_pdn_common.glsl PDN_DBG_* (0..15; 8 / 9 the #869 split views)
        // #870 raw routing (clean_dn_common.glsl DN_F_RAW / DN_F_RAW_SUN): this frame's raw
        // samples pass through (no history, no a-trous filter) without any debug colouring.
        bool  raw = false;            // diffuse / specular channels
        bool  rawSun = false;         // the sun / moon channel
        uint32_t frame = 0;
        // NO-INFORMATION SAMPLES (the projector wall-bleed fix, RenderVKProjStochBleedFix;
        // clean_dn_common.glsl DN_F_INFO): a frame whose picks carried no light is a weight-0
        // sample -- the history holds (or the informative 3x3 seeds it); taps stay in every filter.
        bool  info = false;
        // (RenderVKReSTIRRadiance) a RESERVOIR-REUSING RADIANCE signal (the pass-15 instance in radiance mode;
        // clean_dn_common.glsl DN_F_RADIANCE): the temporal clamp's sigma is floored at the history's own
        // per-sample spread -- the raw 3x3 shares one reused pick and is no measure of the noise.
        bool  radiance = false;
        // DI-only transient A/B: discard mixed radiance history at changed-source
        // footprints. Never set by the GI or ratio denoiser.
        bool  radianceMoveReset = false;
        bool  localReceiverGuides = false; // DI-only previous skinned depth/shading normal
        bool  skinnedVarianceRecovery = false; // DI-only moments, never old colour
        // (#869 / #85) ReSTIR-CONVERGED DIFFUSE: a-trous iterations for texels whose reused
        // reservoir and history are both long (clean_dn_common.glsl DN_CONV_SHIFT); 0 = all of them.
        uint32_t convIters = 0;
        // (#869) THE MOVING-LIGHT RULE (RenderVKReSTIRMoveCut / RenderVKReSTIRMoveHistory;
        // clean_dn_common.glsl DN_MOVE_SHIFT): the history (frames) of a texel lit by a light that
        // moved this frame (the producer's MOT w share); 0 = off.
        uint32_t moveHist = 0;
        // ... applied to the DIFFUSE channel only (clean_dn_common.glsl DN_F_MOVE_DIFF_ONLY): the GI
        // denoiser, whose specular channel is the sky visibility (no light in it to change).
        bool moveDiffuseOnly = false;
        // (GI only, the sealed-room fade; clean_dn_common.glsl dnGiChangedCut) the producer's MOT w bit 8 marks
        // texels whose radiance-cache cell is CHANGED: their diffuse history is capped at DN_GI_CHANGED_HIST.
        bool giChangedCut = false;
        // (#869) MOVING SURFACES (RenderVKReSTIRSurfaceMoveVeto; clean_dn_common.glsl
        // DN_SURF_MOVE_SHIFT): a texel whose surface moved on its own by more than this many
        // denoiser texels (motion vectors vs camera-only) never takes the converged shortcut;
        // 0 = off. Quarter-texel steps, up to 3.75.
        float surfMoveTexels = 0.f;
        // (#87) DISOCCLUSION HISTORY FIX (RenderVKGIDisoccFix; clean_dn_common.glsl DN_HFIX_SHIFT):
        // texels whose diffuse / specular history is shorter than this many frames get the spatial
        // boost (variance floor, history-length-weighted a-trous taps, wider luminance stop); 0 = off.
        // Up to 7.
        uint32_t histFix = 0;
        // (#87) REPROJECTION SEARCH (RenderVKGIReprojSearch; DN_F_REPROJ_SEARCH): a failed bilinear
        // reprojection takes the best-matching texel of the 3x3 around it.
        bool reprojSearch = false;
        // (#1038) YOUNG-TEXEL BOOST (RenderVKDenoiseYoungBoost; clean_dn_common.glsl DN_F_YOUNG_BOOST;
        // acts only with histFix > 0): a texel without history starts from the same-surface 3x3 mean,
        // and young a-trous centres relax their normal stop and widen their step.
        bool youngBoost = false;
        // (#1038) MOVING-SURFACE DEPTH TOLERANCE (RenderVKDenoiseMovingDepthTol; DN_F_MOVE_ZTOL): the
        // history taps of a surface that moved on its own tolerate a depth change of its own motion.
        bool moveZTol = false;
        // SUN / MOON CHANNEL (the merged RT shadow pass; used only when sun is set AND the
        // images carry the channel -- hasSun()).
        bool exactSpecular = false; // raw dominant specular in split layer; residual ownership in GEOM
        bool specularLobeGuard = false; // local radiance only; preserve sharp-lobe support
        uint32_t casterHistoryPolicy = 2; // local/merged sun: off / broad cap / own visibility delta / + mover record
        // (RenderVKShadowMoverMask 2, local radiance only) THE EXACT-MOVER RULE: the producer's mover countdown (MOT w
        // bits 14-17) caps the history at sunDynMax; the a-trous takes only exact taps (MOT w bit 18) around an exact
        // centre (clean_dn_temporal.comp / clean_dn_atrous.comp DN_T_MOVER_EXACT)
        bool moverExact = false;
        // (radiance, every RenderVKShadowMoverMask mode; clean_dn_temporal.comp THE CONVERGENCE RULE) THE MOVER COUNTDOWN:
        // frames a texel stays capped after its rays last met a moving caster (0 = off), and that cap in frames
        uint32_t moverCdLen = 0;
        uint32_t moverCdCap = 0;
        // (radiance, RenderVKShadowFixMoverVariance A/B; clean_dn_temporal.comp THE COUNTDOWN'S NOISE) 0 the countdown
        // withholds its deviation from the variance (as before), 1 a provably steady texel's deviation floors the
        // a-trous's stop (OUT only), 2 ... feeds the history's variance (temporal ctl.y bits 22-23)
        uint32_t moverVarFix = 0;
        // (radiance, RenderVKProjStochasticAdaptiveHistory; clean_dn_temporal.comp THE ADAPTIVE MOVER HISTORY) the adaptive
        // extra passes A the producer ran this frame (0 = the rule off): a countdown texel that took them (MOT w bit 30)
        // keeps moverCdCap / (1 + A) frames (temporal ctl.y bits 24-26)
        uint32_t adaptPasses = 0;
        bool shadowPlaneReconstruction = false; // only local shadow producer writes this plane format
        bool sampleGridHistory = false; // local shadows: invert previous sample lattice
        float rasterJitter[2] = {0.f, 0.f}; // current render-pixel jitter, zero if off
        // (GI only; RenderVKGIStableSamples, clean_gi_common.glsl GI_F_STABLE_SAMPLES) the producer's texels sampled the
        // FIXED lattice floor((m + 0.5) r), no jitter phase (DN_F_GI_STABLE, temporal ctl.y)
        bool giStableSamples = false;
        bool  sun = false;            // this frame's producer wrote the sun channel
        bool  sunCut = false;         // restart the sun history (sun <-> moon switch, a jump)
        bool  sunHard = false;        // hard sun (no disc): moving casters keep 1 frame
        bool  sunExact = true;        // exact history reprojection (RenderVKRTMaskExactReproj)
        bool  sunNoResolve = false;   // existing RR history policy; temporal-only ctl.y bit 3
        float sunMaxHist = 32.f;      // frames (RenderVKRTShadowHistory)
        float sunDynHist = 2.f;       // moving-caster hard-shadow history (RenderVKRTShadowDynamicHistory)
        float sunDynMax = 4.f;        // moving-caster longest history (RenderVKRTShadowDynamicMaxHistory)
        float sunMove = 0.f;          // the light's angular step since last frame, in disc radii
        float sunSpatialCap = 12.f;   // a-trous penumbra radius cap (denoiser texels; RenderVKRTShadowSpatial)
        float sunClamp = 1.5f;        // moving-caster raw box width (sigmas; RenderVKRTShadowClamp)
        uint32_t sunPersist = 4;      // moving-caster countdown (frames; RenderVKRTShadowMoverPersist)
        float sunRays = 0.f;          // disc rays per texel this frame (0 = RenderVKRTSunSamples;
                                      // RenderVKShadowSunOneRay: 1) -- the variance bounds' count
    };

    LLCleanDenoiserVK() = default;
    ~LLCleanDenoiserVK() = default;   // destroy() is explicit (needs the device)
    LLCleanDenoiserVK(const LLCleanDenoiserVK&) = delete;
    LLCleanDenoiserVK& operator=(const LLCleanDenoiserVK&) = delete;

    // Layout, set, placeholders; the two pipelines queued (inside a PipelineBatchScope they land
    // with the batch). dslDraw0: the draw set 0 layout (frame info). nearest: a nearest / clamp
    // sampler for the motion vectors. false: nothing usable (the caller never enables it).
    // narrowAge (THE NARROW AGE, the GI instance): AGE is r32ui -- only the diffuse / specular age word -- and the
    // passes are the DN_AGE_NARROW pair (no sun channel, split channel or mover record: ensure() never gives it one).
    // views (#1608 LAYERED STEREO): the views every image holds (clean_dn_common.glsl DN_L: layer l of view v at
    // l x views + v; MOT a layer per view) and the passes run over in one dispatch, z = views (their pipelines carry
    // kViewCount = views). 1 (the desktop): the images, pipelines and dispatches of before.
    bool init(VkDeviceContext* ctx, VkDescriptorSetLayout dslDraw0, VkSampler nearest, const char* name,
              bool narrowAge = false, uint32_t views = 1);
    void destroy();
    bool pipelinesReady() const { return mTempPipe != VK_NULL_HANDLE && mAtrousPipe != VK_NULL_HANDLE; }

    // THE LEAN HISTORY (per-pixel memory, clean_dn_common.glsl THE LEAN HISTORY): HIST holds only the accumulated
    // diffuse / specular colours, SUNHIST only the sun / moon channel's, AGE the ages every frame reads. While DLSS
    // Ray Reconstruction takes a signal RAW nothing reads its history (a raw frame stores a length-1 history no one
    // reprojects), so the caller may leave it out -- bits: HIST_NO_SUN (SUNHIST the 1 x 1 placeholder: 32 B / texel
    // less), HIST_NO_COLOUR (HIST the placeholder: 32 B / texel less). A change of these alone re-creates only that
    // image and restarts only that history: the other images (the consumer's final image, GEOM, MOT) are kept.
    enum HistMode : uint32_t { HIST_FULL = 0u, HIST_NO_SUN = 1u, HIST_NO_COLOUR = 2u, HIST_NONE = 3u };
    // The images at w x h (SOFT; waits for the device on any change). true: allocated at that
    // size; false: refused (the placeholders stay, history invalid). The caller rewrites its
    // own descriptors to the new views afterwards.
    // sun: with the sun / moon channel (IN / OUT 3 layers, SUNHIST); a change re-allocates.
    // split (#869): with the SPLIT channel (clean_pdn_common.glsl SPLIT CHANNEL: the dominant lights'
    // own visibility; GEOM 5 layers, IN / OUT 4, SPLITAGE at the extent); a change re-allocates.
    // hist (HistMode bits): the histories left out. A change of hist ALONE (same extent / channels) re-creates only
    // HIST / SUNHIST (viewsKept() true afterwards: the callers' set 3 views are unchanged); a refused growth keeps
    // that history out (false; histMode() says what is allocated).
    bool ensure(uint32_t w, uint32_t h, bool sun = false, bool split = false, uint32_t hist = HIST_FULL);
    void release();   // back to the placeholders (waits for the device)
    uint64_t bytesAt(uint32_t w, uint32_t h, bool sun = false, bool split = false, uint32_t hist = HIST_FULL) const;
    uint64_t bytes() const { return allocated() ? bytesAt(mW, mH, mSun, mSplit, mHistMode) : 0; }
    bool allocated() const { return mW > 1 || mH > 1; }
    bool hasSun() const { return allocated() && mSun; }
    bool hasSplit() const { return allocated() && mSplit; }
    uint32_t histMode() const { return allocated() ? mHistMode : (uint32_t)HIST_FULL; }
    bool viewsKept() const { return mViewsKept; }   // the last ensure() changed only HIST / SUNHIST
    // this frame's history is valid (beginFrame: the denoiser ran last frame at this size)
    bool historyValid() const { return mHistOk; }
    // Bumped by every ensure() that changed an image and by release(): a caller that remembers what a frame left in the
    // final image (#1660: the merged sun a parked RT shadow mask's frame reuses) checks it is still that set's.
    uint32_t generation() const { return mGen; }
    uint32_t width() const { return mW; }
    uint32_t height() const { return mH; }

    // Stable while allocated (placeholders otherwise): what the producer / consumer bind (2D_ARRAY views over every
    // layer of every view; MOT's too -- motView2D is its view 0 alone, for the single-view DLSS RR guides).
    VkImageView inView() const   { return mIn.view; }
    VkImageView geomView() const { return mGeom.view; }
    VkImageView motView() const  { return mMot.arrayView; }
    VkImageView motView2D() const { return mMot.view; }
    // MOT's identity (2026-11-21 DEVICE_LOST, the RR guides' stale MOT): a serial, process-wide and never reused, given
    // each time MOT is (re-)created -- a full ensure() or release(), never a history-only change (MOT stays). A consumer
    // that keeps a set naming motView / motView2D across frames keys its rewrite on this, never on the view handle: a
    // re-created view can get the freed one's handle back and the set would keep reading the freed image.
    uint64_t motSerial() const { return mMotSerial; }
    VkImageView outView() const  { return mOut.view; }
    uint32_t views() const { return mViews; }
    // (#1038) the 1 x 1 motion placeholder (GENERAL layout, sampled; a 2D_ARRAY view) and the nearest sampler: what
    // another set binds while no motion target exists.
    VkImageView motionPlaceholderView() const { return mMotionPh.view; }
    VkSampler   nearestSampler() const { return mNearest; }

    // The pre-pass motion vectors (render extent; layout as the renderer keeps them; a 2D_ARRAY view, a layer per
    // view), or VK_NULL_HANDLE for the placeholder. Callers make sure no frame in flight uses the set.
    void setMotion(VkImageView view, VkSampler sampler, VkImageLayout layout);

    // Once per frame that will run, before the producer: flips the GEOM / HIST parity; the
    // history is valid when the last frame ran at this size.
    void beginFrame();
    uint32_t parity() const { return mParity; }
    // The final image's identity for an iteration count (the consumer's push).
    static bool finalIsIn(uint32_t atrous) { return (clampIters(atrous) & 1u) != 0u; }
    static uint32_t clampIters(uint32_t n) { return n < 1u ? 1u : (n > 5u ? 5u : n); }

    // After the producer's writes (the barrier is recorded here): the temporal pass, then the
    // a-trous iterations. dsetDraw0: the draw set 0 (frame info). The final image is complete
    // for compute reads after the caller's next compute -> compute barrier.
    void recordTemporal(VkCommandBuffer cmd, VkDescriptorSet dsetDraw0, uint32_t rw, uint32_t rh, const Params& p, LLCleanRendererVK* timerRenderer = nullptr, const char* timerName = nullptr);
    void recordSpatial(VkCommandBuffer cmd, uint32_t rw, uint32_t rh, const Params& p, LLCleanRendererVK* timerRenderer = nullptr, const char* timerName = nullptr);

    // NOISE PROBE (RenderVKNoiseStats; clean_dn_noise.comp): measurement only. Per denoiser texel the luminance of a
    // signal against the same texel last frame (a still camera), summed per group (all, near / mid / far, edge /
    // interior) -- TRAD = sum |dL| / sum L and the flip rate. Slot 0 = the producer's RAW (record it after the
    // producer, before recordTemporal), slot 1 = the FINAL image (after recordSpatial). noiseBegin once per frame
    // before the probes (zeroes the counters; harvests the read-back slot it is about to reuse), noiseEnd after the
    // last probe. layerMask: the IN / OUT layers summed (bit l = layer l). still: the camera did not move since last
    // frame (else last frame's values are only replaced, never compared). The buffers are created on first use (soft:
    // a refusal skips the probe, retried the next frame) and freed by noiseRelease (also by destroy()).
    void noiseBegin(VkCommandBuffer cmd);
    void recordNoise(VkCommandBuffer cmd, uint32_t slot, bool finalImage, uint32_t atrous, uint32_t layerMask, bool still, LLCleanRendererVK* timerRenderer = nullptr, const char* timerName = nullptr);
    void noiseEnd(VkCommandBuffer cmd);
    // The groups' figures accumulated since the last call ("" when no frame was harvested), then reset.
    std::string noiseText();
    void noiseRelease();

private:
    struct Push { float dims[4]; float prm0[4]; float prm1[4]; uint32_t ctl[4]; float sun0[4]; float sun1[4]; };
    static_assert(sizeof(Push) == 96, "clean_dn_common.glsl DnPush");
    Push makePush(uint32_t rw, uint32_t rh, const Params& p) const;
    bool createSet(uint32_t w, uint32_t h, bool soft, bool sun, bool split, uint32_t hist);
    bool createImage(uint32_t i, uint32_t w, uint32_t h, bool sun, bool split, uint32_t hist);   // (one of the set)
    GpuImage* image(uint32_t i);   // kDnImg's order: IN, MOT, GEOM, HIST, OUT, AGE, SUNHIST, SPLITAGE
    void clearImage(GpuImage* const* im, uint32_t n);
    void destroyImages();
    void writeSet();
    void clearImages();

    VkDeviceContext*      mCtx = nullptr;
    const char*           mName = "denoiser";
    VkSampler             mNearest = VK_NULL_HANDLE;
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkDescPool            mPool;
    VkDescriptorSet       mDset = VK_NULL_HANDLE;
    VkPipelineLayout      mLayout = VK_NULL_HANDLE;   // {draw set 0, mDsl}, 96 B push
    VkPipeline            mTempPipe = VK_NULL_HANDLE;
    VkPipeline            mAtrousPipe = VK_NULL_HANDLE;
    GpuImage mIn{}, mMot{}, mGeom{}, mHist{}, mOut{};   // clean_pdn_common.glsl IMAGES
    GpuImage mAge{};                                    // (binding 6) the ages + mover record (narrow: the ages)
    GpuImage mSunHist{};                                // (binding 7) the sun / moon channel's history
    GpuImage mSplitAge{};                               // (binding 8) the split slots' ages (16-bit)
    GpuImage mMotionPh{};                               // binding 5 without motion vectors
    VkImageView   mMotionView = VK_NULL_HANDLE;
    VkSampler     mMotionSampler = VK_NULL_HANDLE;
    VkImageLayout mMotionLayout = VK_IMAGE_LAYOUT_GENERAL;
    uint32_t mW = 1, mH = 1;
    uint32_t mGen = 0;          // generation()
    const char* mTag = "dn";    // (the VKMem inventory's image names: "pdn.In", "gidn.Geom", ...)
    uint64_t mMotSerial = 0;    // motSerial()
    bool     mSun = false;      // the images carry the sun / moon channel
    bool     mSplit = false;    // (#869) ... and the split channel
    bool     mNarrowAge = false;   // THE NARROW AGE (init): AGE r32ui, the DN_AGE_NARROW passes, never sun / split
    uint32_t mViews = 1;          // (#1608, init) the views every image holds / the passes run over (z)
    uint32_t mHistMode = HIST_FULL;   // (THE LEAN HISTORY) the histories left out (HistMode bits)
    bool     mViewsKept = false;      // the last ensure() re-created only HIST / SUNHIST
    uint32_t mParity = 0;
    bool     mRan = false;      // recordTemporal ran since the last beginFrame
    bool     mHistOk = false;   // this frame's history is valid
    bool     mSunRan = false;   // the sun channel was accumulated since the last beginFrame
    bool     mSunHistOk = false;   // ... so this frame's sun history is valid
    float    mPreviousSampleJitter[2] = {0.f, 0.f};
    bool     mSampleGridHistory = false;
    uint32_t mCasterHistoryPolicy = 2;
    bool mCasterCauseDebug = false;
    bool     mShadowPlaneReconstruction = false;
    bool     mSampleGridKnown = false;
    bool     mSpecularLobeGuard = false;
    bool     mExactSpecular = false;
    bool     mSunNoResolve = false; // changed policy invalidates only the sun channel
    bool     mSunNoResolveKnown = false;

    // NOISE PROBE (recordNoise): its own set (one storage buffer) and pipeline, the counters' read-back ring.
    static constexpr uint32_t kNoiseAcc = 64u;   // clean_dn_noise.comp acc[]
    VkDescriptorSetLayout mDslDraw0 = VK_NULL_HANDLE;   // (init's: set 0 of the probe's layout, never bound)
    VkDescriptorSetLayout mNoiseDsl = VK_NULL_HANDLE;
    VkDescPool            mNoisePool;
    VkDescriptorSet       mNoiseSet = VK_NULL_HANDLE;
    VkPipelineLayout      mNoiseLayout = VK_NULL_HANDLE;
    VkPipeline            mNoisePipe = VK_NULL_HANDLE;
    GpuBuffer             mNoiseBuf{};      // acc[64] + prev[2 x w x h]
    GpuBuffer             mNoiseRb{};       // 3 x acc[64], host visible
    uint32_t mNoiseW = 0, mNoiseH = 0;      // the extent mNoiseBuf's prev[] holds
    uint32_t mNoiseRbSlot = 0;
    bool     mNoiseRbUsed[3] = {false, false, false};
    bool     mNoiseBegun = false;           // noiseBegin ran this frame (the counters are zeroed)
    uint64_t mNoiseFrame = 0;               // noiseBegin count
    uint64_t mNoiseLast[2] = {0, 0};        // the noiseBegin count of each slot's last probe (0 never)
    double   mNoiseSum[kNoiseAcc] = {};     // harvested counters
    uint32_t mNoiseFrames = 0;              // frames harvested into mNoiseSum
    uint32_t mNoiseMoving = 0;              // probes whose camera moved (not compared)
    uint32_t mNoiseStillRun = 0;            // consecutive frames with a still camera
    static constexpr uint32_t kNoiseSettle = 60u;   // still frames before a texel is compared (the history settles)
    float    mNoiseMeanL[2] = {0.f, 0.f};   // each slot's mean luminance last harvested (the flip floor)
};

#endif // LL_LLCLEANDENOISEVK_H
