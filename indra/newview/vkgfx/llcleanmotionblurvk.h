/**
 * @file llcleanmotionblurvk.h
 * @brief Motion blur (#118, RenderVKMotionBlur) for the Vulkan clean renderer: a
 *        tile-based reconstruction filter (McGuire et al. 2012 with the CoD: Advanced
 *        Warfare / Guertin 2014 refinements) on the display-size HDR scene.
 *
 * WHERE IT RUNS: AFTER the temporal resolve (DLSS SR / RR / DLAA, FSR 3.1 / 4 upscaling or
 * Native AA, or the plain upscale blit / nothing) and the SSR copy / no-resolve sharpening,
 * at the DISPLAY (output) resolution, on mHdr -- before the eyedropper, DoF, bloom,
 * exposure and the tonemap. The standard place for it:
 *  - the upscalers always see the unblurred scene: blur never enters their history (no
 *    settle after the camera stops) and DLSS needs no stand-down. Every resolve path runs
 *    it, and frames without one too (the output is then the render-size scene itself);
 *  - SSR's previous-frame copy is taken before it (unblurred);
 *  - frame generation (DLSS-G, FSR FG) takes the finished, tonemapped world after it: it
 *    interpolates frames blurred once, by this pass; nothing in the generator blurs. The
 *    #1517 late weather particles are drawn after the tonemap, unblurred, as before.
 * INPUTS: the motion target, depth and water mask stay render-sized (and jittered under a
 * resolve). The tile pass maps each output pixel to ONE render texel -- the unjittered
 * position, the texel the resolved colour lines up with -- and reads depth, motion and the
 * water mask from it. Motion is in UV units, so the radius (UV x output size), the tile
 * grid, the NeighborMax reach and the max radius are all in output pixels. No resolve
 * follows: the tap jitter is a fixed per-pixel pattern (a stable dither, nothing crawls).
 *
 * PASSES (GPU sub-marks p_mblur_tile / p_mblur_nmax / p_mblur):
 *  1. clean_mblur_tile.comp: per output pixel, the blur radius vector from the motion
 *     target with the camera rotation / translation / object components scaled separately
 *     (RenderVKMotionBlurCameraRotation / CameraTranslation / Object; the shader header
 *     explains why the total vector is scaled, never rebuilt) and the linear depth, into
 *     an RGBA16F velocity image; reduced per 16x16 tile into TileMax.
 *  2. clean_mblur_neighbor.comp: NeighborMax over (2 reach + 1)^2 tiles, reach =
 *     ceil(max radius / 16), with a reach test past the 3x3.
 *  3. clean_mblur_recon.comp: the gather, into an RGBA16F target, only on tiles whose
 *     NeighborMax is over ~0.5 px (whole workgroups return otherwise).
 *  4. clean_mblur_writeback.frag: a fullscreen draw into the scene that copies exactly
 *     those tiles and discards the rest.
 * Still frame: pass 1 reads the motion / depth / water mask and writes the velocity
 * image (~8 B written per output pixel), passes 2-4 are near free (tile-sized work, a
 * discarded fullscreen draw). Moving: + the gather (taps x (RGBA16F + velocity fetch)).
 * VRAM: the velocity image + the reconstruction, 16 B per output pixel (~130 MB at 4K).
 *
 * Off (RenderVKMotionBlur 0, the default) this object does not exist; supersampled
 * snapshot recordings (#417) render a frozen world and skip it, single-frame snapshots
 * carry the live motion. Off in VR / stereo. Every allocation is soft: a failure turns
 * the effect off for that frame (retried after ~300 frames).
 */
#ifndef LL_LLCLEANMOTIONBLURVK_H
#define LL_LLCLEANMOTIONBLURVK_H

#include "vk_device.h"
#include "vk_descpool.h"
#include "vk_pipeline.h"   // PipelineJobSlot (first-use pipelines off the render thread)

#include <cstdint>

class LLCleanRendererVK;

class LLCleanMotionBlurVK
{
public:
    // What the effect needs from the renderer this recording.
    struct Frame
    {
        VkDeviceContext* ctx = nullptr;
        uint32_t ow = 0, oh = 0;                           // output extent (the scene's: display size)
        uint32_t rw = 0, rh = 0;                           // render extent (depth / motion / water mask)
        VkFormat sceneFormat = VK_FORMAT_UNDEFINED;
        // The display-size HDR scene after the resolve: SHADER_READ_ONLY_OPTIMAL; left so.
        VkImage     sceneImage = VK_NULL_HANDLE;
        VkImageView sceneView = VK_NULL_HANDLE;
        // Render-size world depth in depthLayout (DEPTH_ATTACHMENT_OPTIMAL after the SSR
        // copy, or DEPTH_READ_ONLY_OPTIMAL): sampled by pass 1, given back in that layout.
        // Motion target: SHADER_READ_ONLY_OPTIMAL. Pass 2's reactive / water mask (G =
        // water): SHADER_READ_ONLY_OPTIMAL when a resolve sampled it (reactSampled), else
        // still COLOR_ATTACHMENT_OPTIMAL from pass 2 -- then moved to SHADER_READ_ONLY and
        // left there (pass 2 re-enters it from UNDEFINED; nothing after this reads it).
        VkImage       depthImage = VK_NULL_HANDLE;
        VkImageView   depthView = VK_NULL_HANDLE;
        VkImageLayout depthLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
        VkImageView   motionView = VK_NULL_HANDLE;
        VkImage       reactImage = VK_NULL_HANDLE;
        VkImageView   reactView = VK_NULL_HANDLE;
        bool          reactSampled = false;
        VkImageView   exposureView = VK_NULL_HANDLE;      // 1x1 R32F dynamic exposure, GENERAL (debug views)
        // The renderer's target / depth generations (registry rule 1): the set rewrite
        // keys on them, never on the views alone (a freed view's handle can come back).
        uint64_t targetGen = 0, depthGen = 0;
        const float* reprojCam = nullptr;   // this frame's unjittered clip -> last frame's clip (16)
        const float* reprojRot = nullptr;   // the same, last frame's eye on this frame's (16)
        float fovComp = 1.f;                // clip-xy scale removing the speed-FoV change (#663), 1 = none
        const float* waterPlane = nullptr;  // water plane, this frame's unjittered clip space (4)
        const float* lin = nullptr;         // view z from device depth: (x d + y) / (z d + w) (4)
        float jitter[2] = {0.f, 0.f};       // the depth / motion's projection jitter, render px (0 = none)
        float frameDt = 1.f / 60.f;         // seconds of motion the motion target holds
        float staticExposure = 1.f;
        bool  dynExposure = false;
        bool  motionValid = false;          // the pre-pass wrote the motion target this frame
        bool  offscreen = false;            // snapshot recording (twice the taps)
        uint32_t snapSample = 0;            // #932: a shot's accumulation sample 1..N (0 = none)
    };

    LLCleanMotionBlurVK() = default;
    ~LLCleanMotionBlurVK() = default;   // destroy() is explicit (needs the device)
    LLCleanMotionBlurVK(const LLCleanMotionBlurVK&) = delete;
    LLCleanMotionBlurVK& operator=(const LLCleanMotionBlurVK&) = delete;

    // Reads the settings, (re)creates what changed, updates the parameters and records
    // pass 1 (velocity + TileMax). Outside any rendering scope. false = nothing to do this
    // frame (shutter 0, an input missing, an allocation failed): skip the rest.
    bool recordTiles(VkCommandBuffer cmd, const Frame& f, LLCleanRendererVK* timerRenderer = nullptr);
    // After recordTiles returned true: pass 2 (NeighborMax).
    void recordNeighbor(VkCommandBuffer cmd, LLCleanRendererVK* timerRenderer = nullptr);
    // After recordNeighbor: passes 3 + 4 (reconstruction, write-back into the scene);
    // leaves the scene SHADER_READ_ONLY_OPTIMAL, as it came in.
    void recordReconstruct(VkCommandBuffer cmd, LLCleanRendererVK* timerRenderer = nullptr);
    // Frees everything (waits for the device). Safe to call repeatedly.
    void destroy(VkDevice dev);
    // Login warm-up (LLCleanRendererVK::warmFirstUse): starts the pipeline build for this
    // scene format, nothing recorded. true while it compiles.
    bool warm(VkDeviceContext* ctx, VkFormat sceneFormat);
    bool pipelinesPending() const { return mJob.pending(); }

private:
    bool ensurePipelines(const Frame& f);
    bool ensureTargets(VkCommandBuffer cmd, const Frame& f);   // false = allocation failed
    void destroyTargets();
    void writeSet();

    // Parameter block (clean_mblur_common.glsl MbParams, std140).
    struct Params
    {
        float reproj_cam[16];
        float reproj_rot[16];
        float wplane[4];
        float lin[4];
        float scales[4];
        float p0[4];
        float p1[4];
        float p2[4];
        float p3[4];
        float p4[4];   // #836: x wide uniform-motion check, y Karis HDR tap weights; zw render (input) size
        float p5[4];   // xy the inputs' projection jitter (render px), zw unused
    };

    VkDeviceContext*      mCtx = nullptr;
    VkSampler             mLinear = VK_NULL_HANDLE;    // scene taps (bilinear, clamp)
    VkSampler             mNearest = VK_NULL_HANDLE;   // texelFetch-only bindings
    VkDescriptorSetLayout mDsl = VK_NULL_HANDLE;
    VkDescPool            mPool;
    VkDescriptorSet       mSet = VK_NULL_HANDLE;
    VkPipelineLayout      mLayout = VK_NULL_HANDLE;
    VkPipeline            mTilePipe = VK_NULL_HANDLE;
    VkPipeline            mNeighborPipe = VK_NULL_HANDLE;
    VkPipeline            mReconPipe = VK_NULL_HANDLE;
    VkPipeline            mWritebackPipe = VK_NULL_HANDLE;
    VkFormat              mWritebackFormat = VK_FORMAT_UNDEFINED;
    bool                  mPipeFailed = false;         // build failed: don't retry every frame
    PipelineJobSlot       mJob;                        // the background build (ensurePipelines)
    VkPipeline            mStage[4] = {};              // its outputs: tile, neighbour, reconstruct, write-back

    GpuBuffer mParams;              // one Params block, vkCmdUpdateBuffer per recording
    GpuImage  mVel;                 // RGBA16F, output size: xy radius px, z linear depth
    GpuImage  mTileMax;             // RGBA16F, one texel per 16 px tile
    GpuImage  mNeighbor;            // RGBA16F, per tile
    GpuImage  mOut;                 // RGBA16F, output size: the reconstruction
    uint32_t  mW = 0, mH = 0, mTilesX = 0, mTilesY = 0;
    uint32_t  mRetryFrame = 0;      // allocation failed: no retry before this frame count
    uint32_t  mFrameNo = 0;         // allocations' retry clock (recordings)

    // What the set was last written for (generations, never handles alone).
    uint64_t    mSetTargetGen = 0, mSetDepthGen = 0, mSetOwnGen = 0;
    uint64_t    mOwnGen = 1;        // bumped whenever an own image / buffer is re-created
    VkImageView mSetViews[6] = {};  // + the renderer views (a cheap second check)
    // The renderer views the set names (writeSet).
    VkImageView mSceneView = VK_NULL_HANDLE, mBindDepth = VK_NULL_HANDLE, mBindMotion = VK_NULL_HANDLE;
    VkImageView mBindReact = VK_NULL_HANDLE, mBindExposure = VK_NULL_HANDLE;

    // This recording (recordTiles -> the later passes).
    bool     mArmed = false;
    VkImage  mSceneImage = VK_NULL_HANDLE;

    uint32_t mLogW = 0, mLogH = 0, mLogRw = 0, mLogRh = 0, mLogReach = 0;
};

#endif
