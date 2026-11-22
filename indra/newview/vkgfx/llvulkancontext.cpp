/**
 * @file llvulkancontext.cpp
 * @brief See llvulkancontext.h. Concrete Vulkan bring-up + clear/present frame.
 */
#include "linden_common.h"
#include "vk_enumerate.h"
#include "../llvkhitch.h"
#include "../llinstantreplay.h"      // instant replay (GunMetal-DX port)
#include "../slvideorecorder.h"      // Video Studio recording (#1668)
#include "../llviewercontrol.h"
#include "../slbench.h"              // #1304 render benchmark: pipeline batch totals

#include "llvulkancontext.h"
#include "llvktextureprepservice.h"

#include "vk_device.h"
#include "vk_requirements.h"                 // #526 device requirements + user message
#include "vk_swapchain.h"
#include "vk_retirement_retry.h"
#include "vk_display_frame.h"
#include "vk_display_encode.h"
#include "vk_display_mode_transaction.h"
#include "vk_display_calibration.h"
#include "vk_display_calibration_job.h"
#if LL_WINDOWS
#include "vk_display_calibration_windows.h"
#endif
#include "llrendervk/llrendervk.h"       // the facade: gVK
#include "llrendervkbackend.h"           // concrete LLRenderBackend
#include "vk_ui_shaders_spv.h"           // embedded UI SPIR-V
#include "vk_world_shaders_spv.h"        // current-renderer fullscreen display transfer
#include "llcleanrenderervk.h"           // GPU-driven static-opaque world pass
#include "llcleandlssrrvk.h"             // #1517: the DLSS inputs' usage (frame generation tags)
#include "../llcleananimstore.h"          // renderer-generation handoff for avatar animation
#include "llcleansnapshotvk.h"
#include "llsnapshotbufferlimits.h"
#include "llradiancesnapshot.h"           // #873 live snapshots
#include "llcleanclustercaps.h"          // #743 cluster LOD tier log line
#include "llvkneuralrecon.h"             // #870 / #962 optional DLSS shim (slip_nr: SR + RR)
#include "llvkframegen.h"                // DLSS Frame Generation (through the DLSS add-on)
#include "llvkframegenpresent.h"         // frame generation's present thread (#1517)
#include "llxrsession.h"                 // #1073 VR (OpenXR); inert unless VR was chosen at startup
#include "llvrspacewarp.h"               // #1594 VR space warp: the frame policy (VR only)
#include "llvrspacewarpvk.h"             // #1594 VR space warp: history + warp pass (VR only, made on first use)
#include "llfxruntimevk.h"               // FX compile watchdog (shutdown)
#include "llrendervk/vk_offscreen.h"     // OffscreenTarget: milestone PNG readback

#include "llrender.h"                    // LLRender::setBackend
#include "llerror.h"
#include "llformat.h"                    // #458 hostMemText
#include "llmemorygovernor.h"            // #458 UI upload ring cap scaled by RAM
#include "llmemory.h"                    // #471 LLMemory::getCurrentRSS (compile memory probe)
#include "vk_compile_governor.h"
#include "vk_pipeworker.h"               // #1411 P5 early compile worker
#include "vk_pipeline.h"                 // #462 vkSetPipelineCompileThreads / progress hook
#include "vk_pipecache.h"                // #471 learned compile costs (load / save)
#include "workqueue.h"                   // post-batch malloc_trim (General queue)
#include "llthread.h"                    // #1103 on_main_thread (where the post-batch trim runs)
#include "llapp.h"
#include "lleventtimer.h"
#include "llframetimer.h"
#include "lltrans.h"                     // #1547 the slow-recovery notice
#if LL_LINUX
#   include <spawn.h>                   // #1547 notify-send for the slow-recovery notice
#   include <sys/wait.h>
#   include <unistd.h>
extern char** environ;
#endif

#if LL_WINDOWS
#   include "llwin32headers.h"
#   include <psapi.h>                    // #471 GetProcessMemoryInfo (private bytes)
// #919 VK_EXT_full_screen_exclusive: its structs / entry point types live in the Win32
// platform header, which vulkan.h only includes with VK_USE_PLATFORM_WIN32_KHR (not defined
// for the build). It only needs the Windows types (above) and vulkan_core.h.
#   include <vulkan/vulkan_win32.h>
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <condition_variable>   // #1547 the recovery reaper
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <memory>
#include <new>
#include <thread>
#include <vector>

namespace
{
vkdisplay::ReportedLuminance sDisplayCalibration;
SDL_DisplayID sDisplayCalibrationId = 0;
bool sDisplayCalibrationQueryPending = true;
bool sLastAutoWhite = false, sLastAutoPeak = false;
#if LL_LINUX
std::shared_ptr<vkdisplay::CalibrationJob> sDisplayCalibrationJob;
class DisplayCalibrationWake;
DisplayCalibrationWake* sDisplayCalibrationWake = nullptr;
class DisplayCalibrationWake final : public LLEventTimer
{
public:
    DisplayCalibrationWake() : LLEventTimer(0.25f) {}
    ~DisplayCalibrationWake() override { sDisplayCalibrationWake = nullptr; }
    bool tick() override
    {
        if (LLApp::isExiting()) return true;
        if (++mTicks > 80)
        {
            LL_WARNS("Vulkan") << "HDR calibration remains pending; reopen graphics preferences to retry" << LL_ENDL;
            return true;
        }
        return !LLVulkanContext::instance().pollDisplayCalibration();
    }
private:
    unsigned mTicks = 0; // bounded to 20 seconds; an explicit event can retry later
};
#endif
[[maybe_unused]] uint64_t sDisplayCalibrationGeneration = 0; // Linux async discovery only
const char* sDisplayWhiteSource = "not selected";
const char* sDisplayPeakSource = "not selected";
}

#if LL_WINDOWS
namespace
{
// Query the OS SDR reference white for the monitor under this window. The result
// is available only while Windows Advanced Color is active; all buffers are
// fixed and bounded because this runs only when HDR is enabled, never per frame.
float windowsSdrReferenceWhiteNits(SDL_Window* window) noexcept
{
    if (!window) return 0.f;
    HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                              SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);
    HMONITOR monitor = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
    if (!monitor) return 0.f;
    MONITORINFOEXW monitorInfo{};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (!GetMonitorInfoW(monitor, &monitorInfo)) return 0.f;

    UINT32 requiredPaths = 0, requiredModes = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &requiredPaths, &requiredModes) != ERROR_SUCCESS ||
        requiredPaths == 0 || requiredPaths > 32 || requiredModes > 64)
        return 0.f;
    DISPLAYCONFIG_PATH_INFO paths[32]{};
    DISPLAYCONFIG_MODE_INFO modes[64]{};
    UINT32 pathCount = 32, modeCount = 64;
    if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths, &modeCount, modes, nullptr) != ERROR_SUCCESS ||
        pathCount > 32 || modeCount > 64)
        return 0.f;
    for (UINT32 i = 0; i < pathCount; ++i)
    {
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = paths[i].sourceInfo.adapterId;
        source.header.id = paths[i].sourceInfo.id;
        if (DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS ||
            std::wcscmp(source.viewGdiDeviceName, monitorInfo.szDevice) != 0)
            continue;

        DISPLAYCONFIG_SDR_WHITE_LEVEL white{};
        white.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL;
        white.header.size = sizeof(white);
        white.header.adapterId = paths[i].targetInfo.adapterId;
        white.header.id = paths[i].targetInfo.id;
        if (DisplayConfigGetDeviceInfo(&white.header) != ERROR_SUCCESS || white.SDRWhiteLevel == 0)
            return 0.f;
        const float nits = float(white.SDRWhiteLevel) * 0.08f;
        return (nits >= 80.f && nits <= vkdisplay::kDisplayReferenceWhiteMaxNits) ? nits : 0.f;
    }
    return 0.f;
}
}
#endif

// The vkgfx layer uses VK_CHECK/std::abort on hard errors, which is acceptable
// for a de-risking bring-up: a failed swapchain create is not recoverable and we
// want the failure loud. init() below guards the recoverable/expected failures
// (no surface support, minimized) and returns false instead.

struct LLVulkanContext::Impl
{
    SDL_Window*     mWindow  = nullptr;
    VkSurfaceKHR    mSurface = VK_NULL_HANDLE;
    // The main swapchain's rebuild. #1517 frame generation: the present thread is drained and every gate
    // opened first (a held present never lets the device idle); Reflex lets go of the old chain and takes
    // the new one; the driver is asked about pacing again on the new chain; the present thread takes the
    // new chain (its images at the new size, admitted as one set), or lets go of its images when the new
    // chain is a normal one. A rebuild other than the one a refusal of those images asked for lets frame
    // generation try again (the window resized, HDR or the display changed). False: not rebuilt, nothing
    // touched and the request kept (mNeedsRecreate) -- the present thread did not drain (inside a present the
    // compositor holds, an acquire or the generator: a swapchain is never rebuilt under a call into it), or the
    // presentation engine still holds a present of the old chain (VkSwapchain::rebuildDeferred); asked again
    // after a while (rebuildBackoff).
    bool recreateMainSwapchain()
    {
        if (!drainFrameGen())
        {
            LL_WARNS("Vulkan") << "Frame generation: the swapchain is not rebuilt while the present thread is inside "
                                  "a present, an acquire or the generator; it is rebuilt once that returns" << LL_ENDL;
            mNeedsRecreate = true;
            return false;
        }
        // A slot release the present thread could not signal is settled BEFORE the old chain goes: nothing the
        // present queue accepted stands for that job's last commands (its copy into a swapchain image), so the
        // rebuild's device wait -- made without that queue -- would not cover them. Not settled (the queue's
        // wait refused): the chain stays, asked again after a while.
        if (!mFgPresent.settleOwedRelease(mDevice))
        {
            LL_WARNS("Vulkan") << "Frame generation: the swapchain is not rebuilt yet (a slot release is still owed "
                                  "and the present queue could not be waited for); asked again shortly" << LL_ENDL;
            mNeedsRecreate = true;
            rebuildBackoff();
            return false;
        }
        LLVKFrameGen::instance().setReflexSwapchain(VK_NULL_HANDLE, false, false, false);   // (the old chain goes)
        const bool rebuilt = mSwapchain.recreate(mDevice, mWidth, mHeight);
        LLVKFrameGen::instance().setReflexSwapchain(mSwapchain.handle(), mSwapchain.latencyMode(),
                                                    mSwapchain.frameGenImages() != 0, mSwapchain.ignoresVsync());
        if (!rebuilt && mSwapchain.rebuildDeferred())
        {
            // (the old chain is as it was: Reflex has it back, the presenter keeps it. Asked again about twice a
            // second, each try only a look at the proofs; said at first and once a minute)
            if (mSwapchain.rebuildDeferredNote())
                LL_WARNS("Vulkan") << "Swapchain rebuild deferred: the presentation engine still holds a present of the "
                                      "old chain (a window that cannot be seen); asked again until it lets go" << LL_ENDL;
            mNeedsRecreate = true;
            rebuildBackoff();
            return false;
        }
        if (mFgRefusalRebuild) mFgRefusalRebuild = false;   // (to the normal chain the refusal asked for)
        else LLVKFrameGen::instance().rearmImages();
        logFrameGenChain();
        LLVKFrameGen::instance().meteringReset();
        mFgChainBuilt = mFgChainWanted;
        if (vkDeviceIsLost()) return true;
        // #1544: the generator RenderVKFrameGenGenerator names, switched here -- the present thread drained above,
        // the device idle when the rebuild's wait succeeded -- before the presenter's images: the scratch image is
        // in the generator's format.
        LLVKFrameGen::instance().applyGenerator(mDevice, mSwapchain.deviceIdled());
        if (!mSwapchain.deviceIdled() && mFgPresent.holdsImages())
        {
            // The rebuild's device wait failed (no memory): work may still be pending on frame generation's
            // images, so none is released or remade -- the presenter refuses the new chain untouched (no frame
            // is handed to it) and the chain is rebuilt once more after a while, not every frame. Only while
            // the presenter holds images: a chain without frame generation is rebuilt once, as it always was.
            // (asked through mFgRebuildAgain: this rebuild WAS made, and the caller forgets mNeedsRecreate after
            // one -- with the request gone nothing would ask again, and no frame is shown on a chain the
            // presenter refused)
            if (mSwapchain.frameGenImages()) mFgPresent.prepare(mDevice, mSwapchain, mDisplayEncode, frameGenBudget());
            mFgRebuildAgain = true;
            rebuildBackoff();
            return true;
        }
        // (the rebuild waited for an idle device: nothing uses the old backbuffers or generator images -- or its
        // wait failed and the presenter holds none: prepare then refuses the chain untouched, a refusal like any)
        if (mSwapchain.frameGenImages())
        {
            if (!mFgPresent.prepare(mDevice, mSwapchain, mDisplayEncode, frameGenBudget()))
            {
                // No frame could be shown on this chain: a normal chain next, frame generation off until the
                // multiplier changes or the chain is rebuilt for another reason (never a rebuild every frame).
                mFgPresent.releaseChain();
                LLVKFrameGen::instance().imagesRefused(mFgPresent.refusal());
                mFgRefusalRebuild = true;
                mFgRebuildAgain = true;   // (not mNeedsRecreate: the caller forgets that after this rebuild)
            }
        }
        else
            mFgPresent.releaseChain();
        return true;
    }
    bool mFgRefusalRebuild = false;   // the next rebuild is the one a refusal of frame generation's images asked for
    // A rebuild that was made asks for one more (its device wait failed while the presenter holds images; the
    // presenter refused the new chain): renderFrame forgets mNeedsRecreate after a rebuild that returned true,
    // so the request waits here and becomes mNeedsRecreate at the top of the next frame.
    bool mFgRebuildAgain = false;
    // A rebuild that could not be made now (a present proof withheld, a device wait that failed) is asked again
    // no sooner than this: each try waits for the device.
    std::chrono::steady_clock::time_point mRebuildNotBefore{};
    void rebuildBackoff() { mRebuildNotBefore = std::chrono::steady_clock::now() + std::chrono::milliseconds(500); }
    bool rebuildBackedOff() const { return std::chrono::steady_clock::now() < mRebuildNotBefore; }
    // #1517 Task 11b: once per chain built for frame generation, its images and present mode (llfg::frameGenPresentMode:
    // IMMEDIATE, else MAILBOX -- Wayland without tearing control -- else FIFO; generation runs in all three), and
    // whether the multiplier is refresh-bound there (llfg::refreshBound: MAILBOX / FIFO show at most one frame per
    // refresh).
    void logFrameGenChain()
    {
        if (!mSwapchain.frameGenImages()) return;
        const llfg::PresentMode mode = mSwapchain.fgPresentMode();
        LL_INFOS("Vulkan") << "Frame generation: chain of " << mSwapchain.imageCount() << " images for "
                           << mSwapchain.frameGenMax() << " generated frames, present mode "
                           << llfg::presentModeText(mode)
                           << (llfg::refreshBound(mode) ? "; the multiplier is refresh-bound: no more frames are shown "
                                                          "than the display refreshes" : "") << LL_ENDL;
    }
    // #1517 Task 11b (Linux): the main window cannot be seen -- minimised, hidden, or occluded (SDL's window flags;
    // on Wayland the compositor's suspended state: another workspace, fully covered). Such a window's FIFO (and
    // some MAILBOX) presents block until it shows again. So, each frame: the present thread is told first (from
    // then on it takes no image and presents nothing on such a chain), nothing waits for it here (a drain at
    // hide would wait for exactly the presents the compositor holds), no frame is handed to it to present while
    // hidden (mFgHidden drops the job), and generation stops (the context's background: Block::Background).
    // Windows: as before frame generation ran on Linux -- the main loop skips a minimised or hidden window.
    bool mFgHidden = false;
    void frameGenWindowState()
    {
#if LL_LINUX
        const SDL_WindowFlags f = mWindow ? SDL_GetWindowFlags(mWindow) : SDL_WindowFlags(0);
        const bool hidden = llfg::windowHidden(mFseMinimized || (f & SDL_WINDOW_MINIMIZED) != 0,
                                               (f & SDL_WINDOW_HIDDEN) != 0, (f & SDL_WINDOW_OCCLUDED) != 0);
        mFgPresent.setWindowHidden(hidden);
        if (hidden != mFgHidden && mSwapchain.frameGenImages())
            LL_INFOS("Vulkan") << "Frame generation: the window " << (hidden ? "cannot be seen (minimised, hidden or "
                                  "occluded): nothing is presented on its chain until it shows again" : "shows again")
                               << LL_ENDL;
        mFgHidden = hidden;
#endif
    }
    // #1517: the main chain's rebuild waits (the request kept) while the compositor may hold one of its presents
    // -- a frame generation chain on MAILBOX / FIFO, the window hidden or the present thread held
    // (llfg::deferRebuild): a swapchain is never rebuilt under a present call into it. Made once the window
    // shows again.
    bool frameGenRebuildWaits() const
    {
        return llfg::deferRebuild(mSwapchain.frameGenImages() != 0, mSwapchain.fgPresentMode(), mFgHidden,
                                  mFgPresent.held());
    }
    // #1517 frame generation's images are admitted against the device's free video memory, as the
    // renderer's own large targets are (LLCleanRendererVK::vramDriverFits): the driver's live budget
    // (VK_EXT_memory_budget; RenderVKDebugFakeVramBudgetMB caps it for testing) minus its usage, keeping
    // the large-request margin free (vkLargeRequestMargin). Not the HDR image budget: that one keeps the
    // display's own images only.
    LLVKFrameGenPresent::Budget frameGenBudget()
    {
        return [this](uint64_t& freeBytes, uint64_t& margin)
        {
            uint64_t usage = 0, budget = 0;
            if (!mDevice.liveDeviceBudget(usage, budget)) return false;   // (nothing to check against)
            static LLCachedControl<U32> s_fake(gSavedSettings, "RenderVKDebugFakeVramBudgetMB", 0);
            if ((U32)s_fake > 0) budget = std::min<uint64_t>(budget, (uint64_t)(U32)s_fake << 20);
            freeBytes = budget > usage ? budget - usage : 0;
            margin = vkLargeRequestMargin(budget);
            return true;
        };
    }
    // #1517 frame generation: no job left on the present thread and no gate left shut -- before anything the
    // present thread uses is released or rebuilt: the swapchain (a rebuild, exclusive fullscreen's release), the
    // generator's feature and its input copies, shutdown; and at minimise (no present left held on a gate).
    // One bounded try (LLVKFrameGenPresent::drain). False: the present thread did not drain -- it is inside a
    // present the compositor holds, an acquire or the generator. Nothing was waited further, the pacer and the
    // gates are left to it, and the caller defers: it releases nothing and keeps its request. (A device wait
    // needs no drain: deviceIdleWait.)
    bool drainFrameGen()
    {
        if (!mFgPresent.drain()) return false;
        LLVKFrameGen::instance().drainPacer();
        mSwapchain.gatesReleased();
        return true;
    }
    // #1517: vkqDeviceWaitIdle's wait while frame generation's present thread exists (vkSetDeviceIdleWait; any
    // thread). The present queue is that thread's (Vulkan's external synchronisation) -- it may be inside a
    // present for as long as the compositor holds it -- so the device is waited for without it: every other
    // queue idle, then the present queue's accepted work complete (LLVKFrameGenPresent::waitAccepted: the
    // device's sliced waits). Never a drain, never a call on the present queue. That covers everything the
    // renderer's callers free; what the present thread itself uses is let go only after a drainFrameGen() that
    // succeeded (a rebuild, the feature, the inputs, shutdown). False: no present thread -- the driver's own
    // vkDeviceWaitIdle.
    // (#1544 D8: a present queue shared with the renderer is waited idle as well, inside its lock -- it carries
    // the renderer's work; the present thread's next call on it waits that long, and nothing in the queue waits
    // for that thread. Its presents are IMMEDIATE there: none sits in the compositor with the lock.)
    bool deviceIdleWait(VkResult* result)
    {
        if (!mFgPresent.ownsPresentQueue()) return false;
        VkResult r = mDevice.waitIdleExceptPresentQueue();
        if (r == VK_SUCCESS) r = mFgPresent.waitAccepted(mDevice.device());
        *result = r;
        return true;
    }
    // #1517 frame generation: the generated frames the main swapchain is built for
    // (VkSwapchain::setFrameGen; 0: none). A change of on / off from the chain in use, or more generated
    // frames than it was built for (a higher multiplier: generated + 1 images), asks for the loop's usual
    // rebuild -- once per change: a chain that could not be built for it (frameGenImages() still 0) is not
    // rebuilt every frame, and a later change asks again. Fewer generated frames keep the chain.
    uint32_t mFgChainWanted = 0;   // this frame's wish
    uint32_t mFgChainBuilt = 0;    // the wish the chain in use was last built (or matched) for
    void wantFrameGenChain(uint32_t generated)
    {
        mFgChainWanted = generated;
        mSwapchain.setFrameGen(generated);
        if ((generated != 0) == (mSwapchain.frameGenImages() != 0) && generated <= mSwapchain.frameGenMax())
            mFgChainBuilt = generated;   // (the chain in use carries what is wanted)
        else if (generated != mFgChainBuilt)
        {
            mNeedsRecreate = true;
            mOutputPreviewValid = false;
        }
    }
    // #1517 frame generation's present thread: on a chain built for frame generation it owns the
    // swapchain's images and the present queue, and every image of frame generation; the frame renders
    // into its slot's backbuffer and generator inputs and hands it over (renderFrame). One composite
    // descriptor slot of the encode per present slot.
    LLVKFrameGenPresent mFgPresent;
    uint64_t mAntiLagFrame = 0;       // #1544 AMD Anti-Lag: the frame index of the last INPUT stage (0: none yet)
    uint64_t mAntiLagPresented = 0;   // ... the last one whose PRESENT stage was sent
    static_assert(LLVKFrameGenPresent::kMaxSlots <= VkDisplayEncode::kSlots, "a composite descriptor slot per present slot");
    std::vector<LLVKFrameGenPresent::Result> mFgResults;   // (renderFrame: the finished jobs, reused)
    // What this frame's encode wrote for the generated frames (written false: none are generated, the
    // real frame goes out alone, and `refused` says why).
    struct FgInputs
    {
        bool written = false;
        bool camera = false;      // this frame's camera data was there (takeConstants)
        const char* refused = nullptr;
        const GpuImage* depth = nullptr;
        const GpuImage* motion = nullptr;
        llfg::GenCamera constants{};
    };
    // #1544: what the generator reads and its feature, before this frame records, with nothing of frame
    // generation in flight -- ONE drain of the present thread (it reads the old copies and evaluates with the old
    // feature), then the device waited for -- and only when something must be made:
    //  1. every present slot's copies of the upscaler's depth / motion for the size and format it renders at now
    //     (the last frame's), all or none. A refusal is not tried again until their size or the multiplier changes;
    //  2. then the generator's feature for them: `size` (the caller's display size and formats) gets the render
    //     size and every slot's images (LLVKFrameGenPresent::fillGenSize) -- a generator binds them when its feature
    //     is made, so the copies come first. Another quality mode (another render size) makes FSR's again; DLSS's
    //     ignores it. A refusal ends generation for the session, with its own reason.
    // True: ready, or nothing to copy yet (no upscaler output: the frame then says why itself, and no feature is
    // made before its inputs exist). False: `refused` says why (a lasting refusal of the copies), or stays null --
    // the present thread did not drain or the wait failed (not this frame, nothing released, tried again), or the
    // feature was refused (LLVKFrameGen stopped).
    bool prepareFrameGen(uint32_t generated, llfg::GenSize& size, const char*& refused)
    {
        LLVKFrameGen& fg = LLVKFrameGen::instance();
        const GpuImage* depth = nullptr;
        const GpuImage* motion = nullptr;
        if (!mWorld || !mWorld->frameGenInputs(depth, motion)) return true;
        size.renderW = depth->extent.width;   // (the copies' size: the upscaler's render size)
        size.renderH = depth->extent.height;
        size.colorHdr = mSwapchain.displayStatus().encoding == vkdisplay::Encoding::Hdr10;   // (as fillGenInputs')
        const bool inputsFit = mFgPresent.inputsFit(depth->extent, depth->format, motion->extent, motion->format);
        if (inputsFit)
        {
            mFgPresent.fillGenSize(size);
            if (fg.featureFits(size)) return true;
        }
        const char* const why = "not enough video memory for frame generation's inputs";
        if (!inputsFit && mFgPresent.inputsRefused(depth->extent, depth->format, motion->extent, motion->format, generated))
        {
            refused = why;
            return false;
        }
        if (!drainFrameGen() || vkDeviceWaitIdle(mDevice.device()) != VK_SUCCESS) return false;
        if (!inputsFit && !mFgPresent.prepareInputs(depth->extent, depth->format, motion->extent, motion->format,
                                                    generated, frameGenBudget()))
        {
            refused = why;
            return false;
        }
        mFgPresent.fillGenSize(size);
        return fg.featureFits(size) || fg.prepareFeature(mDevice, size);
    }
    // Copy depth/motion into the present slot: before world-overlay raster when needed,
    // otherwise at frame end. A completed overlay pass prevents a second copy overwriting it.
    // False: no valid inputs (the real frame goes out alone).
    bool mFgOverlayGuidesReady = false, mFgOverlayRefused = false;
    uint32_t mFgOverlaySlot = 0;
    bool recordFrameGenInputs(VkCommandBuffer cmd, uint32_t slot, const FgInputs& in);   // (defined with renderFrame)
    VkDeviceContext mDevice;
    std::unique_ptr<LLVKTexturePrepService> mTexturePrepService;
    bool mTexturePrepTeardownStarted = false;
    void beginTexturePrepTeardown() noexcept
    {
        if (mTexturePrepTeardownStarted) return;
        mTexturePrepTeardownStarted = true;
        if (mTexturePrepService) mTexturePrepService->stopAdmissions();
    }
    struct TexturePrepRecord
    {
        uint16_t slot = 0xffffu;
        uint16_t reserved = 0;
        uint32_t generation = 0;
    };
    static constexpr uint32_t kTexturePrepRecordCap = 128;
    static constexpr uint32_t kTexturePrepFrameSlotCap = 8;
    std::array<TexturePrepRecord, kTexturePrepRecordCap> mTexturePrepRecords{};
    uint32_t mTexturePrepRecordCount = 0;
    bool mTexturePrepRecordFailed = false;
    uint64_t mTexturePrepSubmitSerial = 0;
    std::array<uint64_t, kTexturePrepFrameSlotCap> mTexturePrepSlotSerial{};
    VkSwapchain     mSwapchain;
    VkDisplayFrame  mDisplayFrame;
    VkDisplayEncode mDisplayEncode;
    VkDisplayFrame  mHdrStageFrame;
    VkDisplayEncode mHdrStageEncode;
    VkExtent2D      mHdrStageExtent{};
    VkSurfaceFormatKHR mHdrStageTarget{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    uint64_t        mHdrStageBudget = 0;
    bool            mHdrStageReady = false;
    bool            mHdrStageHasFrame = false;
    bool            mHdrStageHasEncode = false;
    bool            mHdrOutputActive = false;
    uint32_t        mCaptureIdleFrames = 0;   // #1451: frames since the snapshot capture image's last use
    // #1517 Task 7e: the label atlas size the slots grow / shrink towards, and the frames in a row whose labels
    // would all have fit one step smaller.
    VkExtent2D      mLabelAtlasSize{llfgl::kAtlasStart.w, llfgl::kAtlasStart.h};
    uint32_t        mLabelFitsSmallerFrames = 0;
    uint32_t        mLabelIdleFrames = 0;   // frames in a row (of a frame generation chain) that drew no labels
    bool            mHdrOutputReady = false;
    VkExtent2D      mHdrOutputExtent{};
    VkSurfaceFormatKHR mHdrEncodeTarget{VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
    uint64_t        mHdrBudget = 0;
    std::string     mHdrOutputFailureReason;
    float           mHdrAppliedPaperWhiteNits = 0.f;
    float           mHdrAppliedPeakNits = 0.f;

    struct UIWindow
    {
        SDL_Window* window = nullptr; // owner destroys after destroyUIWindow
        NativeSurfaceId surfaceID = 0;
        std::unique_ptr<LLNativeWindow> retiringHost;
        bool closing = false;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchain swapchain;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmds[2] = {};
        int requestedWidth = 0, requestedHeight = 0;
        bool resize = false;
        bool failed = false;
        U32 recreateFailures = 0;
        std::chrono::steady_clock::time_point retryAfter{};
    };
    // Empty on the single-window path. Entries/vertex slots are reused on close.
    std::vector<std::unique_ptr<UIWindow>> mUIWindows;
    VkSemaphore mUIRetirement = VK_NULL_HANDLE;
    uint64_t mUIRetirementSignaled = 0;
    uint64_t mUICompleted = 0;
    bool mUIPathLogged = false;
    VkRetirementRetry mUIRetireRetry;
    unsigned mUIWindowPeak = 0;
    VkUISurfaceRecorder mMainUIRecorder;
    std::function<void(U64)> mUIRecorderShutdown;
    const VkUISurfaceRecorder* mAuxUIRecorder = nullptr;
    // Exact renderer-neutral GPU serials, independent of the facade's UI frame clock.
    VkSemaphore mSurfaceCompletion = VK_NULL_HANDLE;
    uint64_t mSurfaceSerial = 0, mSurfaceCompleted = 0;
    // Auxiliary uploads are not covered by the main swapchain's frame-slot fence.
    // One latest common-queue marker covers every preceding auxiliary upload.
    uint64_t mTexturePrepSurfaceProof = 0, mTexturePrepSurfaceSubmit = 0;
    void observeSurfaceCompletion(uint64_t completed)
    {
        mSurfaceCompleted = completed;
        if (mTexturePrepService && mTexturePrepSurfaceProof && completed >= mTexturePrepSurfaceProof)
        {
            mTexturePrepService->retireGpuUse(mTexturePrepSurfaceSubmit);
            mTexturePrepService->reapCompleted();
            mTexturePrepSurfaceProof = mTexturePrepSurfaceSubmit = 0;
        }
    }
    bool ensureSurfaceCompletion()
    {
        if (mSurfaceCompletion) return true;
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        info.pNext = &type;
        return vkCreateSemaphore(mDevice.device(), &info, nullptr, &mSurfaceCompletion) == VK_SUCCESS;
    }
    uint64_t reserveSurfaceSerial()
    {
        uint64_t completed = 0;
        const VkResult result = vkGetSemaphoreCounterValue(mDevice.device(), mSurfaceCompletion, &completed);
        if (result == VK_SUCCESS) observeSurfaceCompletion(completed);
        else if (result == VK_ERROR_DEVICE_LOST)
            vkDeviceLostRaise("UI target completion query", result, "graphics", __FILE__, __LINE__);
        return ++mSurfaceSerial;
    }
    void signalSurfaceSerial(uint64_t serial)
    {
        if (!serial || vkDeviceIsLost()) return;
        VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signal.semaphore = mSurfaceCompletion;
        signal.value = serial;
        signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
        VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        submit.signalSemaphoreInfoCount = 1;
        submit.pSignalSemaphoreInfos = &signal;
        const VkResult result = vkQueueSubmit2(mDevice.queue(), 1, &submit, VK_NULL_HANDLE);
        // Refused markers pin resources until a later accepted higher marker.
        if (result == VK_ERROR_DEVICE_LOST)
            vkDeviceLostRaise("UI target completion signal", result, "graphics", __FILE__, __LINE__);
        else if (result != VK_SUCCESS)
            LL_WARNS("NativeUI") << "UI target completion signal refused: " << result << LL_ENDL;
    }


    // One primary command buffer PER frame-in-flight slot. A single shared command
    // buffer was the flashing bug: with 2 frames in flight and pre-signaled fences,
    // frame N+1's vkBeginCommandBuffer implicitly reset the buffer while frame N's
    // submission was still pending, so the image frame N presented never reliably
    // got its clear+UI (black / stale-VRAM static, rotating through the images).
    // Per-slot buffers are re-recorded only after acquire() waits that slot's fence.
    std::vector<VkCommandBuffer> mCmds;

    // #1547 TESTING AID (RenderVKDebugGpuHang): a REAL GPU hang, so a recovery meets the driver's own
    // reset instead of a simulated loss. Made on first use; nothing exists unless that one-shot debug
    // setting was set. Destroyed with a usable device (exit, a simulated loss); a lost one takes them along.
    VkPipelineLayout mHangLayout = VK_NULL_HANDLE;
    VkPipeline mHangPipe = VK_NULL_HANDLE;
    GpuBuffer mHangFlag{};

    void destroyDebugGpuHang(bool deviceUsable)
    {
        if (deviceUsable && mDevice.device())
        {
            if (mHangPipe) vkDestroyPipeline(mDevice.device(), mHangPipe, nullptr);
            if (mHangLayout) vkDestroyPipelineLayout(mDevice.device(), mHangLayout, nullptr);
            if (mHangFlag.buffer) mDevice.destroyBuffer(mHangFlag);
        }
        mHangPipe = VK_NULL_HANDLE;
        mHangLayout = VK_NULL_HANDLE;
        mHangFlag = GpuBuffer{};
    }

    // Records the hang into `cmd` (the frame's first graphics command buffer, before anything else).
    // mode 1: unbounded -- only the driver's watchdog ends it. mode N >= 2: a finite spin of N million
    // reloads of a host-visible word (roughly N seconds on a PCIe card; not calibrated, the log says how
    // long the frame took). False: not recorded (the objects could not be made; logged).
    bool recordDebugGpuHang(VkCommandBuffer cmd, U32 mode)
    {
        const VkDevice dev = mDevice.device();
        if (!dev || !cmd || mode == 0) return false;
        if (!mHangPipe)
        {
            mHangFlag = mDevice.createBuffer(16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                             /*deviceAddress=*/true);
            if (!mHangFlag.buffer || !mHangFlag.mapped || !mHangFlag.address)
            {
                LL_WARNS("Vulkan") << "RenderVKDebugGpuHang: the flag buffer could not be made; no hang" << LL_ENDL;
                destroyDebugGpuHang(true);
                return false;
            }
            std::memset(mHangFlag.mapped, 0, 16);   // (stays 0: the shader waits for it to change)
            VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 16};
            VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
            li.pushConstantRangeCount = 1;
            li.pPushConstantRanges = &pr;
            if (vkCreatePipelineLayout(dev, &li, nullptr, &mHangLayout) != VK_SUCCESS) mHangLayout = VK_NULL_HANDLE;
            VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            mi.codeSize = kWorldDebugGpuHangSpv_bytes;
            mi.pCode = kWorldDebugGpuHangSpv;
            VkShaderModule m = VK_NULL_HANDLE;
            if (mHangLayout && vkCreateShaderModule(dev, &mi, nullptr, &m) == VK_SUCCESS)
            {
                mHangPipe = createComputePipeline(dev, m, mHangLayout, 0, nullptr, "debug GPU hang");
                vkDestroyShaderModule(dev, m, nullptr);
            }
            if (!mHangPipe)
            {
                LL_WARNS("Vulkan") << "RenderVKDebugGpuHang: the pipeline could not be made; no hang" << LL_ENDL;
                destroyDebugGpuHang(true);
                return false;
            }
        }
        struct { uint32_t lo, hi, iterations, pad; } push{
            (uint32_t)(mHangFlag.address & 0xffffffffull), (uint32_t)(mHangFlag.address >> 32),
            mode >= 2 ? (uint32_t)std::min<U64>((U64)mode * 1000000ull, 0xfffffffeull) : 0u, 0u};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, mHangPipe);
        vkCmdPushConstants(cmd, mHangLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), &push);
        vkCmdDispatch(cmd, 1, 1, 1);
        return true;
    }

    uint32_t mWidth  = 0;
    uint32_t mHeight = 0;
    bool     mNeedsRecreate = false;
    uint32_t mFrameCounter  = 0;

    // EXPERIMENTAL Vulkan UI: the facade-backed LLRender backend (installed into
    // LLRender::setBackend). Owned here; null until initRenderBackend().
    LLRenderVKBackend* mRenderBackend = nullptr;
    bool mFacadeReady = false;

    // GPU-driven world renderer (static opaque). Created lazily on the first
    // frame that requests world rendering; null until then. Behind the VK flag.
    LLCleanRendererVK* mWorld = nullptr;
    // One fixed non-renderable owner for a renderer whose init or isolated synth
    // cleanup could not prove idle. Never expose this pointer to frame/output code.
    LLCleanRendererVK* mDeferredWorld = nullptr;
    double mDeferredWorldRetryAt = 0.0;
    bool mWorldInitTried = false;
    double mWorldInitRetryAt = 0.0;
    // #1384: consecutive inits that ran out of memory (the retry backs off: 2 s doubling
    // to 60 s; reset by a successful init).
    uint32_t mWorldInitOomFails = 0;

    // The sole deferred owner is never renderable. Retrying shutdown performs a
    // fresh device-idle proof inside LLCleanRendererVK::shutdown before the slot is
    // cleared and another live renderer may be initialized.
    bool retryDeferredWorldShutdown(double now)
    {
        if (!mDeferredWorld) return true;
        if (now < mDeferredWorldRetryAt) return false;
        mDeferredWorld->shutdown();
        mDeferredWorldRetryAt = now + 2.0;
        if (mDeferredWorld->shutdownDeferred()) return false;
        delete mDeferredWorld;
        mDeferredWorld = nullptr;
        mWorldInitTried = false;
        mWorldInitRetryAt = std::max(mWorldInitRetryAt, now + 2.0);   // (keeps a #1384 back-off)
        return true;
    }
    // Scene generation is independent from swapchain/display output generation. The
    // world renderer and its uploaded scene mirror stay resident across output changes.
    uint64_t mDisplayGeneration = 1;
    uint64_t mWorldSourceGeneration = 0;
    uint64_t mOutputRequestGeneration = 1;
    VkFormat mOutputRequestFormat = VK_FORMAT_UNDEFINED;
    VkColorSpaceKHR mOutputColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    VkColorSpaceKHR mOutputRequestColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    vkdisplay::Status mOutputPreview{};
    bool mOutputPreviewValid = false;
    bool mOutputRequestLinear = false;
    bool mOutputRequestUiLayer = false;   // #1536: the request's UI layer (HDR, or SDR on a frame generation chain)
    float mOutputRequestWhite = 203.f, mOutputRequestPeak = 1000.f;
    bool mOutputRequestValid = false;
    double mOutputRetryAt = 0.0;
    uint64_t mOutputPendingLogGeneration = 0;
    double mOutputPendingSince = 0.0, mOutputPendingLogAt = 0.0;

    // #1074 VR: while an OpenXR session runs, the world renders into this eye-size target
    // (window format, so the final pipelines are reused) instead of the window image; it is
    // copied into both eye images and blitted aspect-fit onto the window (the mirror). Only
    // ever created in VR; released when the session stops.
    GpuImage mVrTarget;
    bool     mVrBlitChecked = false;
    bool     mVrBlitOk = false;
    VkFilter mVrBlitFilter = VK_FILTER_LINEAR;
    bool     mVrWorldLast = false;   // the previous frame was a VR world frame
    // #1076 VR: the UI wall's own image (window size and format): the UI replayed over a
    // transparent clear, so the wall shows the world between the floaters. VR world frames
    // with the wall shown only; released with the eye target.
    GpuImage mVrUiTarget;
    GpuImage mVrHudDepth;   // #1076 the HUD pass's depth when the HUDs go onto the wall (same size)
    // #1594 VR SPACE WARP: the newest real frame's history and the warp pass (nothing is created
    // until space warp first engages; released with the eye target). mVrUiFresh: the last frame
    // that ran the full path laid the UI into mVrUiTarget (left TRANSFER_SRC) -- a warp frame
    // copies it into the UI swapchain.
    LLVRSpaceWarpVK mSpaceWarp;
    bool mVrUiFresh = false;
    bool vrSpaceWarpSubmit(const LLVKWorldFrame* world, bool warpOnly);
    void vrSpaceWarpCapture(VkCommandBuffer cmd, const LLVKWorldFrame* world, VkImage worldImg, VkExtent2D worldExt,
                            uint32_t worldLayers);
    // #1076: the two soft-cap sources the renderer gets merged: "Keep my FPS" and the VR
    // safety governor (default-constructed = no cap).
    LLVKPerfCaps mAutoTuneCaps;
    LLVKPerfCaps mVrSafetyCaps;
    bool vrEnsureTarget(uint32_t w, uint32_t h);   // (defined with renderFrame)
    bool vrEnsureUiTarget(VkExtent2D ext);
    bool vrEnsureHudDepth(VkExtent2D ext, VkFormat fmt);
    void vrReleaseTarget(bool waitIdle);
    void vrRecordMirror(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent);

    // #919 full-screen exclusive (Windows). mFseExt: the device has the extension and every
    // entry point resolved. The swapchain carries the exclusive chain (mFseChained) while
    // armed AND focused AND not minimised AND not suspended; the mode is acquired on such a
    // swapchain (mFseHeld) -- retried for kFseAcquireWindow after a (re)creation or a loss,
    // then suspended (composited presentation) until the next focus gain.
    bool     mFseExt = false;
    bool     mFseWanted = false;
    // Not focused until SDL says so (FOCUS_GAINED follows show()): a startup fullscreen arms
    // exclusive mode while the window is still hidden, and acquiring it then only fails.
    bool     mFseFocused = false;
    bool     mFseMinimized = false;
    bool     mFseSuspended = false;
    bool     mFseChained = false;
    bool     mFseHeld = false;
    bool     mFseTrying = false;
    std::chrono::steady_clock::time_point mFseTryStart;
#if LL_WINDOWS
    VkSurfaceFullScreenExclusiveInfoEXT      mFseInfo{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_INFO_EXT};
    VkSurfaceFullScreenExclusiveWin32InfoEXT mFseWin32{VK_STRUCTURE_TYPE_SURFACE_FULL_SCREEN_EXCLUSIVE_WIN32_INFO_EXT};
    PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR  mFseCaps2 = nullptr;
    PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT  mFseModes2 = nullptr;
    PFN_vkAcquireFullScreenExclusiveModeEXT         mFseAcquire = nullptr;
    PFN_vkReleaseFullScreenExclusiveModeEXT         mFseRelease = nullptr;

    // The monitor the window is on (exclusive mode is per monitor), into the Win32 info
    // struct chained behind the exclusive info. False without a window handle.
    bool fseRefreshMonitor()
    {
        HWND hwnd = mWindow ? (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(mWindow),
                                                           SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr)
                            : nullptr;
        HMONITOR mon = hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST) : nullptr;
        mFseInfo.fullScreenExclusive = VK_FULL_SCREEN_EXCLUSIVE_APPLICATION_CONTROLLED_EXT;
        mFseInfo.pNext = &mFseWin32;
        mFseWin32.pNext = nullptr;
        mFseWin32.hmonitor = mon;
        return mon != nullptr;
    }

    // Does the surface support exclusive mode on that monitor
    // (VkSurfaceCapabilitiesFullScreenExclusiveEXT, queried WITH the exclusive chain).
    bool fseSurfaceSupported()
    {
        if (!mFseExt || !mFseCaps2 || !mSurface || !fseRefreshMonitor()) return false;
        VkPhysicalDeviceSurfaceInfo2KHR si{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
        si.pNext = &mFseInfo;
        si.surface = mSurface;
        VkSurfaceCapabilitiesFullScreenExclusiveEXT fse{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_FULL_SCREEN_EXCLUSIVE_EXT};
        VkSurfaceCapabilities2KHR caps{VK_STRUCTURE_TYPE_SURFACE_CAPABILITIES_2_KHR};
        caps.pNext = &fse;
        const VkResult r = mFseCaps2(mDevice.physicalDevice(), &si, &caps);
        if (r != VK_SUCCESS)
        {
            LL_WARNS("Vulkan") << "Fullscreen: vkGetPhysicalDeviceSurfaceCapabilities2KHR = " << vkResultString(r) << LL_ENDL;
            return false;
        }
        return fse.fullScreenExclusiveSupported == VK_TRUE;
    }

    // The present modes valid with the exclusive chain (vkGetPhysicalDeviceSurfacePresentModes2EXT).
    bool fsePresentModes(std::vector<VkPresentModeKHR>& out)
    {
        if (!mFseModes2 || !mSurface) return false;
        VkPhysicalDeviceSurfaceInfo2KHR si{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SURFACE_INFO_2_KHR};
        si.pNext = &mFseInfo;
        si.surface = mSurface;
        return vkenum::collect(out,
            [&](uint32_t* n, VkPresentModeKHR* p) { return mFseModes2(mDevice.physicalDevice(), &si, n, p); },
            "vkGetPhysicalDeviceSurfacePresentModes2EXT", 64) == VK_SUCCESS && !out.empty();
    }

    // False (#1517): not released now -- frame generation's present thread did not drain (it may be inside a
    // call into this swapchain); the caller asks again.
    bool fseRelease(const char* why)
    {
        if (!mFseHeld) return true;
        if (!drainFrameGen()) return false;   // (#1517: no present thread call into the chain, no present held on a gate)
        mFseHeld = false;
        if (mFseRelease && mSwapchain.handle() && !vkDeviceIsLost())
        {
            const VkResult r = mFseRelease(mDevice.device(), mSwapchain.handle());
            LL_INFOS("Vulkan") << "Fullscreen: exclusive mode released (" << why << "): " << vkResultString(r) << LL_ENDL;
        }
        return true;
    }
#endif

    void discardHdrOutputStage()
    {
        mHdrStageEncode.destroy();
        mHdrStageFrame.destroy();
        mHdrStageExtent = {};
        mHdrStageTarget = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        mHdrStageBudget = 0;
        mHdrStageReady = false;
        mHdrStageHasFrame = false;
        mHdrStageHasEncode = false;
    }

    bool hdrOutputStageResourcesMatch(const vkdisplay::Status& status, VkExtent2D extent) const
    {
        return vkdisplay::transaction::stageResourcesMatch(
            mHdrStageExtent.width==extent.width && mHdrStageExtent.height==extent.height,
            mHdrStageTarget.format==status.actual.format &&
                mHdrStageTarget.colorSpace==status.actual.colorSpace,
            (mHdrStageHasFrame ? mHdrStageFrame.validForExtent(extent)
                               : (mHdrOutputActive && mDisplayFrame.validForExtent(extent))),
            (mHdrStageHasEncode ? mHdrStageEncode.validForTarget(status.actual)
                                : (mHdrOutputActive && mDisplayEncode.validForTarget(status.actual))));
    }

    bool hdrOutputStageMatches(const vkdisplay::Status& status, VkExtent2D extent) const
    {
        return mHdrStageReady && hdrOutputStageResourcesMatch(status,extent);
    }

    bool stageHdrOutput(const vkdisplay::Status& status, VkExtent2D extent)
    {
        mHdrOutputFailureReason.clear();
        if (status.encoding==vkdisplay::Encoding::Sdr) return true;
        if (hdrOutputStageMatches(status,extent)) return true;

        discardHdrOutputStage();
        const bool liveFrameReady = mHdrOutputActive && mDisplayFrame.validForExtent(extent);
        const bool liveEncodeReady = mHdrOutputActive && mDisplayEncode.validForTarget(status.actual);
        const auto plan=vkdisplay::transaction::makeStagePlan(liveFrameReady,liveEncodeReady);
        uint64_t available=mHdrBudget;
        if (vkdisplay::transaction::stageNeedsHeapAdmission(plan))
        {
            uint64_t usage = 0, heapBudget = 0;
            if (!mDevice.liveDeviceBudget(usage,heapBudget) || heapBudget<=usage)
            {
                mHdrOutputFailureReason = "Vulkan reported no available heap headroom for staged HDR composition";
                return false;
            }
            available = std::min<uint64_t>(heapBudget-usage,512ull<<20);
        }
        VkDeviceContext::NoReliefScope noRelief;
        if (!plan.reuseFrame && !mHdrStageFrame.prepare(mDevice,true,extent,0,available))
        {
            mHdrOutputFailureReason = "staged linear FP16 display target was refused by the image budget or allocation";
            discardHdrOutputStage();
            return false;
        }
        mHdrStageHasFrame = !plan.reuseFrame;
        if (!plan.reuseEncoder)
        {
            const std::span<const uint32_t> vertex(kWorldGlowCombineVertSpv,
                                                   sizeof(kWorldGlowCombineVertSpv)/sizeof(uint32_t));
            const std::span<const uint32_t> fragment(kWorldDisplayOutputFragSpv,
                                                     sizeof(kWorldDisplayOutputFragSpv)/sizeof(uint32_t));
            if (!mHdrStageEncode.prepare(mDevice,true,status.actual,vertex,fragment))
            {
                mHdrOutputFailureReason = "staged final display encoder creation failed";
                discardHdrOutputStage();
                return false;
            }
            mHdrStageHasEncode = true;
        }
        mHdrStageExtent = extent;
        mHdrStageTarget = status.actual;
        mHdrStageBudget = vkdisplay::transaction::stageBudget(plan,mHdrBudget,available);
        mHdrStageReady = hdrOutputStageResourcesMatch(status,extent);
        if (!mHdrStageReady)
        {
            mHdrOutputFailureReason = "staged HDR composition resources failed target identity validation";
            discardHdrOutputStage();
            return false;
        }
        return true;
    }

    void updateHdrMetadata(const vkdisplay::Status& status)
    {
#ifdef VK_EXT_hdr_metadata
        if (!mDevice.optionalExtensionEnabled(VK_EXT_HDR_METADATA_EXTENSION_NAME)) return;
        auto setMetadata = reinterpret_cast<PFN_vkSetHdrMetadataEXT>(
            vkGetDeviceProcAddr(mDevice.device(),"vkSetHdrMetadataEXT"));
        if (!setMetadata || !mSwapchain.handle()) return;
        VkHdrMetadataEXT metadata{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
        metadata.displayPrimaryRed={0.708f,0.292f};
        metadata.displayPrimaryGreen={0.170f,0.797f};
        metadata.displayPrimaryBlue={0.131f,0.046f};
        metadata.whitePoint={0.3127f,0.3290f};
        metadata.maxLuminance=status.peakNits;
        metadata.minLuminance=0.005f;
        metadata.maxContentLightLevel=status.peakNits;
        metadata.maxFrameAverageLightLevel=status.paperWhiteNits;
        const VkSwapchainKHR chain=mSwapchain.handle();
        setMetadata(mDevice.device(),1,&chain,&metadata);
#endif
    }

    bool prepareHdrOutput()
    {
        mHdrOutputFailureReason.clear();
        const auto status=mSwapchain.displayStatus();
        if (status.encoding==vkdisplay::Encoding::Sdr)
        {
            // Keep the active HDR target until the matching SDR renderer bank commits.
            mHdrOutputReady=true;
            return true;
        }
        return stageHdrOutput(status,mSwapchain.extent());
    }

    bool commitHdrOutputStage(const vkdisplay::Status& status, VkExtent2D extent)
    {
        if (status.encoding==vkdisplay::Encoding::Sdr || !hdrOutputStageMatches(status,extent))
            return false;
        // #1517: the live encoder's composite sets (and the frame behind them) are the present thread's while
        // one of its jobs runs. Replaced only with that thread drained -- a drain of their own, checked (today
        // every swap follows a rebuild, which drained; this does not rest on that order). Not drained: not
        // committed now, tried again. (Nothing staged to swap in -- every frame's steady state: no drain.)
        if ((mHdrStageHasFrame || mHdrStageHasEncode) && !drainFrameGen())
        {
            LL_WARNS("Vulkan") << "HDR composition commit waits: frame generation's present thread is inside a job"
                               << LL_ENDL;
            return false;
        }
        if (mHdrStageHasFrame) mDisplayFrame.swap(mHdrStageFrame);
        if (mHdrStageHasEncode) mDisplayEncode.swap(mHdrStageEncode);
        // New frame images (a resize) may reuse a destroyed view's handle value (#1451).
        mDisplayEncode.invalidateInputs();
        mCaptureIdleFrames = 0;
        if (!mDisplayFrame.validForExtent(extent) || !mDisplayEncode.validForTarget(status.actual))
        {
            discardHdrOutputStage();
            return false;
        }
        // The WSI recreation drained prior users; these staged objects now hold the
        // retired resources and can be destroyed after the matching bank is ready.
        mHdrStageFrame.destroy();
        mHdrStageEncode.destroy();
        mHdrOutputActive=true;
        mHdrOutputReady=true;
        mHdrOutputExtent=extent;
        mHdrEncodeTarget=status.actual;
        mHdrBudget=mHdrStageBudget;
        mHdrAppliedPaperWhiteNits=status.paperWhiteNits;
        mHdrAppliedPeakNits=status.peakNits;
        mHdrStageExtent={};
        mHdrStageTarget={VK_FORMAT_UNDEFINED,VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        mHdrStageBudget=0;
        mHdrStageReady=mHdrStageHasFrame=mHdrStageHasEncode=false;
        mHdrOutputFailureReason.clear();
        updateHdrMetadata(status);
        return true;
    }

    bool hdrCompositionValidForCurrentChain() const
    {
        const auto& status=mSwapchain.displayStatus();
        const VkExtent2D extent=mSwapchain.extent();
        const auto renderer=mWorld ? mWorld->displayOutputState()
                                   : LLCleanRendererVK::DisplayOutputState{};
        const bool targetMatches=mHdrEncodeTarget.format==status.actual.format &&
            mHdrEncodeTarget.colorSpace==status.actual.colorSpace;
        const vkdisplay::transaction::CompositionState state{
            mSwapchain.valid(),
            status.encoding!=vkdisplay::Encoding::Sdr,
            mWorld ? renderer.linearNits : status.encoding!=vkdisplay::Encoding::Sdr,
            mHdrOutputActive,
            mDisplayFrame.linear().image!=VK_NULL_HANDLE,
            mDisplayFrame.linear().view!=VK_NULL_HANDLE,
            mDisplayEncode.validForTarget(status.actual),
            mHdrOutputExtent.width==extent.width && mHdrOutputExtent.height==extent.height,
            targetMatches};
        return vkdisplay::transaction::compositionCanRecord(state) &&
            (status.encoding==vkdisplay::Encoding::Sdr || mHdrOutputReady);
    }

    void releaseHdrOutputAfterSdrCommit()
    {
        // (#1517: as commitHdrOutputStage -- the encoder goes only with the present thread drained; else kept,
        // and released by a later frame: the render loop asks while the HDR objects exist)
        if (!drainFrameGen()) return;
        discardHdrOutputStage();
        mDisplayEncode.destroy();
        mDisplayFrame.destroy();
        mHdrOutputActive = false;
        mHdrOutputReady = true;
        mHdrOutputExtent = {};
        mHdrEncodeTarget = {VK_FORMAT_UNDEFINED, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR};
        mHdrBudget = 0;
        mHdrAppliedPaperWhiteNits = 0.f;
        mHdrAppliedPeakNits = 0.f;
    }

    // ---- #1536 SDR frame generation: the SDR UI layer ----
    // SDR draws its interface into a layer of its own exactly while the main swapchain is a frame generation
    // chain (llfg::sdrLayer); the frame then composes world, labels and layer into the slot's backbuffer
    // (VkDisplayEncode::recordLayerComposite), and the present thread composes the same layer over each
    // generated world. On a normal chain nothing below is used and SDR draws as it always did.
    // An SDR chain's output for frame generation's rules: the 8-bit UNORM formats, sRGB-encoded, run; an
    // *_SRGB surface blends in linear light and does not.
    static llfg::Output sdrOutput(VkFormat format)
    {
        return (format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_R8G8B8A8_UNORM) ? llfg::Output::Sdr8
                                                                                           : llfg::Output::SdrOther;
    }
    // The chain in use is an SDR frame generation chain (asked only of an SDR chain).
    bool sdrLayerChain() const { return llfg::sdrLayer(sdrOutput(mSwapchain.format()), mSwapchain.frameGenImages()); }
    // The SDR chain about to be built (in `format`) will be one: frame generation wants its chain and runs in
    // that format. The renderer's UI-layer pipelines are staged on this answer BEFORE the rebuild, as an HDR
    // switch's are. Not when the chain in use was already built for this very wish and came out a normal one
    // (it cannot be built for frame generation): the layer would be staged, and cancelled, at every rebuild.
    bool sdrLayerExpected(VkFormat format) const
    {
        const bool notBuildable = mFgChainBuilt == mFgChainWanted && mSwapchain.frameGenImages() == 0;
        return llfg::sdrLayer(sdrOutput(format), mFgChainWanted) && !notBuildable;
    }
    // The composite for the chain in use: the display encode in its SDR-layer mode, for the chain's format.
    // Kept while it fits; made anew for another format. As commitHdrOutputStage: the live encoder's composite
    // sets are the present thread's while one of its jobs runs, so it is replaced only with that thread
    // drained (and only after a rebuild whose device wait succeeded: the caller asks once the present slots
    // are ready). True: ready. False: not now (not drained; asked again), or -- `failed` -- it could not be made.
    bool prepareSdrLayerEncode(bool& failed)
    {
        failed = false;
        const VkSurfaceFormatKHR target{mSwapchain.format(), mSwapchain.colorSpace()};
        if (mDisplayEncode.mode() == VkDisplayEncode::Mode::SdrLayer && mDisplayEncode.validForTarget(target))
            return true;
        if (!drainFrameGen()) return false;
        const std::span<const uint32_t> vertex(kWorldGlowCombineVertSpv,
                                               sizeof(kWorldGlowCombineVertSpv)/sizeof(uint32_t));
        const std::span<const uint32_t> fragment(kWorldDisplayOutputFragSpv,
                                                 sizeof(kWorldDisplayOutputFragSpv)/sizeof(uint32_t));
        if (mDisplayEncode.prepare(mDevice, true, target, vertex, fragment, true))
        {
            LL_INFOS("Vulkan") << "Frame generation: SDR composite ready (format " << (int)target.format << ")" << LL_ENDL;
            return true;
        }
        failed = true;
        return false;
    }
    // The chain in use is not an SDR frame generation chain (any more): its composite goes, as
    // releaseHdrOutputAfterSdrCommit lets the HDR encoder go -- only with the present thread drained; else
    // kept, and released by a later frame (the render loop asks every frame; a no-op without one).
    void releaseSdrLayerEncode()
    {
        if (mDisplayEncode.mode() != VkDisplayEncode::Mode::SdrLayer) return;
        // (and only after a rebuild whose device wait succeeded: frames recorded before a failed wait may still
        // bind its pipeline and sets; kept until a later rebuild proves the device idle)
        if (!mSwapchain.deviceIdled() || !drainFrameGen()) return;
        mDisplayEncode.destroy();
    }
    // Last-line gate, as hdrCompositionValidForCurrentChain: never record an SDR-layer frame without the
    // present slots it renders into, the composite for this chain's format, and the renderer's committed
    // UI layer in the chain's format (its HUD, marker and interface pipelines write the layer).
    bool sdrLayerValidForCurrentChain() const
    {
        const VkSurfaceFormatKHR target{mSwapchain.format(), mSwapchain.colorSpace()};
        const auto renderer = mWorld ? mWorld->displayOutputState() : LLCleanRendererVK::DisplayOutputState{};
        return mSwapchain.valid() && mFgPresent.ready() &&
               mDisplayEncode.mode() == VkDisplayEncode::Mode::SdrLayer && mDisplayEncode.validForTarget(target) &&
               (!mWorld || (renderer.uiLayer && !renderer.linearNits && renderer.colorFormat == target.format));
    }
    double mSdrLayerWithheldLogAt = 0.0;   // (the gate's warning: at most once a second)
};

// #1411 P0.3: what this device offers for pipeline creation, one line: graphics pipeline libraries
// (+ fast linking, independent interpolation), VK_KHR_pipeline_binary, VK_EXT_shader_object,
// pipeline creation cache control, the subgroup size range and the driver. Bug reports from
// other vendors answer "would GPL / binaries help here" from this line.
static void logCompileCapabilities(VkPhysicalDevice pd, const VkPhysicalDeviceDriverProperties& drv)
{
    auto exts = vkenum::deviceExtensions(pd);
    [[maybe_unused]] uint32_t n = uint32_t(exts.size());
    auto has = [&](const char* e) {
        for (const VkExtensionProperties& x : exts)
            if (!std::strcmp(x.extensionName, e)) return true;
        return false;
    };
    const bool gpl = has("VK_EXT_graphics_pipeline_library");
    const bool bin = has("VK_KHR_pipeline_binary");
    const bool so = has("VK_EXT_shader_object");
    VkPhysicalDeviceGraphicsPipelineLibraryPropertiesEXT gplp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_GRAPHICS_PIPELINE_LIBRARY_PROPERTIES_EXT};
    VkPhysicalDeviceSubgroupSizeControlProperties sg{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &sg;
    if (gpl) sg.pNext = &gplp;
    vkGetPhysicalDeviceProperties2(pd, &p2);
    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    LL_INFOS("VKPipe") << "compile capabilities (#1411): graphics pipeline library "
                       << (gpl ? std::string("yes (fast linking ") + (gplp.graphicsPipelineLibraryFastLinking ? "yes" : "no")
                                     + ", independent interpolation "
                                     + (gplp.graphicsPipelineLibraryIndependentInterpolationDecoration ? "yes" : "no") + ")"
                               : std::string("no"))
                       << "; pipeline binary " << (bin ? "yes" : "no") << "; shader object " << (so ? "yes" : "no")
                       << "; pipeline creation cache control " << (f13.pipelineCreationCacheControl ? "yes" : "no")
                       << "; subgroup size " << sg.minSubgroupSize << "-" << sg.maxSubgroupSize << "; driver "
                       << drv.driverName << " " << drv.driverInfo << " (id " << (unsigned)drv.driverID << ")" << LL_ENDL;
}

LLVulkanContext& LLVulkanContext::instance()
{
    static LLVulkanContext sInstance;
    return sInstance;
}

std::string LLVulkanContext::specStatusText(bool* fallback)
{
    return LLCleanRendererVK::specStatusText(fallback);
}

// #462: persists the pipeline cache from a compile worker every few compiles, so a
// crash (or OOM) during the long cold compile at the login screen keeps its progress.
static VkDeviceContext* s_pipeSaveDev = nullptr;
static std::string s_pipeCostPath;   // #471 learned compile costs ("" = memory only)
static unsigned s_pipeProgressEvery = 8;   // #672: 4 on a cold cache
static void pipelineProgressSave()
{
    if (!s_pipeSaveDev) return;
    s_pipeSaveDev->savePipelineCache("compile progress");
    // #471: with the costs learned so far (+ the compiles in flight), so an OOM crash
    // mid-batch still teaches the next launch which pipelines are heavy. At most once a
    // second (a warm start runs ~100 cache hits in ~30 ms; the batch end saves anyway).
    // Serialized by vk_pipeline's progress mutex.
    static std::chrono::steady_clock::time_point sLast;
    const auto now = std::chrono::steady_clock::now();
    if (now - sLast < std::chrono::seconds(1)) return;
    sLast = now;
    vkpipecache::saveCosts(s_pipeSaveDev->physicalDevice(), s_pipeCostPath, "compile progress");
}

// #471: bytes pipeline compiles may use right now: the live headroom above a reserve for
// the rest of the system. Windows: min(available RAM, commit still available) -- the
// commit charge is what the driver's compiler exhausted (#458). 0 while the memory
// governor is critical (the scheduler then runs one compile at a time).
// RenderVKPipelineCompileBudgetMB > 0 replaces it with a fixed budget (testing).
// #672: AVAILABLE memory (never total), re-read at most every 50 ms -- each worker asks
// before admitting a compile -- less a reserve for the system and, per driver, a margin
// for what its compiler allocates outside our measurement (AMD proprietary: 2 GB; its
// compiler failed with VK_ERROR_OUT_OF_HOST_MEMORY well below the system's limits).
static std::atomic<U64> s_pipeBudgetForced{0};
static std::atomic<U64> s_pipeDriverMargin{0};
static uint64_t pipelineMemProbe();
static uint64_t pipelineMemBudget()
{
    // #1103: the compile concurrency follows the memory governor while batches run (every
    // worker asks here before each admission): 2 at once while tight, 1 while critical
    // (incl. the #698 shed window), no cap when comfortable. Restored as soon as the
    // governor reports comfortable again (it holds an improvement 2 s before reporting it).
    {
        const LLMemoryGovernor::EState st = LLMemoryGovernor::getState();
        const unsigned pcap = st == LLMemoryGovernor::CRITICAL ? 1u : (st == LLMemoryGovernor::TIGHT ? 2u : 0u);
        vkSetPipelinePressureCap(pcap, LLMemoryGovernor::stateName(st));
    }
    const U64 forced = s_pipeBudgetForced.load(std::memory_order_relaxed);
    if (forced)
    {
        // Simulate a machine with `forced` bytes of headroom: like the live budget, it
        // shrinks as committed memory grows (above the lowest level seen), so in-flight
        // compiles' growth uses it up. A constant here over-admitted: the scheduler
        // discounts in-flight estimates by their measured growth, which is only right when
        // the budget itself has already shrunk by that growth.
        static std::atomic<U64> sLow{UINT64_MAX};
        const U64 now = pipelineMemProbe();
        U64 low = sLow.load(std::memory_order_relaxed);
        while (now < low && !sLow.compare_exchange_weak(low, now)) {}
        low = std::min(low, now);
        const U64 grown = now > low ? now - low : 0;
        return forced > grown ? forced - grown : 0;
    }
    if (LLMemoryGovernor::getState() == LLMemoryGovernor::CRITICAL) return 0;
    // Sampled at most every 50 ms (every waiting worker asks every 50 ms).
    static std::mutex sMutex;
    static std::chrono::steady_clock::time_point sAt;
    static uint64_t sBudget = 0;
    static bool sHave = false;
    std::lock_guard<std::mutex> lk(sMutex);
    const auto now = std::chrono::steady_clock::now();
    if (sHave && now - sAt < std::chrono::milliseconds(50)) return sBudget;
    LLMemoryGovernor::SystemMemory sm;
    if (!LLMemoryGovernor::readSystemMemory(sm) || !sm.avail)
    {
        sBudget = UINT64_MAX;   // can't tell: only the thread count limits
    }
    else
    {
        U64 avail = sm.avail;
        if (sm.have_commit) avail = std::min(avail, sm.commit_avail);
        const U64 reserve = std::max<U64>(2048ull << 20, sm.total / 10)
                          + s_pipeDriverMargin.load(std::memory_order_relaxed);
        sBudget = avail > reserve ? avail - reserve : 0;
    }
    sAt = now;
    sHave = true;
    return sBudget;
}

// static
void LLVulkanContext::setPipelineCompileBudgetMB(uint32_t mb)
{
    s_pipeBudgetForced.store((U64)mb << 20, std::memory_order_relaxed);
}

// #471: the process's committed memory for the per-pipeline compile measurement. On
// Windows the private bytes (the commit charge the driver's compiler exhausted in #458);
// elsewhere the resident set.
static uint64_t pipelineMemProbe()
{
#if LL_WINDOWS
    PROCESS_MEMORY_COUNTERS_EX pmem{};
    pmem.cb = sizeof(pmem);
    if (GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pmem, sizeof(pmem)))
        return (uint64_t)pmem.PrivateUsage;
    return 0;
#else
    return LLMemory::getCurrentRSS();
#endif
}

// #672: the pipeline-creation recovery path (out-of-memory step-downs, retries, give-ups).
static void pipelineEvent(bool warn, const char* line)
{
    if (warn)
        LL_WARNS("VKPipe") << line << LL_ENDL;
    else
        LL_INFOS("VKPipe") << line << LL_ENDL;
}

// #471: batch reports -> the log (VKPipe) + a one-line snapshot of the heaviest batch.
static std::mutex s_pipeReportMutex;
static std::string s_pipeReportText;
static double s_pipeReportSumMs = -1.0;

// #1103: the driver compilers' memory is freed on the compile threads, into glibc's
// per-thread arenas, which keep it resident (#1059: Arc B580 / Mesa, 16 GB: 8.6-9.9 GB
// resident after a cold compile, then the OOM killer). After a batch -- its workers have
// all been joined when the report hook runs -- and once no other batch is compiling, the
// freed memory goes back to the OS with malloc_trim(0), measured (RSS before / after, the
// arenas' free bytes before it). Small batches (single admitted creates) add up until they
// are worth an arena walk. Off the main thread it runs right here (the batch's own thread,
// before its job reports done), on the main thread (a synchronous flush) on a General worker.
#if LL_LINUX && defined(__GLIBC__)
static std::atomic<bool> s_trimRunning{false};
static std::atomic<unsigned> s_trimDebtCompiles{0};
static std::atomic<U64> s_trimDebtBytes{0};
constexpr unsigned kTrimDebtCompiles = 4;      // real compiles since the last trim ...
constexpr U64 kTrimDebtBytes = 64ull << 20;    // ... or this much batch growth

static std::string heapTrimText(const LLMemoryGovernor::HeapTrim& t)
{
    constexpr U64 MB = 1ull << 20;
    return llformat("rss %llu MB -> %llu MB after malloc_trim (%lld MB, %.0f ms; %llu MB free in %u arenas before it)",
                    (unsigned long long)(t.rss_before / MB), (unsigned long long)(t.rss_after / MB),
                    (long long)(t.rss_after / MB) - (long long)(t.rss_before / MB), t.ms,
                    (unsigned long long)(t.free_before / MB), t.arenas);
}
#endif

// Text for the batch report line ("" when nothing was compiled / not supported).
static std::string pipelineHeapTrim(const VkPipelineBatchReport& r)
{
#if LL_LINUX && defined(__GLIBC__)
    unsigned real = 0;
    for (const VkPipelineCompileStat& s : r.items)
        if (!s.hit && !s.rest && !s.failed) ++real;
    if (real)
    {
        s_trimDebtCompiles.fetch_add(real);
        s_trimDebtBytes.fetch_add(r.peakBytes > r.baseBytes ? r.peakBytes - r.baseBytes : 0);
    }
    const unsigned debtN = s_trimDebtCompiles.load();
    const U64 debtB = s_trimDebtBytes.load();
    if (!debtN) return std::string();   // nothing compiled since the last trim
    if (debtN < kTrimDebtCompiles && debtB < kTrimDebtBytes)
        return llformat("; heap trim deferred (%u compile(s), +%llu MB since the last)", debtN,
                        (unsigned long long)(debtB >> 20));
    if (const unsigned inflight = vkPipelineCompilesInFlight())
        return llformat("; heap trim deferred (%u compiling in another batch)", inflight);
    if (s_trimRunning.exchange(true)) return "; heap trim already running";
    s_trimDebtCompiles.store(0);
    s_trimDebtBytes.store(0);
    if (on_main_thread())
    {
        const char* what = r.what;   // (a static string)
        try
        {
            if (LL::WorkQueue::ptr_t q = LL::WorkQueue::getInstance("General"))
            {
                q->post([what]()
                        {
                            const LLMemoryGovernor::HeapTrim t = LLMemoryGovernor::trimMallocHeap();
                            s_trimRunning.store(false);
                            LL_INFOS("VKPipe") << "pipeline batch (" << what << "): " << heapTrimText(t) << LL_ENDL;
                        });
                return "; heap trim queued (General worker)";
            }
        }
        catch (...)
        {
            // queue closed (shutdown): no trim
        }
        s_trimRunning.store(false);
        return "; heap trim skipped (no General queue)";
    }
    const LLMemoryGovernor::HeapTrim t = LLMemoryGovernor::trimMallocHeap();
    s_trimRunning.store(false);
    return "; " + heapTrimText(t);
#else
    (void)r;
    return std::string();
#endif
}

static void pipelineBatchReport(const VkPipelineBatchReport& r)
{
    SLBench::notePipelineBatch(r);   // #1304: the render benchmark's report lists every batch
    constexpr int64_t MB = 1 << 20;
    const int64_t growth = ((int64_t)r.peakBytes - (int64_t)r.baseBytes) / MB;
    const U64 endBytes = pipelineMemProbe();             // #1103: at the batch end, before any trim
    const std::string trimText = pipelineHeapTrim(r);
    LL_INFOS("VKPipe") << "pipeline batch (" << r.what << "): " << r.total << " pipelines, "
                       << r.compiled << " compiled + " << r.fromCache << " dedup twins; wall "
                       << (S64)r.wallMs << " ms, sum of compiles " << (S64)r.sumMs << " ms; "
                       << r.threads << " threads, at most " << r.maxConcurrency << " at once; committed "
                       << (S64)(r.baseBytes / MB) << " MB -> peak " << (S64)(r.peakBytes / MB) << " MB (+"
                       << growth << " MB); budget min "
                       << (r.budgetMin != UINT64_MAX ? std::to_string(r.budgetMin / MB) + " MB" : std::string("unlimited"))
                       << ", " << r.overBudget << " started over budget, " << (S64)r.waitMs
                       << " ms waiting for budget; at the end " << (S64)(endBytes / MB) << " MB"
                       << (r.pressureCapMin ? "; memory-pressure cap down to " + std::to_string(r.pressureCapMin)
                                            : std::string())
                       // #1411: the driver's own answer (creation feedback), not the learned-cost estimate
                       << "; driver cache feedback: " << r.appHits << " hits, " << r.appMisses << " misses"
                       << (r.noFeedback ? ", " + std::to_string(r.noFeedback) + " without feedback" : std::string())
                       << trimText << LL_ENDL;
    // CACHE GROWTH (the per-pipeline driver blob size behind vk_pipecache.h kMaxBytes): the shared
    // cache's live size after each batch that compiled into it, and what the misses since the last
    // such line added. (Batches overlap: the bytes of a concurrent batch's misses land in whichever
    // line comes first -- sum the lines, not one.)
    if (r.sharedCache && s_pipeSaveDev && s_pipeSaveDev->pipelineCache())
    {
        static std::mutex s_growthMutex;
        static size_t s_lastLive = 0;
        static unsigned s_missesSince = 0;
        std::lock_guard<std::mutex> lk(s_growthMutex);
        const size_t live = vkpipecache::liveBytes(s_pipeSaveDev->device(), s_pipeSaveDev->pipelineCache(), "batch growth");
        s_missesSince += r.appMisses;
        if (live)
        {
            const int64_t added = s_lastLive ? (int64_t)live - (int64_t)s_lastLive : 0;
            LL_INFOS("VKPipe") << "pipeline cache growth (" << r.what << "): live " << live / 1024 << " KB of the "
                               << vkpipecache::kMaxBytes / (1024 * 1024) << " MB cap"
                               << (s_lastLive ? "; +" + std::to_string(added / 1024) + " KB for " + std::to_string(s_missesSince)
                                                    + " driver misses since the last line"
                                                    + (s_missesSince ? " (" + std::to_string(added / 1024 / std::max(1u, s_missesSince)) + " KB each)" : std::string())
                                              : std::string(" (first line this session)"))
                               << "; progress saves skipped by the rate limit so far: " << vkPipelineProgressSavesSkipped() << LL_ENDL;
            s_lastLive = live;
            s_missesSince = 0;
        }
    }
    // MILESTONE SAVE: a batch that compiled something new into the shared cache is persisted at its
    // end, whatever the progress rate limit skipped (a late batch -- a WBOIT set after a spec key
    // change -- otherwise lost its compiles: the exit does not always reach the shutdown save).
    // Bounded by the batch count (~15 a session); an unchanged cache costs one size query. (Every
    // batch with misses, a per-key spec batch too: a size query if nothing went to the shared cache.)
    if (r.appMisses > 0 && s_pipeSaveDev) s_pipeSaveDev->savePipelineCache("batch end");
    if (r.oomRetries || r.failed)
    {
        LL_WARNS("VKPipe") << "pipeline batch (" << r.what << "): #672 " << r.oomRetries
                           << " out-of-memory result(s) retried, " << r.failed
                           << " pipeline(s) given up; concurrency cap now " << r.capEnd << LL_ENDL;
        for (const VkPipelineCompileStat& s : r.items)
            if (s.failed && !s.skipped)
                LL_WARNS("VKPipe") << "  given up after " << s.attempts << " attempts: " << s.name << LL_ENDL;
    }
    if (r.skipped)   // #1651: optional pipelines too heavy for this machine (their fallback draws)
        LL_INFOS("VKPipe") << "pipeline batch (" << r.what << "): #1651 " << r.skipped
                           << " optional pipeline(s) skipped as too heavy to compile here; the generic ones draw instead"
                           << LL_ENDL;

    std::vector<const VkPipelineCompileStat*> byTime, byMem;
    for (const VkPipelineCompileStat& s : r.items) { byTime.push_back(&s); byMem.push_back(&s); }
    std::sort(byTime.begin(), byTime.end(),
              [](const VkPipelineCompileStat* a, const VkPipelineCompileStat* b) { return a->ms > b->ms; });
    std::sort(byMem.begin(), byMem.end(),
              [](const VkPipelineCompileStat* a, const VkPipelineCompileStat* b) { return a->attribBytes > b->attribBytes; });
    auto line = [&](const char* what, size_t i, const VkPipelineCompileStat& s)
    {
        LL_INFOS("VKPipe") << llformat("  %s %2d: %8.1f ms %6lld MB (raw %lld MB, est %lld MB) c%u %u words%s%s %s",
                                       what, (int)i + 1, s.ms, (long long)(s.attribBytes / MB),
                                       (long long)(s.rawBytes / MB), (long long)(s.estBytes / MB), s.concurrency,
                                       s.spirvWords, s.hit ? " hit" : "", s.rest ? " twin" : "", s.name.c_str())
                           << LL_ENDL;
    };
    for (size_t i = 0; i < byTime.size() && i < 3; ++i) line("time", i, *byTime[i]);
    if (r.peakBytes)
        for (size_t i = 0; i < byMem.size() && i < 3; ++i) line("mem ", i, *byMem[i]);
    // Snapshot for bug reports: the batch that compiled the most (the world batch).
    std::string text = llformat("%s %d/%d %.1fs +%lldMB t%u c%u ob%u:", r.what, (int)r.total, (int)r.compiled,
                                r.wallMs / 1000.0, (long long)growth, r.threads, r.maxConcurrency, r.overBudget);
    for (size_t i = 0; i < byTime.size() && i < 4; ++i)   // fits the 200-char report field
    {
        const VkPipelineCompileStat& s = *byTime[i];
        std::string name = s.name;
        // "LIT_ALPHA (Vert/FragRt)" -> "LIT_ALPHA/FragRt" (the frag tells the variants apart)
        const size_t open = name.find(" ("), slash = name.rfind('/');
        if (open != std::string::npos && slash != std::string::npos && slash > open)
            name = name.substr(0, open) + name.substr(slash, name.size() - slash - (name.back() == ')' ? 1 : 0));
        text += llformat(" %s %.1fs %lldM c%u;", name.c_str(), s.ms / 1000.0, (long long)(s.attribBytes / MB),
                         s.concurrency);
    }
    {
        std::lock_guard<std::mutex> lk(s_pipeReportMutex);
        if (r.sumMs >= s_pipeReportSumMs)
        {
            s_pipeReportSumMs = r.sumMs;
            s_pipeReportText = text;
        }
    }
    // Persist what this batch taught the scheduler.
    if (s_pipeSaveDev) vkpipecache::saveCosts(s_pipeSaveDev->physicalDevice(), s_pipeCostPath, "batch done");
}

// static
static std::string s_pipeCapNote;               // #876 (guarded by s_pipeReportMutex)
static std::atomic<bool> s_driverIntelWindows{false};

std::string LLVulkanContext::pipelineCompileText()
{
    std::lock_guard<std::mutex> lk(s_pipeReportMutex);
    if (s_pipeCapNote.empty()) return s_pipeReportText;
    return s_pipeReportText.empty() ? s_pipeCapNote : s_pipeCapNote + "; " + s_pipeReportText;
}

bool LLVulkanContext::driverIsIntelWindows() { return s_driverIntelWindows.load(); }

std::string LLVulkanContext::pipelineSchedulerText() { return vkPipelineSchedulerText(); }

// #1064 (guarded by s_pipeReportMutex): the driver identity the compile-crash quarantine is keyed by.
static std::string s_driverIdToken;
static std::string s_driverIdLabel;

std::string LLVulkanContext::driverIdentityToken()
{
    std::lock_guard<std::mutex> lk(s_pipeReportMutex);
    return s_driverIdToken;
}

std::string LLVulkanContext::driverIdentityLabel()
{
    std::lock_guard<std::mutex> lk(s_pipeReportMutex);
    return s_driverIdLabel;
}

void LLVulkanContext::setPipelineCompileCapNote(const std::string& note)
{
    std::lock_guard<std::mutex> lk(s_pipeReportMutex);
    s_pipeCapNote = note;
}

namespace { const char* vkPresentModeName(VkPresentModeKHR m); }   // (defined with displayInfoText)

// The swapchain's present mode / image count / frames in flight into the frame timeline
// (LLVKHitch, the HITCH timeline line's header) and, on a recreate that changed one of
// them, one log line: frame-pacing reports need the mode the frames were presented with,
// and vkgfx's own messages go to stderr only. Resizes keep the mode and count, so a
// drag-resize does not spam the log.
static void noteSwapchainMode(const VkSwapchain& sc, bool recreated)
{
    static VkPresentModeKHR s_mode = VK_PRESENT_MODE_MAX_ENUM_KHR;
    static uint32_t s_images = 0, s_in_flight = 0;
    const VkPresentModeKHR mode = sc.presentMode();
    char letter = '?';
    switch (mode)
    {
    case VK_PRESENT_MODE_FIFO_KHR: letter = 'F'; break;
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: letter = 'R'; break;
    case VK_PRESENT_MODE_MAILBOX_KHR: letter = 'M'; break;
    case VK_PRESENT_MODE_IMMEDIATE_KHR: letter = 'I'; break;
    default: break;
    }
    LLVKHitch::timelineSwapchain(vkPresentModeName(mode), letter, sc.imageCount(), sc.framesInFlight());
    if (recreated && (mode != s_mode || sc.imageCount() != s_images || sc.framesInFlight() != s_in_flight))
    {
        LL_INFOS("Vulkan") << "Swapchain recreated: " << sc.extent().width << "x" << sc.extent().height
                           << ", present mode " << vkPresentModeName(mode) << " (vsync " << (VkSwapchain::sVSync ? "on" : "off")
                           << ", RenderVKNoVsyncPresentMode " << VkSwapchain::sNoVsyncMode << "), " << sc.imageCount()
                           << " images, " << sc.framesInFlight() << " frames in flight" << LL_ENDL;
    }
    s_mode = mode; s_images = sc.imageCount(); s_in_flight = sc.framesInFlight();
}

// #1548: the rebuilt renderer against the lost one. Each part counts as back once it reaches the lost renderer's
// count (with a small allowance: the scene moves on meanwhile -- objects leave, textures are evicted); its time
// since the loss is kept. One "scene back" line when every part is back, a progress line every 5 s until then,
// and a "NOT back" line naming the gaps after kRecoverySceneGiveUpSec.
struct LLVulkanContext::RecoveryScene
{
    bool haveTarget = false;
    LLCleanRendererVK::SceneCounts target;
    double lossAt = 0.0;          // LLFrameTimer::getElapsedSeconds() at the start of the recovery
    bool rtRequested = false;     // the new device has ray tracing: the RT part is waited for
    std::vector<uint32_t> seedDemand;   // the lost renderer's last texel demand per material (moved into the new one)
    std::vector<uint8_t>  seedSeen;
    double nextPoll = 0.0, nextProgress = 0.0;
    double instAt = -1.0, meshAt = -1.0, texAt = -1.0, rtAt = -1.0;   // seconds after the loss (-1: not yet)
};

// The device's requests -- the same at startup and for a GPU-loss recovery's device (#1550: every feature comes back
// as it was; the same extension and feature set also keeps the pipeline cache warm). RT extensions only when
// requested. The optional HDR metadata extension is requested even when HDR starts disabled, so a live preference
// toggle does not need a new VkDevice.
void LLVulkanContext::applyDeviceRequests(VkDeviceContext& d)
{
    d.requestOptionalExtension(VK_EXT_HDR_METADATA_EXTENSION_NAME);
    d.setRayTracingRequested(mRayTracingRequested);
    d.setClusterRequested(mClusterRequested);
    d.setPipelineStatsRequested(mPipelineStatsRequested);
    d.setShaderClockRequested(mShaderClockRequested);
    d.setClusterDeviceFeatures(mClusterDeviceFeatures);
    d.setAsyncComputeMode(mAsyncComputeMode);
    d.setAsyncComputeAuto(mAsyncComputeAuto);
    d.setDebugRobustness(mDebugRobustness);
    d.setPipelineCacheFile(mPipeCachePath, mPipeCacheTag);
    // #1544 (D8, hidden; for testing): frame generation's present thread on the renderer's queue, as on a GPU
    // with one graphics queue, also where the family has a queue to spare
    d.setFrameGenSharedQueue(gSavedSettings.getBOOL("RenderVKFrameGenSharedQueue"));
}

// #870 / #1517: the DLSS add-on's device extensions (NVIDIA only, the late provider) and frame generation's (each
// on its own: a missing one never costs DLSS). Only when the add-on loaded before the instance.
void LLVulkanContext::installAddonDeviceProviders(VkDeviceContext& d)
{
    if (!LLVKNeuralRecon::instance().loaded()) return;
    d.setLateExtensionProvider(
        [](VkInstance inst, VkPhysicalDevice pd, std::vector<std::string>& exts, std::string& why)
        { return LLVKNeuralRecon::instance().deviceExtensions(inst, pd, exts, why); });
    d.setOptionalExtensionProvider(
        [](VkInstance inst, VkPhysicalDevice pd, std::vector<std::string>& exts)
        { return LLVKNeuralRecon::instance().fgDeviceExtensions(inst, pd, exts); });
}

// After vkCreateDevice: #870 / #962 add-on init + per-feature capability query (logs the "DLSS:" status line), then
// #1517 frame generation through the add-on just initialised on the device.
void LLVulkanContext::initAddonAndFrameGen(VkDeviceContext& d)
{
    if (LLVKNeuralRecon::instance().loaded())
        LLVKNeuralRecon::instance().initDevice(d.instance(), d.physicalDevice(), d.device(), d.lateExtensionsEnabled(),
                                               d.lateExtensionsWhy());
    LLVKFrameGen::instance().initDirect(d);
}

bool LLVulkanContext::init(SDL_Window* window, uint32_t width, uint32_t height,
                           bool enableValidation)
{
    vkenum::setLogSink(gVkFaultLogSink); // named WSI retries also reach Slipstream.log

    if (mInitialized)
    {
        return true;
    }
    // A non-null implementation after init failure means shutdown could not prove
    // device-wide quiescence (lost device, failed idle, or an in-driver compile). Its
    // context-owned texture-prep service and mapped staging remain charged to that
    // exact VkDeviceContext. Never overwrite it with a second service/device instance.
    if (mImpl)
    {
        LL_WARNS("Vulkan") << "init refused: prior device context is retained pending safe teardown" << LL_ENDL;
        return false;
    }
    if (!window)
    {
        LL_WARNS("Vulkan") << "init: null SDL window" << LL_ENDL;
        return false;
    }
    vkDriverThreadName("main");   // #1546: named in the "threads inside driver calls" line at a loss

    mImpl = new Impl();
    mImpl->mWindow = window;
    mImpl->mWidth  = width;
    mImpl->mHeight = height;

    // 1) Instance extensions SDL needs for a surface on this platform.
    Uint32 sdlExtCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&sdlExtCount);
    if (!sdlExts)
    {
        LL_WARNS("Vulkan") << "SDL_Vulkan_GetInstanceExtensions failed: "
                           << SDL_GetError() << LL_ENDL;
        delete mImpl; mImpl = nullptr;
        return false;
    }
    std::vector<const char*> instExts(sdlExts, sdlExts + sdlExtCount);
    // Enumerate once into bounded stack storage, including when the preference is
    // off: enabling this inert instance extension allows a later toggle without a
    // restart. No optional image, shader or pipeline is created on the SDR path.
    {
        const auto properties = vkenum::instanceExtensions();
        mDisplayColourSpaceExtension = false;
        for (const auto& property : properties)
            if (std::strcmp(property.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0)
                mDisplayColourSpaceExtension = true;
        if (mDisplayColourSpaceExtension)
        {
            bool present = false;
            for (const char* ext : instExts)
                if (std::strcmp(ext, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME) == 0) present = true;
            if (!present) instExts.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
        }
    }
    for (Uint32 i = 0; i < sdlExtCount; ++i)
    {
        LL_INFOS("Vulkan") << "SDL requires instance extension: " << sdlExts[i]
                           << LL_ENDL;
    }

#if LL_WINDOWS
    // #919 full-screen exclusive: VK_EXT_full_screen_exclusive (device) needs
    // VK_KHR_get_surface_capabilities2 (instance). Both optional: requested only where the
    // loader lists the instance extension; the device enables the other where listed.
    {
        auto props = vkenum::instanceExtensions();
        [[maybe_unused]] uint32_t n = uint32_t(props.size());
        bool caps2 = false;
        for (uint32_t i = 0; i < n; ++i)
            if (std::strcmp(props[i].extensionName, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0) caps2 = true;
        if (caps2)
        {
            bool dup = false;
            for (const char* e : instExts)
                if (std::strcmp(e, VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME) == 0) dup = true;
            if (!dup) instExts.push_back(VK_KHR_GET_SURFACE_CAPABILITIES_2_EXTENSION_NAME);
            mImpl->mDevice.requestOptionalExtension(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
        }
        else
        {
            LL_INFOS("Vulkan") << "Fullscreen: VK_KHR_get_surface_capabilities2 not available: no exclusive fullscreen" << LL_ENDL;
        }
    }
#endif

    // #870: the optional neural reconstruction library (DLSS RR via the user-installed slip_nr
    // shim) is loaded BEFORE the instance: its instance extensions (only those the loader
    // lists; all or nothing) and, after the GPU pick, its device extensions (NVIDIA only, the
    // late provider). Missing / failing = RR unavailable with a reason, never fatal.
    {
        LLVKNeuralRecon& nr = LLVKNeuralRecon::instance();
        LLVKNeuralRecon::Config nc;
        nc.enabled = mNrEnabled;
        nc.configuredPath = mNrShimPath;
        nc.viewerLibDir = mNrLibDir;
        nc.defaultDir = mNrDefaultDir;
        nc.dataDir = mNrDataDir;
        nc.engineVersion = mNrEngineVersion;
        if (nr.load(nc))
        {
            auto props = vkenum::instanceExtensions();
            [[maybe_unused]] uint32_t n = uint32_t(props.size());
            std::string missing;
            for (const std::string& want : nr.instanceExtensions())
            {
                bool listed = false;
                for (uint32_t i = 0; i < n; ++i)
                    if (want == props[i].extensionName) listed = true;
                if (!listed) missing += (missing.empty() ? "" : ", ") + want;
            }
            if (!missing.empty())
                nr.instanceExtensionsMissing(missing);
            else
                for (const std::string& want : nr.instanceExtensions())
                {
                    bool dup = false;
                    for (const char* e : instExts)
                        if (want == e) dup = true;
                    if (!dup) instExts.push_back(want.c_str());   // (the strings live in the singleton)
                }
            installAddonDeviceProviders(mImpl->mDevice);
        }
    }

    // #1073 VR (startup only): the OpenXR probe succeeded, so the runtime creates the instance
    // and the device from the same requests (XR_KHR_vulkan_enable2) on the GPU it requires. On
    // the desktop LLXrSession::active() is false: no hook, creation exactly as without VR.
    if (LLXrSession::active())
    {
        LLXrSession::instance().installDeviceHooks(mImpl->mDevice);
        mXrHooked = true;
    }

    // 2) Instance + device (with VK_KHR_swapchain). RT extensions only when requested.
    // Request the optional metadata extension even when HDR starts disabled so a
    // live preference toggle does not require rebuilding the VkDevice.
    applyDeviceRequests(mImpl->mDevice);
    // #526: the requirements report (every device, what it lacks, the choice) goes to the
    // log; on failure it also becomes the message box text (shutdown() clears it).
    auto failWith = [this](const char* step, const std::string& why)
    {
        VkRequirementsReport r = mImpl->mDevice.requirementsReport();
        if (!why.empty()) r.failure = why;
        LL_WARNS("Vulkan") << step << " failed: " << (r.failure.empty() ? std::string("?") : r.failure) << LL_ENDL;
        mInitFailureMessage = vkRequirementsUserMessage(r);
        shutdown();
        return false;
    };
    const bool devOk = mImpl->mDevice.initForPresent(enableValidation, instExts, /*wantSwapchain=*/true);
    for (const std::string& l : vkRequirementsLogLines(mImpl->mDevice.requirementsReport()))
    {
        if (devOk)
            LL_INFOS("Vulkan") << l << LL_ENDL;
        else
            LL_WARNS("Vulkan") << l << LL_ENDL;
    }
    if (!devOk)
    {
        return failWith("VkDeviceContext::initForPresent", std::string());
    }
    {
        // #1411: what the device was created with (the compile worker must create the same one: its
        // log says "device matches" / "device DIFFERS" against this).
        std::string fp = mImpl->mDevice.deviceFingerprintText();
        std::replace(fp.begin(), fp.end(), '\n', ';');
        LL_INFOS("VKPipe") << llformat("device fingerprint %016llx: ", (unsigned long long)mImpl->mDevice.deviceFingerprint())
                           << fp << LL_ENDL;
    }
    // The context owns the one preparation service shared by world and UI uploads. Its
    // worker pool starts lazily on the first admitted source, so startup allocates no
    // worker threads or image staging when the feature is unused.
    try
    {
        mImpl->mTexturePrepService.reset(new (std::nothrow) LLVKTexturePrepService(&mImpl->mDevice));
    }
    catch (...)
    {
        mImpl->mTexturePrepService.reset();
    }
    if (!mImpl->mTexturePrepService)
        LL_WARNS("Vulkan") << "Unified texture preparation unavailable; texture upload callers must defer without synchronous fallback" << LL_ENDL;
    // #870 / #962: the add-on's init + per-feature capability query (logs the "DLSS:" status line); #1517 frame
    // generation through it. (A recovery's device: initAddonAndFrameGen, the same two calls.)
    if (LLVKNeuralRecon::instance().loaded())
        LLVKNeuralRecon::instance().initDevice(mImpl->mDevice.instance(), mImpl->mDevice.physicalDevice(),
                                               mImpl->mDevice.device(), mImpl->mDevice.lateExtensionsEnabled(),
                                               mImpl->mDevice.lateExtensionsWhy());
    LLVKFrameGen::instance().initDirect(mImpl->mDevice);
    // #1073 VR: the session on this device (graphics queue, index 0). A refusal leaves the
    // desktop running on the device (LLXrSession logs why; the viewer shows the notice).
    if (mXrHooked && LLXrSession::active())
    {
        LLXrSession& xr = LLXrSession::instance();
        if (xr.createSession(mImpl->mDevice))
        {
            mXrSession = true;
            xr.setRunningCallback([this](bool running) { onXrRunningChanged(running); });
            LL_INFOS("VR") << "device created by the OpenXR runtime; multiview "
                           << (mImpl->mDevice.multiviewEnabled() ? "enabled" : "not supported") << LL_ENDL;
        }
    }
    // Device limits for every vkdCreatePipelineLayout check (UI, RT, world), logged once
    // incl. maxPushConstantsSize (#238) -- before the first pipeline layout exists.
    vkdInitDeviceLimits(mImpl->mDevice.physicalDevice());
    // #695: the memory layout and the VRAM budget mode chosen (UMA carve-out + shared RAM,
    // or the main device-local heap) -- what a user report needs to tell what happened.
    LL_INFOS("VKMem") << "Memory layout: " << mImpl->mDevice.memLayoutText() << LL_ENDL;

    // #462 / #471: how pipelines compile. AMD's Windows compiler can take GBs per large
    // world-FS variant; (hardware threads - 1) parallel compiles -- 23 on a 12-core Ryzen --
    // ran a 32 GB machine out of commit at the login screen. The batch scheduler
    // (vk_pipeline.h) admits compiles against the LIVE memory headroom (pipelineMemBudget)
    // using each pipeline's learned cost, or a per-driver default for one never measured:
    // 3 GB on AMD's proprietary driver (conservative until learned), 512 MB elsewhere.
    // Threads: hardware threads - 1, hard-capped by RenderVKPipelineCompileThreads.
    {
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &drv;
        vkGetPhysicalDeviceProperties2(mImpl->mDevice.physicalDevice(), &p2);
        const bool amdProprietary = (drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY);
        s_driverIntelWindows = (drv.driverID == VK_DRIVER_ID_INTEL_PROPRIETARY_WINDOWS);   // #876 (the cap is decided in llappviewer)
        {
            // #1064: "<vendorID>-<driverID>-<driverVersion>" (hex, decimal, hex): no spaces, so the
            // crash handler writes it verbatim and the marker / quarantine files compare it as text.
            char tok[64];
            std::snprintf(tok, sizeof(tok), "%04x-%u-%08x", (unsigned)p2.properties.vendorID, (unsigned)drv.driverID,
                          (unsigned)p2.properties.driverVersion);
            std::string label = drv.driverName;
            if (drv.driverInfo[0]) label += std::string(" ") + drv.driverInfo;
            std::lock_guard<std::mutex> lk(s_pipeReportMutex);
            s_driverIdToken = tok;
            s_driverIdLabel = label;
        }
        const char* rdnaRule = vkcompilegovernor::rule(p2.properties.vendorID, p2.properties.deviceID,
            vkcompilegovernor::kWindows, drv.driverInfo, gSavedSettings.getBOOL("VKDebugForceRDNACompileGovernor"));
        vkSetPipelineRdnaGovernor(rdnaRule != nullptr);
        if (rdnaRule)
            LL_INFOS("VKPipe") << "RDNA1/2 compile governor: heavy pipelines 8 GB each, 1 at a time until measured; rule: "
                               << rdnaRule << "; vendor/device " << p2.properties.vendorID << "/" << p2.properties.deviceID
                               << "; allocation plateau 2000 ms; Windows heavy calls serialized across viewer/worker processes"
                               << "; critical results discarded/requeued after driver return"
                               << LL_ENDL;
        U64 defaultCost = amdProprietary ? (3072ull << 20) : (512ull << 20);
        vkSetPipelineCompileThreads(0);   // auto; RenderVKPipelineCompileThreads (llappviewer) may cap it
        const bool warm = mImpl->mDevice.pipelineCacheWarm();
        vkSetPipelineCacheWarm(warm);
        s_pipeCostPath = mPipeCostPath;
        const size_t learned = vkpipecache::loadCosts(mImpl->mDevice.physicalDevice(), s_pipeCostPath);
        // #1103: Mesa's compilers (ANV / RADV / NVK) measured heavier than the 512 MB default
        // on the reporter's machine. A pipeline never measured on this device is assumed to
        // cost what 90% of the measured ones stay under, when that is more (at most 2 GB).
        const bool mesa = drv.driverID == VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA || drv.driverID == VK_DRIVER_ID_MESA_RADV
                       || drv.driverID == VK_DRIVER_ID_MESA_NVK || drv.driverID == VK_DRIVER_ID_MESA_LLVMPIPE;
        const U64 learnedP90 = vkPipelineLearnedColdBytes(90);
        if (mesa && learnedP90 > defaultCost) defaultCost = std::min<U64>(learnedP90, 2048ull << 20);
        vkSetPipelineDefaultCost(defaultCost);
        const unsigned hw = std::max(1u, std::thread::hardware_concurrency());
        // #672: AMD's proprietary compiler ran out of host memory mid cold-compile (#667 /
        // #669: every launch crashed, 11 workers, budget ~25 GB on 32 GB). A margin on top of
        // the system reserve, and on a COLD cache a low starting concurrency (2 with nothing
        // learned, 4 with learned costs) that ramps up as the measured costs show headroom.
        s_pipeDriverMargin.store(amdProprietary ? (2048ull << 20) : 0, std::memory_order_relaxed);
        // #1411: the cap bounds the HEAVY lane only (cheap pipelines never wait behind it) and
        // starts narrower (1 heavy compile with nothing learned on this device, 2 with learned
        // costs: the 6600 XT's RSS grew ~3-4 GB per heavy compile). A loaded cache file is
        // "warm" only until a batch finds it covers < 80% of its pipelines (vk_pipeline.h): the
        // cap is armed then too (vkSetPipelineColdStartCap).
        const unsigned coldCap = amdProprietary && !rdnaRule ? (learned ? 2u : 1u) : 0u;
        const unsigned startCap = warm ? 0u : coldCap;
        vkSetPipelineConcurrencyStart(startCap);
        vkSetPipelineColdStartCap(coldCap);
        const U64 budget = pipelineMemBudget();
        // Compiles in flight may hold at most a quarter of physical RAM, whatever the live
        // headroom (vkSetPipelineMemCap): the headroom alone let them take nearly all of it.
        U64 cap = 0;
        {
            LLMemoryGovernor::SystemMemory sm;
            if (LLMemoryGovernor::readSystemMemory(sm) && sm.total) cap = sm.total / 4;
        }
        vkSetPipelineMemCap(cap);
        LL_INFOS("Vulkan") << "Pipeline compiles: memory-budgeted, up to " << std::max(1u, hw > 2 ? hw - 1 : hw)
                           << " at once (driver " << drv.driverName << (amdProprietary ? ", AMD proprietary" : "")
                           << "; " << (defaultCost >> 20) << " MB assumed per unknown pipeline"
                           << (mesa ? " (Mesa: at least the learned 90th percentile)" : "") << ", " << learned
                           << " learned (90th percentile " << (learnedP90 >> 20) << " MB); compile budget now "
                           << (budget == UINT64_MAX ? std::string("unlimited") : std::to_string(budget >> 20) + " MB")
                           << " (available memory, less the reserve"
                           << (amdProprietary ? " + 2048 MB driver margin" : "") << ")"
                           << "; pipeline cache " << (warm ? "warm" : "cold")
                           << "; compiles in flight capped at "
                           << (cap ? std::to_string(cap >> 20) + " MB (25% of RAM)" : std::string("no cap (RAM unknown)"))
                           << "; memory pressure caps concurrency at 2 (tight) / 1 (critical)"
                           << "; malloc arenas " << LLMemoryGovernor::mallocArenaText() << ")" << LL_ENDL;
        if (startCap)
            LL_INFOS("VKPipe") << "#672: cold pipeline cache on AMD's proprietary driver: starting at " << startCap
                               << " heavy compile(s) at once (cheap ones run beside them), ramping up as measured"
                                  " costs show headroom" << LL_ENDL;
        // #672 / #1411: the cache (+ learned costs) is saved every 4 compiles, every 5 s while
        // compiles finish, and at once after every heavy compile -- whatever the cache file
        // said ("warm" files that were nearly empty never saved: the skipped saves were all
        // hits). A save with nothing new costs one size query.
        vkSetPipelineProgressInterval(5000u);
        s_pipeProgressEvery = 4u;
        // (and never more often than every 30 s: each save rewrites the whole file, up to the
        // 1 GB cap -- the owner's log shows 17 rewrites of 260 MB in 11 s; the milestone saves
        // and the out-of-memory retry's save are not limited)
        vkSetPipelineProgressMinGap(30000u);
        logCompileCapabilities(mImpl->mDevice.physicalDevice(), drv);
    }
    s_pipeSaveDev = &mImpl->mDevice;
    vkSetPipelineProgressHook(&pipelineProgressSave, s_pipeProgressEvery);
    vkSetPipelineMemProbe(&pipelineMemProbe);         // #471 per-pipeline measurement
    vkSetPipelineBatchHook(&pipelineBatchReport);
    vkSetPipelineMemBudget(&pipelineMemBudget);       // #471 memory-aware scheduling
    vkSetPipelineEventHook(&pipelineEvent);           // #672 out-of-memory recovery log
    {
        // #1411 P5: where the out-of-process compile worker is used (AMD proprietary, or
        // SL_VK_PIPELINE_WORKER=1), start it NOW from the previous session's saved recipe (respun to
        // this build's SPIR-V): the login screen's seconds become compile time. The renderer adopts it.
        VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &drv;
        vkGetPhysicalDeviceProperties2(mImpl->mDevice.physicalDevice(), &p2);
        bool want = drv.driverID == VK_DRIVER_ID_AMD_PROPRIETARY;
        if (const char* e = std::getenv("SL_VK_PIPELINE_WORKER")) want = e[0] == '1';
        if (want) vkPipelineEarlyWorkerStart(mImpl->mDevice.pipelineCacheFile(), mImpl->mDevice.rayTracingEnabled());
    }
    // RenderVKPipelineStats: every pipeline from here on logs its executables' statistics
    // ("VKPipeStats: <name> (<vert module>/<frag module>) [stage ...]; stat=value; ...").
    if (mPipelineStatsRequested)
    {
        if (mImpl->mDevice.pipelineStatsEnabled())
            LL_INFOS("VKPipe") << "pipeline statistics ON (RenderVKPipelineStats; VK_KHR_pipeline_executable_properties): "
                               << mImpl->mDevice.shaderCoreSummary() << LL_ENDL;
        else
            LL_WARNS("VKPipe") << "RenderVKPipelineStats: VK_KHR_pipeline_executable_properties (pipelineExecutableInfo)"
                                  " not available on this device: no pipeline statistics" << LL_ENDL;
    }

    // 3) Surface from the SDL_WINDOW_VULKAN window.
    if (!SDL_Vulkan_CreateSurface(window, mImpl->mDevice.instance(), nullptr,
                                  &mImpl->mSurface))
    {
        return failWith("SDL_Vulkan_CreateSurface", std::string("window surface: ") + SDL_GetError());
    }

#if LL_WINDOWS
    // #919 full-screen exclusive entry points (all four, or none of it).
    if (mImpl->mDevice.optionalExtensionEnabled(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME))
    {
        Impl& m = *mImpl;
        m.mFseCaps2 = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
            vkGetInstanceProcAddr(m.mDevice.instance(), "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
        m.mFseModes2 = (PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT)
            vkGetInstanceProcAddr(m.mDevice.instance(), "vkGetPhysicalDeviceSurfacePresentModes2EXT");
        m.mFseAcquire = (PFN_vkAcquireFullScreenExclusiveModeEXT)
            vkGetDeviceProcAddr(m.mDevice.device(), "vkAcquireFullScreenExclusiveModeEXT");
        m.mFseRelease = (PFN_vkReleaseFullScreenExclusiveModeEXT)
            vkGetDeviceProcAddr(m.mDevice.device(), "vkReleaseFullScreenExclusiveModeEXT");
        m.mFseExt = m.mFseCaps2 && m.mFseAcquire && m.mFseRelease;
        Impl* impl = mImpl;
        m.mSwapchain.setPresentModeQuery([impl](std::vector<VkPresentModeKHR>& out) { return impl->fsePresentModes(out); });
        LL_INFOS("Vulkan") << "Fullscreen: VK_EXT_full_screen_exclusive "
                           << (m.mFseExt ? "enabled" : "enabled but its entry points are missing: not used")
                           << "; exclusive on this monitor: " << (m.mFseExt && m.fseSurfaceSupported() ? "supported" : "not supported")
                           << LL_ENDL;
    }
    else
    {
        LL_INFOS("Vulkan") << "Fullscreen: VK_EXT_full_screen_exclusive not available on this device" << LL_ENDL;
    }
#endif

    // 4) Swapchain.
    // The startup preference is installed before a window/display exists.
    sDisplayCalibrationQueryPending = true;
    ++sDisplayCalibrationGeneration;
    setDisplayHdr(mDisplayHdrRequested, gSavedSettings.getF32("RenderVKDisplayPaperWhiteNits"),
                  gSavedSettings.getF32("RenderVKDisplayPeakNits"));
    refreshDisplayHdrPolicy();
    // #1517 Reflex (frame generation): the main swapchain in low latency mode where Reflex can run.
    mImpl->mSwapchain.setLatencyMode(LLVKFrameGen::instance().reflexAvailable());
    if (!mImpl->mSwapchain.init(mImpl->mDevice, mImpl->mSurface, width, height))
    {
        return failWith("swapchain init", "swapchain: the chosen GPU can't present to this window "
                                          "(no present support or zero extent)");
    }
    LLVKFrameGen::instance().setReflexSwapchain(mImpl->mSwapchain.handle(), mImpl->mSwapchain.latencyMode(),
                                                mImpl->mSwapchain.frameGenImages() != 0,
                                                mImpl->mSwapchain.ignoresVsync());
    mImpl->logFrameGenChain();
    // #1517: while frame generation's present thread exists, every device wait is made without the present queue
    // (that thread's own): the other queues idle and the present queue's accepted work complete (deviceIdleWait).
    vkSetDeviceIdleWait([](void* impl, VkResult* result) { return static_cast<Impl*>(impl)->deviceIdleWait(result); }, mImpl);
    mImpl->mOutputColorSpace = mImpl->mSwapchain.colorSpace();
    mImpl->mNeedsRecreate = false; // initial creation consumed the requested display policy

    // 5) One primary command buffer per frame-in-flight slot (see mCmds comment).
    const uint32_t framesInFlight = mImpl->mSwapchain.framesInFlight();
    mImpl->mCmds.resize(framesInFlight, VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = mImpl->mDevice.commandPool();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = framesInFlight;
    VK_CHECK(vkAllocateCommandBuffers(mImpl->mDevice.device(), &ai,
                                      mImpl->mCmds.data()));

    mInitialized = true;
    LL_INFOS("Vulkan") << "RT: requested=" << mRayTracingRequested
                       << " supported=" << mImpl->mDevice.rayQuerySupported()
                       << " enabled=" << mImpl->mDevice.rayTracingEnabled()
                       << " (" << mImpl->mDevice.rayTracingStatus() << ")" << LL_ENDL;
    // #743: one line, every cluster LOD capability and the tier it gets on this device.
    LL_INFOS("Vulkan") << LLCleanClusterCaps::detect(mImpl->mDevice, mClusterForceTier).logLine() << LL_ENDL;
    // One line: how the async queue was decided (RenderVKAsyncCompute -1 = per-GPU auto).
    LL_INFOS("Vulkan") << "VKAsync: " << mImpl->mDevice.asyncComputeDecision() << LL_ENDL;
    LL_INFOS("Vulkan") << "Async compute queue (RenderVKAsyncComputeQueue=" << mAsyncComputeMode << "): "
                       << mImpl->mDevice.asyncComputeStatus() << LL_ENDL;
    LL_INFOS("Vulkan") << "Texture pools (RenderVKTextureSuballoc, #411): " << mImpl->mDevice.texturePoolStatus() << LL_ENDL;
    LL_INFOS("Vulkan") << "Vulkan context initialized: swapchain "
                       << mImpl->mSwapchain.extent().width << "x"
                       << mImpl->mSwapchain.extent().height << ", "
                       << mImpl->mSwapchain.imageCount() << " images, format "
                       << (int)mImpl->mSwapchain.format() << ", present mode "
                       << vkPresentModeName(mImpl->mSwapchain.presentMode()) << ", "
                       << mImpl->mSwapchain.framesInFlight() << " frames in flight" << LL_ENDL;
    noteSwapchainMode(mImpl->mSwapchain, false);
    return true;
}

void LLVulkanContext::setVSync(bool on)
{
    mUserVSync = on;
    if (mXrPresentOverride) return;   // #1073: a VR session runs; applied when it stops
    if (VkSwapchain::sVSync == on) return;
    VkSwapchain::sVSync = on;
    if (mImpl)
    {
        mImpl->mNeedsRecreate = true;
        mImpl->mOutputPreviewValid = false;
    }
}

// #1073 VR: while the session runs, xrWaitFrame paces the viewer and the window is a mirror: a
// FIFO present would cap the headset at the monitor's rate, so the window goes MAILBOX /
// IMMEDIATE (RenderVKNoVsyncPresentMode). The user's vsync comes back when the session stops.
void LLVulkanContext::onXrRunningChanged(bool running)
{
    mXrPresentOverride = running;
    const bool want = running ? false : mUserVSync;
    LL_INFOS("VR") << (running ? "session running: the window presents without vsync (it must never block the headset)"
                               : "session stopped: the window presents with the user's vsync setting again")
                   << LL_ENDL;
    if (mImpl)
    {
        // #1074: nor may its acquire: a short timeout skips the window's frame instead.
        mImpl->mSwapchain.setAcquireTimeout(running ? 2000000ull : UINT64_MAX);
        // The eye-size world target is only used while the session runs (called between
        // frames: waits for the frames in flight before freeing it).
        if (!running) mImpl->vrReleaseTarget(true);
    }
    if (mImpl) onDisplayChanged(); // HDR/SDR consumer policy also changes with XR.
    if (VkSwapchain::sVSync == want) return;
    VkSwapchain::sVSync = want;
    if (mImpl) mImpl->mNeedsRecreate = true;
}

void LLVulkanContext::refreshDisplayHdrPolicy()
{
    if (!mImpl) return;
    vkdisplay::Capability capability;
    capability.colourSpaceExtension = mDisplayColourSpaceExtension;
    const SDL_DisplayID display = SDL_GetDisplayForWindow(mImpl->mWindow);
    capability.displayAvailable = display != 0;
    capability.displayId = display;
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver && std::strcmp(driver, "windows") == 0)
    {
        capability.platform = vkdisplay::Platform::Windows;
        capability.osHdrEnabled = SDL_GetBooleanProperty(SDL_GetDisplayProperties(display),
                                                         SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
    }
    else if (driver && std::strcmp(driver, "wayland") == 0) capability.platform = vkdisplay::Platform::Wayland;
    else if (driver && std::strcmp(driver, "x11") == 0) capability.platform = vkdisplay::Platform::X11;
    // The renderer initializes its display-linear variants before any world frame.
    // Resource preparation is completed transactionally after swapchain creation,
    // before acquisition/presentation; failure retries with the SDR surface pair.
    // Main-only XR and video replay currently consume SDR byte images. Do not
    // hand them PQ/scRGB bytes until those consumers have their own conversion.
    const bool sdrConsumer = mXrPresentOverride || gSavedSettings.getBOOL("SLInstantReplay");
    if (mDisplayHdrRequested && sdrConsumer)
        LL_INFOS("Vulkan") << "HDR display deferred: active XR or instant replay requires SDR output" << LL_ENDL;
    mImpl->mSwapchain.setDisplayPolicy({mDisplayHdrRequested && !sdrConsumer, mDisplayPaperWhiteNits, mDisplayPeakNits},
                                     capability, mDisplayHdrPipelineReady);
}

void LLVulkanContext::setDisplayHdr(bool enabled, float paperWhiteNits, float peakNits)
{
    llassert(SDL_IsMainThread());
    SDL_Window* window = mImpl ? mImpl->mWindow : nullptr;
    const SDL_DisplayID display = enabled && window ? SDL_GetDisplayForWindow(window) : 0;
    const bool autoWhite = gSavedSettings.getBOOL("RenderVKDisplayAutoPaperWhite");
    const bool autoPeak = gSavedSettings.getBOOL("RenderVKDisplayAutoPeak");
#if LL_LINUX
    if ((!enabled || (!autoWhite && !autoPeak)) && sDisplayCalibrationWake)
        delete sDisplayCalibrationWake;
#endif
    const bool autoEnabled = (autoWhite && !sLastAutoWhite) || (autoPeak && !sLastAutoPeak);
    if (enabled != mDisplayHdrRequested || autoWhite != sLastAutoWhite || autoPeak != sLastAutoPeak)
    {
        ++sDisplayCalibrationGeneration;
        sDisplayCalibrationQueryPending = true;
    }
    sLastAutoWhite = autoWhite;
    sLastAutoPeak = autoPeak;
    if (enabled && (autoWhite || autoPeak) && window && display &&
        (sDisplayCalibrationQueryPending || !mDisplayHdrRequested || autoEnabled || display != sDisplayCalibrationId))
    {
#if LL_WINDOWS
        sDisplayCalibrationQueryPending = false;
        vkdisplay::ReportedLuminance measured;
        measured.referenceWhiteNits = windowsSdrReferenceWhiteNits(window);
        if (measured.referenceWhiteNits > 0.f) measured.referenceSource = "Windows SDR white";
        vkdisplay::queryWindowsPeak(window, measured);
        sDisplayCalibration = measured;
        sDisplayCalibrationId = display;
        LL_INFOS("Vulkan") << "HDR display reported reference " << measured.referenceWhiteNits << " nits ("
                           << measured.referenceSource << "), maximum " << measured.peakNits << " nits ("
                           << measured.peakSource << "); stored overrides " << paperWhiteNits << "/" << peakNits
                           << " nits" << LL_ENDL;
#elif LL_LINUX
        const char* driver = SDL_GetCurrentVideoDriver();
        SDL_Rect rect{};
        auto worker = LL::WorkQueue::getInstance("General");
        if (driver && std::strcmp(driver, "wayland") == 0 && SDL_GetDisplayBounds(display, &rect))
        {
            try
            {
                // One bounded main-thread wake exists only while a request is pending.
                if (!sDisplayCalibrationWake) sDisplayCalibrationWake = new DisplayCalibrationWake();
                if (!sDisplayCalibrationJob && worker)
                {
                    auto job = std::make_shared<vkdisplay::CalibrationJob>();
                    auto lease = std::make_shared<vkdisplay::CalibrationJobLease>(job);
                    job->generation = sDisplayCalibrationGeneration;
                    job->display = display;
                    job->window = window;
                    job->bounds = {rect.x, rect.y, rect.w, rect.h};
                    sDisplayCalibrationJob = job;
                    sDisplayCalibrationQueryPending = false;
                    sDisplayCalibrationId = display;
                    const bool posted = worker->tryPost([job, lease]() {
                        (void)lease;
                        try
                        {
                            vkdisplay::queryKscreenLuminance(job->bounds, job->report);
                            job->ran = true;
                        }
                        catch (...) {} // Lease publishes completion even on exception.
                    });
                    if (!posted)
                    {
                        sDisplayCalibrationJob.reset();
                        sDisplayCalibrationQueryPending = true;
                    }
                }
            }
            catch (...)
            {
                sDisplayCalibrationJob.reset();
                sDisplayCalibrationQueryPending = true;
            }
        }
#endif
    }
    const auto chosen = vkdisplay::chooseCalibration(sDisplayCalibration, autoWhite, paperWhiteNits, autoPeak, peakNits);
    sDisplayWhiteSource = !autoWhite && gSavedSettings.getBOOL("RenderVKDisplayLegacyWhiteAmbiguous") &&
                          std::strcmp(chosen.referenceSource, "user override") == 0
                        ? "previous value (origin unknown)" : chosen.referenceSource;
    sDisplayPeakSource = chosen.peakSource;
    const auto request = vkdisplay::sanitized({enabled, chosen.referenceWhiteNits, chosen.peakNits});
    if (request.enabled == mDisplayHdrRequested && request.paperWhiteNits == mDisplayPaperWhiteNits &&
        request.peakNits == mDisplayPeakNits) return;
    mDisplayHdrRequested = request.enabled;
    mDisplayPaperWhiteNits = request.paperWhiteNits;
    mDisplayPeakNits = request.peakNits;
    LL_INFOS("Vulkan") << "HDR display calibration: reference " << mDisplayPaperWhiteNits << " nits ("
                       << sDisplayWhiteSource << "), peak " << mDisplayPeakNits << " nits ("
                       << chosen.peakSource << ")" << LL_ENDL;
    mDisplayHdrPipelineReady = true; // explicit user change retries a prior fail-closed refusal
    if (mImpl)
    {
        mImpl->mNeedsRecreate = true;
        mImpl->mOutputPreviewValid = false;
        // renderFrame retires or supersedes the tracked generation after its job finishes.
    }
}

LLVulkanContext::DisplayHdrInfo LLVulkanContext::displayHdrInfo() const
{
    DisplayHdrInfo info;
    info.requested = mDisplayHdrRequested;
    info.paperWhiteNits = mDisplayPaperWhiteNits; info.peakNits = mDisplayPeakNits;
    info.whiteSource = sDisplayWhiteSource; info.peakSource = sDisplayPeakSource;
    if (mImpl && mImpl->mSwapchain.valid())
    {
        const auto& status = mImpl->mSwapchain.displayStatus();
        info.displayCapable = status.displayCapable;
        info.encoding = static_cast<uint32_t>(status.encoding);
        info.actualFormat = static_cast<uint32_t>(status.actual.format);
        info.actualColorSpace = static_cast<uint32_t>(status.actual.colorSpace);
        info.displayId = status.displayId;
        info.reason = vkdisplay::reasonText(status.displayCapable ? status.reason : status.supportReason);
    }
    return info;
}

void LLVulkanContext::onDisplayChanged()
{
    llassert(SDL_IsMainThread());
    if (!mImpl) return;
    ++sDisplayCalibrationGeneration;
    sDisplayCalibration = {};
    sDisplayCalibrationId = 0;
    sDisplayCalibrationQueryPending = true;
    setDisplayHdr(mDisplayHdrRequested, gSavedSettings.getF32("RenderVKDisplayPaperWhiteNits"),
                  gSavedSettings.getF32("RenderVKDisplayPeakNits"));
    gSavedSettings.setU32("RenderVKDisplayCalibrationGeneration",
        gSavedSettings.getU32("RenderVKDisplayCalibrationGeneration") + 1);
    mDisplayHdrPipelineReady = true; // monitor move/capability change retries HDR negotiation
    mImpl->mNeedsRecreate = true;
    mImpl->mOutputPreviewValid = false;
    // renderFrame retires or supersedes the tracked generation after its job finishes.
}

void LLVulkanContext::refreshDisplayCalibration()
{
    llassert(SDL_IsMainThread());
    if (!mImpl || !mDisplayHdrRequested) return;
    ++sDisplayCalibrationGeneration;
    sDisplayCalibrationQueryPending = true;
    setDisplayHdr(true, gSavedSettings.getF32("RenderVKDisplayPaperWhiteNits"),
                  gSavedSettings.getF32("RenderVKDisplayPeakNits"));
}

bool LLVulkanContext::pollDisplayCalibration()
{
#if LL_LINUX
    llassert(SDL_IsMainThread());
    if (!mImpl || !mDisplayHdrRequested ||
        (!gSavedSettings.getBOOL("RenderVKDisplayAutoPaperWhite") && !gSavedSettings.getBOOL("RenderVKDisplayAutoPeak")))
        return false;
    if (sDisplayCalibrationJob)
    {
        if (!sDisplayCalibrationJob->released.load(std::memory_order_acquire)) return true;
        auto job = sDisplayCalibrationJob;
        sDisplayCalibrationJob.reset();
        SDL_Window* window = mImpl->mWindow;
        SDL_Rect current{};
        const bool currentDisplay = window == job->window && window &&
            SDL_GetDisplayForWindow(window) == job->display && SDL_GetDisplayBounds(job->display, &current) &&
            current.x == job->bounds.x && current.y == job->bounds.y &&
            current.w == job->bounds.width && current.h == job->bounds.height;
        const auto decision = vkdisplay::decideCalibrationPoll(true, job->ran, currentDisplay,
            job->generation == sDisplayCalibrationGeneration, sDisplayCalibrationQueryPending);
        if (decision == vkdisplay::CalibrationPollDecision::Apply)
        {
            sDisplayCalibration = job->report;
            LL_INFOS("Vulkan") << "HDR display reported reference " << job->report.referenceWhiteNits << " nits ("
                               << job->report.referenceSource << "), maximum " << job->report.peakNits << " nits ("
                               << job->report.peakSource << "); stored overrides "
                               << gSavedSettings.getF32("RenderVKDisplayPaperWhiteNits") << "/"
                               << gSavedSettings.getF32("RenderVKDisplayPeakNits") << " nits" << LL_ENDL;
            setDisplayHdr(true, gSavedSettings.getF32("RenderVKDisplayPaperWhiteNits"),
                          gSavedSettings.getF32("RenderVKDisplayPeakNits"));
            gSavedSettings.setU32("RenderVKDisplayCalibrationGeneration",
                gSavedSettings.getU32("RenderVKDisplayCalibrationGeneration") + 1);
        }
        else
        {
            sDisplayCalibrationQueryPending = true;
        }
    }
    if (sDisplayCalibrationQueryPending && !sDisplayCalibrationJob)
        setDisplayHdr(true, gSavedSettings.getF32("RenderVKDisplayPaperWhiteNits"),
                      gSavedSettings.getF32("RenderVKDisplayPeakNits"));
    return sDisplayCalibrationQueryPending || bool(sDisplayCalibrationJob);
#else
    return false;
#endif
}

void LLVulkanContext::setNoVsyncPresentMode(uint32_t mode)
{
    mode = mode == 1 ? 1u : 0u;
    if (VkSwapchain::sNoVsyncMode == mode) return;
    VkSwapchain::sNoVsyncMode = mode;
    if (mImpl)
    {
        mImpl->mNeedsRecreate = true;
        mImpl->mOutputPreviewValid = false;
    }
}

// ---------------------------------------------------------------------------
// #919 full-screen exclusive (Windows, VK_EXT_full_screen_exclusive, APPLICATION_CONTROLLED)
// ---------------------------------------------------------------------------
// How long acquiring the mode is retried (each frame) after a swapchain creation or a loss
// before presentation falls back to composited until the next focus gain. The window's
// fullscreen transition is asynchronous, so the first attempts can fail legitimately.
#if LL_WINDOWS
static const double kFseAcquireWindowSec = 2.0;
#endif

bool LLVulkanContext::fullScreenExclusiveAvailable()
{
#if LL_WINDOWS
    return mInitialized && mImpl && mImpl->mFseExt && mImpl->fseSurfaceSupported();
#else
    return false;
#endif
}

void LLVulkanContext::setFullScreenExclusive(bool want)
{
    if (!mImpl) return;
#if LL_WINDOWS
    if (want && !mImpl->mFseExt)
    {
        LL_INFOS("Vulkan") << "Fullscreen: exclusive presentation requested, but VK_EXT_full_screen_exclusive is not available" << LL_ENDL;
        want = false;
    }
    if (mImpl->mFseWanted == want) return;
    mImpl->mFseWanted = want;
    mImpl->mFseSuspended = false;
    LL_INFOS("Vulkan") << "Fullscreen: exclusive presentation " << (want ? "armed" : "disarmed") << LL_ENDL;
#else
    (void)want;
#endif
}

bool LLVulkanContext::fullScreenExclusiveArmed() const
{
    return mImpl && mImpl->mFseWanted;
}

bool LLVulkanContext::fullScreenExclusiveHeld() const
{
    return mImpl && mImpl->mFseHeld;
}

void LLVulkanContext::onWindowFocusChanged(bool focused)
{
    if (!mImpl) return;
    if (focused)
    {
        mImpl->mFseSuspended = false;   // a new chance after a failed acquire / refused chain
    }
    mImpl->mFseFocused = focused;
}

void LLVulkanContext::onWindowMinimized(bool minimized)
{
    if (!mImpl) return;
    if (minimized && !mImpl->mFseMinimized)
    {
#if LL_LINUX
        // (#1517 Task 11b fix 1) the present thread told, never drained: a minimised window's MAILBOX / FIFO
        // presents may be held by the compositor, and the drain would wait for them
        mImpl->mFgPresent.setWindowHidden(true);
        mImpl->mFgHidden = true;
#else
        // (#1517 frame generation: no present left held on a gate. One bounded try: not drained, nothing is
        // released here either way)
        (void)mImpl->drainFrameGen();
#endif
    }
    mImpl->mFseMinimized = minimized;
}

// Before the swapchain recreate in renderFrame: decides whether the swapchain must carry the
// exclusive chain, and releases the mode before the swapchain it belongs to goes away.
void LLVulkanContext::fseBeforeRecreate()
{
#if LL_WINDOWS
    Impl& m = *mImpl;
    if (!m.mFseExt) return;
    if (m.mSwapchain.takeExclusiveLost())
    {
        const bool was_held = m.mFseHeld;
        m.mFseHeld = false;   // (lost: nothing to release)
        if (was_held)
        {
            LL_INFOS("Vulkan") << "Fullscreen: VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT (the mode was taken away); re-acquiring" << LL_ENDL;
        }
        if (!m.mFseTrying)
        {
            m.mFseTrying = true;
            m.mFseTryStart = std::chrono::steady_clock::now();
        }
    }
    bool active = m.mFseWanted && m.mFseFocused && !m.mFseMinimized && !m.mFseSuspended;
    if (active && !m.mFseChained && !m.fseSurfaceSupported())
    {
        LL_INFOS("Vulkan") << "Fullscreen: exclusive mode not supported on this monitor; composited presentation" << LL_ENDL;
        m.mFseSuspended = true;
        active = false;
    }
    if (active != m.mFseChained)
    {
        // (#1517: not released under a present thread call into the chain -- the whole step is made next frame)
        if (!m.fseRelease(active ? "rebuild" : (!m.mFseWanted ? "left exclusive fullscreen"
                                               : m.mFseMinimized ? "minimized"
                                               : !m.mFseFocused ? "focus lost" : "fallback")))
            return;
        if (active)
        {
            m.fseRefreshMonitor();
        }
        m.mSwapchain.setCreateChain(active ? static_cast<const void*>(&m.mFseInfo) : nullptr);
        m.mFseChained = active;
        m.mFseTrying = active;
        m.mFseTryStart = std::chrono::steady_clock::now();
        m.mNeedsRecreate = true;
        LL_INFOS("Vulkan") << "Fullscreen: swapchain rebuilt " << (active ? "WITH" : "without")
                           << " the exclusive chain" << LL_ENDL;
    }
    else if (m.mNeedsRecreate && m.mFseHeld)
    {
        // A resize / present-mode change rebuilds the exclusive swapchain: release first,
        // acquire again on the new one.
        // (#1517: not released now -- the rebuild itself is deferred by the same drain; asked again with it)
        if (!m.fseRelease("swapchain recreate")) return;
        m.mFseTrying = true;
        m.mFseTryStart = std::chrono::steady_clock::now();
    }
#endif
}

// After the recreate: acquire the mode on a swapchain that carries the chain.
void LLVulkanContext::fseAfterRecreate()
{
#if LL_WINDOWS
    Impl& m = *mImpl;
    if (!m.mFseExt || !m.mFseChained || m.mFseHeld || !m.mSwapchain.valid() || m.mNeedsRecreate) return;
    if (m.mSwapchain.chainRejected())
    {
        LL_WARNS("Vulkan") << "Fullscreen: the driver refused the exclusive swapchain; composited presentation "
                              "until the window regains focus" << LL_ENDL;
        m.mFseSuspended = true;   // fseBeforeRecreate drops the chain next frame
        m.mFseTrying = false;
        return;
    }
    if (!m.mFseTrying)
    {
        m.mFseTrying = true;
        m.mFseTryStart = std::chrono::steady_clock::now();
    }
    const VkResult r = m.mFseAcquire(m.mDevice.device(), m.mSwapchain.handle());
    if (r == VK_SUCCESS)
    {
        m.mFseHeld = true;
        m.mFseTrying = false;
        LL_INFOS("Vulkan") << "Fullscreen: exclusive mode acquired (VK_EXT_full_screen_exclusive, "
                           << m.mSwapchain.extent().width << "x" << m.mSwapchain.extent().height << ", present mode "
                           << vkPresentModeName(m.mSwapchain.presentMode()) << ")" << LL_ENDL;
        return;
    }
    const double tried = std::chrono::duration<double>(std::chrono::steady_clock::now() - m.mFseTryStart).count();
    if (tried >= kFseAcquireWindowSec)
    {
        LL_WARNS("Vulkan") << "Fullscreen: vkAcquireFullScreenExclusiveModeEXT = " << vkResultString(r) << " for "
                           << tried << " s; composited presentation until the window regains focus" << LL_ENDL;
        m.mFseSuspended = true;   // fseBeforeRecreate rebuilds without the chain next frame
        m.mFseTrying = false;
    }
#endif
}

void LLVulkanContext::waitFrameSlot()
{
    if (!mInitialized || !mImpl || vkDeviceIsLost()) return;   // (#135)
    const auto t0 = std::chrono::steady_clock::now();
    {
        VK_HITCH_SCOPE("vk_paceWait");
        mImpl->mSwapchain.waitFrameSlot(mImpl->mDevice);
    }
    LLVKHitch::timelinePace(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
}

void LLVulkanContext::onResize(uint32_t width, uint32_t height)
{
    if (!mImpl) return;
    mImpl->mWidth = width;
    mImpl->mHeight = height;
    mImpl->mNeedsRecreate = true;
    mImpl->mOutputPreviewValid = false;
}

bool LLVulkanContext::runWorldSynthTest(const std::string& png_path,
                                        const LLVKWorldFrame& cam,
                                        uint32_t width, uint32_t height)
{
    if (vkDeviceIsLost()) return false;   // #135
    if (!mInitialized || !mImpl)
    {
        LL_WARNS("Vulkan") << "runWorldSynthTest: context not initialized" << LL_ENDL;
        return false;
    }
    if (!mImpl->retryDeferredWorldShutdown(LLFrameTimer::getElapsedSeconds())) return false;

    // Offscreen RGBA8 color target (TRANSFER_SRC-capable) so we can copy it back
    // to host memory + PNG. Depth is owned by the world renderer itself.
    OffscreenTarget off;
    off.init(mImpl->mDevice, width, height, /*withDepth=*/false);

    // A dedicated world renderer instance: init() calls fillFromCleanStores(),
    // which mirrors the (already-injected) clean-store cube; syncFromStores()
    // rebuilds the VK buffers if anything changed. Isolated from the live
    // swapchain renderer so this proof never disturbs the present path.
    LLCleanRendererVK* w = new LLCleanRendererVK();
    if (!w->init(mImpl->mDevice, off.colorFormat()))
    {
        LL_WARNS("Vulkan") << "runWorldSynthTest: world renderer init failed" << LL_ENDL;
        w->shutdown();
        if (w->shutdownDeferred())
        {
            // The context owns at most one deferred synth renderer. The target has not
            // been submitted to, so it can be released while the renderer stays retained.
            mImpl->mDeferredWorld = w;
            mImpl->mDeferredWorldRetryAt = LLFrameTimer::getElapsedSeconds() + 2.0;
            off.destroy(mImpl->mDevice);
            return false;
        }
        delete w;
        off.destroy(mImpl->mDevice);
        return false;
    }
    w->pollPipelines(true);
    w->syncFromStores();

    // Camera + lighting push blocks (same packing as renderFrame's world path).
    VKDrawPush draw{};
    std::memcpy(draw.view_proj, cam.view_proj, sizeof(float)*16);
    std::memcpy(draw.sun_dir, cam.sun_dir, sizeof(float)*4);
    std::memcpy(draw.sun_color, cam.sun_color, sizeof(float)*4);
    std::memcpy(draw.ambient_color, cam.ambient_color, sizeof(float)*4);
    std::memcpy(draw.cam_time, cam.cam_pos, sizeof(float)*3);
    draw.cam_time[3] = cam.time;
    std::memcpy(draw.sky_horizon, cam.sky_horizon, sizeof(float)*4);
    std::memcpy(draw.sky_zenith,  cam.sky_zenith,  sizeof(float)*4);
    std::memcpy(draw.inv_view_proj, cam.inv_view_proj, sizeof(float)*16);
    std::memcpy(draw.water_fog, cam.water_fog, sizeof(float)*4);
    VKCullPush cull{};
    std::memcpy(cull.frustum_planes, cam.frustum_planes, sizeof(cull.frustum_planes));
    std::memcpy(cull.cam_pos, cam.cam_pos, sizeof(float)*4);
    std::memcpy(cull.lod_cam_pos, cam.lod_cam_pos, sizeof(float)*4);
    VKSkyPush sky{};
    std::memcpy(sky.horizon, cam.sky_horizon, sizeof(float)*4);
    std::memcpy(sky.zenith, cam.sky_zenith, sizeof(float)*4);
    std::memcpy(sky.inv_view_proj, cam.inv_view_proj, sizeof(float)*16);
    std::memcpy(sky.cam, cam.cam_pos, sizeof(float)*3); sky.cam[3] = 1.f;

    // Record the whole world pass into one immediate submit: transition the
    // offscreen color UNDEFINED -> COLOR_ATTACHMENT_OPTIMAL (recordWorld LOADs
    // it), then draw. recordWorld leaves it in COLOR_ATTACHMENT_OPTIMAL, which is
    // exactly the layout readbackToPng expects for its TRANSFER_SRC transition.
    VkImage colorImg = off.color().image;
    VkImageView colorView = off.color().view;
    mImpl->mDevice.submitImmediate([&](VkCommandBuffer cmd)
    {
        VkImageMemoryBarrier2 toColor{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        toColor.srcStageMask  = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        toColor.dstStageMask  = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        toColor.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        toColor.oldLayout     = VK_IMAGE_LAYOUT_UNDEFINED;
        toColor.newLayout     = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        toColor.image         = colorImg;
        toColor.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1; dep.pImageMemoryBarriers = &toColor;
        vkCmdPipelineBarrier2(cmd, &dep);

        w->setFrameSlot(0);   // offscreen immediate submit is fully serialized
        w->setUiLayer(VK_NULL_HANDLE);   // #1451: no UI layer offscreen
        w->recordWorld(cmd, colorView, width, height, draw, cull, sky);
    });
    // submitImmediate returns only after its private fence completes, unless the device was
    // already lost (its early-return case). Do not turn that early return into a false submit
    // acknowledgement for recorded texture tickets.
    if (vkDeviceIsLost()) w->onFrameDropped(false);
    else w->onTextureFrameSubmitted();

    const bool ok = off.readbackToPng(mImpl->mDevice, png_path);

    w->shutdown();
    if (w->shutdownDeferred())
    {
        // submitImmediate's private fence completed before readback, so `off` is no
        // longer referenced. Retain the renderer's own handles in one bounded context slot.
        mImpl->mDeferredWorld = w;
        mImpl->mDeferredWorldRetryAt = LLFrameTimer::getElapsedSeconds() + 2.0;
    }
    else delete w;
    off.destroy(mImpl->mDevice);

    LL_INFOS("Vulkan") << "runWorldSynthTest: png=" << png_path
                       << " ok=" << (ok?1:0) << LL_ENDL;
    return ok;
}

float LLVulkanContext::getMaxSamplerAnisotropy() const
{
    if (!mInitialized || !mImpl) return 0.f;
    VkPhysicalDevice phys = mImpl->mDevice.physicalDevice();
    if (phys == VK_NULL_HANDLE) return 0.f;
    VkPhysicalDeviceFeatures f{};
    vkGetPhysicalDeviceFeatures(phys, &f);
    if (!f.samplerAnisotropy) return 0.f;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(phys, &p);
    return p.limits.maxSamplerAnisotropy;
}

bool LLVulkanContext::isIntegratedGPU() const
{
    if (!mInitialized || !mImpl) return false;
    VkPhysicalDevice phys = mImpl->mDevice.physicalDevice();
    if (phys == VK_NULL_HANDLE) return false;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(phys, &p);
    return p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU
        || p.deviceType == VK_PHYSICAL_DEVICE_TYPE_CPU;
}

bool LLVulkanContext::supportsHardwareRayTracing() const
{
    if (!mInitialized || !mImpl) return false;
    VkPhysicalDevice phys = mImpl->mDevice.physicalDevice();
    if (phys == VK_NULL_HANDLE) return false;
    auto exts = vkenum::deviceExtensions(phys);
    [[maybe_unused]] uint32_t n = uint32_t(exts.size());
    bool accel = false, query = false;
    for (const VkExtensionProperties& e : exts)
    {
        if (std::strcmp(e.extensionName, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) == 0) accel = true;
        if (std::strcmp(e.extensionName, VK_KHR_RAY_QUERY_EXTENSION_NAME) == 0) query = true;
    }
    return accel && query;
}

void LLVulkanContext::getDeviceIdentity(std::string& deviceName,
                                        std::string& vendorShort,
                                        std::string& apiVersion,
                                        bool& isNVIDIA, bool& isAMD, bool& isIntel,
                                        uint32_t& vramMB) const
{
    if (!mInitialized || !mImpl)
    {
        return; // leave outputs untouched; caller keeps its placeholders
    }
    VkPhysicalDevice phys = mImpl->mDevice.physicalDevice();
    if (phys == VK_NULL_HANDLE)
    {
        return;
    }

    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(phys, &p);

    deviceName = p.deviceName; // e.g. "NVIDIA GeForce RTX 5090"

    // PCI vendor IDs (Khronos-registered).
    switch (p.vendorID)
    {
        case 0x10DE: vendorShort = "NVIDIA"; isNVIDIA = true; break;
        case 0x1002:
        case 0x1022: vendorShort = "ATI";    isAMD    = true; break; // is_ati field name is historical
        case 0x8086: vendorShort = "Intel";  isIntel  = true; break;
        default:     vendorShort = "Vulkan"; break;
    }

    char ver[64];
    std::snprintf(ver, sizeof(ver), "Vulkan %u.%u.%u",
                  VK_VERSION_MAJOR(p.apiVersion),
                  VK_VERSION_MINOR(p.apiVersion),
                  VK_VERSION_PATCH(p.apiVersion));
    apiVersion = ver;

    // VRAM: largest DEVICE_LOCAL heap, in MB.
    VkPhysicalDeviceMemoryProperties mem{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    VkDeviceSize largest = 0;
    for (uint32_t i = 0; i < mem.memoryHeapCount; ++i)
    {
        if ((mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) &&
            mem.memoryHeaps[i].size > largest)
        {
            largest = mem.memoryHeaps[i].size;
        }
    }
    vramMB = (uint32_t)(largest / (1024ull * 1024ull));
}

// driverVersion is vendor-encoded: NVIDIA 10.8.8.6 bits, Intel on Windows 18.14, else the
// Vulkan packing.
static std::string decode_driver_version(const VkPhysicalDeviceProperties& p)
{
    const uint32_t v = p.driverVersion;
    char ver[64];
    if (p.vendorID == 0x10DE)
        std::snprintf(ver, sizeof(ver), "%u.%u.%u.%u", (v >> 22) & 0x3ffu, (v >> 14) & 0xffu, (v >> 6) & 0xffu, v & 0x3fu);
#if LL_WINDOWS
    else if (p.vendorID == 0x8086)
        std::snprintf(ver, sizeof(ver), "%u.%u", v >> 14, v & 0x3fffu);
#endif
    else
        std::snprintf(ver, sizeof(ver), "%u.%u.%u", VK_VERSION_MAJOR(v), VK_VERSION_MINOR(v), VK_VERSION_PATCH(v));
    return ver;
}

std::string LLVulkanContext::driverVersionText() const
{
    if (!mInitialized || !mImpl) return std::string();
    VkPhysicalDevice phys = mImpl->mDevice.physicalDevice();
    if (phys == VK_NULL_HANDLE) return std::string();
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(phys, &p2);
    std::string out = drv.driverName;
    if (drv.driverInfo[0])
    {
        if (!out.empty()) out += " ";
        out += drv.driverInfo;
    }
    else
    {
        if (!out.empty()) out += " ";
        out += decode_driver_version(p2.properties);
    }
    return out;
}

std::string LLVulkanContext::deviceLostDiagnostics() const
{
    // Physical-device queries and our own allocation tracking only: valid on a lost device.
    if (!mImpl || mImpl->mDevice.physicalDevice() == VK_NULL_HANDLE) return "Vulkan device: not initialized\n";
    VkPhysicalDevice phys = mImpl->mDevice.physicalDevice();
    VkPhysicalDeviceDriverProperties drv{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRIVER_PROPERTIES};
    VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
    p2.pNext = &drv;
    vkGetPhysicalDeviceProperties2(phys, &p2);
    const VkPhysicalDeviceProperties& p = p2.properties;
    char buf[1024];   // driverName + driverInfo are each up to 256 bytes
    std::string out;
    const uint32_t v = p.driverVersion;
    const std::string ver = decode_driver_version(p);
    std::snprintf(buf, sizeof(buf), "GPU: %s (vendor 0x%04x device 0x%04x), Vulkan %u.%u.%u\n", p.deviceName,
                  p.vendorID, p.deviceID, VK_VERSION_MAJOR(p.apiVersion), VK_VERSION_MINOR(p.apiVersion),
                  VK_VERSION_PATCH(p.apiVersion));
    out += buf;
    std::snprintf(buf, sizeof(buf), "Driver: %s %s (driverVersion %s, raw 0x%08x)\n", drv.driverName, drv.driverInfo, ver.c_str(), v);
    out += buf;
    const VkDeviceContext::MemStats ms = mImpl->mDevice.memStats();
    for (uint32_t i = 0; i < ms.heapCount; ++i)
    {
        if (!ms.heapDeviceLocal[i]) continue;
        std::snprintf(buf, sizeof(buf), "VRAM heap %u: ours %llu MB, process %s%llu MB, budget %llu MB%s\n", i,
                      (unsigned long long)(ms.heapUsed[i] >> 20), ms.budgetExt ? "" : "n/a ",
                      (unsigned long long)(ms.heapDriverUsage[i] >> 20), (unsigned long long)(ms.heapBudget[i] >> 20),
                      ms.budgetExt ? "" : " (heap size: no VK_EXT_memory_budget)");
        out += buf;
    }
    if (ms.uma)
    {   // #695
        const VkDeviceContext::BudgetHeaps bh = VkDeviceContext::budgetHeaps(ms);
        std::snprintf(buf, sizeof(buf), "UMA budget: carve-out heap %u %llu MB + shared heap %u %llu MB (ceiling %llu MB); "
                                        "%llu MB spilled to shared memory (%llu allocations)\n",
                      bh.heap, (unsigned long long)(bh.devBudget >> 20), bh.hostHeap, (unsigned long long)(bh.hostBudget >> 20),
                      (unsigned long long)(ms.umaHostCeil >> 20), (unsigned long long)(ms.umaSpillBytes >> 20),
                      (unsigned long long)ms.umaSpills);
        out += buf;
    }
    std::snprintf(buf, sizeof(buf), "Allocations: %u (buffers %llu MB, images %llu MB, host-visible %llu MB)\n",
                  ms.allocCount, (unsigned long long)(ms.bufferBytes >> 20), (unsigned long long)(ms.imageBytes >> 20),
                  (unsigned long long)(ms.hostVisibleBytes >> 20));
    out += buf;
    {
        // #407: our allocations by category (device-local heaps), from the tracker alone.
        std::string cats = "VRAM by category (MB):";
        for (uint32_t c = 0; c < VKMEM_COUNT; ++c)
        {
            if (!ms.catDev[c]) continue;
            std::snprintf(buf, sizeof(buf), " %s %llu", vkMemCatName(c), (unsigned long long)(ms.catDev[c] >> 20));
            cats += buf;
        }
        out += cats + "\n";
    }
    out += "Ray tracing: " + rayTracingStatus() + "\n";
    out += "Async compute: " + mImpl->mDevice.asyncComputeDecision() + "; queue: "
         + mImpl->mDevice.asyncComputeStatus() + "\n";
    out += "Texture pools: " + mImpl->mDevice.texturePoolStatus() + "\n";
    if (ms.texPools)
    {
        // #411: blocks, live vs block bytes (occupancy), and the driver allocations they replaced.
        std::snprintf(buf, sizeof(buf), "Texture pool blocks: %u (%llu MB), %u images %llu MB live (%.0f%% occupied), "
                      "slack %llu MB\n", ms.texPoolBlocks, (unsigned long long)(ms.texPoolBlockBytes >> 20), ms.texPooledImages,
                      (unsigned long long)(ms.texPoolLive >> 20),
                      ms.texPoolBlockBytes ? 100.0 * (double)ms.texPoolLive / (double)ms.texPoolBlockBytes : 0.0,
                      (unsigned long long)(ms.texPoolSlack >> 20));
        out += buf;
        // #614: what compaction could still free, the empty blocks VMA keeps, and its work so far.
        std::snprintf(buf, sizeof(buf), "Texture pool compaction: reclaimable %llu MB, %u empty blocks (%llu MB); "
                      "moved %llu images / %llu MB, %llu blocks evacuated, %llu empty released, %llu busy skips\n",
                      (unsigned long long)(ms.texPoolReclaim >> 20), ms.texPoolEmptyBlocks,
                      (unsigned long long)(ms.texPoolEmptyBytes >> 20), (unsigned long long)ms.texCompactMoves,
                      (unsigned long long)(ms.texCompactMoved >> 20), (unsigned long long)ms.texCompactEvacuated,
                      (unsigned long long)ms.texCompactReleased, (unsigned long long)ms.texCompactSkipped);
        out += buf;
    }
    const char* async = mImpl->mDevice.computeQueue() != VK_NULL_HANDLE ? "yes" : "no";
    std::snprintf(buf, sizeof(buf), "Async compute queue: %s; frames presented: %llu\n", async,
                  (unsigned long long)mImpl->mFrameCounter);
    out += buf;
    return out;
}

LLRenderBackend* LLVulkanContext::initRenderBackend()
{
    if (!mInitialized || !mImpl)
    {
        LL_WARNS("Vulkan") << "initRenderBackend before context init" << LL_ENDL;
        return nullptr;
    }
    if (mImpl->mRenderBackend)
    {
        return mImpl->mRenderBackend;
    }

    if (!gVK.initFromSpv(mImpl->mDevice,
                         kUiVertSpv, kUiVertSpv_bytes,
                         kUiFragSpv, kUiFragSpv_bytes))
    {
        LL_WARNS("Vulkan") << "LLRenderVK facade init failed" << LL_ENDL;
        return nullptr;
    }
    // Alpha-tested fragment variant for offscreen render targets (#83).
    gVK.initAlphaTestFragSpv(kUiFragATestSpv, kUiFragATestSpv_bytes);
    // In-world label occlusion variant (pre-HUD text layer, #487; per pixel, #1126).
    gVK.initOcclSpv(kUiVertOcclSpv, kUiVertOcclSpv_bytes, kUiFragOcclSpv, kUiFragOcclSpv_bytes);
    gVK.initFrameGenOverlaySpv(kUiFragFgColorSpv, kUiFragFgColorSpv_bytes,
                               kUiVertFgGuideSpv, kUiVertFgGuideSpv_bytes,
                               kUiFragFgGuideSpv, kUiFragFgGuideSpv_bytes);
    // #1133 VR in-world text billboards: the SPIR-V only (modules / pipeline on the first VR stereo frame).
    gVK.initWorldTextSpv(kUiVertWorldSpv, kUiVertWorldSpv_bytes, kUiFragWorldSpv, kUiFragWorldSpv_bytes);
    mImpl->mFacadeReady = true;

    mImpl->mRenderBackend = new LLRenderVKBackend();
    LLRender::setBackend(mImpl->mRenderBackend);
    LL_INFOS("Vulkan") << "LLRenderVK facade + backend installed; gGL now "
                          "dispatches to Vulkan." << LL_ENDL;
    return mImpl->mRenderBackend;
}

// sync2 image-layout barrier helper.
static void imageBarrier(VkCommandBuffer cmd, VkImage image,
                         VkImageLayout oldLayout, VkImageLayout newLayout,
                         VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess,
                         VkPipelineStageFlags2 dstStage, VkAccessFlags2 dstAccess,
                         uint32_t layers = 1)   // #1075: every layer of a layered (stereo) target
{
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = srcStage;
    b.srcAccessMask = srcAccess;
    b.dstStageMask = dstStage;
    b.dstAccessMask = dstAccess;
    b.oldLayout = oldLayout;
    b.newLayout = newLayout;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};

    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
}

void LLVulkanContext::pipelineCompileProgress(U32& done, U32& queued, F64& idle_sec) const
{
    unsigned d = 0, q = 0;
    double idle = 0.0;
    vkPipelineCompileProgress(d, q, idle);
    done = d;
    queued = q;
    idle_sec = idle;
}

bool LLVulkanContext::worldPipelinesPending() const
{
    if (!mInitialized || !mImpl) return false;
    if (!mImpl->mWorld) return !mImpl->mWorldInitTried;   // created on the first renderFrame
    // Only the world's required base bank withholds the world. Optional RT,
    // diagnostic, and first-use groups publish their own fallback-ready subsets;
    // a global compile gate here blanked an otherwise usable frame while those
    // unrelated jobs ran.
    return mImpl->mWorld->pipelinesPending();
}

bool LLVulkanContext::startupPipelinesPending() const
{
    if (worldPipelinesPending()) return true;
    return mInitialized && mImpl && mImpl->mWorld && mImpl->mWorld->rtPipelinesPending();
}

bool LLVulkanContext::benchPipelinesPending(const char** what) const
{
    const char* w = nullptr;
    if (worldPipelinesPending()) w = "world";
    else if (mInitialized && mImpl && mImpl->mWorld && mImpl->mWorld->fullQualityPipelinesPending()) w = "full-quality lighting";
    else if (mInitialized && mImpl && mImpl->mWorld && mImpl->mWorld->rtPipelinesPending()) w = "ray-query";
    else if (mInitialized && mImpl && mImpl->mWorld && mImpl->mWorld->specSettling()) w = "spec";
    else if (mInitialized && mImpl && mImpl->mWorld && mImpl->mWorld->alphaOITPipelinesPending()) w = "WBOIT";
    if (what) *what = w;
    return w != nullptr;
}

void LLVulkanContext::shaderGateProgress(U32& done, U32& queued) const
{
    unsigned d = 0, q = 0, live = 0;
    vkPipelineGateProgress(d, q, live);
    done = d;
    queued = q;
}

bool LLVulkanContext::rayQuerySupported() const
{
    return mInitialized && mImpl && mImpl->mDevice.rayQuerySupported();
}

bool LLVulkanContext::rayTracingEnabled() const
{
    return mInitialized && mImpl && mImpl->mDevice.rayTracingEnabled();
}

bool LLVulkanContext::textureCompressionBC() const
{
    return mInitialized && mImpl && mImpl->mDevice.textureCompressionBC();
}

std::string LLVulkanContext::rayTracingStatus() const
{
    return (mInitialized && mImpl) ? mImpl->mDevice.rayTracingStatus() : std::string("not initialized");
}

std::string LLVulkanContext::asyncComputeSummary() const
{
    if (!mInitialized || !mImpl) return "not initialized";
    return mImpl->mDevice.asyncComputeDecision() + "; queue: " + mImpl->mDevice.asyncComputeStatus();
}

// Lost device (#135): the readbacks would hand out whatever the dead GPU left in the
// host-visible buffers (the logout snapshot keeps the previous session's instead).
bool LLVulkanContext::readWorldSnapshot(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h)
{
    if (vkDeviceIsLost()) return false;
    return mImpl && mImpl->mWorld && mImpl->mWorld->readSnapshotRGB(rgb, w, h);
}

bool LLVulkanContext::readWorldPixel(int32_t x, int32_t y, uint8_t rgba[4])
{
    if (vkDeviceIsLost()) return false;
    return mImpl && mImpl->mWorld && mImpl->mWorld->readWorldPixel(x, y, rgba);
}

bool LLVulkanContext::requestCapture(uint32_t point)
{
    if (vkDeviceIsLost() || !mImpl || !mImpl->mWorld) return false;
    mImpl->mWorld->requestCapture(point);
    return true;
}

bool LLVulkanContext::readCapture(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h)
{
    if (vkDeviceIsLost()) return false;
    return mImpl && mImpl->mWorld && mImpl->mWorld->readCaptureRGB(rgb, w, h);
}

bool LLVulkanContext::liveFrameInfo(uint32_t& w, uint32_t& h) const
{
    w = h = 0;
    if (!mInitialized || !mImpl || vkDeviceIsLost() || !mImpl->mWorld || !mImpl->mWorld->ready()) return false;
    if (!mImpl->mSwapchain.valid()) return false;
    const VkExtent2D e = mImpl->mSwapchain.extent();
    w = e.width; h = e.height;
    // Live snapshots read the renderer's capture source, which is the linear FP16
    // display target while HDR is active. The swapchain is PQ/10-bit in that mode;
    // validating its format rejects the valid FP16 source and falls back to the old
    // readback path, which does not share the live snapshot's crop/resample pipeline.
    const VkFormat captureFormat = mImpl->mHdrOutputActive ? VK_FORMAT_R16G16B16A16_SFLOAT
                                                            : mImpl->mSwapchain.format();
    return w && h && LLCleanSnapshotVK::sourceFormatSupported(captureFormat);
}

bool LLVulkanContext::liveRenderExtent(uint32_t& rw, uint32_t& rh, bool& scaleCapped) const
{
    rw = rh = 0;
    scaleCapped = false;
    if (!mInitialized || !mImpl || vkDeviceIsLost() || !mImpl->mWorld || !mImpl->mWorld->ready()) return false;
    return mImpl->mWorld->liveRenderExtent(rw, rh, scaleCapped);
}

bool LLVulkanContext::hdrDisplayActive() const
{
    return mInitialized && mImpl && !vkDeviceIsLost() && mImpl->mHdrOutputActive
        && mImpl->mDisplayFrame.linear().image != VK_NULL_HANDLE;
}

LLVulkanContext::SnapshotOutputState LLVulkanContext::snapshotOutputState() const
{
    SnapshotOutputState result;
    if (!mInitialized || !mImpl || vkDeviceIsLost() || !mImpl->mWorld || !mImpl->mWorld->ready()) return result;
    const auto output = mImpl->mWorld->displayOutputState();
    result.valid = true;
    result.hdr = hdrDisplayActive();
    result.linearNits = output.linearNits;
    result.format = static_cast<uint32_t>(output.colorFormat);
    result.generation = output.generation;
    result.white = output.paperWhiteNits;
    result.peak = output.peakNits;
    return result;
}

bool LLVulkanContext::beginLiveSnapshot(const LiveSnapshot& req, std::string& why)
{
    uint32_t sw = 0, sh = 0;
    if (!liveFrameInfo(sw, sh))
    {
        why = "no live frame to capture (renderer not ready or unsupported swapchain format)";
        return false;
    }
    LLCleanSnapshotVK::Request r;
    r.point = req.point;
    r.mode = req.mode == 1 ? LLCleanSnapshotVK::MODE_DEPTH
           : req.mode == 2 ? LLCleanSnapshotVK::MODE_ACCUM : LLCleanSnapshotVK::MODE_RESAMPLE;
    if (r.mode == LLCleanSnapshotVK::MODE_DEPTH) r.point = 1;
    r.outW = req.outW; r.outH = req.outH;
    std::memcpy(r.crop, req.crop, sizeof(r.crop));
    r.frames = req.frames;
    r.motionReject = req.motionReject;
    r.dither = req.dither;
    r.sharpen = req.sharpen; r.adaptiveSigma = req.adaptiveSigma; r.mipBias = req.mipBias;   // (#1093)
    r.nativeRender = req.nativeRender; r.warmup = req.warmup;   // (#1434)
    r.hdrRgbeOutput = req.hdrRgbeOutput;
    r.pairedHdr = req.pairedHdr;
    r.depthRange[0] = req.depthNear; r.depthRange[1] = req.depthFar;
    return mImpl->mWorld->liveSnap()->arm(&mImpl->mDevice, r, sw, sh, why);
}

bool LLVulkanContext::liveSnapshotFinished() const
{
    if (!mImpl || !mImpl->mWorld || vkDeviceIsLost()) return true;
    return !mImpl->mWorld->liveSnap()->armed();   // (frames still to render while armed)
}

bool LLVulkanContext::readLiveSnapshot(std::vector<uint8_t>& rgb, uint32_t& w, uint32_t& h, std::string& why,
                                        std::vector<uint8_t>* pairedHdr)
{
    if (!mImpl || !mImpl->mWorld || vkDeviceIsLost()) { why = "no device"; return false; }
    LLCleanSnapshotVK* s = mImpl->mWorld->liveSnap();
    if (s->failed()) why = s->failReason();
    else if (!s->finished()) why = "the shot's frames were not all rendered";
    const bool ok = s->read(rgb, w, h, pairedHdr);
    if (ok) why.clear();
    else if (why.empty()) why = s->failed() ? s->failReason() : "snapshot readback failed";
    return ok;
}

void LLVulkanContext::cancelLiveSnapshot()
{
    if (mImpl && mImpl->mWorld && !vkDeviceIsLost()) mImpl->mWorld->liveSnap()->cancel();
}

bool LLVulkanContext::beginWorldOverlay()
{
    return mInitialized && mImpl && mImpl->mFacadeReady && gVK.beginWorldOverlay();
}

void LLVulkanContext::endWorldOverlay()
{
    if (mInitialized && mImpl && mImpl->mFacadeReady) gVK.endWorldOverlay();
}

bool LLVulkanContext::beginPreHudLayer(const float* proj, const float* view, bool occlude)
{
    return mInitialized && mImpl && mImpl->mFacadeReady && gVK.beginPreHudLayer(proj, view, occlude);
}

void LLVulkanContext::endPreHudLayer()
{
    if (mInitialized && mImpl && mImpl->mFacadeReady) gVK.endPreHudLayer();
}

// #1133 VR: what the in-world text pass drew (tag VR, grep "in-world text"): the first frame, a
// change between drawn / refused, every 30 s while it runs, and any drop (10 s throttle).
static void vrLogWorldText(const LLRenderVK::WorldTextStats& s)
{
    static int s_state = -1;   // -1 never, 0 nothing to draw, 1 drawn, 2 refused
    static F64 s_next = 0.0, s_dropNext = 0.0;
    const int state = s.failed ? 2 : (s.recorded ? 1 : 0);
    const F64 now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    const bool dropped = s.dropped2d || s.droppedTopo || s.droppedCap || s.droppedTex;
    const bool changed = state != s_state && (s_state < 0 || state == 2 || s_state == 2 || (state == 1 && now >= s_next));
    if (state != 2 && s_state != 2 && state != s_state && !changed)
        return;   // (labels coming and going: the 30 s report covers it)
    if (changed || (state == 1 && now >= s_next) || (dropped && now >= s_dropNext))
    {
        if (state == 2)
            LL_WARNS("VR") << "in-world text: the billboard pass's resources were refused; no labels for a while "
                              "(retried in ~300 frames)" << LL_ENDL;
        else
            LL_INFOS("VR") << "in-world text (world billboards, both eyes): " << s.batches << " batches, " << s.verts
                           << " vertices, " << s.draws << " draws, " << s.textures << " textures, " << s.occluded
                           << " occluded; dropped: " << s.dropped2d << " 2D, " << s.droppedTopo << " lines, "
                           << s.droppedCap << " over the cap, " << s.droppedTex << " over the image slots" << LL_ENDL;
        s_state = state;
        s_next = now + 30.0;
        if (dropped) s_dropNext = now + 10.0;
    }
}

bool LLVulkanContext::vrWorldText() const
{
    return mInitialized && mImpl && mImpl->mFacadeReady && gVK.worldTextViews();
}

// Per-frame push blocks for LLCleanRendererVK::recordWorld from the world frame.
static void fillWorldPushes(const LLVKWorldFrame& wf, VKDrawPush& draw, VKCullPush& cull, VKSkyPush& sky)
{
    const LLVKWorldFrame* world = &wf;
    std::memcpy(draw.view_proj, world->view_proj, sizeof(float)*16);
    std::memcpy(draw.sun_dir, world->sun_dir, sizeof(float)*4);
    std::memcpy(draw.sun_color, world->sun_color, sizeof(float)*4);
    std::memcpy(draw.ambient_color, world->ambient_color, sizeof(float)*4);
    std::memcpy(draw.cam_time, world->cam_pos, sizeof(float)*3);
    draw.cam_time[3] = world->time;
    std::memcpy(draw.sky_horizon, world->sky_horizon, sizeof(float)*4);
    std::memcpy(draw.sky_zenith,  world->sky_zenith,  sizeof(float)*4);
    std::memcpy(draw.inv_view_proj, world->inv_view_proj, sizeof(float)*16);
    std::memcpy(draw.water_fog, world->water_fog, sizeof(float)*4);

    std::memcpy(cull.frustum_planes, world->frustum_planes, sizeof(cull.frustum_planes));
    std::memcpy(cull.cam_pos, world->cam_pos, sizeof(float)*4);
    std::memcpy(cull.lod_cam_pos, world->lod_cam_pos, sizeof(float)*4);

    std::memcpy(sky.horizon, world->sky_horizon, sizeof(float)*4);
    std::memcpy(sky.zenith, world->sky_zenith, sizeof(float)*4);
    // Camera-relative sky rays (#555): the translation-free inverse; cam.w = 2 tells
    // clean_sky.frag not to subtract the eye (cam.xyz stays the eye: underwater test).
    std::memcpy(sky.inv_view_proj, world->inv_view_proj_rel, sizeof(float)*16);
    std::memcpy(sky.cam, world->cam_pos, sizeof(float)*3); sky.cam[3] = 2.f;

}

bool LLVulkanContext::renderOffscreen(const LLVKWorldFrame& world, uint32_t w, uint32_t h,
                                      std::vector<uint8_t>& rgb, std::vector<float>* depth, uint32_t frames,
                                      uint32_t aaFrames, bool rgba, std::vector<uint8_t>* hdrHalf, float* hdrWhiteNits)
{
    if (hdrHalf) hdrHalf->clear();
    if (!mInitialized || !mImpl || !world.enabled || !w || !h) return false;
    if (vkDeviceIsLost()) return false;   // #135
    LLCleanRendererVK* W = mImpl->mWorld;
    if (!W || !W->ready()) return false;
    // #1075: a stereo session's recordings are N-view (layered targets, multiview scopes); an
    // offscreen snapshot is one view into a one-layer image -- refused (snapshots are in the
    // stereo-off set, design §8.3).
    if (W->stereoActive())
    {
        LL_WARNS_ONCE("Vulkan") << "offscreen snapshot refused: not available while rendering stereo views (#1075)" << LL_ENDL;
        return false;
    }
    VkDevice dev = mImpl->mDevice.device();
    // (#1517: with frame generation's present thread running this waits without its queue -- deviceIdleWait;
    // nothing the snapshot touches is that thread's)
    const VkResult idleResult = vkDeviceWaitIdle(dev);
    if (idleResult != VK_SUCCESS || vkDeviceIsLost())
    {
        LL_WARNS("Vulkan") << "offscreen render refused: device idle was not proven ("
                            << vkResultString(idleResult) << ")" << LL_ENDL;
        return false;
    }

    GpuImage img;
    {
        VkMemScope memcat(VKMEM_TARGET);   // #407 (snapshot colour target; the renderer scopes its own)
        // #730: soft -- the first snapshot-size allocation; out of memory fails the snapshot
        // here (the !img.image return below) instead of ending in the fatal handler.
        VkDeviceContext::SoftAllocScope soft(&mImpl->mDevice);
        // Match the live renderer pipelines: in HDR this is FP16, not the PQ swapchain (and
        // sampled: a world + HUD shot composites its UI layer over it, #1451; #1536 also in the SDR layer).
        img = mImpl->mDevice.createImage2D(w, h, W->displayOutputState().colorFormat,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
                | (W->displayOutputState().uiLayer ? VK_IMAGE_USAGE_SAMPLED_BIT : 0u),
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (!img.image) return false;
    // The transparent avatar layer (#1328; llcleansnapmattevk.cpp): its holdout pipelines, or no
    // render -- without them the layer would come out opaque.
    const bool avatarLayer = world.avatar_matte && !depth;
    const bool colorHdrPair = hdrHalf && !avatarLayer && !depth;
    if (avatarLayer && !W->snapMatteInit())
    {
        mImpl->mDevice.destroyImage(img);
        return false;
    }
    GpuImage hdrImage;
    GpuBuffer hdrReadback;
    uint64_t hdrBytes = 0;
    const float capturedWhite = W->displayOutputState().paperWhiteNits;
    if (hdrHalf && (!hdrWhiteNits || !mImpl->mHdrOutputActive || depth ||
                    uint64_t(w) * h > LLRadianceSnapshot::kMaxPixels ||
                    W->displayOutputState().colorFormat != VK_FORMAT_R16G16B16A16_SFLOAT))
    {
        mImpl->mDevice.destroyImage(img);
        return false;
    }
    if (hdrHalf && avatarLayer)
    {
        if (!W->snapHdrReady()
            || W->displayOutputState().colorFormat != VK_FORMAT_R16G16B16A16_SFLOAT
            || uint64_t(w) * h > LLRadianceSnapshot::kMaxPixels
            || !LLSnapshotBufferLimits::imageBytes(w, h, 8u, SIZE_MAX, hdrBytes))
        {
            mImpl->mDevice.destroyImage(img);
            return false;
        }
        {
            VkDeviceContext::SoftAllocScope soft(&mImpl->mDevice, true);
            VkMemScope memcat(VKMEM_TARGET);
            hdrImage = mImpl->mDevice.createImage2D(w, h, W->displayOutputState().colorFormat,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            if (hdrImage.image)
            {
                VkMemScope readback(VKMEM_READBACK);
                hdrReadback = mImpl->mDevice.createBuffer(hdrBytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            }
        }
        if (!hdrImage.image || !hdrReadback.buffer || !hdrReadback.mapped)
        {
            if (hdrReadback.buffer) mImpl->mDevice.destroyBuffer(hdrReadback);
            if (hdrImage.image) mImpl->mDevice.destroyImage(hdrImage);
            mImpl->mDevice.destroyImage(img);
            return false;
        }
    }
    // #1451: with HDR output on, the HUD attachments and selection outlines are UI-layer pipelines
    // (RGBA8). A shot that includes the HUD draws them into its own transient layer, which the
    // display encode composites into the shot's source as SDR blends them (the world here already
    // runs the SDR post chain; the paired .hdr file's world is in nits, composited at paper white).
    // #1536: the same on an SDR frame generation chain, whose renderer also draws them into a UI layer: the
    // shot's layer is composed over its world with SDR frame generation's composite, into an 8-bit capture.
    const LLCleanRendererVK::DisplayOutputState shotOutput = W->displayOutputState();
    const bool hdrLayerShot = shotOutput.linearNits && mImpl->mHdrOutputActive;
    const bool sdrLayerShot = shotOutput.uiLayer && !shotOutput.linearNits && !mImpl->mHdrOutputActive;
    GpuImage offLayer, offCapture;
    if (shotOutput.uiLayer && (hdrLayerShot || sdrLayerShot) && world.hud_enabled && !depth && !avatarLayer)
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(&mImpl->mDevice);
        offLayer = mImpl->mDevice.createImage2D(w, h, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        if (offLayer.image)
            offCapture = mImpl->mDevice.createImage2D(w, h,
                sdrLayerShot ? shotOutput.colorFormat : VK_FORMAT_R16G16B16A16_SFLOAT,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        if (!offLayer.image || !offCapture.image)
        {
            if (offLayer.image) mImpl->mDevice.destroyImage(offLayer);
            offLayer = GpuImage{};
            LL_WARNS_ONCE("Snapshot") << "Snapshot: no memory for its HUD layer (" << w << "x" << h
                                      << "); HUD attachments left out" << LL_ENDL;
        }
    }
    VkFence fence = VK_NULL_HANDLE;
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VK_CHECK(vkCreateFence(dev, &fi, nullptr, &fence));
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = mImpl->mDevice.commandPool();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VK_CHECK(vkAllocateCommandBuffers(dev, &ai, &cmd));

    { VK_HITCH_SCOPE("vk_syncFromStores"); W->syncFromStores(); }
    VKDrawPush draw{}; VKCullPush cull{}; VKSkyPush sky{};
    fillWorldPushes(world, draw, cull, sky);
    const uint32_t slot = mImpl->mSwapchain.frameSlot();
    // SNAPSHOT SUPERSAMPLING (#417): the renderer saves the live view's temporal state and,
    // with aaFrames > 0 (never for depth: the raw depth must stay unjittered), allocates the
    // accumulation targets -- 0 back when they don't fit, then the old path below runs.
    // Supersampled: recording 0 is the warm-up (SSR's previous frame at this size), 1..N
    // are jittered and averaged; the capture takes the last, converged one. Otherwise
    // `frames` unjittered recordings (warm-up for the history-based passes) as before.
    W->mOffscreenHdrPair = colorHdrPair;
    const uint32_t aa = W->beginOffscreen(w, h, depth ? 0u : aaFrames);
    frames = aa ? aa + 1u : std::max(frames, 1u);
    // The layer's warm-up depth and occluder, after the accumulators (vkSnapshotFitAvatarLayer
    // budgets both): none, no recording -- the shot fails below.
    if (avatarLayer && !W->snapMatteBegin(w, h)) frames = 0;
    if (aa)
        LL_DEBUGS("Vulkan") << "offscreen snapshot " << w << "x" << h << ": supersampled, " << aa
                           << " jittered frames" << LL_ENDL;
    bool hdrRecorded = false, hdrComplete = true;
    for (uint32_t f = 0; f < frames; ++f)
    {
        W->mOffscreenFrame = f;
        W->snapMatteFrame(f);
        const bool last = (f + 1 == frames);
        if (last && !depth) W->requestCapture(world.hud_enabled ? 2u : 1u);
        VK_CHECK(vkResetCommandBuffer(cmd, 0));
        beginTexturePrepRecording();
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
        imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                     VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkClearColorValue clear{}; clear.float32[3] = 1.f;
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
        imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                     VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                     VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);

        W->setFrameSlot(slot);
        W->specFrameTick();   // setting specialization: bind / fall back at the frame boundary
        W->setLights(world);
        W->setProbes(world);
        W->setShadows(world);
        W->setMirror(world);
        W->setTemporal(world);
        W->mHideSelf = world.hide_self || (W->mSnapMatte && world.avatar_hide_self);   // (#1328: after snapMatteFrame)
        W->mHideSelfHead = world.hide_self_head;
        W->mHeadlessSelf = world.headless_self;   // #1585
        W->mOrthoView = world.ortho_view;   // snapshot of the orthographic view (GunMetal-DX port)
        W->mOrtho = world.ortho;            // region map capture (GunMetal-DX port)
        W->mOrthoPpm = world.ortho_ppm;
        W->setSelection(world);
        W->setPhysShapes(world);
        W->setAtmosphere(world);
        W->setSwapImage(img.image, img.format, W->displayOutputState().generation);
        if (last && hdrImage.image)
        {
            imageBarrier(cmd, hdrImage.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            W->setSnapHdrView(hdrImage.view);
        }
        W->mWorldOverlayHook = [](VkCommandBuffer, VkImageView, VkImageView, VkImage, uint32_t, uint32_t) {};
        if (offLayer.image) vkDisplayLayerBegin(cmd, offLayer, true);   // #1451: this recording's, transparent
        W->setUiLayer(offLayer.image ? offLayer.view : VK_NULL_HANDLE);
        W->setSdrFrame(false);   // (offscreen already runs the SDR post chain, the pair excepted)
        W->recordWorld(cmd, img.view, w, h, draw, cull, sky,
                       world.hud_enabled ? world.hud_view_proj : nullptr, world.hud_viewport);
        if (last && hdrImage.image) hdrRecorded = W->snapHdrRecorded();
        if (colorHdrPair) hdrComplete = hdrComplete && W->displayOutputComplete();
        W->setSnapHdrView(VK_NULL_HANDLE);
        if (last && hdrImage.image)
        {
            imageBarrier(cmd, hdrImage.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            VkBufferImageCopy copy{};
            copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            copy.imageExtent = {w, h, 1};
            vkCmdCopyImageToBuffer(cmd, hdrImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, hdrReadback.buffer, 1, &copy);
            VkMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT; barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            barrier.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT; barrier.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
            VkDependencyInfo dependency{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dependency.memoryBarrierCount = 1; dependency.pMemoryBarriers = &barrier;
            vkCmdPipelineBarrier2(cmd, &dependency);
        }
        // #1451: a world + HUD shot under HDR reads its UI layer composited over the world.
        bool hudComposited = false;
        if (offLayer.image && W->capturePending() == 2u)
        {
            const auto out = W->displayOutputState();
            imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            vkDisplayLayerSample(cmd, offLayer);
            imageBarrier(cmd, offCapture.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            const vkdisplay::Request request{true, out.paperWhiteNits, out.peakNits};
            const uint64_t shotBytes = uint64_t(w) * h * 8u;   // the shot's images were admitted by vkSnapshotFit
            hudComposited = sdrLayerShot
                ? mImpl->mDisplayEncode.recordLayerComposite(cmd, slot, img.view, offLayer.view, offCapture.view,
                                                             VkExtent2D{w, h}, VK_NULL_HANDLE, nullptr, true)
                : mImpl->mDisplayEncode.record(cmd, slot, img.view, offLayer.view, offCapture.view,
                      VkExtent2D{w, h}, request, VkDisplayEncode::Content::CompositeNits, shotBytes, shotBytes,
                      false, !colorHdrPair);
            mImpl->mDisplayEncode.invalidateInputs();   // the transient views die with this shot
            imageBarrier(cmd, img.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
            if (hudComposited)
                W->setSwapImage(offCapture.image, offCapture.format, out.generation);
            else
                LL_WARNS_ONCE("Snapshot") << "Snapshot: HUD layer composite unavailable; HUD attachments left out"
                                          << LL_ENDL;
        }
        W->recordCaptureAt(cmd, 2, w, h);
        if (hudComposited) W->setSwapImage(img.image, img.format, W->displayOutputState().generation);
        if (last && depth) W->recordDepthCapture(cmd, w, h);
        W->snapMatteRecord(cmd, w, h);
        const VkResult endResult = vkEndCommandBuffer(cmd);
        if (endResult != VK_SUCCESS)
        {
            // This recording never reached the queue; a ticket can safely discard its
            // candidate and retry without treating the offscreen fence as copy proof.
            W->onFrameDropped(endResult != VK_ERROR_DEVICE_LOST);
            texturePrepRecordingDropped(endResult != VK_ERROR_DEVICE_LOST && !vkDeviceIsLost());
            VK_CHECK(endResult);
        }

        VkCommandBufferSubmitInfo ci{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
        ci.commandBuffer = cmd;
        VkSubmitInfo2 si{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
        si.commandBufferInfoCount = 1; si.pCommandBufferInfos = &ci;
        const VkResult submitResult = vkQueueSubmit2(mImpl->mDevice.queue(), 1, &si, fence);
        if (submitResult != VK_SUCCESS)
        {
            // Failed submit did not accept this command buffer. Device loss is still
            // quarantined by onFrameDropped's device-state check.
            W->onFrameDropped(submitResult != VK_ERROR_DEVICE_LOST);
            texturePrepRecordingDropped(submitResult != VK_ERROR_DEVICE_LOST && !vkDeviceIsLost());
            VK_CHECK(submitResult);
        }
        texturePrepRecordingSubmitted(slot);
        const VkResult waitResult = vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        if (waitResult != VK_SUCCESS || vkDeviceIsLost())
        {
            // The submit was accepted, but without fence proof it cannot be retried or
            // retired. Quarantine this frame's ticket until process/device teardown.
            W->onFrameDropped(false);
            if (waitResult != VK_SUCCESS) VK_CHECK(waitResult);
            break;
        }
        // This immediate path has its own accepted-submit proof: its private fence is
        // complete before the next AA recording reuses the same descriptor slot.
        if (mImpl->mTexturePrepService)
        {
            mImpl->mTexturePrepService->retireGpuUse(mImpl->mTexturePrepSubmitSerial);
            mImpl->mTexturePrepService->reapCompleted();
            mImpl->mTexturePrepService->beginFrame();   // (each snapshot frame: its staging budget)
        }
        W->onTextureFrameSubmitted();
        if (vkDeviceIsLost()) break;   // #135: the readback below would be garbage
        VK_CHECK(vkResetFences(dev, 1, &fence));
    }
    const bool layerOk = W->snapMatteEnd();
    uint32_t cw = 0, ch = 0;
    bool ok = frames > 0 && !vkDeviceIsLost() && layerOk && (!hdrHalf || (avatarLayer ? hdrRecorded : hdrComplete));
    if (ok && depth)
    {
        uint32_t dw = 0, dh = 0;
        ok = W->readDepthCapture(*depth, dw, dh) && dw == w && dh == h;
    }
    else if (ok)
    {
        if (colorHdrPair)
        {
            uint32_t hw = 0, hh = 0;
            ok = W->readCaptureFP16(*hdrHalf, hw, hh, *hdrWhiteNits) && hw == w && hh == h;
        }
        if (ok) ok = W->readCaptureRGB(rgb, cw, ch, rgba) && cw == w && ch == h;
    }
    if (ok && hdrHalf && avatarLayer)
    {
        try
        {
            hdrHalf->resize((size_t)hdrBytes);
            std::memcpy(hdrHalf->data(), hdrReadback.mapped, (size_t)hdrBytes);
            *hdrWhiteNits = capturedWhite;
        }
        catch (const std::bad_alloc&) { hdrHalf->clear(); ok = false; }
    }
    W->setSnapHdrView(VK_NULL_HANDLE);
    if (hdrReadback.buffer) mImpl->mDevice.destroyBuffer(hdrReadback);
    if (hdrImage.image) mImpl->mDevice.destroyImage(hdrImage);
    W->mOrthoView = false;   // (the next live frame sets its own)
    W->mOrtho = false;       // (live frames are never a map capture)
    W->mOrthoPpm = 0.f;
    W->endOffscreen();   // the next on-screen frame resets like a window resize
    W->mOffscreenHdrPair = false;
    W->setSwapImage(VK_NULL_HANDLE, VK_FORMAT_UNDEFINED, 0);   // the next presented frame sets its own
    W->setUiLayer(VK_NULL_HANDLE);
    vkFreeCommandBuffers(dev, mImpl->mDevice.commandPool(), 1, &cmd);
    vkDestroyFence(dev, fence, nullptr);
    if (offCapture.image) mImpl->mDevice.destroyImage(offCapture);
    if (offLayer.image) mImpl->mDevice.destroyImage(offLayer);
    mImpl->mDevice.destroyImage(img);
    return ok;
}

uint32_t LLVulkanContext::maxImageDimension() const
{
    if (!mImpl) return 0;
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(mImpl->mDevice.physicalDevice(), &props);
    return props.limits.maxImageDimension2D;
}

// Swapchain images are allocated by the driver / WSI, not through VkDeviceContext (#407):
// VkSwapchain measures them (memory requirements of their create info x image count).
// The estimate (count x extent x texel size) is only the fallback when that query gave 0.
static uint64_t swapchainBytes(const VkSwapchain& sc)
{
    if (sc.memoryBytes()) return sc.memoryBytes();
    uint32_t bpp = 4;
    switch (sc.format())
    {
    case VK_FORMAT_R16G16B16A16_SFLOAT:
    case VK_FORMAT_R16G16B16A16_UNORM: bpp = 8; break;
    case VK_FORMAT_R32G32B32A32_SFLOAT: bpp = 16; break;
    default: bpp = 4; break;   // B8G8R8A8 / R8G8B8A8 / A2B10G10R10 / A2R10G10B10
    }
    const VkExtent2D e = sc.extent();
    return (uint64_t)sc.imageCount() * e.width * e.height * bpp;
}

namespace
{
const char* vkSurfaceFormatName(VkFormat f)
{
    switch (f)
    {
    case VK_FORMAT_B8G8R8A8_UNORM: return "BGRA8";
    case VK_FORMAT_B8G8R8A8_SRGB: return "BGRA8-sRGB";
    case VK_FORMAT_R8G8B8A8_UNORM: return "RGBA8";
    case VK_FORMAT_R8G8B8A8_SRGB: return "RGBA8-sRGB";
    case VK_FORMAT_A2B10G10R10_UNORM_PACK32: return "A2B10G10R10";
    case VK_FORMAT_A2R10G10B10_UNORM_PACK32: return "A2R10G10B10";
    case VK_FORMAT_R16G16B16A16_SFLOAT: return "RGBA16F";
    default: return "other";
    }
}

const char* vkColorSpaceName(VkColorSpaceKHR c)
{
    switch (c)
    {
    case VK_COLOR_SPACE_SRGB_NONLINEAR_KHR: return "sRGB";
    case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT: return "scRGB";
    case VK_COLOR_SPACE_HDR10_ST2084_EXT: return "HDR10";
    case VK_COLOR_SPACE_DISPLAY_P3_NONLINEAR_EXT: return "P3";
    default: return "other";
    }
}

const char* vkPresentModeName(VkPresentModeKHR m)
{
    switch (m)
    {
    case VK_PRESENT_MODE_FIFO_KHR: return "fifo";
    case VK_PRESENT_MODE_FIFO_RELAXED_KHR: return "fifo-relaxed";
    case VK_PRESENT_MODE_MAILBOX_KHR: return "mailbox";
    case VK_PRESENT_MODE_IMMEDIATE_KHR: return "immediate";
    default: return "other";
    }
}
} // namespace

std::string LLVulkanContext::displayInfoText() const
{
    // One short line (bug reports, < 200 chars): the swapchain as created (it is what the
    // VRAM accounting measures) and the monitors -- count, the window's display mode, HDR.
    std::string o;
    char buf[160];
    if (mImpl && mImpl->mSwapchain.valid())
    {
        const VkSwapchain& sc = mImpl->mSwapchain;
        std::snprintf(buf, sizeof(buf), "swapchain %ux%u %s/%s %s x%u (%llu MB)", sc.extent().width, sc.extent().height,
                      vkSurfaceFormatName(sc.format()), vkColorSpaceName(sc.colorSpace()), vkPresentModeName(sc.presentMode()),
                      sc.imageCount(), (unsigned long long)((swapchainBytes(sc) + (1u << 19)) >> 20));
        o = buf;
    }
    int count = 0;
    SDL_DisplayID* ids = SDL_GetDisplays(&count);
    if (ids) SDL_free(ids);
    std::snprintf(buf, sizeof(buf), "%smonitors %d", o.empty() ? "" : "; ", count);
    o += buf;
    const SDL_DisplayID win_disp = (mImpl && mImpl->mWindow) ? SDL_GetDisplayForWindow(mImpl->mWindow) : 0;
    if (win_disp)
    {
        const SDL_DisplayMode* dm = SDL_GetCurrentDisplayMode(win_disp);
        const bool hdr = SDL_GetBooleanProperty(SDL_GetDisplayProperties(win_disp), SDL_PROP_DISPLAY_HDR_ENABLED_BOOLEAN, false);
        if (dm)
        {
            std::snprintf(buf, sizeof(buf), ", window on %dx%d@%.0fHz scale %.2f", dm->w, dm->h, (double)dm->refresh_rate,
                          (double)dm->pixel_density);
            o += buf;
        }
        o += hdr ? ", HDR on" : ", HDR off";
    }
    return o;
}

LLVKVramSummary LLVulkanContext::vramSummary() const
{
    LLVKVramSummary s;
    if (!mImpl || !mImpl->mWorld) return s;
    const LLVramStatus& v = mImpl->mWorld->vramStatus();
    if (!v.valid) return s;
    s.valid = true;
    s.capacity = v.capacity; s.reserve = v.reserve; s.budget = v.budget; s.usage = v.usage;
    s.fixed = v.fixed; s.overhead = v.overhead;
    s.fixedSmoothed = v.fixedSmoothed; s.scalable = v.scalable; s.swapchain = v.swapchain;
    s.slack = v.slackDev; s.pad = v.pad; s.unattributed = v.unattributed;
    s.rung = v.rung; s.rungCount = v.rungCount; s.step = v.step; s.mode = v.mode;
    return s;
}

std::string LLVulkanContext::vramStatusText(int part) const
{
    if (!mImpl || !mImpl->mWorld) return part == 1 ? std::string() : std::string("n/a (no Vulkan world renderer)");
    const LLVramStatus& v = mImpl->mWorld->vramStatus();
    return part == 0 ? v.summaryText() : part == 1 ? v.categoryText() : part == 3 ? v.overheadText()
         : part == 4 ? v.texPoolText() : v.compactText();
}

// #458: allocation-traffic rates, sampled by hostMemTick (main thread).
namespace
{
struct HostMemRates
{
    bool   have_prev = false, have_rate = false;
    double prev_t = 0.0, next_log = 0.0;
    uint64_t prev_alloc[2] = {}, prev_free[2] = {}, prev_alloc_b[2] = {}, prev_free_b[2] = {};
    uint64_t prev_cat_alloc[VKMEM_COUNT] = {};
    double alloc_s[2] = {}, free_s[2] = {}, alloc_mb_s[2] = {}, free_mb_s[2] = {};
    double cat_alloc_s[VKMEM_COUNT] = {};
};
HostMemRates s_host_rates;
}

std::string LLVulkanContext::hostMemText() const
{
    if (!mInitialized || !mImpl) return std::string();
    const VkDeviceContext::MemStats ms = mImpl->mDevice.memStats();
    auto mb = [](uint64_t b) { return (double)b / (1024.0 * 1024.0); };
    const uint64_t dev_only = ms.bufferBytes + ms.imageBytes > ms.hostVisibleBytes
                            ? ms.bufferBytes + ms.imageBytes - ms.hostVisibleBytes : 0;
    std::string o = llformat("host-visible %.0f MB / %u allocs [", mb(ms.hostVisibleBytes), ms.hostVisibleCount);
    bool first = true;
    for (uint32_t c = 0; c < VKMEM_COUNT; ++c)
    {
        if (!ms.catHVCount[c]) continue;
        o += llformat("%s%s %.0f/%u", first ? "" : " ", vkMemCatName(c), mb(ms.catHV[c]), ms.catHVCount[c]);
        first = false;
    }
    o += llformat("] device-only %.0f MB", mb(dev_only));
    const HostMemRates& r = s_host_rates;
    if (r.have_rate)
    {
        o += llformat(" | /s: alloc hv %.1f (%.0f MB) dev %.1f (%.0f MB), free hv %.1f (%.0f MB) dev %.1f (%.0f MB)",
                      r.alloc_s[1], r.alloc_mb_s[1], r.alloc_s[0], r.alloc_mb_s[0],
                      r.free_s[1], r.free_mb_s[1], r.free_s[0], r.free_mb_s[0]);
        // the three busiest categories by allocation calls
        uint32_t top[3] = {VKMEM_COUNT, VKMEM_COUNT, VKMEM_COUNT};
        for (uint32_t c = 0; c < VKMEM_COUNT; ++c)
        {
            if (r.cat_alloc_s[c] < 0.05) continue;
            for (int k = 0; k < 3; ++k)
            {
                if (top[k] == VKMEM_COUNT || r.cat_alloc_s[c] > r.cat_alloc_s[top[k]])
                {
                    for (int j = 2; j > k; --j) top[j] = top[j - 1];
                    top[k] = c;
                    break;
                }
            }
        }
        if (top[0] != VKMEM_COUNT)
        {
            o += " busiest";
            for (int k = 0; k < 3 && top[k] != VKMEM_COUNT; ++k)
                o += llformat(" %s %.1f", vkMemCatName(top[k]), r.cat_alloc_s[top[k]]);
        }
        o += llformat(" | total allocs %llu frees %llu",
                      (unsigned long long)(ms.allocCalls[0] + ms.allocCalls[1]),
                      (unsigned long long)(ms.freeCalls[0] + ms.freeCalls[1]));
    }
    return o;
}

void LLVulkanContext::hostMemTick(double now, bool memTight)
{
    if (!mInitialized || !mImpl) return;
    HostMemRates& r = s_host_rates;
    if (r.have_prev && now - r.prev_t < 1.0) return;
    // The UI upload staging ring's cap, like the world texture ring: RAM / 64, here within
    // [128 MB, 256 MB] (a 4K media frame, 32 MB, must stay under cap / 4).
    if (mImpl->mFacadeReady)
        gVK.setStageRingMax((uint64_t)LLMemoryGovernor::scaleCacheBudget(256ull << 20, 64, 128ull << 20));
    const VkDeviceContext::MemStats ms = mImpl->mDevice.memStats();
    if (r.have_prev)
    {
        const double dt = std::max(now - r.prev_t, 1.0e-3);
        for (int k = 0; k < 2; ++k)
        {
            r.alloc_s[k] = (double)(ms.allocCalls[k] - r.prev_alloc[k]) / dt;
            r.free_s[k] = (double)(ms.freeCalls[k] - r.prev_free[k]) / dt;
            r.alloc_mb_s[k] = (double)(ms.allocBytesTotal[k] - r.prev_alloc_b[k]) / dt / (1024.0 * 1024.0);
            r.free_mb_s[k] = (double)(ms.freeBytesTotal[k] - r.prev_free_b[k]) / dt / (1024.0 * 1024.0);
        }
        for (uint32_t c = 0; c < VKMEM_COUNT; ++c)
            r.cat_alloc_s[c] = (double)(ms.catAllocCalls[c] - r.prev_cat_alloc[c]) / dt;
        r.have_rate = true;
    }
    for (int k = 0; k < 2; ++k)
    {
        r.prev_alloc[k] = ms.allocCalls[k];
        r.prev_free[k] = ms.freeCalls[k];
        r.prev_alloc_b[k] = ms.allocBytesTotal[k];
        r.prev_free_b[k] = ms.freeBytesTotal[k];
    }
    for (uint32_t c = 0; c < VKMEM_COUNT; ++c) r.prev_cat_alloc[c] = ms.catAllocCalls[c];
    r.prev_t = now;
    r.have_prev = true;
    const bool high = r.have_rate && (r.alloc_s[1] > 20.0 || r.alloc_mb_s[1] > 64.0);
    if ((high || memTight) && now >= r.next_log)
    {
        r.next_log = now + 5.0;
        LL_INFOS("VKMem") << "hostmem" << (high ? " (high traffic)" : "") << (memTight ? " (memory tight)" : "")
                          << ": " << hostMemText() << LL_ENDL;
    }
}

std::string LLVulkanContext::texInspectText(const LLUUID& obj, int face, unsigned map, const float* uv_px) const
{
    if (!mImpl || !mImpl->mWorld) return std::string();
    return mImpl->mWorld->texInspectText(obj, face, map, uv_px);
}

std::string LLVulkanContext::texResidencyBrief(const LLUUID& id) const
{
    if (!mImpl || !mImpl->mWorld) return std::string();
    return mImpl->mWorld->texResidencyBrief(id);
}

// <GMDX> Ported from GunMetal-DX (squeedledorf, LGPL 2.1), v0.3.0 cf5f1f4 (rez effects).
bool LLVulkanContext::texOnGpu(const LLUUID& id) const
{
    if (!mImpl || !mImpl->mWorld) return true;
    return mImpl->mWorld->texOnGpu(id);
}
// </GMDX>

std::string LLVulkanContext::vramReadoutText() const
{
    if (!mImpl || !mImpl->mWorld) return "Video memory: not measured yet";
    return mImpl->mWorld->vramStatus().readoutText();
}

float LLVulkanContext::vramShadowRangeScale() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->vramShadowRangeScale() : 1.f;
}

float LLVulkanContext::vramMeshLodScale() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->vramMeshLodScale() : 1.f;
}

float LLVulkanContext::vramAvatarLodScale() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->vramAvatarLodScale() : 1.f;
}

float LLVulkanContext::vramTerrainLodScale() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->vramTerrainLodScale() : 1.f;
}

float LLVulkanContext::vramDrawDistance(float setting) const
{
    const float scale = (mImpl && mImpl->mWorld) ? mImpl->mWorld->vramDrawDistScale() : 1.f;
    return LLVramBudgetVK::effectiveDrawDistance(setting, scale);
}

bool LLVulkanContext::autoTuneSample(LLVKAutoTuneSample& out) const
{
    if (!mImpl || !mImpl->mWorld) { out.valid = false; return false; }
    mImpl->mWorld->autoTuneSample(out);
    return out.valid;
}

void LLVulkanContext::setAutoTunePerfCaps(const LLVKPerfCaps& caps)
{
    if (!mImpl) return;
    mImpl->mAutoTuneCaps = caps;
    // #1076: with the VR safety governor's caps (default: no cap -- merged() then returns
    // `caps` unchanged, as on the desktop where the governor never runs).
    if (mImpl->mWorld) mImpl->mWorld->setPerfCaps(LLVKPerfCaps::merged(caps, mImpl->mVrSafetyCaps));
}

void LLVulkanContext::setVrSafetyPerfCaps(const LLVKPerfCaps& caps)
{
    if (!mImpl) return;
    mImpl->mVrSafetyCaps = caps;
    if (mImpl->mWorld) mImpl->mWorld->setPerfCaps(LLVKPerfCaps::merged(mImpl->mAutoTuneCaps, caps));
}

uint32_t LLVulkanContext::shadowMapResolution() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->shadowMapResolution() : 0u;
}

uint32_t LLVulkanContext::shadowMapBorder() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->shadowMapBorder() : 0u;
}

uint64_t LLVulkanContext::freeDeviceMemory() const
{
    if (!mImpl) return 0;
    const VkDeviceContext::MemStats ms = mImpl->mDevice.memStats();
    if (ms.uma)
    {   // #695: the combined UMA budget (carve-out + shared host heap share) minus both heaps' usage
        const VkDeviceContext::BudgetHeaps bh = VkDeviceContext::budgetHeaps(ms);
        const uint64_t used = (ms.budgetExt ? ms.heapDriverUsage[bh.heap] + ms.heapDriverUsage[bh.hostHeap]
                                            : ms.heapUsed[bh.heap] + ms.heapUsed[bh.hostHeap]);
        return bh.budget > used ? bh.budget - used : 0;
    }
    uint64_t best = 0;
    for (uint32_t i = 0; i < ms.heapCount; ++i)
    {
        if (!ms.heapDeviceLocal[i]) continue;
        const uint64_t used = ms.budgetExt ? ms.heapDriverUsage[i] : ms.heapUsed[i];
        if (ms.heapBudget[i] > used) best = std::max<uint64_t>(best, ms.heapBudget[i] - used);
    }
    return best;
}

LLVulkanContext::SnapshotVram LLVulkanContext::snapshotVram() const
{
    SnapshotVram s;
    if (!mInitialized || !mImpl) return s;
    s.usable = freeDeviceMemory();
    if (mImpl->mWorld)
    {
        // The renderer's own budget (user cap / OS budget less its reserve): a snapshot that
        // fits the OS budget but not this one starts the ladder shedding mid-render.
        const LLVramStatus& v = mImpl->mWorld->vramStatus();
        if (v.valid && v.budget) s.usable = std::min<uint64_t>(s.usable, v.budget > v.usage ? v.budget - v.usage : 0);
        s.captureBytesPerPixel = mImpl->mWorld->displayOutputState().colorFormat == VK_FORMAT_R16G16B16A16_SFLOAT ? 8u : 4u;
    }
    const VkDeviceContext::MemStats ms = mImpl->mDevice.memStats();
    s.liveTargetBytes = ms.catDev[VKMEM_TARGET] + ms.catDev[VKMEM_RT_MASK] + ms.catDev[VKMEM_DENOISE];
    const VkExtent2D e = mImpl->mSwapchain.extent();
    s.livePx = (uint64_t)e.width * e.height;
    return s;
}

bool LLVulkanContext::latestPick(LLVKPickResult& out) const
{
    return mImpl && mImpl->mWorld && mImpl->mWorld->pickLatest(out);
}

// <SS:Skoomastream> aux rays (sscombatray.cpp). Ported from GunMetal-DX (squeedledorf,
// LGPL 2.1), v0.3.0 cf5f1f4.
void LLVulkanContext::takeAuxPicks(std::vector<LLVKPickResult>& out)
{
    if (mImpl && mImpl->mWorld) mImpl->mWorld->takeAuxPicks(out);
}

void LLVulkanContext::pollPickAnswers()
{
    // (#748 static pass) the answer of a frame in flight that has completed already, now -- not
    // when its slot comes round (a held click's answer a frame sooner; llcleancursorpick.h)
    if (!mImpl || !mImpl->mWorld || vkDeviceIsLost()) return;
    VK_HITCH_SCOPE("pick_harvest");   // (#757 crash breadcrumb)
    const VkDevice dev = mImpl->mDevice.device();
    for (uint32_t s = 0; s < mImpl->mSwapchain.framesInFlight(); ++s)
        if (mImpl->mSwapchain.slotDone(dev, s)) mImpl->mWorld->pickHarvestDone(s);
}

bool LLVulkanContext::pickNow(const LLVKPickRequest& req, LLVKPickResult& out)
{
    if (vkDeviceIsLost()) return false;   // #135
    return mImpl && mImpl->mWorld && mImpl->mWorld->ready() && mImpl->mWorld->pickNow(req, out);
}

bool LLVulkanContext::faceQuery(const std::vector<uint32_t>& instance_ids, const LLVKRectQuery* rect,
                                std::vector<LLVKFaceQueryResult>& out)
{
    if (vkDeviceIsLost()) return false;   // #135
    return mImpl && mImpl->mWorld && mImpl->mWorld->ready() && mImpl->mWorld->faceQuery(instance_ids, rect, out);
}

const char* LLVulkanContext::pickFailure() const
{
    if (vkDeviceIsLost()) return "device lost";
    if (!mImpl || !mImpl->mWorld) return "no world renderer";
    return mImpl->mWorld->pickFailure();
}

bool LLVulkanContext::pickGpuAvailable() const
{
    return !vkDeviceIsLost() && mImpl && mImpl->mWorld && mImpl->mWorld->ready() && mImpl->mWorld->pickAvailable();
}

bool LLVulkanContext::latestPartPick(LLVKPartPickResult& out) const
{
    return mImpl && mImpl->mWorld && mImpl->mWorld->partPickLatest(out);
}

bool LLVulkanContext::latestAcoustics(LLVKAcousticsResult& out) const
{
    return mImpl && mImpl->mWorld && mImpl->mWorld->acousticsLatest(out);
}

bool LLVulkanContext::latestAcousticsBounce(LLVKAcousticsBounce& out, uint32_t seen) const
{
    return mImpl && mImpl->mWorld && mImpl->mWorld->acousticsBounceLatest(out, seen);
}

uint32_t LLVulkanContext::acousticsBounceDispatched() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->acousticsBounceDispatched() : 0u;
}

uint32_t LLVulkanContext::worldViewCount() const
{
    return (mImpl && mImpl->mWorld) ? mImpl->mWorld->viewCount() : 1u;
}

bool LLVulkanContext::partPickNow(const LLVKPickRequest& req, LLVKPartPickResult& out)
{
    if (vkDeviceIsLost()) return false;   // #135
    return mImpl && mImpl->mWorld && mImpl->mWorld->ready() && mImpl->mWorld->partPickNow(req, out);
}

// #1074 VR: the eye-size world target. The window format (the final / UI pipelines are
// compiled for it, and its bytes are the eye images' UNORM sibling); kept while the session
// runs, so the renderer's screen targets keep one size (no target churn).
bool LLVulkanContext::Impl::vrEnsureTarget(uint32_t w, uint32_t h)
{
    const VkFormat fmt = mSwapchain.format();
    if (!w || !h || fmt == VK_FORMAT_UNDEFINED) return false;
    // #1075 LAYERED STEREO: one layer per view the world renders (the eyes; 1 = M1 mono).
    const uint32_t layers = mWorld ? std::max(1u, mWorld->viewCount()) : 1u;
    if (mVrTarget.image && mVrTarget.extent.width == w && mVrTarget.extent.height == h && mVrTarget.format == fmt &&
        mVrTarget.layers == layers)
        return true;
    if (mVrTarget.image) vrReleaseTarget(true);   // (another size / format: rare)
    if (!mVrBlitChecked)
    {
        mVrBlitChecked = true;
        VkFormatProperties fp{};
        vkGetPhysicalDeviceFormatProperties(mDevice.physicalDevice(), fmt, &fp);
        const VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
        mVrBlitOk = (ff & VK_FORMAT_FEATURE_BLIT_SRC_BIT) && (ff & VK_FORMAT_FEATURE_BLIT_DST_BIT);
        mVrBlitFilter = (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
        if (!mVrBlitOk)
            LL_WARNS("VR") << "the window format cannot be blitted: the desktop window shows no mirror of the headset" << LL_ENDL;
    }
    {
        VkMemScope memcat(VKMEM_TARGET);
        // Soft: out of memory leaves VR without the world for now (logged), never fatal.
        VkDeviceContext::SoftAllocScope soft(&mDevice);
        // (#1075: layers 1 is exactly the M1 createImage2D target)
        mVrTarget = mDevice.createScreenImage(w, h, layers, fmt,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (!mVrTarget.image)
    {
        static uint32_t s_warned = 0;
        if (s_warned++ < 5) LL_WARNS("VR") << "could not allocate the " << w << "x" << h << " world target for the headset" << LL_ENDL;
        mVrTarget = GpuImage{};
        return false;
    }
    LL_INFOS("VR") << "world target for the headset: " << w << "x" << h << " x" << layers
                   << (layers > 1 ? " layers (stereo: one per eye)" : "")
                   << " (the world renders at the eye size while the session runs)" << LL_ENDL;
    return true;
}

void LLVulkanContext::Impl::vrReleaseTarget(bool waitIdle)
{
    // #1594 (nothing unless space warp ran this session)
    mSpaceWarp.release(mDevice);
    LLVRSpaceWarp::instance().dropHistory("the headset's target was released");
    mVrUiFresh = false;
    if ((!mVrTarget.image && !mVrUiTarget.image && !mVrHudDepth.image) || vkDeviceIsLost()) return;
    if (waitIdle && mDevice.device()) vkDeviceWaitIdle(mDevice.device());
    if (mVrTarget.image) mDevice.destroyImage(mVrTarget);
    mVrTarget = GpuImage{};
    if (mVrUiTarget.image) mDevice.destroyImage(mVrUiTarget);   // #1076
    mVrUiTarget = GpuImage{};
    if (mVrHudDepth.image) mDevice.destroyImage(mVrHudDepth);
    mVrHudDepth = GpuImage{};
}

// #1076 VR: the HUD pass's depth for HUDs drawn onto the UI wall image (its size, the world
// depth format the HUD pipelines are compiled for). Recreated only when the window size changes.
bool LLVulkanContext::Impl::vrEnsureHudDepth(VkExtent2D ext, VkFormat fmt)
{
    if (!ext.width || !ext.height || fmt == VK_FORMAT_UNDEFINED) return false;
    if (mVrHudDepth.image && mVrHudDepth.extent.width == ext.width && mVrHudDepth.extent.height == ext.height &&
        mVrHudDepth.format == fmt)
        return true;
    if (mVrHudDepth.image)
    {
        if (mDevice.device()) vkDeviceWaitIdle(mDevice.device());   // (a window resize: rare)
        mDevice.destroyImage(mVrHudDepth);
        mVrHudDepth = GpuImage{};
    }
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(&mDevice);   // out of memory: no HUDs on the wall for now
        mVrHudDepth = mDevice.createImage2D(ext.width, ext.height, fmt, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                                            VK_IMAGE_ASPECT_DEPTH_BIT);
    }
    if (!mVrHudDepth.image)
    {
        mVrHudDepth = GpuImage{};
        return false;
    }
    return true;
}

// #1076 VR: the UI wall image at the window size (the UI is laid out in window pixels), in the
// window format (the UI pipelines are compiled for it; its bytes are the UI swapchain's UNORM
// sibling). Recreated only when the window size changes.
bool LLVulkanContext::Impl::vrEnsureUiTarget(VkExtent2D ext)
{
    const VkFormat fmt = mSwapchain.format();
    if (!ext.width || !ext.height || fmt == VK_FORMAT_UNDEFINED) return false;
    if (mVrUiTarget.image && mVrUiTarget.extent.width == ext.width && mVrUiTarget.extent.height == ext.height &&
        mVrUiTarget.format == fmt)
        return true;
    if (mVrUiTarget.image)
    {
        if (mDevice.device()) vkDeviceWaitIdle(mDevice.device());   // (a window resize: rare)
        mDevice.destroyImage(mVrUiTarget);
        mVrUiTarget = GpuImage{};
    }
    {
        VkMemScope memcat(VKMEM_TARGET);
        VkDeviceContext::SoftAllocScope soft(&mDevice);   // out of memory: no wall content for now, never fatal
        mVrUiTarget = mDevice.createImage2D(ext.width, ext.height, fmt,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_IMAGE_ASPECT_COLOR_BIT);
    }
    if (!mVrUiTarget.image)
    {
        static uint32_t s_warned = 0;
        if (s_warned++ < 5) LL_WARNS("VR") << "could not allocate the " << ext.width << "x" << ext.height << " UI wall image" << LL_ENDL;
        mVrUiTarget = GpuImage{};
        return false;
    }
    LL_INFOS("VR") << "UI wall image: " << ext.width << "x" << ext.height << LL_ENDL;
    return true;
}

// After the world (and its captures): the target, in COLOR_ATTACHMENT_OPTIMAL, blitted
// aspect-fit (centred, bars in the window's clear colour) onto the window image, which is in
// COLOR_ATTACHMENT_OPTIMAL; both are left there. Same-format UNORM blit: the sRGB-encoded bytes
// are scaled, never re-encoded. A null windowImage (no window image this frame): nothing.
void LLVulkanContext::Impl::vrRecordMirror(VkCommandBuffer cmd, VkImage windowImage, VkExtent2D windowExtent)
{
    if (!windowImage || !mVrTarget.image || !mVrBlitOk || !windowExtent.width || !windowExtent.height) return;
    const uint32_t sw = mVrTarget.extent.width, sh = mVrTarget.extent.height;
    const float fit = std::min((float)windowExtent.width / (float)sw, (float)windowExtent.height / (float)sh);
    const int32_t dw = std::max(1, (int32_t)((float)sw * fit)), dh = std::max(1, (int32_t)((float)sh * fit));
    const int32_t dx = ((int32_t)windowExtent.width - dw) / 2, dy = ((int32_t)windowExtent.height - dh) / 2;
    // (#1075: a stereo target's every layer moves together; the mirror shows layer 0, the left eye)
    imageBarrier(cmd, mVrTarget.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                     VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, mVrTarget.layers);
    imageBarrier(cmd, windowImage, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageBlit rg{};
    rg.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.srcOffsets[1] = {(int32_t)sw, (int32_t)sh, 1};
    rg.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rg.dstOffsets[0] = {dx, dy, 0};
    rg.dstOffsets[1] = {dx + dw, dy + dh, 1};
    vkCmdBlitImage(cmd, mVrTarget.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, windowImage,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg, mVrBlitFilter);
    imageBarrier(cmd, windowImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_BLIT_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    imageBarrier(cmd, mVrTarget.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                 VK_PIPELINE_STAGE_2_BLIT_BIT, 0,
                 VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT,
                 VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_READ_BIT,
                 mVrTarget.layers);
}

// #1594 VR SPACE WARP: this frame's warp of the newest real frame to its own display-time eyes,
// submitted now -- its own command buffer and fence, then xrEndFrame -- ahead of anything else
// this frame puts on the queue (a real frame's world follows it, never precedes it). The eye
// copies and the UI layers are LLXrSession::recordFrame's (warped: declared at this frame's
// located poses, no depth layer). False: nothing submitted, the XR frame is still open (the
// frame then renders and shows the world as without space warp).
bool LLVulkanContext::Impl::vrSpaceWarpSubmit(const LLVKWorldFrame* world, bool warpOnly)
{
    LLVRSpaceWarp& pol = LLVRSpaceWarp::instance();
    if (!world || !world->warp_valid || !world->views_are_eyes || !mSpaceWarp.hasHistory() || !pol.historyValid())
        return false;
    LLXrHeadPose hp;
    if (!LLXrSession::instance().headPose(hp)) return false;
    // A cut (teleport, region change, recentre, a sit jump): the history's eyes are elsewhere. The
    // frame renders the world instead (a fresh history).
    {
        const float* ps = mSpaceWarp.historyEye(0).pos;
        const float* pt = world->warp_views[0].eye;
        const float dx = pt[0] - ps[0], dy = pt[1] - ps[1], dz = pt[2] - ps[2];
        const float jump = std::sqrt(dx * dx + dy * dy + dz * dz);
        const double ageMs = (double)(hp.displayTime - pol.historyRenderNs()) * 1e-6;
        if (!(jump < 4.f) || ageMs > 250.0 || ageMs < -50.0)
        {
            LL_INFOS("VR") << "space warp: no warp from a real frame " << ageMs << " ms old and " << jump
                           << " m away (a cut): the world renders" << LL_ENDL;
            pol.dropHistory("a cut");
            mSpaceWarp.dropHistory();
            return false;
        }
    }
    LLVRSpaceWarpVK::WarpIn in;
    for (uint32_t v = 0; v < LLVRSpaceWarpVK::kEyes; ++v)
    {
        const LLVKViewFrame& vf = world->warp_views[v];
        std::memcpy(in.eyes[v].view, vf.view, sizeof(in.eyes[v].view));
        std::memcpy(in.eyes[v].proj, vf.proj, sizeof(in.eyes[v].proj));
        for (int k = 0; k < 3; ++k) in.eyes[v].pos[k] = vf.eye[k];
    }
    for (uint32_t h = 0; h < 2; ++h)
    {
        LLVRSpaceWarpVK::Hand& o = in.hands[h];
        o.valid = world->warp_hand_valid[h];
        std::memcpy(o.pos, world->warp_hand_pos[h], sizeof(o.pos));
        std::memcpy(o.axis, world->warp_hand_axis[h], sizeof(o.axis));
        std::memcpy(o.beamFrom, world->warp_beam_from[h], sizeof(o.beamFrom));
        std::memcpy(o.beamTo, world->warp_beam_to[h], sizeof(o.beamTo));
    }
    // Object motion: the source frame's motion vectors span the gap to the real frame before it; the
    // fraction of that gap since the source frame's own (camera) time carries moving content forward.
    const int64_t gap = pol.historyRenderNs() - pol.historyPrevRenderNs();
    in.extrapolate = (gap > 0 && gap < 200000000ll && pol.historyPrevRenderNs() > 0)
                   ? (float)((double)(hp.displayTime - pol.historyRenderNs()) / (double)gap) : 0.f;
    const uint32_t dbg = pol.debugView();
    in.debug = (dbg == 2u && !warpOnly) ? 1u : dbg;
    in.swapRB = LLXrSession::instance().eyesBgra();
    const VkImage ui = (mVrUiFresh && LLXrSession::instance().uiImageWanted() && mVrUiTarget.image) ? mVrUiTarget.image
                                                                                                   : VK_NULL_HANDLE;
    const VkFormat winFmt = mSwapchain.format();
    const VkExtent2D winExt = mSwapchain.extent();
    const bool ok = mSpaceWarp.warpAndSubmit(mDevice, mDevice.queue(), in,
        [ui, winFmt, winExt](VkCommandBuffer c, VkImage out, VkExtent2D ext, uint32_t layers)
        {
            LLXrSession::instance().recordFrame(c, VK_NULL_HANDLE, winFmt, winExt, out, VK_FORMAT_R8G8B8A8_UNORM, ext, ui,
                                                layers, nullptr, true);
        });
    if (!ok)
    {
        pol.noteWarp(false, -1.f);
        return false;
    }
    LLXrSession::instance().endFrame();   // (the warp's submit is the frame's GPU work for the runtime)
    pol.noteWarp(true, mSpaceWarp.takeGpuMs());
    return true;
}

// #1594 VR SPACE WARP: a real frame's world (the eye target in TRANSFER_SRC, its depth and motion,
// the eyes it was rendered from, the controllers as drawn) into the warp's history, at the tail of
// the frame's last graphics command buffer.
void LLVulkanContext::Impl::vrSpaceWarpCapture(VkCommandBuffer cmd, const LLVKWorldFrame* world, VkImage worldImg,
                                               VkExtent2D worldExt, uint32_t worldLayers)
{
    LLVRSpaceWarp& pol = LLVRSpaceWarp::instance();
    LLVRSpaceWarpVK::CaptureIn in;
    in.color = worldImg;
    in.colorFormat = mVrTarget.format;
    in.colorExtent = worldExt;
    in.layers = worldLayers;
    // (RGBA16F: LLCleanRendererVK::kMotionFormat)
    in.motionFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool ok = world && world->views_are_eyes && world->view_count >= LLVRSpaceWarpVK::kEyes && mWorld &&
                    mWorld->warpInputs(in.depth, in.depthFormat, in.depthLayout, in.depthExtent, in.motion,
                                       in.motionLayout, in.motionExtent, in.motionValid);
    if (!ok)
    {
        pol.noteCaptured(false);
        mSpaceWarp.dropHistory();
        return;
    }
    for (uint32_t v = 0; v < LLVRSpaceWarpVK::kEyes; ++v)
    {
        const LLVKViewFrame& vf = world->views[v];
        std::memcpy(in.eyes[v].view, vf.view, sizeof(in.eyes[v].view));
        std::memcpy(in.eyes[v].proj, vf.proj, sizeof(in.eyes[v].proj));
        for (int k = 0; k < 3; ++k) in.eyes[v].pos[k] = vf.eye[k];
        std::memcpy(in.camPrev[v], mWorld->taaReproj(v), sizeof(in.camPrev[v]));
    }
    for (uint32_t h = 0; h < 2; ++h)
    {
        LLVRSpaceWarpVK::Hand& o = in.hands[h];
        o.valid = world->warp_valid && world->warp_hand_valid[h];
        std::memcpy(o.pos, world->warp_hand_pos[h], sizeof(o.pos));
        std::memcpy(o.axis, world->warp_hand_axis[h], sizeof(o.axis));
        std::memcpy(o.beamFrom, world->warp_beam_from[h], sizeof(o.beamFrom));
        std::memcpy(o.beamTo, world->warp_beam_to[h], sizeof(o.beamTo));
        const float bx = o.beamTo[0] - o.beamFrom[0], by = o.beamTo[1] - o.beamFrom[1], bz = o.beamTo[2] - o.beamFrom[2];
        o.beam = o.valid && (bx * bx + by * by + bz * bz) > 1e-4f;
    }
    pol.noteCaptured(mSpaceWarp.capture(mDevice, cmd, in));
}

NativeSurfaceId LLVulkanContext::createUIWindow(SDL_Window* window, NativeSurfaceId surfaceID)
{
    if (!surfaceID || surfaceID >= NativeSurfaceVRUI || !mInitialized || !mImpl ||
        !mImpl->mFacadeReady || vkDeviceIsLost()) return 0;
    for (const auto& existing : mImpl->mUIWindows)
        if (existing && existing->surfaceID == surfaceID) return 0;
    size_t index = 0;
    while (index < mImpl->mUIWindows.size() && mImpl->mUIWindows[index]) ++index;
    if (index == 16) return 0;
    int pixelWidth = 0, pixelHeight = 0;
    SDL_GetWindowSizeInPixels(window, &pixelWidth, &pixelHeight);
    uint64_t estimate = uint64_t(std::max(0, pixelWidth)) * std::max(0, pixelHeight) * 16;
    for (const auto& existing : mImpl->mUIWindows) if (existing) estimate += existing->swapchain.memoryBytes();
    if (estimate > 256ull * 1024 * 1024) return 0; // includes draining windows; WSI estimate, not driver residency
    try { mImpl->mUIWindows.reserve(16); }
    catch (const std::bad_alloc&) { return 0; }
    auto entry = std::unique_ptr<Impl::UIWindow>(new (std::nothrow) Impl::UIWindow);
    if (!entry) return 0;
    entry->window = window;
    entry->surfaceID = surfaceID;
    auto& device = mImpl->mDevice;
    if (!SDL_Vulkan_CreateSurface(window, device.instance(), nullptr, &entry->surface)) return 0;
    if (!mImpl->mUIPathLogged)
    {
        LL_INFOS("NativeUI") << "Auxiliary presentation retirement: "
            << (device.swapchainMaintenance() ? "present fences" : "presenting-queue idle fallback") << LL_ENDL;
        mImpl->mUIPathLogged = true;
    }
    // Install before any fallible GPU allocation so partial initialization uses
    // the same teardown as a normal close.
    if (index == mImpl->mUIWindows.size()) mImpl->mUIWindows.emplace_back();
    mImpl->mUIWindows[index] = std::move(entry);
    auto& w = *mImpl->mUIWindows[index];
    if (!mImpl->mUIRetirement)
    {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        ci.pNext = &type;
        if (vkCreateSemaphore(device.device(), &ci, nullptr, &mImpl->mUIRetirement) != VK_SUCCESS)
        {
            destroyUIWindow(surfaceID);
            return 0;
        }
        mImpl->mUIRetirementSignaled = 0;
        gVK.setExternalRetirement(true);
    }
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(window, &width, &height);
    w.swapchain.setUIOnly();
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = device.queueFamily();
    bool ready = false;
    try
    {
        ready = width > 0 && height > 0 &&
            w.swapchain.init(device, w.surface, width, height, 2) &&
            vkCreateCommandPool(device.device(), &pci, nullptr, &w.pool) == VK_SUCCESS;
    }
    catch (const std::bad_alloc&) { ready = false; }
    if (ready)
    {
        VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ai.commandPool = w.pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 2;
        ready = vkAllocateCommandBuffers(device.device(), &ai, w.cmds) == VK_SUCCESS;
    }
    if (!ready)
    {
        LL_WARNS("Popout") << "Could not allocate native UI presentation resources" << LL_ENDL;
        destroyUIWindow(surfaceID);
        return 0;
    }
    w.requestedWidth = width;
    w.requestedHeight = height;
    unsigned live = 0;
    uint64_t bytes = 0;
    for (const auto& item : mImpl->mUIWindows)
        if (item) { ++live; bytes += item->swapchain.memoryBytes(); }
    mImpl->mUIWindowPeak = std::max(mImpl->mUIWindowPeak, live);
    LL_INFOS("NativeUI") << "Native presentation live=" << live << " capacity=16 peak=" << mImpl->mUIWindowPeak
        << " estimatedWSIBytes=" << bytes << LL_ENDL;
    return surfaceID;
}

U64 LLVulkanContext::uiFrameCounter() const
{
    return mImpl && mImpl->mFacadeReady ? gVK.frameCounter() : 0;
}

bool LLVulkanContext::uiWindowFailed(NativeSurfaceId handle) const
{
    if (!mImpl || !handle) return true;
    for (const auto& w : mImpl->mUIWindows)
        if (w && w->surfaceID == handle) return w->closing || w->failed || w->swapchain.failed();
    return true;
}

void LLVulkanContext::invalidateUIWindow(NativeSurfaceId handle)
{
    if (mImpl) for (auto& window : mImpl->mUIWindows)
        if (window && window->surfaceID == handle) window->resize = true;
}

void LLVulkanContext::destroyUIWindow(NativeSurfaceId handle, std::unique_ptr<LLNativeWindow> host)
{
    if (!mImpl || !handle) return;
    size_t index = 0;
    while (index < mImpl->mUIWindows.size() &&
           (!mImpl->mUIWindows[index] || mImpl->mUIWindows[index]->surfaceID != handle)) ++index;
    if (index == mImpl->mUIWindows.size()) return;
    auto& entry = mImpl->mUIWindows[index];
    entry->closing = true;
    if (host)
    {
        host->attach(nullptr);
        SDL_HideWindow(host->handle());
        entry->retiringHost = std::move(host);
    }
    // Partial init has no pending work, so can be released immediately. Active
    // windows retain their native host/surface and slot until both proofs retire.
    auto& device = mImpl->mDevice;
    if (!vkDeviceIsLost() && entry->swapchain.retired(device.device()))
    {
        entry->swapchain.destroy(device);
        if (entry->pool) vkDestroyCommandPool(device.device(), entry->pool, nullptr);
        if (mImpl->mFacadeReady) gVK.releaseFrameSlots(2 + static_cast<U32>(index) * 2, 2);
        SDL_Vulkan_DestroySurface(device.instance(), entry->surface, nullptr);
        entry.reset(); // native window LAST
    }
}

void LLVulkanContext::pollUIWindowRetirement()
{
    if (!mImpl || vkDeviceIsLost()) return;
    auto& device = mImpl->mDevice;
    // Approved compatibility policy: queue-idle is the conventional WSI release
    // assumption on drivers without present fences, not a PE scanout proof.
    // The approved compatibility path batches rare retirement events. Never on
    // a steady frame; never waits the device or any other queue.
    unsigned drain = 0;
    for (const auto& window : mImpl->mUIWindows)
        if (window && (window->closing || window->resize) && window->swapchain.needsPresentDrain()) ++drain;
    const auto retryNow = std::chrono::steady_clock::now();
    if (drain && mImpl->mUIRetireRetry.ready(retryNow))
    {
        const auto start = std::chrono::steady_clock::now();
        const VkResult result = vkQueueWaitIdle(device.queue());
        const auto elapsed = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        LL_INFOS("NativeUI") << "Auxiliary retirement queue=" << device.queue() << " batch=" << drain
            << " reason=close/resize/out-of-date wait=" << elapsed << " ms result=" << result << LL_ENDL;
        if (result == VK_SUCCESS)
        {
            mImpl->mUIRetireRetry.succeeded();
            for (auto& window : mImpl->mUIWindows) if (window) window->swapchain.presentingQueueIdle();
        }
        else if (result == VK_ERROR_DEVICE_LOST)
            vkDeviceLostRaise("auxiliary retirement queue wait", result, "graphics", __FILE__, __LINE__);
        else
        {
            mImpl->mUIRetireRetry.refused(retryNow);
            if (mImpl->mUIRetireRetry.exhausted())
                LL_WARNS("NativeUI") << "Auxiliary queue retirement refused three times; retaining bounded windows until exit" << LL_ENDL;
        }
    }
    for (U32 i = 0; i < mImpl->mUIWindows.size(); ++i)
        if (mImpl->mUIWindows[i] && mImpl->mUIWindows[i]->closing) destroyUIWindow(mImpl->mUIWindows[i]->surfaceID);
    if (!mImpl->mUIRetirement) return;
    uint64_t completed = 0;
    const VkResult result = vkGetSemaphoreCounterValue(device.device(), mImpl->mUIRetirement, &completed);
    if (result == VK_SUCCESS)
    {
        mImpl->mUICompleted = completed;
        gVK.setExternalRetirement(true, completed);
        if (completed >= mImpl->mUIRetirementSignaled &&
            std::none_of(mImpl->mUIWindows.begin(), mImpl->mUIWindows.end(), [](const auto& w) { return bool(w); }))
        {
            vkDestroySemaphore(device.device(), mImpl->mUIRetirement, nullptr);
            mImpl->mUIRetirement = VK_NULL_HANDLE;
            gVK.setExternalRetirement(false);
        }
    }
    else if (result == VK_ERROR_DEVICE_LOST)
        vkDeviceLostRaise("pop-out retirement poll", result, "graphics", __FILE__, __LINE__);
}

void LLVulkanContext::finishUIWindowsFrame()
{
    if (!mImpl || !mImpl->mUIRetirement || vkDeviceIsLost()) return;
    if (std::none_of(mImpl->mUIWindows.begin(), mImpl->mUIWindows.end(), [](const auto& w) { return bool(w); })) return;
    const uint64_t frame = gVK.frameCounter();
    if (frame <= mImpl->mUIRetirementSignaled) return;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = mImpl->mUIRetirement;
    signal.value = frame;
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.signalSemaphoreInfoCount = 1;
    submit.pSignalSemaphoreInfos = &signal;
    const VkResult result = vkQueueSubmit2(mImpl->mDevice.queue(), 1, &submit, VK_NULL_HANDLE);
    if (result == VK_SUCCESS) mImpl->mUIRetirementSignaled = frame;
    else if (result == VK_ERROR_DEVICE_LOST)
        vkDeviceLostRaise("pop-out retirement submit", result, "graphics", __FILE__, __LINE__);
    else
    {
        // Do not let a refused completion marker pin an ever-growing deferred
        // texture queue. Dock at the next UI boundary; retain completion markers
        // until the GPU proves retirement before restoring the single-window policy.
        LL_WARNS("Popout") << "UI completion marker refused: " << vkResultString(result)
                           << "; docking native windows" << LL_ENDL;
        for (auto& window : mImpl->mUIWindows) if (window) window->failed = true;
    }
}

bool LLVulkanContext::renderUIWindow(NativeSurfaceId handle, const std::function<void()>& draw, bool advanceFrame)
{
    if (!mImpl || !mImpl->mFacadeReady || vkDeviceIsLost() || !handle) return false;
    size_t index = 0;
    while (index < mImpl->mUIWindows.size() &&
           (!mImpl->mUIWindows[index] || mImpl->mUIWindows[index]->surfaceID != handle)) ++index;
    if (index == mImpl->mUIWindows.size()) return false;
    auto& w = *mImpl->mUIWindows[index];
    auto& device = mImpl->mDevice;
    auto& sc = w.swapchain;
    if (w.closing || w.failed || sc.failed()) return false;
    int width = 0, height = 0;
    SDL_GetWindowSizeInPixels(w.window, &width, &height);
    if (width <= 0 || height <= 0 || (SDL_GetWindowFlags(w.window) & SDL_WINDOW_MINIMIZED)) return false;
    if (w.resize || w.requestedWidth != width || w.requestedHeight != height)
    {
        w.resize = true;
        uint64_t estimate = uint64_t(width) * height * 16;
        for (const auto& existing : mImpl->mUIWindows)
            if (existing && existing.get() != &w) estimate += existing->swapchain.memoryBytes();
        if (estimate > 256ull * 1024 * 1024) { w.failed = true; return false; }
        if (!sc.retired(device.device())) return false;
        const auto now = std::chrono::steady_clock::now();
        if (now < w.retryAfter) return false;
        try { w.resize = !sc.recreate(device, width, height); }
        catch (const std::bad_alloc&) { w.failed = true; return false; }
        if (w.resize)
        {
            w.retryAfter = now + std::chrono::milliseconds(500);
            w.failed = ++w.recreateFailures >= 3;
            return false;
        }
        w.recreateFailures = 0;
        w.requestedWidth = width;
        w.requestedHeight = height;
    }
    width = static_cast<int>(sc.extent().width);
    height = static_cast<int>(sc.extent().height);
    // Never wait for an idle/background window's compositor or GPU slot.
    if (!sc.slotDone(device.device(), sc.frameSlot())) return false;
    const U32 slot = sc.frameSlot();
    const U32 vertexSlot = 2 + static_cast<U32>(index) * 2 + slot;
    try
    {
        if (!mImpl->mAuxUIRecorder)
        {
            gVK.beginFrameToView(VK_NULL_HANDLE, sc.format(), width, height);
            gVK.setWorldOverlayDepthFormat(VK_FORMAT_UNDEFINED);
            draw();
            if (!gVK.prepareBufferedFrame(vertexSlot)) w.failed = true;
        }
    }
    catch (const std::bad_alloc&) { w.failed = true; }
    if (w.failed)
    {
        gVK.discardBufferedFrame();
        return false;
    }
    if (!mImpl->ensureSurfaceCompletion()) return false;
    const uint64_t targetSerial = mImpl->reserveSurfaceSerial();
    U32 imageIndex = 0;
    bool recreate = false;
    if (!sc.acquire(device, &imageIndex, &recreate))
    {
        w.resize |= recreate;
        return false;
    }
    w.resize |= recreate;
    if (!mImpl->mAuxUIRecorder) gVK.setFrameColorView(sc.imageView(imageIndex));
    VkCommandBuffer cmd = w.cmds[slot];
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    beginTexturePrepRecording();
    VkResult result = vkBeginCommandBuffer(cmd, &bi);
    if (result == VK_SUCCESS)
    try
    {
        if (!mImpl->mAuxUIRecorder)
        {
            gVK.recordUploads(cmd);
            gVK.recordOffscreen(cmd, vertexSlot);
        }
        VkImageMemoryBarrier2 barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barrier.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        barrier.image = sc.image(imageIndex);
        barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 1;
        dep.pImageMemoryBarriers = &barrier;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = sc.imageView(imageIndex);
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.clearValue.color = {{0.12f, 0.12f, 0.12f, 1.f}};
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea.extent = sc.extent();
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &ri);
        vkCmdEndRendering(cmd);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier2(cmd, &dep);
        if (mImpl->mAuxUIRecorder)
        {
            VkUISurfaceTarget target;
            target.surfaceID = w.surfaceID;
            target.cmd = cmd;
            target.colorImage = sc.image(imageIndex);
            target.colorView = sc.imageView(imageIndex);
            target.colorFormat = sc.format();
            target.extent = sc.extent();
            target.dpiScaleX = target.dpiScaleY = SDL_GetWindowDisplayScale(w.window);
            target.frameSerial = targetSerial;
            target.completedSerial = mImpl->mSurfaceCompleted;
            (*mImpl->mAuxUIRecorder)(target);
        }
        else gVK.recordFrame(cmd, vertexSlot, advanceFrame);
        barrier.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
        barrier.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
        barrier.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
        barrier.dstAccessMask = 0;
        barrier.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        vkCmdPipelineBarrier2(cmd, &dep);
        std::array<LLRenderVK::PreparedUploadToken, Impl::kTexturePrepRecordCap> prepared{};
        const uint32_t preparedCount = gVK.takePreparedRecordedUploads(prepared.data(), uint32_t(prepared.size()));
        if (preparedCount > prepared.size()) (void)recordTexturePrepTicket(0xffffu, 0);
        else for (uint32_t i = 0; i < preparedCount; ++i)
            (void)recordTexturePrepTicket(prepared[i].slot, prepared[i].generation);
        const VkResult endResult = vkEndCommandBuffer(cmd);
        result = texturePrepRecordingReady() ? endResult : VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    catch (const std::bad_alloc&)
    {
        gVK.discardBufferedFrame();
        result = VK_ERROR_OUT_OF_HOST_MEMORY;
    }
    VkSemaphoreSubmitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    wait.semaphore = sc.imageAvailableSemaphore();
    wait.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkSemaphoreSubmitInfo signal{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    signal.semaphore = sc.renderFinishedSemaphore(imageIndex);
    signal.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    VkCommandBufferSubmitInfo cb{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cb.commandBuffer = cmd;
    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = submit.signalSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &wait;
    submit.pSignalSemaphoreInfos = &signal;
    if (result != VK_SUCCESS)
    {
        texturePrepRecordingDropped(true); // no auxiliary submit was attempted
        w.failed = true; // acquire fence retires the unconsumed acquire semaphore
        if (result == VK_ERROR_DEVICE_LOST) vkDeviceLostRaise("native UI command recording", result, "graphics", __FILE__, __LINE__);
        return false;
    }
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cb;
    result = vkQueueSubmit2(device.queue(), 1, &submit, sc.inFlightFence());
    if (result != VK_SUCCESS)
    {
        texturePrepRecordingDropped(false); // submit result did not prove source retirement
        w.failed = true;
        if (result == VK_ERROR_DEVICE_LOST) vkDeviceLostRaise("native UI submit", result, "graphics", __FILE__, __LINE__);
        return false;
    }
    const bool hadPreparedUploads = mImpl->mTexturePrepRecordCount != 0;
    texturePrepRecordingSubmitted(UINT32_MAX); // common-queue timeline proof, never a main slot fence
    if (hadPreparedUploads)
    {
        mImpl->mTexturePrepSurfaceProof = targetSerial;
        mImpl->mTexturePrepSurfaceSubmit = mImpl->mTexturePrepSubmitSerial;
    }
    sc.submitted();
    mImpl->signalSurfaceSerial(targetSerial);
    sc.present(device, imageIndex, &recreate);
    w.resize |= recreate;
    return true;
}

bool LLVulkanContext::setMainUIRecorder(VkUISurfaceRecorder recorder)
{
    if (!mImpl || (recorder && !mImpl->ensureSurfaceCompletion())) return false;
    mImpl->mMainUIRecorder = std::move(recorder);
    return true;
}

U64 LLVulkanContext::completedUISurfaceSerial()
{
    if (!mImpl || !mImpl->mSurfaceCompletion || vkDeviceIsLost()) return 0;
    uint64_t value = 0;
    const VkResult result = vkGetSemaphoreCounterValue(mImpl->mDevice.device(), mImpl->mSurfaceCompletion, &value);
    if (result == VK_SUCCESS) mImpl->observeSurfaceCompletion(value);
    else if (result == VK_ERROR_DEVICE_LOST)
        vkDeviceLostRaise("UI target completion query", result, "graphics", __FILE__, __LINE__);
    return mImpl->mSurfaceCompleted;
}

VkDeviceContext* LLVulkanContext::uiDeviceContext()
{
    return mInitialized && mImpl && !vkDeviceIsLost() ? &mImpl->mDevice : nullptr;
}

LLCleanRendererVK* LLVulkanContext::uiWorldRenderer()
{
    return mInitialized && mImpl && !vkDeviceIsLost() ? mImpl->mWorld : nullptr;
}

void LLVulkanContext::setUIRecorderShutdown(std::function<void(U64)> callback)
{
    if (mImpl) mImpl->mUIRecorderShutdown = std::move(callback);
}

bool LLVulkanContext::renderUIWindow(NativeSurfaceId handle, const VkUISurfaceRecorder& recorder, bool advanceFrame)
{
    if (!mImpl || !recorder || mImpl->mAuxUIRecorder || !mImpl->ensureSurfaceCompletion()) return false;
    struct Reset { const VkUISurfaceRecorder*& value; ~Reset() { value = nullptr; } } reset{mImpl->mAuxUIRecorder};
    mImpl->mAuxUIRecorder = &recorder;
    return renderUIWindow(handle, std::function<void()>(), advanceFrame);
}

LLVKTexturePrepService* LLVulkanContext::texturePrepService() noexcept
{
    return mImpl && mInitialized && !mImpl->mTexturePrepTeardownStarted
        ? mImpl->mTexturePrepService.get() : nullptr;
}

void LLVulkanContext::beginTexturePrepTeardown() noexcept
{
    mInitialized = false;
    if (mImpl) mImpl->beginTexturePrepTeardown();
}

void LLVulkanContext::beginTexturePrepRecording() noexcept
{
    if (!mImpl) return;
    // A leftover entry means the preceding submission was ambiguous. Keep its service
    // record pinned rather than making its mapped buffer available to another ticket.
    mImpl->mTexturePrepRecordFailed = mImpl->mTexturePrepRecordCount != 0;
}

bool LLVulkanContext::recordTexturePrepTicket(uint16_t slot, uint32_t generation) noexcept
{
    if (!mImpl || !mImpl->mTexturePrepService || mImpl->mTexturePrepRecordFailed
        || mImpl->mTexturePrepRecordCount >= Impl::kTexturePrepRecordCap
        || mImpl->mSwapchain.frameSlot() >= Impl::kTexturePrepFrameSlotCap
        || slot >= LLVKTexturePrepService::kTicketCount || !generation)
    {
        if (mImpl) mImpl->mTexturePrepRecordFailed = true;
        return false;
    }
    LLVKTexturePrepService::Ticket ticket{};
    ticket.slot = slot;
    ticket.generation = generation;
    if (!mImpl->mTexturePrepService->noteRecordedForFrame(ticket))
    {
        mImpl->mTexturePrepRecordFailed = true;
        return false;
    }
    mImpl->mTexturePrepRecords[mImpl->mTexturePrepRecordCount++] = {slot, 0, generation};
    return true;
}

bool LLVulkanContext::texturePrepRecordingReady() const noexcept
{
    return mImpl && !mImpl->mTexturePrepRecordFailed;
}

void LLVulkanContext::texturePrepRecordingSubmitted(uint32_t frameSlot) noexcept
{
    if (!mImpl || !mImpl->mTexturePrepService || !mImpl->mTexturePrepRecordCount
        || mImpl->mTexturePrepRecordFailed || (frameSlot != UINT32_MAX && frameSlot >= Impl::kTexturePrepFrameSlotCap)
        || mImpl->mTexturePrepSubmitSerial == UINT64_MAX)
    {
        if (mImpl && mImpl->mTexturePrepRecordCount)
        {
            if (mImpl->mTexturePrepService)
            {
                for (uint32_t i = 0; i < mImpl->mTexturePrepRecordCount; ++i)
                {
                    const Impl::TexturePrepRecord& record = mImpl->mTexturePrepRecords[i];
                    LLVKTexturePrepService::Ticket ticket{};
                    ticket.slot = record.slot;
                    ticket.generation = record.generation;
                    mImpl->mTexturePrepService->quarantineRecorded(ticket);
                }
            }
            mImpl->mTexturePrepRecordFailed = true; // retain/quarantine unassociated GPU reads
        }
        return;
    }
    const uint64_t serial = ++mImpl->mTexturePrepSubmitSerial;
    if (frameSlot != UINT32_MAX) mImpl->mTexturePrepSlotSerial[frameSlot] = serial;
    for (uint32_t i = 0; i < mImpl->mTexturePrepRecordCount; ++i)
    {
        const Impl::TexturePrepRecord& record = mImpl->mTexturePrepRecords[i];
        LLVKTexturePrepService::Ticket ticket{};
        ticket.slot = record.slot;
        ticket.generation = record.generation;
        if (!mImpl->mTexturePrepService->markRecorded(ticket, serial))
        {
            // The command buffer was accepted, so a failed bookkeeping transition is
            // not a cancellation. Keep this stage unavailable until a later explicit
            // quiescence proof or device teardown.
            mImpl->mTexturePrepService->quarantineRecorded(ticket);
            LL_WARNS("Vulkan") << "texture preparation ticket could not attach to accepted submit; retaining bounded ticket" << LL_ENDL;
        }
    }
    mImpl->mTexturePrepRecordCount = 0;
    mImpl->mTexturePrepRecordFailed = false;
}

void LLVulkanContext::texturePrepRecordingDropped(bool submissionsQuiesced) noexcept
{
    if (!mImpl || !mImpl->mTexturePrepService) return;
    if (!submissionsQuiesced)
    {
        // Keep every pending service record charged and inaccessible. A failed/unknown
        // idle or device loss is not proof that a partially submitted copy has stopped.
        for (uint32_t i = 0; i < mImpl->mTexturePrepRecordCount; ++i)
        {
            const Impl::TexturePrepRecord& record = mImpl->mTexturePrepRecords[i];
            LLVKTexturePrepService::Ticket ticket{};
            ticket.slot = record.slot;
            ticket.generation = record.generation;
            mImpl->mTexturePrepService->quarantineRecorded(ticket);
        }
        mImpl->mTexturePrepRecordFailed = true;
        return;
    }
    for (uint32_t i = 0; i < mImpl->mTexturePrepRecordCount; ++i)
    {
        const Impl::TexturePrepRecord& record = mImpl->mTexturePrepRecords[i];
        LLVKTexturePrepService::Ticket ticket{};
        ticket.slot = record.slot;
        ticket.generation = record.generation;
        mImpl->mTexturePrepService->dropRecorded(ticket);
    }
    mImpl->mTexturePrepRecordCount = 0;
    mImpl->mTexturePrepRecordFailed = false;
    mImpl->mTexturePrepService->reapCompleted();
}

// #1517 frame generation: the generator's inputs for a real frame's generated frames -- slot `slot`'s
// world WITHOUT the interface (fgHudless, in the output's encoding) and its copies of the upscaler's depth and
// motion vectors, the main view's camera -- and the image it writes: the present thread's scratch image.
// Never the interface layer, never a swapchain image or a backbuffer. #1544: the display's peak (HDR10; 0 in SDR),
// the milliseconds since the rendered frame before and this frame's counter (LLVKFrameGen::frameMs / frameSerial).
static llfg::GenInputs fillGenInputs(const LLVKFrameGenPresent& f, uint32_t slot, const llfg::GenCamera& c, bool hdr10,
                                     float peakNits, float frameMs, uint64_t serial)
{
    // (#1544 in the renderer's own terms: each generator derives its conventions from them)
    llfg::GenInputs e;
    e.color = llfg::imageRef(f.fgHudless(slot));
    e.depth = llfg::imageRef(f.fgDepth(slot));
    e.motion = llfg::imageRef(f.fgMotion(slot));
    e.output = llfg::imageRef(f.fgScratch());
    e.camera = c;
    e.colorHdr = hdr10;
    e.reset = c.reset;
    e.peakNits = peakNits;
    e.frameMs = frameMs;
    e.serial = serial;
    return e;
}

// #1517 frame generation, at the frame's end (the encode wrote slot `slot`'s fgHudless / fgUi; #1536 SDR: the
// frame drew the world and the interface into them): the
// upscaler's depth and motion vectors, which the next frame rewrites, copied into the slot's own images --
// the present thread reads them after this frame's GPU end, until it releases the slot. A frame whose
// inputs were not written has none, and LLVKFrameGen is told why.
bool LLVulkanContext::Impl::recordFrameGenInputs(VkCommandBuffer cmd, uint32_t slot, const FgInputs& in)
{
    LLVKFrameGen& fg = LLVKFrameGen::instance();
    const char* refused = !in.written ? (in.refused ? in.refused : "the frame's picture could not be made")
                        : !mDisplayEncode.frameGenComposites() ? "no display pipeline for generated frames"
                        : (!in.depth || !in.motion) ? "this frame has no depth / motion inputs"
                        : !mFgPresent.inputsFit(in.depth->extent, in.depth->format, in.motion->extent,
                                                in.motion->format) ? "the upscaler's inputs changed size this frame"
                                                                   : nullptr;
    if (refused)
    {
        fg.directRefused(refused);   // (the real frame goes out alone)
        return false;
    }
    if (mFgOverlayRefused) {
        fg.directRefused("world overlay guides could not be admitted this frame");
        return false;
    }
    if (mFgOverlayGuidesReady && mFgOverlaySlot == slot) return true;
    vkBreadcrumb(cmd, "f_framegen");
    // The upscaler's writes of this frame -> the copies' reads.
    VkMemoryBarrier2 written{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    written.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    written.srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT;
    written.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    written.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &written;
    vkCmdPipelineBarrier2(cmd, &dep);
    const std::pair<const GpuImage*, const GpuImage*> copies[2] = {{in.depth, &mFgPresent.fgDepth(slot)},
                                                                   {in.motion, &mFgPresent.fgMotion(slot)}};
    for (const auto& c : copies)
    {
        // Rewritten whole (UNDEFINED): the present thread's last reads of it come before this frame's wait
        // for the slot's release.
        imageBarrier(cmd, c.second->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = region.srcSubresource;
        region.extent = {c.first->extent.width, c.first->extent.height, 1};
        // (the upscaler's inputs stay in GENERAL, as its passes and the generator use them)
        vkCmdCopyImage(cmd, c.first->image, VK_IMAGE_LAYOUT_GENERAL, c.second->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                       1, &region);
        imageBarrier(cmd, c.second->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                     VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
    }
    // The copies' reads -> the upscaler's next writes of its inputs (an execution dependency).
    VkMemoryBarrier2 read{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    read.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    read.dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    dep.pMemoryBarriers = &read;
    vkCmdPipelineBarrier2(cmd, &dep);
    return true;
}

bool LLVulkanContext::renderFrame(const std::function<void()>& uiRender,
                                  const LLVKWorldFrame* world)
{
    if (!mGraveyard.empty()) pollRecoveryGraveyard();   // #1547: reaped epochs' objects freed
    if (!mInitialized || !mImpl)
    {
        return false;
    }
    mImpl->mFgOverlayGuidesReady = mImpl->mFgOverlayRefused = false;
    if (mImpl->mTexturePrepService)
    {
        mImpl->mTexturePrepService->reapCompleted();
        // This frame's fresh staging budget (LLVKTexturePrepService::kFreshStagesPerFrame).
        mImpl->mTexturePrepService->beginFrame();
    }
    // #135: after a device loss nothing records or submits (the viewer's main loop shows
    // the message and exits; see vkDeviceIsLost). Before the swapchain recreate and the
    // pipeline poll, which would touch the lost device too.
    if (vkDeviceIsLost())
    {
        return false;
    }
    mImpl->frameGenWindowState();   // #1517 Task 11b: a hidden window -- the presenter told, nothing presented on its chain

    // #1517 FRAME GENERATION: this frame's situation for its rules. The upscaler (DLSS or FSR: either writes
    // the generator's depth and motion vectors) is last frame's (this frame's runs later).
    auto frameGenContext = [&]()
    {
        llfg::Context fc;
#if LL_WINDOWS || LL_LINUX
        fc.platform = true;
#endif
        fc.inputsOn = mImpl->mWorld && mImpl->mWorld->upscalerInputsLastFrame();
        // (review I1) HDR output on, and the encoding of the chain actually presenting (not the
        // HDR target in flight)
        const vkdisplay::Encoding enc = mImpl->mHdrOutputActive ? mImpl->mSwapchain.displayStatus().encoding
                                                                 : vkdisplay::Encoding::Sdr;
        // (#1536 SDR: frame generation takes the 8-bit UNORM formats, sRGB-encoded; an *_SRGB surface blends in
        // linear light and is not supported)
        const VkFormat chainFormat = mImpl->mSwapchain.format();
        fc.output = enc == vkdisplay::Encoding::Hdr10 ? llfg::Output::Hdr10
                  : enc == vkdisplay::Encoding::ScRgb ? llfg::Output::ScRgb
                  : Impl::sdrOutput(chainFormat);
        fc.vr = mXrSession;
        // (minimised; on Linux also hidden or occluded: mFgHidden. An unfocused window still shows.)
        fc.background = mImpl->mFseMinimized || mImpl->mFgHidden;
        fc.mainWindow = true;
        // (#1544 D8: one graphics queue and no IMMEDIATE mode on this display -- found when the chain was built)
        fc.queueHeld = mImpl->mSwapchain.frameGenQueueHeld();
        return fc;
    };
    // #1517 FRAME GENERATION: the render submit starts (PC Latency; a no-op unless Reflex paces this
    // frame).
    LLVKFrameGen::instance().marker(LLVKFrameGen::RenderStart);
    // #1517 FRAME GENERATION (LLVKFrameGen's generator makes the frames, the viewer presents and paces
    // them): the main window's desktop frames. On / off is live: the main swapchain is built for it
    // while its lasting conditions hold (chainGenerated: the setting, DLSS chosen, the output in use one
    // it runs on -- the Context's output: HDR10 or, #1536, 8-bit SDR), else it is a normal chain with vsync; a change rebuilds it below,
    // where every rebuild of this loop happens -- so does the present thread's call for one (its present
    // or acquire found the chain out of date).
    LLVKFrameGen& fgd = LLVKFrameGen::instance();
    const bool fgDirect = fgd.directReady() && !mXrSession;
    // (#1544: nor on a display where a present could hold the one graphics queue -- Block::NeedsImmediate)
    const llfg::Context fgChainCtx = frameGenContext();
    mImpl->wantFrameGenChain(fgDirect ? fgd.chainGenerated(llfg::outputGenerates(fgChainCtx.output) && !fgChainCtx.queueHeld) : 0);
    // #1544: another generator asked for (RenderVKFrameGenGenerator, live) -- switched in the rebuild below
    // (recreateMainSwapchain: applyGenerator), where the present thread is drained and the device idle.
    if (fgDirect && fgd.generatorChangeAsked())
    {
        mImpl->mNeedsRecreate = true;
        mImpl->mOutputPreviewValid = false;
    }
    if (mImpl->mSwapchain.frameGenImages() && mImpl->mFgPresent.needsRecreate())   // (C1: this chain's only)
    {
        mImpl->mNeedsRecreate = true;
        mImpl->mOutputPreviewValid = false;
    }
    // (the last rebuild's own request for one more: kept past that rebuild's caller, which forgot mNeedsRecreate;
    // from here it is a request like any -- the backoff below applies)
    if (mImpl->mFgRebuildAgain)
    {
        mImpl->mFgRebuildAgain = false;
        mImpl->mNeedsRecreate = true;
        mImpl->mOutputPreviewValid = false;
    }

    fseBeforeRecreate();   // #919 (Windows): exclusive chain on / off, release before a rebuild
    bool deferSwapchainRecreate = false;
    bool outputCandidateCommitted = false;
    vkdisplay::Status preview{};
    const bool displayRequestPending = mImpl->mNeedsRecreate;
    // #1517: never under a present the compositor holds -- the request is kept until the window shows again
    // (frameGenRebuildWaits) -- and, after a rebuild that could not be made (a present proof withheld, a device
    // wait that failed), not again at once (rebuildBackedOff).
    if (displayRequestPending && (mImpl->frameGenRebuildWaits() || mImpl->rebuildBackedOff()))
    {
        deferSwapchainRecreate = true;
        LL_DEBUGS("Vulkan") << "Swapchain rebuild deferred: frame generation's presents may be held while the window "
                               "cannot be seen, or the last try could not be made" << LL_ENDL;
    }
    if (displayRequestPending && mXrPresentOverride && mImpl->mFseMinimized)
    {
        // #1074 VR: the mirror window is minimised, so every recreate fails -- and each one
        // waits for the device to idle, which would stall the headset every frame. Retry
        // twice a second until the window is back. (mXrPresentOverride: a session runs.)
        static std::chrono::steady_clock::time_point s_last{};
        const auto now = std::chrono::steady_clock::now();
        if (now - s_last < std::chrono::milliseconds(500)) deferSwapchainRecreate = true;
        else s_last = now;
    }
    if (displayRequestPending && !deferSwapchainRecreate)
    {
        refreshDisplayHdrPolicy();
        const double now = LLFrameTimer::getElapsedSeconds();
        if (mImpl->mOutputRetryAt > now)
            deferSwapchainRecreate = true;
        else if (mImpl->mWorld &&
                 (mImpl->mOutputPreviewValid ||
                  mImpl->mSwapchain.previewDisplayStatus(mImpl->mDevice, mImpl->mOutputPreview)))
        {
            mImpl->mOutputPreviewValid = true;
            preview = mImpl->mOutputPreview;
            const bool targetLinear = preview.encoding != vkdisplay::Encoding::Sdr;
            const VkFormat targetFormat = targetLinear
                ? VK_FORMAT_R16G16B16A16_SFLOAT : preview.actual.format;
            // #1536: SDR on a frame generation chain draws its interface into a UI layer, as HDR does: the
            // renderer's pipelines for it are staged with the format, before the chain is rebuilt for it.
            const bool targetUiLayer = targetLinear || mImpl->sdrLayerExpected(preview.actual.format);
            const auto active = mImpl->mWorld->displayOutputState();
            const bool outputDiffers = active.colorFormat != targetFormat ||
                                       active.linearNits != targetLinear ||
                                       active.uiLayer != targetUiLayer ||
                                       mImpl->mOutputColorSpace != preview.actual.colorSpace;
            const bool calibrationDiffers = active.paperWhiteNits != preview.paperWhiteNits ||
                                            active.peakNits != preview.peakNits;
            if (outputDiffers || calibrationDiffers)
            {
                const bool samePending = mImpl->mOutputRequestValid &&
                    mImpl->mOutputRequestFormat == targetFormat &&
                    mImpl->mOutputRequestLinear == targetLinear &&
                    mImpl->mOutputRequestUiLayer == targetUiLayer &&
                    mImpl->mOutputRequestColorSpace == preview.actual.colorSpace &&
                    mImpl->mOutputRequestWhite == preview.paperWhiteNits &&
                    mImpl->mOutputRequestPeak == preview.peakNits;
                if (!samePending)
                {
                    mImpl->mOutputRequestGeneration = std::max(
                        mImpl->mOutputRequestGeneration, active.generation) + 1;
                    if (mImpl->mOutputRequestGeneration == 0) ++mImpl->mOutputRequestGeneration;
                    mImpl->mOutputRequestFormat = targetFormat;
                    mImpl->mOutputRequestLinear = targetLinear;
                    mImpl->mOutputRequestUiLayer = targetUiLayer;
                    mImpl->mOutputRequestColorSpace = preview.actual.colorSpace;
                    mImpl->mOutputRequestWhite = preview.paperWhiteNits;
                    mImpl->mOutputRequestPeak = preview.peakNits;
                    mImpl->mOutputRequestValid = true;
                    mImpl->mOutputPendingLogGeneration = mImpl->mOutputRequestGeneration;
                    mImpl->mOutputPendingSince = now;
                    mImpl->mOutputPendingLogAt = 0.0;
                }
                const uint64_t requestGeneration = mImpl->mOutputRequestGeneration;
                if (!samePending)
                    (void)mImpl->mWorld->requestDisplayOutput(targetFormat, targetLinear,
                        preview.paperWhiteNits, preview.peakNits, requestGeneration, targetUiLayer);
                mImpl->mWorld->pollDisplayOutput();
                if (!mImpl->mWorld->displayOutputReady(requestGeneration))
                {
                    const char* failure = mImpl->mWorld->displayOutputFailureReason();
                    if (failure)
                    {
                        LL_WARNS("Vulkan") << "Display output candidate rejected: "
                                            << failure
                                            << "; retaining current output" << LL_ENDL;
                        mImpl->mOutputRequestValid = false;
                        mImpl->mWorld->cancelDisplayOutput(requestGeneration);
                        // Keep the old chain and renderer together, then retry at a
                        // bounded cadence in case the refusal was transient budget
                        // pressure. The old mode remains fully presentable meanwhile.
                        mImpl->mOutputRetryAt = now + 2.0;
                        deferSwapchainRecreate = true;
                        // #1536: a rejected SDR UI layer (frame generation's only reason to ask for one) would be
                        // asked again every 2 s, deferring every rebuild meanwhile: frame generation's images are
                        // refused instead (its status says why), and the wish falls back to a normal chain.
                        if (targetUiLayer && !targetLinear)
                        {
                            LLVKFrameGen::instance().imagesRefused("no display pipeline for generated frames");
                            mImpl->mFgRefusalRebuild = true;   // (the next rebuild is this refusal's: it keeps it)
                        }
                    }
                    else
                    {
                        // Keep presenting the old output mode while its bounded candidate
                        // target set and pipeline bank compile. Do not destroy the active
                        // world renderer or touch any scene-owned resource.
                        if (mImpl->mOutputPendingLogGeneration != requestGeneration)
                        {
                            mImpl->mOutputPendingLogGeneration = requestGeneration;
                            mImpl->mOutputPendingSince = now;
                            mImpl->mOutputPendingLogAt = 0.0;
                        }
                        if (mImpl->mOutputPendingLogAt == 0.0 || now - mImpl->mOutputPendingLogAt >= 1.0)
                        {
                            const char* reason = mImpl->mWorld->displayOutputPendingReason();
                            LL_INFOS("Vulkan") << "Display output staging pending: generation="
                                                << requestGeneration << " reason="
                                                << (reason ? reason : "not ready") << " elapsed_ms="
                                                << (now - mImpl->mOutputPendingSince) * 1000.0
                                                << " output_pipeline_ms="
                                                << mImpl->mWorld->displayOutputPipelineMs()
                                                << " (GPU queue drain not started; current output remains active)"
                                                << LL_ENDL;
                            mImpl->mOutputPendingLogAt = now;
                        }
                        deferSwapchainRecreate = true;
                    }
                }
            }
            else if (mImpl->mOutputRequestValid)
            {
                // The user's latest request returned to the already-active mode
                // while an older format bank was staging (for example HDR -> SDR
                // before HDR compilation completed). Let that job finish before its
                // fixed staging storage can be reused, then discard its stale result.
                mImpl->mWorld->pollDisplayOutput();
                if (mImpl->mWorld->cancelDisplayOutput(mImpl->mOutputRequestGeneration))
                {
                    LL_INFOS("Vulkan") << "Discarded superseded display output generation "
                                        << mImpl->mOutputRequestGeneration
                                        << "; active output already matches latest request" << LL_ENDL;
                    mImpl->mOutputRequestValid = false;
                    mImpl->mOutputPendingSince = 0.0;
                    mImpl->mOutputPendingLogAt = 0.0;
                    // Permit one WSI recreation for the original dirty event; subsequent
                    // frames no longer retry the obsolete candidate.
                }
                else
                {
                    deferSwapchainRecreate = true;
                    if (mImpl->mOutputPendingLogAt == 0.0 || now - mImpl->mOutputPendingLogAt >= 1.0)
                    {
                        const char* reason = mImpl->mWorld->displayOutputPendingReason();
                        LL_INFOS("Vulkan") << "Display output request superseded; waiting to retire staging: "
                                            << (reason ? reason : "pipeline job still in flight")
                                            << " output_pipeline_ms=" << mImpl->mWorld->displayOutputPipelineMs()
                                            << " (no WSI recreate or GPU drain)" << LL_ENDL;
                        mImpl->mOutputPendingLogAt = now;
                    }
                }
            }
        }
        else if (mImpl->mWorld)
        {
            // A failed bounded surface-format query is transient (monitor/window
            // changes can race it). Keep the old chain and retry next frame.
            deferSwapchainRecreate = true;
            LL_DEBUGS("Vulkan") << "Display output staging deferred: surface format query unavailable"
                                 << LL_ENDL;
        }
    }

    // Admit the new HDR composition extent and encoder before retiring the live
    // swapchain. A resize can require a new FP16 image even when output format and
    // calibration are unchanged, so it has no renderer-bank request to protect it.
    if (displayRequestPending && !deferSwapchainRecreate)
    {
        if (!mImpl->mOutputPreviewValid &&
            mImpl->mSwapchain.previewDisplayStatus(mImpl->mDevice,mImpl->mOutputPreview))
            mImpl->mOutputPreviewValid=true;
        if (!mImpl->mOutputPreviewValid)
        {
            deferSwapchainRecreate=true;
        }
        else
        {
            const bool hdrCandidate=mImpl->mOutputPreview.encoding!=vkdisplay::Encoding::Sdr;
            bool hdrStageReady=!hdrCandidate;
            if (hdrCandidate)
            {
                VkExtent2D candidateExtent{};
                hdrStageReady=mImpl->mSwapchain.previewExtent(mImpl->mDevice,mImpl->mWidth,
                    mImpl->mHeight,candidateExtent) &&
                    mImpl->stageHdrOutput(mImpl->mOutputPreview,candidateExtent);
            }
            if (!vkdisplay::transaction::mayRetireSwapchain(hdrCandidate,hdrStageReady))
            {
                const double now=LLFrameTimer::getElapsedSeconds();
                deferSwapchainRecreate=true;
                mImpl->mOutputRetryAt=now+0.25;
                LL_INFOS("Vulkan") << "HDR resize/mode pre-admission deferred: "
                                    << (mImpl->mHdrOutputFailureReason.empty()
                                        ? "surface extent unavailable"
                                        : mImpl->mHdrOutputFailureReason)
                                    << "; keeping current swapchain and live composition" << LL_ENDL;
            }
        }
    }

    // (#1517: the present thread drained first -- inside a present, an acquire or the generator, the rebuild waits)
    if (displayRequestPending && !deferSwapchainRecreate && !mImpl->drainFrameGen())
        deferSwapchainRecreate = true;
    const bool outputModeCommitAttempt = mImpl->mOutputRequestValid;
    // (#1517) The rebuild's try. False: NOT rebuilt -- the present thread did not drain after all, an owed release
    // could not be settled, or the presentation engine still holds a present of the old chain. The old chain is in
    // use as it was, so nothing that follows a rebuild runs: the request is kept (mNeedsRecreate) and nothing
    // else is touched -- a pending output-mode request (an HDR switch) stays pending, never judged against the
    // old chain's format; the rebuild that is made settles it.
    bool rebuilt = false;
    if (displayRequestPending && !deferSwapchainRecreate)
    {
        rebuilt = mImpl->recreateMainSwapchain();
        if (!rebuilt) deferSwapchainRecreate = true;
    }
    if (rebuilt)
    {
        if (outputModeCommitAttempt)
            LL_INFOS("Vulkan") << "Display output swapchain recreation device_idle_wait_ms="
                                << mImpl->mSwapchain.lastDeviceIdleWaitMs()
                                << " (GPU drain wait; separate from output pipeline batch time)"
                                << LL_ENDL;
        mImpl->mOutputPreviewValid = false;
        const bool chainValid = mImpl->mSwapchain.valid();
        if (chainValid) noteSwapchainMode(mImpl->mSwapchain, true);
        bool hdrPrepared = chainValid;
        if (hdrPrepared && (mDisplayHdrRequested || mImpl->mHdrOutputActive || !mImpl->mHdrOutputReady))
            hdrPrepared = mImpl->prepareHdrOutput();
        if (!hdrPrepared)
        {
            LL_WARNS("Vulkan") << "Display output preparation failed: "
                                << mImpl->mHdrOutputFailureReason
                                << "; retaining the current renderer output and retrying with SDR policy"
                                << LL_ENDL;
            if (chainValid && mDisplayHdrRequested)
            {
                mDisplayHdrPipelineReady = false;
                refreshDisplayHdrPolicy();
                mImpl->recreateMainSwapchain();
                if (mImpl->mSwapchain.valid()) noteSwapchainMode(mImpl->mSwapchain, true);
                (void)mImpl->prepareHdrOutput();
                // The renderer may still own the old HDR bank. Keep all recording
                // withheld until an SDR bank and SDR chain commit together.
                mImpl->mNeedsRecreate = true;
                deferSwapchainRecreate = true;
            }
            else
            {
                mImpl->mNeedsRecreate = true;
                deferSwapchainRecreate = true;
            }
        }

        if (chainValid && !mImpl->mWorld &&
            mImpl->mSwapchain.displayStatus().encoding == vkdisplay::Encoding::Sdr &&
            (mImpl->mHdrOutputActive || mImpl->mHdrStageReady))
            mImpl->releaseHdrOutputAfterSdrCommit();

        if (mImpl->mWorld && mImpl->mOutputRequestValid &&
            mImpl->mWorld->displayOutputReady(mImpl->mOutputRequestGeneration))
        {
            const auto actual = mImpl->mSwapchain.displayStatus();
            const bool actualLinear = actual.encoding != vkdisplay::Encoding::Sdr;
            const VkFormat actualFormat = actualLinear
                ? VK_FORMAT_R16G16B16A16_SFLOAT : actual.actual.format;
            // (#1536: the chain that was built has a UI layer -- HDR, or an SDR frame generation chain)
            const bool actualUiLayer = actualLinear || mImpl->sdrLayerChain();
            // (the rule: only a rebuild that was made settles the request -- committed on a chain of its format
            // and UI layer, cancelled on another; the renderer's bank is ready in this branch)
            using vkdisplay::transaction::OutputRequestStep;
            const OutputRequestStep step = vkdisplay::transaction::outputRequestAfterRebuild(rebuilt, true,
                actualFormat == mImpl->mOutputRequestFormat && actualLinear == mImpl->mOutputRequestLinear &&
                actualUiLayer == mImpl->mOutputRequestUiLayer);
            if (step == OutputRequestStep::Commit &&
                mImpl->mWorld->commitDisplayOutput(mImpl->mOutputRequestGeneration))
            {
                outputCandidateCommitted = true;
                mImpl->mOutputRequestValid = false;
                mImpl->mOutputPendingSince = 0.0;
                mImpl->mOutputPendingLogAt = 0.0;
                mImpl->mOutputColorSpace = actual.actual.colorSpace;
                mImpl->mOutputRetryAt = 0.0;
                mImpl->mHdrAppliedPaperWhiteNits = actual.paperWhiteNits;
                mImpl->mHdrAppliedPeakNits = actual.peakNits;
                if (!actualLinear && (mImpl->mHdrOutputActive || mImpl->mHdrStageReady))
                    mImpl->releaseHdrOutputAfterSdrCommit();
                LL_INFOS("Vulkan") << "Display mode committed without rebuilding scene resources; renderer "
                                    << "generation " << mImpl->mWorld->displayOutputState().generation
                                    << ", scene generation " << mImpl->mDisplayGeneration << LL_ENDL;
            }
            else if (step != OutputRequestStep::Keep)
            {
                // WSI negotiation can differ from the preflight query. No commands
                // using the old renderer are recorded against the new chain; request
                // its actual mode on the next frame and retain all CPU/GPU world data.
                mImpl->mWorld->cancelDisplayOutput(mImpl->mOutputRequestGeneration);
                mImpl->mOutputRequestValid = false;
                mImpl->mOutputPendingSince = 0.0;
                mImpl->mOutputPendingLogAt = 0.0;
                if (mDisplayHdrRequested && !actualLinear)
                {
                    mDisplayHdrPipelineReady = false;
                    refreshDisplayHdrPolicy();
                }
                mImpl->mNeedsRecreate = true;
                deferSwapchainRecreate = true;
                LL_WARNS("Vulkan") << "Display output changed during recreation; withholding world draw until "
                                    << "the renderer output bank matches the actual surface format" << LL_ENDL;
            }
        }
        else if (mImpl->mOutputRequestValid)
        {
            // Allocation/pipeline failure leaves the active world bank and resources
            // untouched. Keep the negotiated chain in SDR and report the bounded cause.
            if (mImpl->mHdrOutputActive)
            {
                mImpl->mNeedsRecreate = true;
                deferSwapchainRecreate = true;
            }
            else if (const char* failure = mImpl->mWorld->displayOutputFailureReason())
            {
                LL_WARNS("Vulkan") << "Display output commit refused: "
                                    << failure << LL_ENDL;
                mImpl->mWorld->cancelDisplayOutput(mImpl->mOutputRequestGeneration);
                mImpl->mOutputRequestValid = false;
                mImpl->mOutputRetryAt = LLFrameTimer::getElapsedSeconds() + 2.0;
                mImpl->mNeedsRecreate = true;
                deferSwapchainRecreate = true;
            }
            else
            {
                mImpl->mNeedsRecreate = true;
                deferSwapchainRecreate = true;
            }
        }
        if (!deferSwapchainRecreate) mImpl->mNeedsRecreate = false;
    }
    else if (!displayRequestPending &&
             (mDisplayHdrRequested || mImpl->mHdrOutputActive || !mImpl->mHdrOutputReady))
    {
        if (!mImpl->prepareHdrOutput())
        {
            LL_WARNS("Vulkan") << "HDR display preparation failed: "
                                << mImpl->mHdrOutputFailureReason << LL_ENDL;
        }
    }

    // Calibration is push-only: update paper white/peak on the existing output
    // bank. No world restart or format-pipeline build is needed.
    if (mImpl->mHdrOutputActive && !outputCandidateCommitted && mImpl->mWorld &&
        (mImpl->mHdrAppliedPaperWhiteNits != mDisplayPaperWhiteNits ||
         mImpl->mHdrAppliedPeakNits != mDisplayPeakNits))
    {
        if (mImpl->mWorld->configureDisplayOutput(true, mDisplayPaperWhiteNits, mDisplayPeakNits))
        {
            mImpl->mHdrAppliedPaperWhiteNits = mDisplayPaperWhiteNits;
            mImpl->mHdrAppliedPeakNits = mDisplayPeakNits;
        }
    }
    fseAfterRecreate();    // #919 (Windows): acquire exclusive mode on the exclusive swapchain

    // Publish candidate composition resources only after WSI and renderer output
    // state agree. In the SDR case, retain HDR objects until the SDR bank commits.
    if (mImpl->mSwapchain.valid())
    {
        const auto& actual=mImpl->mSwapchain.displayStatus();
        const VkExtent2D extent=mImpl->mSwapchain.extent();
        if (actual.encoding!=vkdisplay::Encoding::Sdr)
        {
            const auto state=mImpl->mWorld ? mImpl->mWorld->displayOutputState()
                                           : LLCleanRendererVK::DisplayOutputState{};
            const bool rendererMatches=!mImpl->mWorld ||
                (state.linearNits && state.colorFormat==VK_FORMAT_R16G16B16A16_SFLOAT);
            if (rendererMatches && !mImpl->commitHdrOutputStage(actual,extent))
            {
                mImpl->mNeedsRecreate=true;
                LL_WARNS("Vulkan") << "HDR composition commit withheld: staged frame/encoder identity mismatch"
                                    << LL_ENDL;
            }
        }
        else if ((!mImpl->mWorld || !mImpl->mWorld->displayOutputState().linearNits) &&
                 (mImpl->mHdrOutputActive || mImpl->mHdrStageReady))
        {
            mImpl->releaseHdrOutputAfterSdrCommit();
        }
    }

    // #1536 SDR FRAME GENERATION: this frame is an SDR-layer frame -- not HDR, and the main swapchain is a
    // frame generation chain. The ONE test every SDR-layer branch of this function hangs on: with it false
    // (a normal SDR chain) nothing of the SDR path differs from before. `layer`: the interface (and the
    // markers, HUDs and in-world text) draws into a UI layer -- HDR's, or the present slot's in SDR.
    const bool sdrLayer = !mImpl->mHdrOutputActive &&
                          llfg::sdrLayer(frameGenContext().output, mImpl->mSwapchain.frameGenImages());
    const bool layer = mImpl->mHdrOutputActive || sdrLayer;
    // Its composite follows the chain: made (present thread drained) once the chain's present slots are
    // ready, let go on any other chain. One that cannot be made: a normal chain next, with the reason.
    if (sdrLayer)
    {
        // (a rebuild may have made the present slots' images anew, and a destroyed view's handle value can
        // come back for a new one: the composite's per-slot descriptor cache starts over, as after an HDR resize)
        if (rebuilt) mImpl->mDisplayEncode.invalidateInputs();
        bool encodeFailed = false;
        if (mImpl->mFgPresent.ready() && !mImpl->prepareSdrLayerEncode(encodeFailed) && encodeFailed)
        {
            fgd.imagesRefused("no display pipeline for generated frames");
            mImpl->mFgRefusalRebuild = true;
        }
    }
    else
        mImpl->releaseSdrLayerEncode();

    // Last-line safety gate: never record or present a frame whose HDR flag is set
    // without a live FP16 image/view and matching encoder for this exact chain extent.
    if (!mImpl->hdrCompositionValidForCurrentChain())
    {
        mImpl->mNeedsRecreate=true;
        LL_WARNS("Vulkan") << "Display frame withheld: composition resources do not match the active WSI chain"
                            << LL_ENDL;
        return false;
    }
    // (#1536) ... nor an SDR-layer frame without its present slots, its composite and the renderer's committed
    // UI layer: withheld (the last picture stays), and the chain is rebuilt -- as a normal one after a refusal,
    // or once the renderer's layer is staged.
    if (sdrLayer && !mImpl->sdrLayerValidForCurrentChain())
    {
        mImpl->mNeedsRecreate = true;
        mImpl->mOutputPreviewValid = false;
        const double now = LLFrameTimer::getElapsedSeconds();
        if (now - mImpl->mSdrLayerWithheldLogAt >= 1.0)
        {
            mImpl->mSdrLayerWithheldLogAt = now;
            LL_WARNS("Vulkan") << "Display frame withheld: SDR frame generation's present slots, composite or renderer "
                                  "UI layer do not match the active WSI chain" << LL_ENDL;
        }
        return false;
    }
    if (mImpl->mWorld)
    {
        const auto committed = mImpl->mWorld->displayOutputState();
        const VkFormat expected = mImpl->mHdrOutputActive
            ? VK_FORMAT_R16G16B16A16_SFLOAT : mImpl->mSwapchain.format();
        if (committed.linearNits != mImpl->mHdrOutputActive || committed.colorFormat != expected)
        {
            // A surface can change between the bounded preflight and WSI creation.
            // Do not submit a command buffer whose attachment specialization differs
            // from the newly negotiated chain. The previous present remains visible;
            // the next frame stages the actual output format.
            return false;
        }
        if (committed.uiLayer != layer)
        {
            // (#1536) The renderer draws into a UI layer this chain's frames do not have (a frame generation
            // chain that came out a normal one). The same rule; the next frame stages the output the chain has.
            mImpl->mNeedsRecreate = true;
            mImpl->mOutputPreviewValid = false;
            return false;
        }
    }

    // Bring up the GPU-driven world renderer on the FIRST frame -- the login screen --
    // not the first world frame: its pipelines (dozens of variants of the large world
    // FS, seconds each on a cold driver cache, i.e. every new release) then compile on
    // background threads while the user logs in, instead of stalling after the
    // progress bar (#213). It turns ready() when they are done. Failure is non-fatal
    // (UI-only present continues).
    const bool wantWorld = world && world->enabled;
    const double worldInitNow = LLFrameTimer::getElapsedSeconds();
    mImpl->retryDeferredWorldShutdown(worldInitNow);
    if (mImpl->mWorld)
    {
        VK_HITCH_SCOPE("vk_pollPipelines");
        mImpl->mWorld->pollPipelines();
    }
    // Do not construct or expose another renderer until the retained one has passed
    // shutdown's fresh device-idle proof and its fixed owner slot has been cleared.
    if (!mImpl->mWorld && !mImpl->mDeferredWorld && !mImpl->mWorldInitTried &&
        worldInitNow >= mImpl->mWorldInitRetryAt)
    {
        mImpl->mWorldInitTried = true;
        LLCleanRendererVK* w = new LLCleanRendererVK();
        if (mRecoveryScene)
        {
            // #1548: a renderer rebuilt after a GPU loss refills at the raised pace, visible / near first from its
            // first texture pass (the lost renderer's last demand) -- set before init, which runs that pass.
            w->setRecoveryRefill(true);
            if (!mRecoveryScene->seedDemand.empty())
                w->seedTexDemand(std::move(mRecoveryScene->seedDemand), std::move(mRecoveryScene->seedSeen));
        }
        w->mWantVrHudPipes = mXrSession;   // #1076 VR: the wall's HUD pipelines (desktop: false)
        // #1075: a stereo VR session renders one layer per eye (desktop: false).
        w->mStereoRequested = mXrSession && LLXrSession::instance().stereo();
        const VkFormat worldFormat = mImpl->mHdrOutputActive ? VK_FORMAT_R16G16B16A16_SFLOAT
                                                              : mImpl->mSwapchain.format();
        // (#1536: made on an SDR frame generation chain it starts with the UI layer that chain's frames draw into)
        const bool displayConfigured = w->configureDisplayOutput(mImpl->mHdrOutputActive,
                                                                  mDisplayPaperWhiteNits, mDisplayPeakNits, sdrLayer);
        // #1384: an allocation init cannot do without leaves through VkInitAllocFailure (no
        // longer the out-of-memory exit): init fails, shutdown() below releases what it made,
        // the UI keeps running, and init is tried again later.
        bool initOk = false;
        bool initOom = false;
        if (displayConfigured)
        {
            try
            {
                VkInitAllocEscape escape;
                initOk = w->init(mImpl->mDevice, worldFormat);
            }
            catch (const VkInitAllocFailure& f)
            {
                initOom = true;
                ++mImpl->mWorldInitOomFails;
                const double wait = std::min(60.0, 2.0 * double(1u << std::min(mImpl->mWorldInitOomFails - 1u, 5u)));
                LL_WARNS("VKMem") << "#1384 world renderer init out of memory: " << f.what << " = "
                                  << vkResultString(f.result) << ", " << (f.bytes >> 10) << " KB on heap " << f.heap
                                  << (f.file ? llformat(" at %s:%d", f.file, f.line) : std::string())
                                  << " (attempt " << mImpl->mWorldInitOomFails << "); released, UI-only, retry in "
                                  << wait << " s" << LL_ENDL;
                mImpl->mWorldInitRetryAt = worldInitNow + wait;
            }
        }
        if (initOk)
        {
            mImpl->mWorldInitOomFails = 0;
            mImpl->mWorld = w;
            // init() takes fresh bounded mirrors from the persistent CPU stores; the
            // following syncFromStores is the publication handshake before first draw.
            mImpl->mWorldSourceGeneration = 0;
        }
        else
        {
            LL_WARNS("Vulkan") << "World renderer init failed; UI-only present."
                               << LL_ENDL;
            w->shutdown();
            if (!initOom) mImpl->mWorldInitRetryAt = worldInitNow + 2.0;
            if (w->shutdownDeferred())
            {
                // Keep one failed-init renderer out of mWorld so frame/output code
                // cannot use it before a later idle proof permits cleanup and retry.
                mImpl->mDeferredWorld = w;
                mImpl->mDeferredWorldRetryAt = worldInitNow + 2.0;
                mImpl->mWorldInitTried = true;
            }
            else
            {
                delete w;
                // Init can be refused by transient budget pressure. Retry at a bounded
                // cadence instead of leaving the viewer permanently UI-only after one
                // failed attempt.
                mImpl->mWorldInitTried = false;
            }
        }
    }
    const bool worldAvailable = wantWorld && mImpl->mWorld && mImpl->mWorld->ready();
    // Keep drawing disabled until the replacement renderer's format-specialized
    // geometry/color/post pipelines are ready in either output mode. In HDR this is
    // also required because the final transfer consumes absolute nits; optional RT
    // variants are excluded by the existing pipeline-gate contract.
    const bool generationPipelinesReady = worldAvailable && !worldPipelinesPending();
    const bool doWorld = worldAvailable &&
        mImpl->mWorldSourceGeneration == mImpl->mDisplayGeneration && generationPipelinesReady;
    // #1074 VR world frame: the session runs and LLVRCamera put this frame's head pose into the
    // camera. The world then renders into the eye-size target (copied into both eyes, mirrored
    // onto the window) instead of the window image. Desktop: mXrSession is false.
    uint32_t vrW = 0, vrH = 0;
    const bool vrWorld = mXrSession && doWorld && LLXrSession::instance().sessionRunning() &&
                         LLXrSession::instance().worldCameraApplied() && LLXrSession::instance().renderExtent(vrW, vrH) &&
                         mImpl->vrEnsureTarget(vrW, vrH);
    if (vrWorld != mImpl->mVrWorldLast)
    {
        // Into / out of VR world frames (session start / stop, login): the render size and the
        // view count change. Let the frames in flight (and their compute work) finish first;
        // once per transition.
        mImpl->mVrWorldLast = vrWorld;
        if (vrWorld && mImpl->mDevice.device()) vkDeviceWaitIdle(mImpl->mDevice.device());
        LL_INFOS("VR") << (vrWorld ? "the world renders for the headset now (eye size, head-tracked camera)"
                                   : "the world renders for the desktop window again")
                       << LL_ENDL;
    }
    // #1594 VR SPACE WARP (llvrspacewarp.h): while it is engaged every shown frame is a warp of the
    // newest real frame, submitted HERE, before this frame records anything -- a real frame's world
    // then follows it on the queue (and goes into the history at its end: vrWarpCapture); a warp-only
    // frame stops after it (no world, no UI build, the mirror keeps the last real frame). Desktop:
    // mXrSession is false. VR with space warp off: kind() is Off and nothing here runs.
    bool vrWarpCapture = false;
    if (mXrSession)
    {
        const LLVRSpaceWarp::Kind wk = LLVRSpaceWarp::instance().kind();
        if (wk != LLVRSpaceWarp::Kind::Off)
        {
            if (!vrWorld) LLVRSpaceWarp::instance().dropHistory("no world for the headset this frame");
            const bool warpFrame = wk == LLVRSpaceWarp::Kind::Real || wk == LLVRSpaceWarp::Kind::Warp;
            const bool warped = vrWorld && warpFrame && mImpl->vrSpaceWarpSubmit(world, wk == LLVRSpaceWarp::Kind::Warp);
            if (warped && wk == LLVRSpaceWarp::Kind::Warp) return true;   // the headset has its frame: done
            vrWarpCapture = vrWorld;
        }
    }
    if (worldAvailable)
    {
        // CPU-side: mirror the latest clean-store arrays into VK buffers (or keep
        // the prototype scene). Must run before command recording. The store fill
        // starts on a worker here and overlaps the UI buffering below; the join (+ the
        // main-thread texture resolve) follows it.
        { VK_HITCH_SCOPE("vk_syncBegin"); mImpl->mWorld->syncFromStoresBegin(); }
    }

    // If a UI render is supplied and the facade is ready, buffer the whole UI pass
    // now (CPU-side: gGL calls dispatch into gVK, filling batches). This must run
    // BEFORE command recording; gVK.recordFrame(cmd) lays the batches down below.
    uint64_t targetSerial = 0;
    const bool doUI = uiRender && mImpl->mFacadeReady;
    if (doUI)
    {
        VkExtent2D ext = mImpl->mSwapchain.extent();
        // HDR (#1451): the UI draws with its SDR pipelines into the RGBA8 UI layer, composited
        // over the world at paper white by the display encode. (#1536: so it does on an SDR frame
        // generation chain, composed over the world as the SDR swapchain would blend it.)
        const VkFormat uiFormat = layer ? VK_FORMAT_R8G8B8A8_UNORM : mImpl->mSwapchain.format();
        gVK.beginFrameToView(VK_NULL_HANDLE /*view set below*/, uiFormat, ext.width, ext.height, layer);
        // #1133 VR: a stereo world frame into the layered eye target draws the in-world text in
        // the world (world billboards, both eyes); the desktop (vrWorld false): off, as always.
        gVK.setWorldTextViews(vrWorld && doWorld && mImpl->mWorld && mImpl->mWorld->stereoActive() &&
                              mImpl->mVrTarget.layers >= 2u && mImpl->mVrTarget.arrayView != VK_NULL_HANDLE);
        // LL's depth-tested 3D overlays get the world depth only when a world pass
        // records this frame (see beginWorldOverlay / mWorldOverlayHook).
        gVK.setWorldOverlayDepthFormat(doWorld ? mImpl->mWorld->worldDepthFormat()
                                               : VK_FORMAT_UNDEFINED);
        gVK.setWorldOverlayReverseZ(doWorld && mImpl->mWorld->worldReverseZ());
        // #1141 VR: a layered stereo world frame lays LL's 3D overlays (edit manipulators, beacons,
        // property lines) into each eye; the overlay pass loads world->proj (setVKOverlayCamera).
        // Desktop: mXrSession is false -- never called (beginFrameToView left it off).
        if (mXrSession && vrWorld && world && mImpl->mWorld->stereoActive())
            gVK.setWorldOverlayStereo(true, world->proj);
        { VK_HITCH_SCOPE("vk_uiBuild"); uiRender(); }
    }
    if (worldAvailable)
    {
        { VK_HITCH_SCOPE("vk_syncFromStores"); mImpl->mWorld->syncFromStores(); }
        if (mImpl->mWorldSourceGeneration != mImpl->mDisplayGeneration)
        {
            // A new renderer has empty per-renderer mirrors. Its first fill therefore
            // copies the complete retained instance range, repacks resident geometry,
            // refreshes all material/skin-meta rows, and creates generation-local GPU
            // buffers/descriptors. Dynamic light/shadow/avatar data is supplied again
            // by the normal per-frame setters below on the first drawable frame.
            mImpl->mWorldSourceGeneration = mImpl->mDisplayGeneration;
            LL_INFOS("Vulkan") << "World scene replay complete for display generation "
                                << mImpl->mDisplayGeneration << LL_ENDL;
        }
    }

    // The frame-in-flight slot for this frame. acquire() has already waited this
    // slot's in-flight fence, so its command buffer + the facade's per-slot VBO
    // are guaranteed retired and safe to re-record/overwrite.
    const uint32_t slot = mImpl->mSwapchain.frameSlot();
    // #1517 frame generation's present thread owns the images of a chain built for frame generation:
    // this frame renders into a backbuffer of the present thread's
    // and hands it over at its end; it never acquires or presents a swapchain image itself.
    const bool fgChain = mImpl->mSwapchain.frameGenImages() != 0;

    // #1517 frame generation: how many frames to generate (0: a plain frame), decided on the
    // chain this frame presents to. Not on a frame that cannot carry them: this frame only (no world,
    // no UI layer -- HDR's composition or, #1536, the SDR layer --, a snapshot frame), or for a lasting cause, which
    // LLVKFrameGen is told (the status says why, not "ok"). The present thread generates and shows them.
    uint32_t fgWant = 0;
    if (fgDirect)
    {
        fgWant = fgd.decideDirect(frameGenContext(), mImpl->mSwapchain.frameGenImages());
        const bool snapshotFrame = doWorld && mImpl->mWorld &&
            (mImpl->mWorld->capturePending() != 0u || mImpl->mWorld->liveSnapWantsSdr());
        if (fgWant && (!doWorld || vrWorld || !layer || snapshotFrame))
            fgWant = 0;
        const char* refused = nullptr;
        // The generator's world: HDR10's 10-bit image at the HDR frame's size, or (#1536 SDR) the present
        // slot's world image -- the chain's own format and size (there is no HDR frame in SDR).
        const VkExtent2D fgSize = sdrLayer ? mImpl->mSwapchain.extent() : mImpl->mDisplayFrame.linear().extent;
        const VkFormat fgFormat = VkDisplayEncode::fgHudlessFormat(
            VkSurfaceFormatKHR{mImpl->mSwapchain.format(), mImpl->mSwapchain.colorSpace()});
        // (#1544 what the generator's feature is made for: the world's size and format, the scratch image's format
        // -- the generator's own: llfg::scratchFormat --; prepareFrameGen adds the render size and the images)
        llfg::GenSize fgGen;
        fgGen.displayW = fgSize.width;
        fgGen.displayH = fgSize.height;
        fgGen.colorFormat = fgFormat;
        fgGen.scratchFormat = llfg::scratchFormat(fgd.generator(), fgFormat);
        if (fgWant && !mImpl->mDisplayEncode.frameGenReady())
            refused = "no display pipeline for generated frames";
        else if (fgWant && !mImpl->mFgPresent.ready())
            refused = "could not create the objects for showing generated frames";
        // The generator's depth / motion copies, every present slot's, for what the upscaler renders now, and its
        // feature for them, with nothing of frame generation in flight (not made now -- the present thread did not
        // drain: no generated frames this frame, nothing refused; the feature refused: no more this session).
        else if (fgWant && !mImpl->prepareFrameGen(fgWant, fgGen, refused) && !refused)
            fgWant = 0;
        if (refused)
        {
            fgd.directRefused(refused);
            fgWant = 0;
        }
    }

    uint32_t imageIndex = 0;
    uint32_t fgSlot = 0;   // (a frame generation chain: the present thread's slot this frame renders into)
    bool needsRecreate = false;
    bool acquired;
    {
        const auto t0 = std::chrono::steady_clock::now();
        {
            VK_HITCH_SCOPE("vk_acquireWait");
            if (fgChain)
            {
                // The slot's backbuffer once the present thread is done with it (the renderer's only
                // backpressure), then the frame slot (its fence; a refused wait leaves it untouched).
                fgSlot = mImpl->mFgPresent.nextSlot();
                const auto w0 = std::chrono::steady_clock::now();
                // A bounded wait on every chain. Busy: the frame is not rendered and the main loop goes on
                // pumping events -- and, with the present thread held, the GPU was checked inside the wait (the
                // device's sliced waits on the frames in flight and the present queue's accepted work: a GPU
                // that hung is raised there, as any GPU hang; this frame's own fence wait is never reached).
                const VkPresentHandoff::SlotWait slotWait = mImpl->mFgPresent.ready() ? mImpl->mFgPresent.waitSlot(fgSlot)
                                                                                      : VkPresentHandoff::SlotWait::Gone;
                const bool slotFree = slotWait == VkPresentHandoff::SlotWait::Free;
                fgd.noteSlotWait(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - w0).count());
                if (slotWait == VkPresentHandoff::SlotWait::Busy && fgDirect)
                {
                    fgd.frameSkipped();   // (nothing rendered: the generator's history carries over)
                    // (held with the GPU done: never ended by the viewer -- the status names the call the thread
                    // is inside: the display holding a frame, an image, the generator; neutral anywhere else)
                    if (mImpl->mFgPresent.held()) fgd.directRefused(mImpl->mFgPresent.heldReason());
                }
                acquired = slotFree && mImpl->mSwapchain.beginFrameWithoutImage(mImpl->mDevice);
            }
            else
                acquired = mImpl->mSwapchain.acquire(mImpl->mDevice, &imageIndex, &needsRecreate);
        }
        LLVKHitch::timelineAcquire(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    // #1074 VR: no window image (the mirror window is minimised, resizing, or its acquire timed
    // out): the headset still gets this frame -- recorded and submitted on the slot's fence
    // with no window image, no UI pass and no present. Desktop: skip the frame as before.
    bool headless = false;
    if (!acquired)
    {
        if (needsRecreate)
        {
            mImpl->mNeedsRecreate = true;
            mImpl->mOutputPreviewValid = false;
        }
        if (fgChain || !vrWorld || !mImpl->mSwapchain.beginFrameWithoutImage(mImpl->mDevice))
            return true; // benign; skip this frame
        headless = true;
    }
    // acquire() has waited this slot's graphics fence. Retire only service buffers
    // associated with a serial from this exact slot, never by frame age.
    if (mImpl->mTexturePrepService && slot < Impl::kTexturePrepFrameSlotCap)
    {
        mImpl->mTexturePrepService->retireGpuUse(mImpl->mTexturePrepSlotSerial[slot]);
        mImpl->mTexturePrepService->reapCompleted();
    }
    // acquire() returned true but may still flag SUBOPTIMAL: render + present this
    // frame normally, then recreate the swapchain afterward.
    if (needsRecreate)
    {
        mImpl->mNeedsRecreate = true;
        mImpl->mOutputPreviewValid = false;
    }
    // #1517 frame generation: this frame generates fgCount frames (the present thread's job). None
    // on a frame that asks for a rebuild (a resize: it presents alone).
    const uint32_t fgCount = (fgChain && fgWant && !headless && !needsRecreate) ? fgWant : 0;
    LLVKFrameGenPresent::Job fgJob;

    VkCommandBuffer cmd = mImpl->mCmds[slot];
    // The frame's target: the swapchain image, or on a frame generation chain the slot's backbuffer.
    VkImage img = headless ? VK_NULL_HANDLE
                : fgChain  ? mImpl->mFgPresent.backbuffer(fgSlot).image
                           : mImpl->mSwapchain.image(imageIndex);
    const VkImageView imgView = headless ? VK_NULL_HANDLE
                              : fgChain  ? mImpl->mFgPresent.backbuffer(fgSlot).view
                                         : mImpl->mSwapchain.imageView(imageIndex);
    // Where the frame leaves it: presented, or copied into a swapchain image by the present thread.
    const VkImageLayout imgFinalLayout = fgChain ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    // Record: UNDEFINED -> TRANSFER_DST, clear, (UI pass), -> PRESENT_SRC.
    beginTexturePrepRecording();
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
    // #790 breadcrumbs for the frame's head and tail: a device loss whose last finished
    // crumb is the previous frame's p_probe left everything from there to the next
    // frame's first world sub-mark (UI uploads, offscreen targets, the clear, the world
    // prologue, the UI replay, the present transition) without one.
    vkBreadcrumb(cmd, "f_begin");
    {
        // #1547 TESTING AID: a deliberate GPU hang (RenderVKDebugGpuHang; one-shot, never persisted, not in
        // any UI). Taken back to 0 before anything is recorded, so it fires in exactly one frame.
        static LLCachedControl<U32> s_hang(gSavedSettings, "RenderVKDebugGpuHang", 0);
        const U32 hangMode = s_hang;
        if (hangMode != 0)
        {
            gSavedSettings.setU32("RenderVKDebugGpuHang", 0);
            LL_WARNS("Vulkan") << "RenderVKDebugGpuHang " << hangMode << ": DELIBERATE GPU HANG (testing aid) -- "
                               << (hangMode == 1 ? std::string("an unbounded compute spin; the driver's watchdog must reset the GPU")
                                                 : llformat("a finite compute spin of %u million reloads (about %u s)",
                                                            hangMode, hangMode))
                               << " (renderer epoch " << vkEpochCurrent() << ", frame " << mImpl->mFrameCounter << ")"
                               << LL_ENDL;
            vkBreadcrumb(cmd, "debug_gpu_hang");
            if (mImpl->recordDebugGpuHang(cmd, hangMode))
                LL_WARNS("Vulkan") << "RenderVKDebugGpuHang: recorded into this frame's command buffer" << LL_ENDL;
        }
    }
    // ASYNC COMPUTE (roadmap phase 3a): world frames opt the renderer in. recordWorld may
    // then end `cmd` at its fork / join points and continue in its own command buffers
    // (asyncCurrentCmd); the swapchain clear goes into a separate command buffer
    // (asyncWsiCmd), the only submission that waits the acquire semaphore, and
    // asyncSubmitFrame submits everything in order on both queues.
    // #1075 VR world frames too: the XR eye copies, the depth layer copy and the mirror blit
    // are recorded into the frame's LAST graphics command buffer (asyncCurrentCmd below), the
    // tail asyncSubmitFrame submits last on the graphics queue -- after the joins of every
    // segment the world's output depends on -- so xrEndFrame (after the submit, main thread)
    // still follows all the eye work on the graphics queue the runtime was given (family /
    // index 0). The async queue is another VkQueue, never handed to the runtime, and every
    // submit stays on the main thread (VR_QUEUE_AUDIT option A: no lock). A window-less frame
    // has no acquire / render-finished semaphores (asyncSubmitFrame and the #697 drop path
    // take null ones) and no WSI command buffer.
    LLCleanRendererVK* asyncW = (doWorld && mImpl->mWorld->asyncAvailable()) ? mImpl->mWorld : nullptr;
    if (asyncW)
    {
        VK_HITCH_SCOPE("vk_asyncBegin");
        asyncW->setFrameSlot(slot);
        asyncW->asyncBeginFrame(cmd);
    }
    // #1074: the facade's per-frame recording (uploads, offscreen segments, world overlays)
    // only in frames whose UI pass is recorded too: gVK.recordFrame closes the facade's frame
    // (upload ring marks, deferred frees). A VR frame can have no UI pass (no window image, or
    // the window minimised); its uploads wait for the next frame that has one. Desktop
    // (mXrSession false, never headless): exactly as before.
    const bool uiRecords = doUI && !headless;
    const bool uiFrame = !mXrSession || uiRecords;
    // #1076 VR: a VR world frame with the UI wall shown also lays the UI into the wall's own
    // image (cleared transparent, so the world shows between the floaters). Desktop: vrWorld
    // is false, so this is false and nothing below that depends on it runs.
    const bool vrUi = vrWorld && uiRecords && LLXrSession::instance().uiImageWanted() &&
                      mImpl->vrEnsureUiTarget(mImpl->mSwapchain.extent());
    // UI texture uploads queued since the last frame (minimap, font atlas, previews):
    // recorded first, before any pass samples them (no per-upload submit-and-wait).
    if (mImpl->mFacadeReady && uiFrame)
    {
        VK_HITCH_SCOPE("vk_uiUploads");
        gVK.recordUploads(cmd);
        std::array<LLRenderVK::PreparedUploadToken, Impl::kTexturePrepRecordCap> prepared{};
        const uint32_t preparedCount = gVK.takePreparedRecordedUploads(prepared.data(), (uint32_t)prepared.size());
        if (preparedCount > prepared.size())
            (void)recordTexturePrepTicket(0xffffu, 0); // fail closed; never submit an untracked staged read
        else
            for (uint32_t i = 0; i < preparedCount; ++i)
                (void)recordTexturePrepTicket(prepared[i].slot, prepared[i].generation);
        vkBreadcrumb(cmd, "f_ui_uploads");
    }
    // Offscreen render targets buffered this frame (#83: previews, visual-param
    // hints, the avatar appearance composite): before the world (which copies the
    // composite into its BOM slot) and the UI (which samples the previews).
    if (mImpl->mFacadeReady && uiRecords) { VK_HITCH_SCOPE("vk_uiOffscreen"); gVK.recordOffscreen(cmd, slot); vkBreadcrumb(cmd, "f_ui_offscreen"); }

    // Swapchain clear + transitions: `cmd`, or the async frame's WSI command buffer.
    // (#1075: a window-less VR frame has no window image to clear: no WSI command buffer)
    VkCommandBuffer wsiCmd = (asyncW && !headless) ? asyncW->asyncWsiCmd() : VK_NULL_HANDLE;
    VkCommandBuffer clearCmd = wsiCmd ? wsiCmd : cmd;

    // First scope = the acquire semaphore's wait stages (see the submit below), so the
    // layout transition + clear chain after the presentation engine releases the image.
    // (#1074: a window-less VR frame has no window image: every window step is skipped.)
    if (!headless)
    {
        imageBarrier(clearCmd, img,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT, 0,
                     VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
    }

    // Background clear. A mild dark slate behind the UI (or the animated teal when
    // no UI is drawn, so a human can confirm the frame is live).
    float pulse = 0.35f + 0.10f * (float)((mImpl->mFrameCounter >> 3) & 1);
    VkClearColorValue clear{};
    clear.float32[0] = doUI ? 0.06f : 0.10f;
    clear.float32[1] = doUI ? 0.06f : pulse;
    clear.float32[2] = doUI ? 0.08f : 0.55f;
    if (wantWorld && !doWorld)
    {
        // The world is wanted but its pipelines are still compiling (normally hidden by
        // the startup progress hold, llstartup.cpp; this is the post-timeout fallback):
        // a neutral daytime-sky backdrop behind the UI instead of a black world.
        clear.float32[0] = 0.42f; clear.float32[1] = 0.58f; clear.float32[2] = 0.78f;
    }
    clear.float32[3] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (!headless)
    {
        vkCmdClearColorImage(clearCmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             &clear, 1, &range);
    }
    vkBreadcrumb(clearCmd, "f_clear");

    bool hdrCompositionComplete = true;
    Impl::FgInputs fgIn;   // #1517 frame generation: what this frame's encode wrote for it
    bool labelsInAtlas = false;   // #1517 Task 7e: this frame's labels are in the slot's atlas (and the encode drew them)
    if (doWorld || doUI || layer)
    {
        VkExtent2D ext = mImpl->mSwapchain.extent();
        // The world's target: HDR's linear image; (#1536) on an SDR frame generation chain the present slot's
        // world image -- the generator's input as it is, no interface in it; else the frame's image itself.
        VkImageView view = mImpl->mHdrOutputActive ? mImpl->mDisplayFrame.linear().view
                         : sdrLayer ? mImpl->mFgPresent.fgHudless(fgSlot).view : imgView;
        // HDR (#1451): the UI, the world overlays and in-world text go into the UI layer. (#1536 SDR layer: the
        // present slot's own layer -- the generated frames compose that very image, no copy.)
        const VkImageView uiView = mImpl->mHdrOutputActive ? mImpl->mDisplayFrame.uiLayer().view
                                 : sdrLayer ? mImpl->mFgPresent.fgUi(fgSlot).view : view;
        // #1451 G5: a frame an SDR snapshot samples (a presented-frame capture, or a live snapshot
        // for an SDR image) renders its world through the SDR post chain, as an offscreen snapshot
        // does, so the shot is what SDR mode saves; the encode shows it at paper white.
        const bool sdrFrame = mImpl->mHdrOutputActive && doWorld && !vrWorld && mImpl->mWorld &&
                              (mImpl->mWorld->capturePending() != 0u || mImpl->mWorld->liveSnapWantsSdr());
        // #1517 Task 7e: on a frame that generates, the labels anchored in the world (name tags, chat bubbles,
        // hover text) are drawn into the slot's label atlas instead of the UI layer, and every picture of the
        // batch draws them between the world and the interface where they were at its moment: they glide with
        // the generated frames, sharp, and the generator never sees them. Not on a snapshot frame (its capture
        // points read the UI layer as before). Off, or no atlas: in the UI layer, stepping at the real rate.
        static LLCachedControl<bool> s_fg_labels(gSavedSettings, "RenderVKFrameGenMovingLabels", true);
        const bool fgLabels = layer && fgCount && s_fg_labels && doWorld && !vrWorld &&
            !sdrFrame && mImpl->mWorld && mImpl->mWorld->capturePending() == 0u &&
            !mImpl->mWorld->liveSnapPending(1u) && !mImpl->mWorld->liveSnapPending(2u) &&
            !mImpl->mWorld->liveSnapPending(3u) &&
            (mImpl->mSwapchain.displayStatus().encoding == vkdisplay::Encoding::Hdr10 || sdrLayer) &&
            mImpl->mFgPresent.ensureLabels(fgSlot, mImpl->mLabelAtlasSize, fgd.frameDone(), fgd.lastFrameDoneValue(),
                                           mImpl->frameGenBudget());
        // Every frame that owns a present slot, labels or not: the retired atlases the GPU is done with are
        // freed, and after llfgl::kIdleShrinkFrames frames in a row without labels (the setting off, snapshots,
        // SDR frames) the slot's atlas goes back too -- through the retire list, never under the GPU.
        if (fgChain && !headless)
        {
            mImpl->mLabelIdleFrames = fgLabels ? 0 : std::min(mImpl->mLabelIdleFrames + 1, llfgl::kIdleShrinkFrames);
            mImpl->mFgPresent.tendLabels(fgSlot, mImpl->mLabelIdleFrames >= llfgl::kIdleShrinkFrames,
                                         fgd.frameDone(), fgd.lastFrameDoneValue());
        }
        {
            LLRenderVK::LabelTarget target;
            if (fgLabels)
            {
                const GpuImage& atlas = mImpl->mFgPresent.labelAtlas(fgSlot);
                const GpuBuffer& list = mImpl->mFgPresent.labelBuffer(fgSlot);
                target = LLRenderVK::LabelTarget{atlas.image, atlas.view, atlas.extent, list.buffer, list.size,
                                                 mImpl->mFrameCounter};
            }
            gVK.setLabelTarget(fgLabels ? &target : nullptr);
        }

        // TRANSFER_DST -> COLOR_ATTACHMENT so both the world pass and the UI pass
        // render into this image view with the clear preserved (LOAD). (Async frames: the
        // world's later swap-image accesses chain on this barrier's COLOR_ATTACHMENT
        // second scope across the submission boundary -- same queue, submission order.)
        if (!headless)
        {
            imageBarrier(clearCmd, img,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);   // LOAD reads
        }
        if (wsiCmd && asyncW) asyncW->vkEndOrNoteOom(wsiCmd);   // #697 (OOM: the frame is dropped)
        else if (wsiCmd) VK_CHECK(vkEndCommandBuffer(wsiCmd));

        if (mImpl->mHdrOutputActive)
        {
            VkClearColorValue linearClear = clear;
            linearClear.float32[0] *= mDisplayPaperWhiteNits;
            linearClear.float32[1] *= mDisplayPaperWhiteNits;
            linearClear.float32[2] *= mDisplayPaperWhiteNits;
            mImpl->mDisplayFrame.beginLinear(cmd, linearClear);
            mImpl->mDisplayFrame.beginUiLayer(cmd, true);   // transparent: nothing drawn over the world yet
            // The snapshot capture image is freed once shots stop: kCaptureIdleFrames frames after its
            // last use, acquire() has waited every frame slot that used it.
            constexpr uint32_t kCaptureIdleFrames = 240;
            if (mImpl->mDisplayFrame.capture().image && ++mImpl->mCaptureIdleFrames > kCaptureIdleFrames)
            {
                mImpl->mDisplayFrame.releaseCapture();
                mImpl->mCaptureIdleFrames = 0;
            }
        }
        if (sdrLayer)
        {
            // #1536 the present slot's world image, cleared as the frame's image is (the world LOADs it), and
            // its interface layer, cleared transparent: nothing drawn over the world yet. Both rewritten whole
            // (UNDEFINED): the present thread's last reads of this slot come before the frame's wait for its
            // release (the frame's first submission waits it).
            const GpuImage& slotWorld = mImpl->mFgPresent.fgHudless(fgSlot);
            imageBarrier(cmd, slotWorld.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            vkCmdClearColorImage(cmd, slotWorld.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
            imageBarrier(cmd, slotWorld.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);   // LOAD reads
            vkDisplayLayerBegin(cmd, mImpl->mFgPresent.fgUi(fgSlot), true);
        }
        // HDR snapshots that include the HUD (capture point 2) or the UI (point 3): the shot reads
        // the world with the UI layer composited -- the display encode writes that composite as
        // FP16 nits into the frame's capture image, which stands in for the swap image for the
        // copy. Only on a frame that captures there; no shot, no pass.
        // #1536 SDR frame generation: the same shots read the world with the slot's layer composed over it as the
        // frame's picture will be -- into the backbuffer (the final composite rewrites it at the frame's end),
        // which stands in for the swap image for the copy.
        auto recordCapturePoint = [&](uint32_t point, uint32_t w, uint32_t h)
        {
            LLCleanRendererVK* W = mImpl->mWorld;
            VkDisplayFrame& frame = mImpl->mDisplayFrame;
            bool composite = false;
            bool sdrComposite = false;
            const GpuImage& slotWorld = mImpl->mFgPresent.fgHudless(fgSlot);
            if (mImpl->mHdrOutputActive && !vrWorld &&
                (W->capturePending() == point || W->liveSnapPending(point)))
            {
                if (frame.prepareCapture(0, mImpl->mHdrBudget))
                {
                    const vkdisplay::Request request{true, mDisplayPaperWhiteNits, mDisplayPeakNits};
                    frame.sampleLinear(cmd);
                    frame.sampleUiLayer(cmd);
                    frame.beginCapture(cmd);
                    composite = mImpl->mDisplayEncode.record(cmd, slot, frame.linear().view, frame.uiLayer().view,
                        frame.capture().view, ext, request, VkDisplayEncode::Content::CompositeNits,
                        frame.residentBytes(), mImpl->mHdrBudget, false, sdrFrame);
                    frame.resumeLinear(cmd);
                    frame.beginUiLayer(cmd, false);
                }
                if (composite)
                {
                    mImpl->mCaptureIdleFrames = 0;
                    W->setSwapImage(frame.capture().image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                    W->displayOutputState().generation);
                }
                else
                    LL_WARNS_ONCE("Vulkan") << "HDR snapshot: no room for the UI/HUD composite; the shot has the world only"
                                            << LL_ENDL;
            }
            else if (sdrLayer && !vrWorld && !headless && (W->capturePending() == point || W->liveSnapPending(point)))
            {
                const GpuImage& slotUi = mImpl->mFgPresent.fgUi(fgSlot);
                // The world's and the layer's writers so far -> the composite's reads; then both back for the
                // rest of the frame (the world's later passes, the interface LOAD the layer).
                imageBarrier(cmd, slotWorld.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
                vkDisplayLayerSample(cmd, slotUi);
                // (labels: a frame with a snapshot or a live snapshot pending draws them into the layer -- fgLabels
                // is off then -- so the shot has them)
                sdrComposite = mImpl->mDisplayEncode.recordLayerComposite(cmd, slot, slotWorld.view, slotUi.view, imgView,
                                                                          ext, VK_NULL_HANDLE, nullptr, true);
                imageBarrier(cmd, slotWorld.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
                vkDisplayLayerBegin(cmd, slotUi, false);
                if (sdrComposite)
                    W->setSwapImage(img, mImpl->mSwapchain.format(), W->displayOutputState().generation);
                else
                    LL_WARNS_ONCE("Vulkan") << "Snapshot: the interface could not be composed over the world; the shot has "
                                               "the world only" << LL_ENDL;
            }
            W->recordCaptureAt(cmd, point, w, h);
            W->recordLiveSnap(cmd, point, w, h);   // #873
            if (composite)
                W->setSwapImage(frame.linear().image, VK_FORMAT_R16G16B16A16_SFLOAT,
                                W->displayOutputState().generation);
            if (sdrComposite)
                W->setSwapImage(slotWorld.image, mImpl->mSwapchain.format(), W->displayOutputState().generation);
        };
        // #1074 VR world frame: the world's colour target is the eye-size VR target (cleared
        // here; the world LOADs it), not the window image. Its previous use (last frame's eye
        // copies and mirror blit, transfer reads) is ordered before by the first scope.
        VkImage worldImg = mImpl->mHdrOutputActive ? mImpl->mDisplayFrame.linear().image
                         : sdrLayer ? mImpl->mFgPresent.fgHudless(fgSlot).image : img;
        VkImageView worldView = view;
        VkExtent2D worldExt = ext;
        uint32_t worldLayers = 1;   // #1075: the VR target's layers (one per eye in stereo)
        if (vrWorld)
        {
            worldImg = mImpl->mVrTarget.image;
            worldLayers = std::max(1u, mImpl->mVrTarget.layers);
            // (stereo: the whole array -- the world's final pass writes one layer per view)
            worldView = worldLayers > 1u && mImpl->mVrTarget.arrayView ? mImpl->mVrTarget.arrayView : mImpl->mVrTarget.view;
            worldExt = mImpl->mVrTarget.extent;
            imageBarrier(cmd, worldImg, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, worldLayers);
            VkClearColorValue black{};
            black.float32[3] = 1.0f;
            const VkImageSubresourceRange worldRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, worldLayers};
            vkCmdClearColorImage(cmd, worldImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &worldRange);
            imageBarrier(cmd, worldImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, worldLayers);
        }
        // #1076 VR: the UI wall image, cleared to transparent black (the UI's premultiplied
        // blend over it gives the coverage in alpha). Its previous use (last frame's copy into
        // the UI swapchain) is ordered before by the first scope.
        if (vrUi)
        {
            const VkImage uiT = mImpl->mVrUiTarget.image;
            imageBarrier(cmd, uiT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkClearColorValue clearUi{};   // (0, 0, 0, 0)
            vkCmdClearColorImage(cmd, uiT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearUi, 1, &range);
            imageBarrier(cmd, uiT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
        }

        // WORLD FIRST: GPU-driven static-opaque geometry into the swapchain, with
        // the renderer's own depth buffer. Its dynamic-rendering scope LOADs the
        // color (keeps the clear) and CLEARs depth.
        if (doWorld)
        {
            VKDrawPush draw{}; VKCullPush cull{}; VKSkyPush sky{};
            VK_HITCH_SCOPE("vk_worldSetup");
            { VK_HITCH_SCOPE("ws_fillPushes"); fillWorldPushes(*world, draw, cull, sky); }

            mImpl->mWorld->setFrameSlot(slot);   // ring-index the instance staging copy
            { VK_HITCH_SCOPE("ws_specTick"); mImpl->mWorld->specFrameTick(); }   // setting specialization: bind / fall back at the frame boundary
            { VK_HITCH_SCOPE("ws_lights"); mImpl->mWorld->setLights(*world); }
            { VK_HITCH_SCOPE("ws_probes"); mImpl->mWorld->setProbes(*world); }
            { VK_HITCH_SCOPE("ws_shadows"); mImpl->mWorld->setShadows(*world); }
            { VK_HITCH_SCOPE("ws_mirror"); mImpl->mWorld->setMirror(*world); }
            { VK_HITCH_SCOPE("ws_temporal"); mImpl->mWorld->setTemporal(*world); }
            mImpl->mWorld->mHideSelf = world->hide_self;
            mImpl->mWorld->mHideSelfHead = world->hide_self_head;
            mImpl->mWorld->mHeadlessSelf = world->headless_self;   // #1585
            mImpl->mWorld->mOrthoView = world->ortho_view;   // orthographic view (GunMetal-DX port)
            { VK_HITCH_SCOPE("ws_selection"); mImpl->mWorld->setSelection(*world); }
            { VK_HITCH_SCOPE("ws_physShapes"); mImpl->mWorld->setPhysShapes(*world); }   // #1176
            { VK_HITCH_SCOPE("ws_pick"); mImpl->mWorld->setPick(*world); }   // raster (#702) + particle (#481) cursor pick rays
            { VK_HITCH_SCOPE("ws_acoustics"); mImpl->mWorld->setAcoustics(*world); }   // acoustics probe (#683)
            { VK_HITCH_SCOPE("ws_atmos"); mImpl->mWorld->setAtmosphere(*world); }
            mImpl->mWorld->setSwapImage(worldImg,
                                        (mImpl->mHdrOutputActive && !vrWorld) ? VK_FORMAT_R16G16B16A16_SFLOAT : mImpl->mSwapchain.format(),
                                        mImpl->mWorld->displayOutputState().generation);
            // HDR (#1451), the SDR layer (#1536): the HUD attachments and selection outlines draw into the UI layer.
            mImpl->mWorld->setUiLayer((layer && !vrWorld) ? uiView : VK_NULL_HANDLE);
            // #1517: this frame's picture is meant for the generator -- DLSS-G or FSR FG, HDR10 or the SDR layer
            // (the world's falling precipitation then goes into the UI layer, which every generated frame lays
            // over the generated world: LLCleanRendererVK::wxLateWanted).
            mImpl->mWorld->setFrameGenFrame(fgCount != 0 && layer && !vrWorld && !sdrFrame);
            mImpl->mWorld->setSdrFrame(sdrFrame);
            mImpl->mWorld->setSwapchainBytes(swapchainBytes(mImpl->mSwapchain));   // #407
            // World-overlay segment (property/ban lines, beacons, manipulators),
            // recorded against the world depth before the HUD pass clears it, then
            // the pre-HUD text layer (name tags, hover text, chat bubbles: #468) so
            // HUD attachments draw over in-world text. A no-UI "with HUD" snapshot
            // (capture point 2, right after the world) must not contain that text:
            // that frame leaves it to the UI replay, after the capture.
            LLCleanRendererVK* W = mImpl->mWorld;
            // Normally overlays draw into the UI layer at world depth. On a generating
            // frame their color instead enters the world with matching slot-owned guides;
            // pre-HUD labels still use the moving atlas / UI layer, never these guides.
            const VkImageView uiOverlayView = !layer ? VK_NULL_HANDLE : uiView;
            mImpl->mWorld->mWorldOverlayHook =
                [slot, uiRecords, W, uiOverlayView, state=mImpl, fgSlot,
                 fgOverlays=(fgCount != 0 && layer && !vrWorld && !sdrFrame),
                 worldFormat=(mImpl->mHdrOutputActive ? mImpl->mDisplayFrame.linear().format : mImpl->mSwapchain.format()),
                 white=(mImpl->mHdrOutputActive ? mDisplayPaperWhiteNits : 0.0f)]
                (VkCommandBuffer c, VkImageView color, VkImageView depth,
                 VkImage depthImage, uint32_t w, uint32_t h)
                {
                    // Executed in recordWorld, on the MAIN recording thread, before HUDs.
                    if (uiRecords && fgOverlays && gVK.worldOverlayPending()) {
                        LLRenderVK::FrameGenOverlay overlay;
                        overlay.color = color; overlay.colorFormat = worldFormat; overlay.whiteNits = white;
                        const bool inputs = W->frameGenInputs(overlay.sourceDepth, overlay.sourceMotion);
                        overlay.depth = &state->mFgPresent.fgDepth(fgSlot);
                        overlay.motion = &state->mFgPresent.fgMotion(fgSlot);
                        std::memcpy(&overlay.reprojection[0][0], W->taaReproj(0), sizeof(glm::mat4));
                        const float* jitter = W->frameGenOverlayJitter();
                        overlay.jitterNdc = {jitter[2], jitter[3]};
                        if (inputs && state->mFgPresent.inputsFit(overlay.sourceDepth->extent, overlay.sourceDepth->format,
                                                                  overlay.sourceMotion->extent, overlay.sourceMotion->format) &&
                            gVK.prepareFrameGenOverlay(slot, overlay)) {
                            Impl::FgInputs guides;
                            guides.written = true; guides.depth = overlay.sourceDepth; guides.motion = overlay.sourceMotion;
                            if (state->recordFrameGenInputs(c, fgSlot, guides)) {
                                gVK.recordFrameGenOverlay(c, slot, overlay, depth, w, h);
                                state->mFgOverlayGuidesReady = true;
                                state->mFgOverlaySlot = fgSlot;
                            } else state->mFgOverlayRefused = true;
                        } else state->mFgOverlayRefused = true;
                    }
                    if (uiRecords)   // (#1074: = doUI on the desktop)
                        gVK.recordWorldOverlay(c, slot, uiOverlayView ? uiOverlayView : color, depth, depthImage, w, h,
                                               W->capturePending() != 2u && !W->liveSnapPoint2Pending());
                };
            // #1141 VR STEREO: the overlay segment into each eye of the VR target (its layer views),
            // each world-space batch re-projected with eye v's view-projection: eyeFix[v] = the
            // eye's world -> clip times the inverse of the overlay camera's (world->proj * view),
            // in double (the head camera is km from the origin). Only when the UI build left
            // overlay batches and the facade's pipelines match the target's format (HDR output
            // builds them for RGBA16F: no overlay in the eyes then, logged once). Desktop: never.
            if (mXrSession)
            {
                std::function<void(VkCommandBuffer, const VkImageView*, uint32_t, uint32_t, uint32_t)> vh;
                const GpuImage& vt = mImpl->mVrTarget;
                const uint32_t nv = std::min<uint32_t>(world->view_count, LLVKWorldFrame::kMaxViews);
                if (vrWorld && uiRecords && gVK.worldOverlayStereo() && gVK.worldOverlayPending() &&
                    vt.layers >= nv && nv > 1u)
                {
                    if (gVK.frameColorFormat() == vt.format)
                    {
                        glm::dmat4 P, V;
                        for (int k = 0; k < 16; ++k)
                        {
                            (&P[0][0])[k] = world->proj[k];
                            (&V[0][0])[k] = world->view[k];
                        }
                        const glm::dmat4 invHead = glm::inverse(P * V);
                        std::array<glm::mat4, LLVKWorldFrame::kMaxViews> fix{};
                        std::array<VkImageView, LLVKWorldFrame::kMaxViews> colors{};
                        for (uint32_t v = 0; v < nv; ++v)
                        {
                            glm::dmat4 E;
                            for (int k = 0; k < 16; ++k) (&E[0][0])[k] = world->views[v].view_proj[k];
                            fix[v] = glm::mat4(E * invHead);
                            colors[v] = v == 0u ? vt.view : vt.layerView[v];
                        }
                        vh = [slot, fix, colors](VkCommandBuffer c, const VkImageView* depth, uint32_t n,
                                                 uint32_t w, uint32_t h)
                        {
                            gVK.recordWorldOverlayViews(c, slot, colors.data(), depth,
                                                        std::min<uint32_t>(n, (uint32_t)colors.size()), w, h, fix.data());
                        };
                    }
                    else
                    {
                        static bool s_warned = false;
                        if (!s_warned)
                        {
                            s_warned = true;
                            LL_WARNS("VR") << "edit overlays: the UI pipelines' format (" << (int)gVK.frameColorFormat()
                                           << ") is not the headset target's (" << (int)vt.format
                                           << "): manipulators and beacons are not drawn in the headset (HDR output on?)"
                                           << LL_ENDL;
                        }
                    }
                }
                mImpl->mWorld->mWorldOverlayViewsHook = std::move(vh);
            }
            // #1133 VR: the stereo frame's in-world text, world billboards for both eyes (the
            // eyes' unjittered world -> clip and positions; with no located eyes both are the
            // head, still one image). Desktop: mXrSession false -- never set, never called.
            if (mXrSession && gVK.worldTextViews() && world->view_count >= 2u)
            {
                struct Eyes { float vp[2][16]; float eye[2][3]; } eyes;
                for (uint32_t v = 0; v < 2u; ++v)
                {
                    std::memcpy(eyes.vp[v], world->views[v].view_proj, sizeof(eyes.vp[v]));
                    for (int k = 0; k < 3; ++k) eyes.eye[v][k] = world->views[v].eye[k];
                }
                const VkFormat eyeFmt = mImpl->mVrTarget.format;
                mImpl->mWorld->mWorldTextViewsHook =
                    [slot, uiRecords, eyes, eyeFmt](VkCommandBuffer c, VkImageView colorArray, VkImage depthImage,
                                                    VkImageView depthArray, VkImageLayout depthLayout, uint32_t w, uint32_t h)
                    {
                        if (!uiRecords) return;   // (no UI pass this frame: its uploads wait too)
                        gVK.recordWorldTextViews(c, slot, colorArray, eyeFmt, depthImage, depthArray, depthLayout, w, h,
                                                 eyes.vp, eyes.eye);
                        vrLogWorldText(gVK.worldTextStats());
                    };
            }
            else if (mImpl->mWorld->mWorldTextViewsHook)
                mImpl->mWorld->mWorldTextViewsHook = nullptr;
            const float* hudViewProj = world->hud_enabled ? world->hud_view_proj : nullptr;
            if (mXrSession)
            {
                // #1076 VR: HUD attachments go onto the UI wall's image (laid out in window
                // pixels, like the UI), never into the eye-size world; no wall, no HUDs.
                // Desktop: mXrSession is false -- the HUD target is never written.
                LLCleanRendererVK::HudTarget ht;
                if (vrUi && hudViewProj && mImpl->vrEnsureHudDepth(ext, mImpl->mWorld->worldDepthFormat()))
                {
                    ht.color = mImpl->mVrUiTarget.view;
                    ht.depth = mImpl->mVrHudDepth.view;
                    ht.depthImage = mImpl->mVrHudDepth.image;
                    ht.width = ext.width;
                    ht.height = ext.height;
                }
                mImpl->mWorld->mHudTarget = ht;
                if (vrWorld && !ht.color) hudViewProj = nullptr;
            }
            // #1075: the layers worldView spans (the VR target's 2D_ARRAY view: one per eye; else 1)
            mImpl->mWorld->mOutputLayers = (vrWorld && worldLayers > 1u && worldView == mImpl->mVrTarget.arrayView)
                                         ? worldLayers : 1u;
            // #1141: those layers' 2D views (the per-eye edit selection passes). VR sessions only.
            if (mXrSession)
            {
                const GpuImage& vt = mImpl->mVrTarget;
                for (uint32_t v = 0; v < 4u; ++v)
                    mImpl->mWorld->mOutputLayerView[v] = (mImpl->mWorld->mOutputLayers > 1u && v < vt.layers)
                        ? (v == 0u ? vt.view : vt.layerView[v]) : VK_NULL_HANDLE;
            }
            // #1075 VR: the lens' hidden area per eye (XR_KHR_visibility_mask), drawn into the eyes'
            // depth in a stereo world frame whose views are the located eyes. Desktop: never called.
            if (mXrSession)
            {
                const LLXrSession& xs = LLXrSession::instance();
                mImpl->mWorld->setVisibilityMask(xs.visibilityMaskGeneration(), xs.visibilityMask(0), xs.visibilityMask(1),
                                                 mImpl->mWorld->mOutputLayers > 1u && world->views_are_eyes);
            }
            mImpl->mWorld->recordWorld(cmd, worldView, worldExt.width, worldExt.height,
                                       draw, cull, sky,
                                       hudViewProj,
                                       world->hud_viewport);
            hdrCompositionComplete = mImpl->mWorld->displayOutputComplete();
            mImpl->mWorld->setUiLayer(VK_NULL_HANDLE);   // this frame's only: never a stale view later
            // Async compute: recordWorld may have ended `cmd`; continue in its last one.
            if (asyncW) cmd = asyncW->asyncCurrentCmd(cmd);
            // user snapshot, world + HUD (before the UI replay)
            recordCapturePoint(2, worldExt.width, worldExt.height);
            // Video Studio (#1668): a recording takes the world here, before the interface is composed
            // (the studio hides the HUDs). SDR desktop frames only; HDR output records PNG frames instead.
            if (!vrWorld && !headless && !mImpl->mHdrOutputActive && SLVideoRecorder::recording())
                SLVideoRecorder::captureWorld(cmd, worldImg, mImpl->mSwapchain.format(), worldExt.width, worldExt.height,
                                              mImpl->mDevice);
        }
        // #1074 VR: the eye-size world, aspect-fit onto the window (the mirror), before the UI.
        if (vrWorld) mImpl->vrRecordMirror(cmd, img, ext);

        if (uiRecords && mImpl->mMainUIRecorder && !headless)
        {
            VkUISurfaceTarget target;
            target.cmd = cmd;
            target.colorImage = mImpl->mHdrOutputActive ? mImpl->mDisplayFrame.uiLayer().image
                              : sdrLayer ? mImpl->mFgPresent.fgUi(fgSlot).image : img;
            target.colorView = uiView;
            target.colorFormat = layer ? VK_FORMAT_R8G8B8A8_UNORM : mImpl->mSwapchain.format();
            target.extent = ext;
            target.dpiScaleX = target.dpiScaleY = SDL_GetWindowDisplayScale(mImpl->mWindow);
            target.frameSerial = targetSerial = mImpl->reserveSurfaceSerial();
            target.completedSerial = mImpl->mSurfaceCompleted;
            // The backdrop is the world (HDR: its linear-nits image, not the UI layer; #1536 the SDR layer: the
            // slot's world image -- the world alone, HUDs not in it).
            target.sceneImage = mImpl->mHdrOutputActive ? mImpl->mDisplayFrame.linear().image
                              : sdrLayer ? mImpl->mFgPresent.fgHudless(fgSlot).image : img;
            target.sceneView = view;
            target.sceneFormat = mImpl->mHdrOutputActive ? VK_FORMAT_R16G16B16A16_SFLOAT : mImpl->mSwapchain.format();
            target.sceneExtent = ext;
            target.sceneLayout = target.colorLayout;
            target.sceneUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
            // #703: recorded by recordFrame() at its mark in the UI batches (the z slot the
            // presented views had in the LLView draw), or under all of them without a mark.
            // #1451: with HDR on the UI target is the SDR UI layer (sRGB 0..1), which the display
            // encode composites at paper white -- RmlUi writes it as is, like the LLView UI.
            target.displayPaperWhiteNits = 0.f;
            gVK.setExternalLayer([this, target](VkCommandBuffer layerCmd, uint32_t segment) mutable
            {
                target.cmd          = layerCmd;
                target.layerSegment = segment;
                mImpl->mMainUIRecorder(target);
            });
        }
        // UI ON TOP: replay the buffered UI batches over the (cleared + world)
        // color. Its own dynamic-rendering scope also LOADs, so it composites.
        if (uiRecords)
        {
            if (mImpl->mMainUIRecorder && !targetSerial) targetSerial = mImpl->reserveSurfaceSerial();
            VK_HITCH_SCOPE("vk_uiRecord");
            vkBreadcrumb(cmd, "f_ui");
            // #1076 VR: the same UI into the wall's image first (the batches stay for the
            // window's replay below), then ready for the copy into the UI swapchain.
            if (vrUi)
            {
                gVK.replayTo(cmd, slot, mImpl->mVrUiTarget.view);
                if (mImpl->mMainUIRecorder)
                {
                    VkUISurfaceTarget target;
                    target.surfaceID = NativeSurfaceVRUI;
                    target.cmd = cmd;
                    target.colorImage = mImpl->mVrUiTarget.image;
                    target.colorView = mImpl->mVrUiTarget.view;
                    target.colorFormat = mImpl->mVrUiTarget.format;
                    target.extent = ext;
                    target.dpiScaleX = target.dpiScaleY = SDL_GetWindowDisplayScale(mImpl->mWindow);
                    target.frameSerial = targetSerial;
                    target.completedSerial = mImpl->mSurfaceCompleted;
                    mImpl->mMainUIRecorder(target);
                }
                imageBarrier(cmd, mImpl->mVrUiTarget.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                             VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
                vkBreadcrumb(cmd, "f_ui_wall");
            }
            mImpl->mVrUiFresh = vrUi;   // #1594 (the wall image a warp frame copies: TRANSFER_SRC from here)
            if (layer)
            {
                // The layer's earlier writers this frame (its clear, world overlays, HUD) ->
                // the UI pass's LOAD.
                VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
                mb.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                mb.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                mb.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
                mb.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
                VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
                dep.memoryBarrierCount = 1; dep.pMemoryBarriers = &mb;
                vkCmdPipelineBarrier2(cmd, &dep);
            }
            gVK.setFrameColorView(uiView);
            gVK.recordFrame(cmd, slot);
            vkBreadcrumb(cmd, "f_ui_done");
        }
        // user snapshot, full frame incl. UI (the swap image was set by the world pass;
        // #1074 in VR world frames: the eye-size world without the UI)
        if (doWorld) recordCapturePoint(3, worldExt.width, worldExt.height);
        // (The SDR snapshot frame's captures -- points 1 to 3 -- read it as SDR; done.)
        if (doWorld && mImpl->mWorld) mImpl->mWorld->setSdrFrame(false);
        // (#1517) Why a frame that should generate has no generator inputs: the real frame goes out alone.
        auto fgInputsRefusal = [&]() -> const char*
        {
            const GpuImage* inDepth = nullptr;
            const GpuImage* inMotion = nullptr;
            return !(mImpl->mWorld && mImpl->mWorld->frameGenInputs(inDepth, inMotion))
                       ? "this frame has no depth / motion inputs"
                 : !fgIn.camera ? "this frame has no camera data"
                 : !mImpl->mFgPresent.inputsFit(inDepth->extent, inDepth->format, inMotion->extent, inMotion->format)
                       ? "the upscaler's inputs changed size this frame"
                       : "the frame's picture could not be made";
        };
        if (mImpl->mHdrOutputActive)
        {
            mImpl->mDisplayFrame.sampleLinear(cmd);
            mImpl->mDisplayFrame.sampleUiLayer(cmd);
            if (hdrCompositionComplete && doWorld && mImpl->mWorld && !sdrFrame)
                mImpl->mWorld->recordHdrEncodeInputProbe(cmd, mImpl->mDisplayFrame.linear().view,
                                                         ext.width, ext.height);
            const vkdisplay::Request request{true, mDisplayPaperWhiteNits, mDisplayPeakNits};
            auto presentHdrFallback = [&]()
            {
                const auto linearFallback = vkdisplay::LinearNits{
                    clear.float32[0] * mDisplayPaperWhiteNits,
                    clear.float32[1] * mDisplayPaperWhiteNits,
                    clear.float32[2] * mDisplayPaperWhiteNits};
                const auto encodedFallback = vkdisplay::encode(linearFallback,
                    vkdisplay::encodingOf(mImpl->mHdrEncodeTarget));
                VkRenderingAttachmentInfo fallbackColour{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
                fallbackColour.imageView=imgView;
                fallbackColour.imageLayout=VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                fallbackColour.loadOp=VK_ATTACHMENT_LOAD_OP_CLEAR;
                fallbackColour.storeOp=VK_ATTACHMENT_STORE_OP_STORE;
                fallbackColour.clearValue.color.float32[0]=encodedFallback.red;
                fallbackColour.clearValue.color.float32[1]=encodedFallback.green;
                fallbackColour.clearValue.color.float32[2]=encodedFallback.blue;
                fallbackColour.clearValue.color.float32[3]=1.f;
                VkRenderingInfo fallbackRendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
                fallbackRendering.renderArea={{0,0},ext};
                fallbackRendering.layerCount=1;
                fallbackRendering.colorAttachmentCount=1;
                fallbackRendering.pColorAttachments=&fallbackColour;
                vkCmdBeginRendering(cmd,&fallbackRendering);
                vkCmdEndRendering(cmd);
            };
            bool encoded = false;
            if (!hdrCompositionComplete)
            {
                // A failed/missing HDR final pass leaves relative-linear scene values,
                // not absolute nits. Never feed that partial image to the PQ/scRGB
                // encoder; show a valid encoded background and fail closed to SDR on
                // the next drained swapchain transaction.
                LL_WARNS("Vulkan") << "HDR composition incomplete (world final/UI pass); refusing partial encode and"
                                       " falling back to SDR" << LL_ENDL;
                mDisplayHdrPipelineReady = false;
                refreshDisplayHdrPolicy();
                mImpl->mNeedsRecreate = true;
                mImpl->mOutputPreviewValid = false;
                presentHdrFallback();
                fgIn.refused = "the frame's picture could not be made";   // (#1517 direct)
            }
            else
            {
                // #1517 FRAME GENERATION: a frame it runs on also gets the world alone and the UI layer
                // in the output's encoding from this same pass (into its present slot's images), then its
                // generator inputs copied below. Any piece missing (a snapshot frame, no DLSS this frame or
                // no constants from it, the images or the pipeline refused): this frame is not generated,
                // nothing else changes.
                LLVKFrameGen& fg = LLVKFrameGen::instance();
                // #1517 Task 7e: the labels drawn into the slot's atlas this frame (they are in no other layer).
                const bool labelsDrawn = fgLabels && gVK.labelFrame().count > 0;
                labelsInAtlas = labelsDrawn;
                const GpuImage* fgDepth = nullptr;
                const GpuImage* fgMotion = nullptr;
                const bool fgFrame = fgCount && !sdrFrame && mImpl->mDisplayEncode.frameGenOutputs() &&
                    mImpl->mWorld && mImpl->mWorld->frameGenInputs(fgDepth, fgMotion) &&
                    (fgIn.camera = fg.takeConstants(fgIn.constants)) &&
                    mImpl->mFgPresent.inputsFit(fgDepth->extent, fgDepth->format, fgMotion->extent, fgMotion->format);
                if (fgFrame) mImpl->mFgPresent.beginInputs(cmd, fgSlot);
                encoded = mImpl->mDisplayEncode.record(cmd, slot,
                    mImpl->mDisplayFrame.linear().view, mImpl->mDisplayFrame.uiLayer().view,
                    imgView, ext,
                    request, VkDisplayEncode::Content::CompleteDisplay,
                    mImpl->mDisplayFrame.residentBytes(), mImpl->mHdrBudget, true, sdrFrame,
                    fgFrame ? mImpl->mFgPresent.fgHudless(fgSlot).view : VK_NULL_HANDLE,
                    fgFrame ? mImpl->mFgPresent.fgUi(fgSlot).view : VK_NULL_HANDLE,
                    labelsDrawn ? mImpl->mFgPresent.labelAtlas(fgSlot).view : VK_NULL_HANDLE,
                    labelsDrawn ? &mImpl->mFgPresent.labelBuffer(fgSlot) : nullptr);
                if (fgFrame)
                {
                    mImpl->mFgPresent.endInputs(cmd, fgSlot);
                    fgIn.written = encoded;
                    fgIn.depth = fgDepth;
                    fgIn.motion = fgMotion;
                }
                if (fgCount && !fgIn.written) fgIn.refused = fgInputsRefusal();
                if (!encoded)
                {
                    LL_WARNS("Vulkan") << "HDR final transfer failed; presenting a visible background and disabling HDR"
                                           " until retried" << LL_ENDL;
                    mDisplayHdrPipelineReady = false;
                    refreshDisplayHdrPolicy();
                    mImpl->mNeedsRecreate = true;
                    mImpl->mOutputPreviewValid = false;
                    presentHdrFallback();
                }
            }
        }
        if (sdrLayer)
        {
            // #1536 SDR FRAME GENERATION: the real picture. The world is in the present slot's world image as
            // SDR draws it (the generator's input: nothing re-encoded, no interface in it), the interface in the
            // slot's layer, this frame's moving labels in its atlas; one composite lays labels, then the layer,
            // over the world into the backbuffer, as the SDR swapchain would have blended them. The present
            // thread runs the same composite over each generated world, with the same layer and atlas. A frame
            // on this chain that generates nothing (no world, a snapshot frame) is composed the same way.
            const GpuImage& slotWorld = mImpl->mFgPresent.fgHudless(fgSlot);
            const GpuImage& slotUi = mImpl->mFgPresent.fgUi(fgSlot);
            // The frame's writers of the world image (the world's passes, its copies) and of the layer -> the
            // composite's reads.
            imageBarrier(cmd, slotWorld.image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            vkDisplayLayerSample(cmd, slotUi);
            // #1517 Task 7e: the labels drawn into the slot's atlas this frame (they are in no other layer).
            const bool labelsDrawn = fgLabels && gVK.labelFrame().count > 0;
            const bool composed = mImpl->mDisplayEncode.recordLayerComposite(cmd, slot, slotWorld.view, slotUi.view,
                imgView, ext,
                labelsDrawn ? mImpl->mFgPresent.labelAtlas(fgSlot).view : VK_NULL_HANDLE,
                labelsDrawn ? &mImpl->mFgPresent.labelBuffer(fgSlot) : nullptr);
            labelsInAtlas = labelsDrawn && composed;
            // Then as the present thread reads them (its composites, the generator, its copy): GENERAL, the
            // state LLVKFrameGenPresent::endInputs leaves HDR10's in.
            for (const GpuImage* i : {&slotWorld, &slotUi})
                imageBarrier(cmd, i->image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                             VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
                             VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_MEMORY_READ_BIT);
            if (!composed)
            {
                // (the gate above makes this unreachable while the composite exists; the backbuffer keeps its
                // clear, and the chain is rebuilt as a normal one, where SDR draws directly)
                LL_WARNS("Vulkan") << "Frame generation: the SDR picture could not be composed; presenting a visible "
                                      "background and going back to a normal swapchain" << LL_ENDL;
                fgd.imagesRefused("no display pipeline for generated frames");
                mImpl->mFgRefusalRebuild = true;
                mImpl->mNeedsRecreate = true;
                mImpl->mOutputPreviewValid = false;
                fgIn.refused = "the frame's picture could not be made";
            }
            else if (fgCount)
            {
                // The generator's inputs: the world image is written; the upscaler's depth and motion and the
                // camera are copied / taken as in HDR10. Any piece missing: this frame is not generated.
                const GpuImage* fgDepth = nullptr;
                const GpuImage* fgMotion = nullptr;
                if (mImpl->mWorld && mImpl->mWorld->frameGenInputs(fgDepth, fgMotion) &&
                    (fgIn.camera = fgd.takeConstants(fgIn.constants)) &&
                    mImpl->mFgPresent.inputsFit(fgDepth->extent, fgDepth->format, fgMotion->extent, fgMotion->format))
                {
                    fgIn.written = true;
                    fgIn.depth = fgDepth;
                    fgIn.motion = fgMotion;
                }
                else
                    fgIn.refused = fgInputsRefusal();
            }
        }
        // #1517 Task 7e: the atlas size follows the labels: one step bigger when some did not fit, one step
        // smaller after llfgl::kIdleShrinkFrames frames in a row whose labels would all have fit it.
        if (fgLabels)
        {
            const LLRenderVK::LabelFrame& lf = gVK.labelFrame();
            const VkExtent2D used = mImpl->mFgPresent.labelAtlas(fgSlot).extent;
            mImpl->mLabelFitsSmallerFrames = (lf.fitsSmaller && !lf.overflowed) ? mImpl->mLabelFitsSmallerFrames + 1 : 0;
            const llfgl::Extent next = llfgl::nextAtlasSize(llfgl::Extent{used.width, used.height}, lf.overflowed,
                                                            mImpl->mLabelFitsSmallerFrames);
            if (next.w != used.width || next.h != used.height) mImpl->mLabelFitsSmallerFrames = 0;
            mImpl->mLabelAtlasSize = VkExtent2D{next.w, next.h};
        }
        // #1517 frame generation: once the real frame is complete (encoded, or its fallback), its
        // generator inputs are the slot's own; the present thread generates the frames from them.
        if (fgCount && mImpl->recordFrameGenInputs(cmd, fgSlot, fgIn))
        {
            static LLCachedControl<bool> s_show_gen(gSavedSettings, "RenderVKFrameGenShowGenerated", false);
            const bool hdr10 = mImpl->mSwapchain.displayStatus().encoding == vkdisplay::Encoding::Hdr10;
            const vkdisplay::Request labelRequest =
                vkdisplay::sanitized({true, mDisplayPaperWhiteNits, mDisplayPeakNits});   // (as the encode's)
            fgJob.generated = fgCount;
            // (#1544: the generator gets the display's peak as the composites do -- 0 in SDR)
            fgJob.eval = fillGenInputs(mImpl->mFgPresent, fgSlot, fgIn.constants, hdr10,
                                       sdrLayer ? 0.f : labelRequest.peakNits, fgd.frameMs(), fgd.frameSerial());
            fgJob.eval.debug = fgd.generatorDebug();
            fgJob.eval.reset = fgd.frameReset(fgIn.constants.reset);
            fgJob.ui = mImpl->mFgPresent.fgUi(fgSlot).view;
            // Every generated frame composes the interface (and, #1517 Task 7e, the slot's labels) in nits as the
            // real frame's: the encode's own paper white and peak, labels or not. (#1536 SDR: composed in the
            // SDR encoded space, no nits: 0.)
            fgJob.labels = labelsInAtlas;
            fgJob.paperWhiteNits = sdrLayer ? 0.f : labelRequest.paperWhiteNits;
            fgJob.peakNits = sdrLayer ? 0.f : labelRequest.peakNits;
            fgJob.showGenerated = s_show_gen;
        }
        // #1073 VR: the headset's images for the XR frame begun at the top of this viewer frame,
        // in the frame's last graphics command buffer. Desktop: mXrSession false.
        // #1074: in VR world frames the world target (-> TRANSFER_SRC) is copied into both eyes;
        // otherwise (login, loading) the eyes are dark and the window shows as the panel.
        if (mXrSession)
        {
            if (vrWorld)
            {
                imageBarrier(cmd, worldImg, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                             VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT |
                                 VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                             VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT |
                                 VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                             VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, worldLayers);
            }
            // #1594 VR SPACE WARP: every VR world frame's camera time (the motion vectors' interval);
            // while engaged, this world into the warp's history (eye target TRANSFER_SRC, above).
            if (vrWorld && doWorld)
            {
                LLXrHeadPose hp;
                if (LLXrSession::instance().headPose(hp)) LLVRSpaceWarp::instance().noteRecorded(hp.renderTime);
                if (vrWarpCapture) mImpl->vrSpaceWarpCapture(cmd, world, worldImg, worldExt, worldLayers);
            }
            // #1075: a stereo world (one layer per eye) and its depth for the depth layer.
            LLXrWorldDepth xrDepth;
            // #1137 A/B (RenderVKVRDepthLayer, Persist 0, live): the depth layer feeds the runtime's
            // reprojection; off = no depth copy, no depth layer (does the image tear less / cost less?)
            static LLCachedControl<bool> s_xr_depth(gSavedSettings, "RenderVKVRDepthLayer", true);
            const bool haveDepth = vrWorld && worldLayers > 1u && s_xr_depth && mImpl->mWorld &&
                                   mImpl->mWorld->worldDepthForXr(xrDepth.image, xrDepth.format, xrDepth.layout,
                                                                  xrDepth.extent, xrDepth.layers, xrDepth.distAtZero,
                                                                  xrDepth.distAtOne);
            LLXrSession::instance().recordFrame(cmd, img, mImpl->mSwapchain.format(), ext,
                                                vrWorld ? worldImg : VK_NULL_HANDLE, mImpl->mSwapchain.format(), worldExt,
                                                vrUi ? mImpl->mVrUiTarget.image : VK_NULL_HANDLE, worldLayers,
                                                haveDepth ? &xrDepth : nullptr);
        }
        // Instant replay: the finished frame, UI included (in world only; GunMetal-DX port).
        // Off: one cached-setting test (llinstantreplay.cpp).
        if (doWorld && !headless && !vkDeviceIsLost()
            && (!mImpl->mHdrOutputActive || !gSavedSettings.getBOOL("SLInstantReplay"))) // never feed PQ/scRGB to the SDR video encoder
            LLInstantReplay::captureFrame(cmd, img, mImpl->mSwapchain.format(), ext.width, ext.height, mImpl->mDevice);
        vkBreadcrumb(cmd, "f_present");

        if (!headless)
        {
            // (#1517 frame generation chain: the present thread's copy reads it after the frame-done value)
            imageBarrier(cmd, img,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         imgFinalLayout,
                         VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
        }
    }
    else
    {
        imageBarrier(cmd, img,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, imgFinalLayout,
                     VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT,
                     VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, 0);
    }

    // #697: out of memory ending / submitting the frame DROPS it (LLCleanRendererVK,
    // DROPPED FRAMES): an empty submission keeps the swapchain + fence contract and the
    // renderer re-streams next frame. Other failures stay fatal (VK_CHECK / device lost).
    LLCleanRendererVK::AsyncWsi wsi;
    // (#1074 window-less VR frame: no acquire to wait, no present to signal; the fence only. #1517 a
    // frame generation chain: no swapchain image either -- the backbuffer's waits are below.)
    const bool wsiImage = !headless && !fgChain;
    wsi.acquire = wsiImage ? mImpl->mSwapchain.imageAvailableSemaphore() : VK_NULL_HANDLE;
    wsi.acquireStage = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;
    wsi.renderFinished = wsiImage ? mImpl->mSwapchain.renderFinishedSemaphore(imageIndex) : VK_NULL_HANDLE;
    wsi.fence = mImpl->mSwapchain.inFlightFence();
    // #1517 a frame generation chain. The frame's last submission signals the frame-done value the present
    // thread's work for it waits; its first waits the present thread's release of the slot (the job two
    // frames back still reads its backbuffer and generator inputs, which this frame rewrites).
    if (fgChain)
    {
        fgJob.done = fgd.nextFrameDoneValue();
        wsi.frameDone = fgd.frameDone();
        wsi.frameDoneValue = fgJob.done;
        wsi.released = mImpl->mFgPresent.released();
        wsi.releasedValue = mImpl->mFgPresent.releasedValue(fgSlot);
    }
    LLCleanRendererVK* dropW = doWorld ? mImpl->mWorld : nullptr;   // (re-streams after a drop)
    const uint32_t dropsBefore = dropW ? dropW->framesDropped() : 0;
    bool endOom = !texturePrepRecordingReady();
    if (endOom && asyncW) asyncW->noteFrameOom();
    if (asyncW) endOom = !asyncW->vkEndOrNoteOom(cmd) || endOom;   // (asyncSubmitFrame drops the frame)
    else
    {
        const VkResult er = vkInjectOomTake() ? VK_ERROR_OUT_OF_HOST_MEMORY : vkEndCommandBuffer(cmd);
        if (vkIsOom(er)) endOom = true;
        else VK_CHECK(er);
    }

    // Async frame: every segment on both queues, in recording order (acquire wait on the
    // WSI command buffer, render-finished + fence on the last graphics submission).
    bool submitted = false;
    bool prepSubmitAccepted = false;
    if (asyncW)
    {
        VK_HITCH_SCOPE("vk_submit");
        submitted = asyncW->asyncSubmitFrame(mImpl->mDevice.queue(), wsi);
    }
    if (!submitted && endOom)
    {
        // (non-async frame whose command buffer could not be ended)
        LLCleanRendererVK::submitEmptyFrame(mImpl->mDevice.queue(), wsi, true);
        if (dropW) dropW->onFrameDropped(true); // command buffer was never submitted
        else vkLowMemNote(VKLM_FRAME, true);
        submitted = true;
    }
    if (!submitted)
    {
    // Submit: wait on image-available, signal render-finished, fence in-flight.
    VkSemaphoreSubmitInfo waitSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    waitSem.semaphore = wsi.acquire;
    // The image's first use is the CLEAR (vkCmdClearColorImage) + its layout transition:
    // the wait must cover that stage too, not only attachment output.
    waitSem.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_2_CLEAR_BIT;

    VkSemaphoreSubmitInfo signalSem{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
    // Per-IMAGE render-finished semaphore: present(imageIndex) waits this exact one.
    signalSem.semaphore = wsi.renderFinished;
    signalSem.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkCommandBufferSubmitInfo cmdInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO};
    cmdInfo.commandBuffer = cmd;

    // #1517 frame generation: + the wait for the slot's release and the frame-done value (wsi).
    static_assert(VkDisplayEncode::kMaxFgComposites >= VkSwapchain::kMaxBatchPresents,
                  "a composite per generated frame + the debug view's spare");
    VkSemaphoreSubmitInfo waits[2]{}, signals[2]{};
    uint32_t waitCount = 0, signalCount = 0;
    if (wsi.acquire) waits[waitCount++] = waitSem;   // (#1074 window-less VR frame, #1517 frame generation chain: none)
    if (wsi.renderFinished) signals[signalCount++] = signalSem;
    if (wsi.released && wsi.releasedValue)
    {
        waits[waitCount] = VkSemaphoreSubmitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        waits[waitCount].semaphore = wsi.released;
        waits[waitCount].value = wsi.releasedValue;
        waits[waitCount++].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }
    if (wsi.frameDone && wsi.frameDoneValue)
    {
        signals[signalCount] = VkSemaphoreSubmitInfo{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO};
        signals[signalCount].semaphore = wsi.frameDone;
        signals[signalCount].value = wsi.frameDoneValue;
        signals[signalCount++].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    }

    VkSubmitInfo2 submit{VK_STRUCTURE_TYPE_SUBMIT_INFO_2};
    submit.waitSemaphoreInfoCount = waitCount;
    submit.pWaitSemaphoreInfos = waitCount ? waits : nullptr;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &cmdInfo;
    submit.signalSemaphoreInfoCount = signalCount;
    submit.pSignalSemaphoreInfos = signalCount ? signals : nullptr;

    VkResult sr;
    { VK_HITCH_SCOPE("vk_submit");
      sr = vkInjectOomTake() ? VK_ERROR_OUT_OF_HOST_MEMORY
                             : vkQueueSubmit2(mImpl->mDevice.queue(), 1, &submit, mImpl->mSwapchain.inFlightFence()); }
    if (vkIsOom(sr))
    {
        // #697: nothing was submitted (a failed vkQueueSubmit2 has no effect): replace it
        // with the empty submission and drop the frame.
        LLCleanRendererVK::submitEmptyFrame(mImpl->mDevice.queue(), wsi, true);
        if (dropW) dropW->onFrameDropped(true); // vkQueueSubmit2 rejected the command buffer
        else vkLowMemNote(VKLM_FRAME, true);
    }
    else
    {
        VK_CHECK(sr);
        prepSubmitAccepted = true;
    }
    }

    // A dropped async frame increments the renderer counter inside asyncSubmitFrame.
    // Only an unchanged counter proves the texture upload command was accepted.
    if (dropW && dropW->framesDropped() == dropsBefore)
    {
        dropW->onTextureFrameSubmitted();
        texturePrepRecordingSubmitted(slot);
    }
    else if (dropW)
        texturePrepRecordingDropped(dropW->lastFrameDropQuiesced());
    else if (prepSubmitAccepted)
        texturePrepRecordingSubmitted(slot);
    else
        texturePrepRecordingDropped(true); // known pre-submit refusal; no queue consumed the command
    // #1517 frame generation chain: a dropped frame shows nothing (it was never drawn).
    // (Task 11b, Linux) Nor is anything presented while the window cannot be seen (a hidden FIFO / MAILBOX chain
    // may block its present until the window shows again; mFgHidden stays false elsewhere).
    fgJob.dropped = (dropW ? dropW->framesDropped() != dropsBefore : !prepSubmitAccepted) || mImpl->mFgHidden;

    // #873 live snapshot: a shot whose last frame was recorded this frame waits on this
    // frame's fence (never a device idle). (A dropped frame already failed the shot.)
    if (doWorld) mImpl->mWorld->liveSnapSubmitted(wsi.fence);
    // Instant replay: queue the encode, or forget the capture of a dropped frame (#697 paths)
    if (!vkDeviceIsLost()) LLInstantReplay::afterSubmit(!dropW || dropW->framesDropped() == dropsBefore);
    // Video Studio's recorder (#1668), the same: a dropped frame is re-rendered at the same ride time.
    if (!vkDeviceIsLost() && SLVideoRecorder::busy()) SLVideoRecorder::afterSubmit(!dropW || dropW->framesDropped() == dropsBefore);
    LLVKHitch::timelineSubmitted();   // (frame timeline: frame start -> submit)
    // #1073 VR: release the XR images and xrEndFrame after the frame's submit (the runtime's
    // reads follow our work on the same queue), before the mirror present. Desktop: skipped.
    if (mXrSession)
    {
        // #1137: the renderer's sampled GPU frame span into the VR pacing line / governor.
        if (mImpl->mWorld)
        {
            LLVKAutoTuneSample s;
            mImpl->mWorld->autoTuneSample(s);
            LLXrSession::instance().noteGpuFrame(s.gpuFrameMs, s.gpuAgeSec);
        }
        LLXrSession::instance().endFrame();
    }
    mImpl->signalSurfaceSerial(targetSerial);
    LLVKFrameGen::instance().marker(LLVKFrameGen::RenderEnd);   // #1517 (no-op unless Reflex paces this frame)
    bool presentNeedsRecreate = false;
    if (headless)
    {
        // #1074: nothing to present; the next frame takes the next slot as after a present.
        mImpl->mSwapchain.endFrameWithoutPresent();
    }
    else
    {
        const auto t0 = std::chrono::steady_clock::now();
        {
            VK_HITCH_SCOPE("vk_present");
            // #1544 AMD Anti-Lag: this frame's PRESENT stage (its INPUT: antiLagInput), before its present or, on a
            // frame generation chain, its hand-over to the present thread (as Reflex's PresentStart).
            if (mImpl->mDevice.antiLag() && mImpl->mAntiLagFrame && mImpl->mAntiLagFrame != mImpl->mAntiLagPresented)
            {
                mImpl->mAntiLagPresented = mImpl->mAntiLagFrame;   // (once per INPUT stage: none in VR)
                static LLCachedControl<bool> s_anti_lag(gSavedSettings, "RenderVKAntiLag", true);
                mImpl->mDevice.antiLagUpdate(VK_ANTI_LAG_STAGE_PRESENT_AMD, mImpl->mAntiLagFrame, s_anti_lag);
            }
            LLVKFrameGen::instance().marker(LLVKFrameGen::PresentStart);
            if (fgChain)
            {
                // #1517: the present thread shows it, after its generated frames (each with a swapchain image
                // taken just before it is shown); the frame slot moves on as after a present. Its work there is
                // out of band under this frame's Reflex id.
                fgJob.reflexFrame = fgd.reflexFrame();
                if (!mImpl->mFgPresent.push(fgSlot, fgJob))
                    LL_WARNS_ONCE("Vulkan") << "Frame generation: the present thread took no frame; it is not shown"
                                            << LL_ENDL;
                mImpl->mSwapchain.endFrameWithoutPresent();
            }
            else
                mImpl->mSwapchain.present(mImpl->mDevice, imageIndex, &presentNeedsRecreate);
            LLVKFrameGen::instance().marker(LLVKFrameGen::PresentEnd);
            if (fgDirect && !fgChain) fgd.batchPresented(1, 0);   // (#1517: a frame that generated nothing)
        }
        LLVKHitch::timelinePresent(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    }
    if (presentNeedsRecreate)
    {
        mImpl->mNeedsRecreate = true;
        mImpl->mOutputPreviewValid = false;
    }
    // #1517: what the present thread showed for the frames it finished (the 5 s statistics, the status).
    mImpl->mFgResults.clear();
    mImpl->mFgPresent.takeResults(mImpl->mFgResults);
    for (const LLVKFrameGenPresent::Result& r : mImpl->mFgResults)
    {
        if (r.refused) fgd.directRefused(r.refused);
        if (!r.shown) continue;
        fgd.notePresentCall(r.presentMs);
        fgd.batchPresented(r.shown, r.generated);
    }

    // #1547: the rebuilt renderer's first frame -- a world frame when the world is wanted (the login screen:
    // its first frame) -- ends the "recovering" overlay; the viewer then posts the system message. A world that
    // does not come back within two minutes ends it too (the log says so).
    if (mRecoveryOverlay && !vkDeviceIsLost())
    {
        const bool timedOut = LLFrameTimer::getElapsedSeconds() - mRecoveryStartedAt > 120.0;
        if (doWorld || !wantWorld || timedOut)
        {
            if (timedOut && !doWorld)
                LL_WARNS("Vulkan") << "#1547 the rebuilt world renderer has not drawn a frame in 120 s; the "
                                      "recovery notice is taken down" << LL_ENDL;
            mRecoveryOverlay = false;
            mRecoveryCompleted = true;
        }
    }
    // #1548: until the rebuilt renderer holds what the lost one did (the "scene back" line).
    if (mRecoveryScene && doWorld && mImpl->mWorld && mImpl->mWorld->ready() && !vkDeviceIsLost())
        pollRecoveryScene();
    ++mImpl->mFrameCounter;
    return true;
}

// #1074 VR: the OpenXR teardown at the viewer's exit. LLAppViewer::cleanup calls this while
// logging still runs (shutdown() below is not reached on a normal exit, which is why M0's
// teardown lines were missing): the session ends (xrRequestExitSession, xrEndSession,
// xrDestroySession, the swapchains and views), then the XrInstance is destroyed. The Vulkan
// device and instance the runtime created stay until process exit, as on the desktop; nothing
// of the runtime uses them after its session is gone. No-op without VR.
void LLVulkanContext::shutdownXr()
{
    if (!mXrHooked && !mXrSession) return;
    if (!mImpl || vkDeviceIsLost())
    {
        // #135: never touch a lost device; the runtime cleans up with the process.
        LL_WARNS("VR") << "VR teardown skipped: the graphics device was lost" << LL_ENDL;
        return;
    }
    LL_INFOS("VR") << "VR teardown at exit" << LL_ENDL;
    if (mImpl->mDevice.device()) vkDeviceWaitIdle(mImpl->mDevice.device());
    if (mXrSession)
    {
        LLXrSession::instance().shutdownSession();
        mXrSession = false;
        mXrPresentOverride = false;
    }
    mImpl->vrReleaseTarget(false);
    if (mXrHooked)
    {
        LLXrSession::instance().shutdownInstance();
        mXrHooked = false;
    }
    LL_INFOS("VR") << "VR teardown done" << LL_ENDL;
}

// #1517: frame generation's present thread at the viewer's exit. LLAppViewer::cleanup calls this before the
// main window is destroyed (shutdown() below is not reached on a normal exit): the present thread is told to stop
// -- from then on it takes no image and presents nothing more -- and is joined, or, still inside a driver call
// after the bound (a present the compositor holds while the window cannot be seen), left behind: the window is
// then kept (LLViewerWindow::destroyWindow), what that call answers later is not reported as a lost device
// (vkLostDeviceReported), and the process ends without static destructors. An idle thread ends at once. Nothing
// is released here: everything stays until process exit, as on every normal exit.
void LLVulkanContext::stopFrameGenAtExit()
{
    if (!mImpl) return;
    (void)mImpl->mFgPresent.stop();
}

void LLVulkanContext::shutdown()
{
    vkSetPipelineProgressHook(nullptr, 0);   // #462: no saves through a dying device
    vkSetPipelineBatchHook(nullptr);         // #471
    vkSetPipelineMemProbe(nullptr);
    vkSetPipelineMemBudget(nullptr);
    vkSetPipelineEventHook(nullptr);         // #672
    s_pipeSaveDev = nullptr;
    if (!mImpl)
    {
        mInitialized = false;
        return;
    }
    // Keep one service for this VkDeviceContext lifetime. Teardown closes admission
    // before any proof attempt; a failed proof retains its slots and byte charges.
    beginTexturePrepTeardown();
    if (vkDeviceIsLost())
    {
        // #135: never tear down on a lost device (some drivers crash or hang in vkDestroy*
        // after a TDR); everything is released at process exit.
        LL_WARNS("Vulkan") << "shutdown skipped: the graphics device was lost" << LL_ENDL;
        // #1517: its thread joined -- or, still inside a driver call after a bounded wait, left behind (the
        // process then ends without static destructors) -- nothing destroyed on the lost device. (mImpl, which
        // that thread uses, is never deleted on this path.)
        (void)mImpl->mFgPresent.stop();
        LLInstantReplay::shutdown(true);   // instant replay: join the save thread only
        SLVideoRecorder::shutdown(true);   // Video Studio's recorder (#1668), with it
        // #1546: no longer initialised (mImpl is retained, so a later init() refuses -- it used to answer
        // true and do nothing on the dead device). In-session recovery does not come through here: it
        // tears the lost epoch down with recoverFromDeviceLoss.
        mInitialized = false;
        return;
    }
    // #1517 frame generation: every gate open before the device idles and the main swapchain
    // goes (shutdownDirect below stops the pacer); Reflex lets go of the main swapchain.
    if (!mImpl->drainFrameGen())
    {
        // The present thread did not drain within its bound: it is inside a present the compositor holds (the
        // window cannot be seen), an acquire or the generator. Nothing it uses may be destroyed, so -- as for a
        // failed idle proof below -- everything is kept and released at process exit. The thread is told to stop
        // (the frames still queued are dropped) and, if it has not ended after one more bounded wait, left
        // behind: mImpl is never deleted, the device waits of the rest of the exit keep off its queue
        // (deviceIdleWait stays installed), and the process ends without static destructors.
        const bool ended = mImpl->mFgPresent.stop();
        LL_WARNS("Vulkan") << "shutdown deferred: frame generation's present thread was inside a present, an acquire "
                              "or the generator (a window that cannot be seen?) and "
                           << (ended ? "ended late" : "was left behind") << "; nothing is destroyed" << LL_ENDL;
        LLInstantReplay::shutdown(true);   // instant replay: join the save thread only (as the lost path)
        SLVideoRecorder::shutdown(true);   // Video Studio's recorder (#1668), with it
        mInitialized = false;
        return;
    }
    LLVKFrameGen::instance().setReflexSwapchain(VK_NULL_HANDLE, false, false, false);
    if (mImpl->mDevice.device())
    {
        const VkResult idleResult = vkDeviceWaitIdle(mImpl->mDevice.device());
        if (idleResult != VK_SUCCESS || vkDeviceIsLost())
        {
            // The service owns mapped source/staging leases. Without a successful idle
            // proof, leave the entire device context alive rather than destroying storage
            // that an uncertain queue submission may still read.
            LL_WARNS("Vulkan") << "shutdown deferred: device idle was not proven for texture staging ("
                                << vkResultString(idleResult) << ")" << LL_ENDL;
            LLInstantReplay::shutdown(true);   // (a replay being saved is finished; nothing destroyed)
            SLVideoRecorder::shutdown(true);   // Video Studio's recorder (#1668), with it
            mInitialized = false;
            return;
        }
    }
    if (LLFxRuntimeVK::instance().compileInDriver())
    {
        // A driver compile prevents actual device destruction. Do this before any
        // output/world/service teardown so the same context and staging ownership stay
        // intact for a later safe shutdown retry.
        LL_WARNS("Vulkan") << "shutdown deferred: an effect compile is still running in the driver; "
                           << "the device context and texture-prep service remain retained" << LL_ENDL;
        return;
    }
    if (mImpl->mTexturePrepService)
    {
        texturePrepRecordingDropped(true);
        mImpl->mTexturePrepService->retireGpuUse(mImpl->mTexturePrepSubmitSerial);
        mImpl->mTexturePrepService->reapCompleted();
    }
    for (auto& window : mImpl->mUIWindows) if (window) destroyUIWindow(window->surfaceID);
    pollUIWindowRetirement();
    if (std::any_of(mImpl->mUIWindows.begin(), mImpl->mUIWindows.end(), [](const auto& w) { return bool(w); }))
    {
        // Normal frame polling was stopped for shutdown. A pending presentation
        // is never freed on a deadline; bounded resources go with process exit.
        LL_WARNS("NativeUI") << "Pending auxiliary presentation retained until process exit" << LL_ENDL;
        LLInstantReplay::shutdown(true);   // (a replay being saved is finished; nothing destroyed)
        SLVideoRecorder::shutdown(true);   // Video Studio's recorder (#1668), with it
        mInitialized = false;
        return;
    }
    mImpl->mMainUIRecorder = {};
    if (mImpl->mUIRecorderShutdown)
    {
        // Device idle above is an exact GPU proof even for a refused final marker.
        mImpl->mSurfaceCompleted = mImpl->mSurfaceSerial;
        mImpl->mUIRecorderShutdown(mImpl->mSurfaceCompleted);
        mImpl->mUIRecorderShutdown = {};
    }
    if (mImpl->mSurfaceCompletion)
    {
        vkDestroySemaphore(mImpl->mDevice.device(), mImpl->mSurfaceCompletion, nullptr);
        mImpl->mSurfaceCompletion = VK_NULL_HANDLE;
    }
    LLInstantReplay::shutdown(false);   // instant replay: its encoder objects go before the device
    SLVideoRecorder::shutdown(false);   // Video Studio's recorder (#1668), with it
    // #1073 VR: the session, its swapchains and image views go before the device.
    if (mXrSession)
    {
        LLXrSession::instance().shutdownSession();
        mXrSession = false;
        mXrPresentOverride = false;
    }
    // #1517 frame generation's present thread: joined (drained above, the device idle), its objects gone
    // before the encode, the display frame, the swapchain and the device. Not ended after all (a call that
    // does not return, entered since the drain: not expected): left behind, and everything it uses kept.
    mImpl->mFgPresent.shutdown(mImpl->mDevice);
    if (mImpl->mFgPresent.ownsPresentQueue())
    {
        LL_WARNS("Vulkan") << "shutdown deferred: frame generation's present thread did not end; nothing more is "
                              "destroyed" << LL_ENDL;
        mInitialized = false;
        return;
    }
    vkSetDeviceIdleWait(nullptr, nullptr);
    mImpl->vrReleaseTarget(false);   // #1074 (device idle above; none outside VR)
    mImpl->mDisplayEncode.destroy();
    mImpl->mDisplayFrame.destroy();
    mImpl->discardHdrOutputStage();
    mImpl->destroyDebugGpuHang(true);
    mImpl->mHdrOutputActive = false;
    mImpl->mHdrOutputReady = false;
    // EXPERIMENTAL Vulkan world: tear down the GPU-driven world renderer first.
    if (mImpl->mWorld)
    {
        mImpl->mWorld->shutdown();
        if (mImpl->mWorld->shutdownDeferred())
        {
            // Keep the renderer, device and its quarantined source images alive until a
            // later successful device-wide idle proof permits teardown.
            mInitialized = false;
            return;
        }
        delete mImpl->mWorld;
        mImpl->mWorld = nullptr;
    }
    if (mImpl->mDeferredWorld)
    {
        mImpl->mDeferredWorld->shutdown();
        if (mImpl->mDeferredWorld->shutdownDeferred())
        {
            mInitialized = false;
            return;
        }
        delete mImpl->mDeferredWorld;
        mImpl->mDeferredWorld = nullptr;
    }
    // #1517 frame generation, direct: its pacer, feature and objects before the DLSS add-on goes.
    LLVKFrameGen::instance().shutdownDirect(mImpl->mDevice);
    // #870: after the world renderer released its RR feature, before the device goes.
    LLVKNeuralRecon::instance().shutdown();
    // EXPERIMENTAL Vulkan UI: uninstall + tear down the facade backend first.
    if (mImpl->mRenderBackend)
    {
        LLRender::setBackend(nullptr);
        delete mImpl->mRenderBackend;
        mImpl->mRenderBackend = nullptr;
    }
    if (mImpl->mFacadeReady)
    {
        gVK.shutdown();
        mImpl->mFacadeReady = false;
    }
    if (mImpl->mTexturePrepService)
    {
        mImpl->mTexturePrepService->shutdown();
        mImpl->mTexturePrepService.reset();
    }
#if LL_WINDOWS
    (void)mImpl->fseRelease("shutdown");   // #919: before its swapchain goes (the present thread is gone)
#endif
    if (!mImpl->mSwapchain.destroy(mImpl->mDevice))
    {
        // (#1517) The presentation engine still holds a present of the main chain (a window that cannot be
        // seen): the chain, its surface and the device stay until process exit. Or the device was lost in the
        // meantime (nothing is held then; nothing is touched either).
        if (mImpl->mSwapchain.destroyWithheld())
            LL_WARNS("Vulkan") << "shutdown deferred: the main swapchain's last present is still held by the "
                                  "presentation engine; the swapchain and the device are kept" << LL_ENDL;
        else
            LL_WARNS("Vulkan") << "shutdown skipped: the graphics device was lost; the swapchain and the device "
                                  "are kept" << LL_ENDL;
        mInitialized = false;
        return;
    }
    if (mImpl->mSurface && mImpl->mDevice.instance())
    {
        // SDL created the surface; SDL owns the destroy on this Vulkan instance.
        SDL_Vulkan_DestroySurface(mImpl->mDevice.instance(), mImpl->mSurface,
                                  nullptr);
        mImpl->mSurface = VK_NULL_HANDLE;
    }
    mImpl->mDevice.shutdown();
    // #1073 VR: the XrInstance after the VkInstance the runtime created for it.
    if (mXrHooked)
    {
        LLXrSession::instance().shutdownInstance();
        mXrHooked = false;
    }
    delete mImpl;
    mImpl = nullptr;
    mInitialized = false;
    delete mRecoveryScene;   // (#1548: a recovery still measuring its scene at exit)
    mRecoveryScene = nullptr;
    LL_INFOS("Vulkan") << "Vulkan context shut down" << LL_ENDL;
}

// =====================================================================================================
// #1547 GPU-LOSS RECOVERY (M1 of #1545; design: research/GPU_RECOVERY_DESIGN.md, owner decisions sec. 7)
// =====================================================================================================
// Sequence (main thread, between frames, in one call; the window keeps the last frame meanwhile):
//   1. epoch N is quiesced and torn down while it is still current (teardownLostEpoch): frame generation's
//      present thread stopped (bounded), the world renderer shut down (its compile jobs, fill worker and FX
//      worker joined), RmlUi, the UI facade, the backend, texture preparation, instant replay (its clip
//      discarded), frame generation and DLSS let go. A SIMULATED loss leaves the device healthy: it is
//      proven idle (bounded) and torn down normally through open gates (children destroyed: validation
//      clean). A DRIVER-REPORTED loss keeps every gate shut: the teardown takes the #135 lost branches.
//   2. what is left of epoch N -- its main / pop-out swapchains and surfaces, then its VkDevice -- goes to
//      the reaper thread, which explicitly releases retained storage/VMA before destroying the device. Only after a
//      confirmed loss or on a healthy device: recovery never starts on an unconfirmed loss (no watchdog,
//      owner decision O1). The main thread waits (bounded) for the swapchains -- a window takes a new
//      swapchain only after the old one is gone (WSI: one non-retired swapchain per native window) -- and
//      then for the device: no vkDestroyDevice of a retired epoch may overlap the next device's life. The
//      NVIDIA driver's per-device event threads all wait on one shared driver object (the core: the same
//      pointer in the old and the new device's thread); the retired device's thread woke with an event
//      inside its vkDestroyDevice and read state the destroy had already cleared (GTX 1080 / 580.178.04
//      soak, third recovery: SIGSEGV on that driver thread while the reaper joined it; the main thread was
//      only blocked on the loader lock -- held across vkDestroyDevice -- in VMA's vkGetInstanceProcAddr).
//   3. vkEpochBegin(N+1) empties the handle-keyed tables; a new VkDevice is created on the same VkInstance
//      and physical device (it must be the same one), then the swapchain on the same surface, texture
//      preparation, the UI facade and backend. The world renderer is created by renderFrame on the next
//      frames exactly as at login (fillFromCleanStores mirrors the CPU stores; textures re-source through
//      the eviction path). Every feature comes back as it was (#1550): the new device has the startup
//      device's requests (ray tracing included), the DLSS add-on released the old device in the teardown and
//      binds to the new one (same-GPU discovery retained across NGX shutdown), frame generation starts on it.
//      VR is not recovered yet (#1551).
//   4. renderFrame clears the overlay at the rebuilt renderer's first world frame; the viewer posts the
//      system message. The graveyard entry (the old Impl's CPU objects) is freed on the main thread once the
//      reaper has destroyed the device.
struct LLVulkanContext::Grave
{
    uint32_t epoch = 0;
    Impl* impl = nullptr;                       // epoch N's context objects (CPU; deleted on the main thread)
    bool healthy = false;                       // a simulated loss: the device works (proven idle)
    bool keepDevice = false;                    // a thread may still use the device (an FX compile in the driver)
    VkInstance instance = VK_NULL_HANDLE;
    std::vector<VkSwapchainKHR> swapchains;     // still alive after the teardown (a lost device): raw destroys
    std::vector<VkSurfaceKHR> auxSurfaces;      // pop-out windows' surfaces (instance children)
    std::atomic<bool> swapchainsReleased{false};
    std::atomic<bool> deviceReleased{false};
    std::atomic<double> deviceMs{0.0};          // how long the device's destruction took on the reaper
    // #1548: what the lost renderer held (the target of the rebuilt renderer's "scene back" line), and its last
    // per-material texel demand (the rebuilt renderer's first refill order).
    bool haveScene = false;
    LLCleanRendererVK::SceneCounts scene;
    std::vector<uint32_t> texDemand;
    std::vector<uint8_t> texSeen;
    // #1550: the DLSS add-on let go of this device (it binds to the next one); else why not.
    bool addonReleased = true;
    std::string addonWhy;
    // #1550: the process's memory-budget heap usage (main heap) right before / after the reaper's vkDestroyDevice --
    // whether the device's video memory really came back (0: not measured).
    std::atomic<uint64_t> usageBeforeReap{0}, usageAfterReap{0}, trackedAfterReap{0};
    uint64_t expectedDeviceBytes = 0, trackedBeforeTeardown = 0;
    bool memoryUsageKnown = false;
};

namespace
{
constexpr double kRecoverySceneGiveUpSec = 300.0;
constexpr double kRecoveryRefillMaxSec = 120.0;
}

void LLVulkanContext::pollRecoveryScene()
{
    RecoveryScene& r = *mRecoveryScene;
    const double now = LLFrameTimer::getElapsedSeconds();
    if (now < r.nextPoll) return;
    r.nextPoll = now + 0.25;
    const LLCleanRendererVK::SceneCounts c = mImpl->mWorld->sceneCounts();
    const LLCleanRendererVK::SceneCounts& t = r.target;
    const double since = now - r.lossAt;
    auto atLeast = [](uint64_t cur, uint64_t want, double frac) { return (double)cur >= (double)want * frac; };
    if (r.instAt < 0.0 && (!r.haveTarget || atLeast(c.instances, t.instances, 0.99)))
        r.instAt = since;
    // Meshes and textures: against what the scene needs NOW (it moves on: the mesh and texture counts of a
    // live scene drift several percent in a minute) -- every instance has a placed mesh, nothing is queued
    // for upload, no material waits on a texture -- with the lost renderer's counts as a sanity floor.
    if (r.meshAt < 0.0 && c.meshesPending == 0 && (double)c.instancesNoMesh <= 0.005 * c.instances
        && (!r.haveTarget || atLeast(c.meshes, t.meshes, 0.90)))
        r.meshAt = since;
    if (r.texAt < 0.0 && (double)c.texUnsettled <= std::max(8.0, 0.01 * c.textures)
        && (double)c.texUpPending <= std::max(8.0, 0.01 * c.textures)
        && (!r.haveTarget || (atLeast(c.textures, t.textures, 0.90) && atLeast(c.textureBytes, t.textureBytes, 0.75))))
        r.texAt = since;
    if (r.rtAt < 0.0 && (!r.rtRequested
                         || (c.rt && c.tlas && (!r.haveTarget || atLeast(c.blas, t.blas, 0.95)))))
        r.rtAt = since;
    const bool done = r.instAt >= 0.0 && r.meshAt >= 0.0 && r.texAt >= 0.0 && r.rtAt >= 0.0;
    // The texture refill runs at the raised pace until the textures are back (at most kRecoveryRefillMaxSec).
    mImpl->mWorld->setRecoveryRefill(r.texAt < 0.0 && since < kRecoveryRefillMaxSec);
    auto line = [&]()
    {
        auto MB = [](uint64_t b) { return (unsigned long long)(b >> 20); };
        auto at = [](double s) { return s < 0.0 ? std::string("not yet") : llformat("%.1f s", s); };
        std::string rt = !r.rtRequested ? std::string("RT off (not requested)")
                       : llformat("RT AS %s (%u/%u BLAS, TLAS %s)", c.rt && c.tlas ? "built" : "NOT built",
                                  c.blas, t.blas, c.tlas ? "yes" : "no");
        return llformat("%u/%u instances, %u/%u meshes (%u pending, %u instances without one), %u/%u textures (%llu/%llu MB; %u materials "
                        "waiting, %u sharper queued), %s | after the loss: instances %s, meshes %s, textures %s, RT %s",
                        c.instances, t.instances, c.meshes, t.meshes, c.meshesPending, c.instancesNoMesh,
                        c.textures, t.textures,
                        MB(c.textureBytes), MB(t.textureBytes), c.texUnsettled, c.texUpPending, rt.c_str(),
                        at(r.instAt).c_str(), at(r.meshAt).c_str(), at(r.texAt).c_str(), at(r.rtAt).c_str());
    };
    if (done)
    {
        LL_INFOS("Vulkan") << "#1548 scene back: " << line() << (r.haveTarget ? "" : " (no target: the lost renderer "
                              "had not drawn)") << LL_ENDL;
        delete mRecoveryScene;
        mRecoveryScene = nullptr;
        return;
    }
    if (since > kRecoverySceneGiveUpSec)
    {
        LL_WARNS("Vulkan") << "#1548 scene NOT back " << llformat("%.0f", kRecoverySceneGiveUpSec)
                           << " s after the loss: " << line() << LL_ENDL;
        delete mRecoveryScene;
        mRecoveryScene = nullptr;
        return;
    }
    if (now >= r.nextProgress)
    {
        if (r.nextProgress > 0.0) LL_INFOS("Vulkan") << "#1548 scene returning: " << line() << LL_ENDL;
        r.nextProgress = now + 5.0;
    }
}

namespace
{
// How long a recovery waits for the reaper to destroy the retired device before the new one is created. The
// soak's healthy reaps: median ~0.6 s, but 1 in 6 over 3 s and up to 12.4 s, almost all of it the driver's
// vkDestroyDevice. Longer: the recovery fails (exit, #135). 30 s by owner decision, with the user told (below).
constexpr double kRecoveryDeviceReleaseMs = 30000.0;
// How long a recovery after a driver-reported loss keeps trying to create the new device. AMD RX 7900 XTX,
// Windows 10, driver 24.10.1: vkCreateDevice failed in the same second as the loss, and Windows logged the
// driver's reset done (event 4101) one second later -- the device was asked for mid-reset (on NVIDIA Linux
// the retired device's ~13 s vkDestroyDevice hid that). Backoff between attempts: 250 ms doubling to 2 s.
constexpr double kRecoveryCreateRetryMs = 20000.0;
constexpr double kRecoveryCreateBackoffFirstMs = 250.0;
constexpr double kRecoveryCreateBackoffMaxMs = 2000.0;
// After this long the user is told what the frozen window is doing (most recoveries finish before it).
constexpr double kRecoveryNoticeAfterMs = 2000.0;
// #1550: how long the DLSS add-on may take to release a lost device (features, then its shutdown on that device). Past it the
// add-on is left to its thread: DLSS and frame generation stay off and that device is kept.
constexpr double kRecoveryAddonReleaseMs = 5000.0;

// While a recovery waits on the driver nothing can draw in the window (the in-window notice needs the new
// device). The window keeps answering the compositor (no "not responding" prompt: SDL_PumpEvents only queues
// the input, the main loop handles it after the rebuild), and a long wait tells the user in the window title
// and, on Linux, a desktop notification, so a slow driver is not taken for a hang and the viewer closed.
class RecoveryWaitNotice
{
public:
    explicit RecoveryWaitNotice(SDL_Window* window)
        : mWindow(window), mBits(gSavedSettings.getU32("RenderVKDebugRecoveryWaitNotice")) {}
    ~RecoveryWaitNotice()
    {
        if (mShown && mWindow && (mBits & 2u)) SDL_SetWindowTitle(mWindow, mOldTitle.c_str());
#if LL_LINUX
        if (mNotifyPid > 0) waitpid(mNotifyPid, nullptr, WNOHANG);   // (notify-send returns at once)
#endif
    }
    void pump(double waitedMs)
    {
        if (mBits & 1u) SDL_PumpEvents();
        if (!mShown && waitedMs > kRecoveryNoticeAfterMs) show();
    }

private:
    void show()
    {
        mShown = true;
        const std::string msg = LLTrans::getString("GraphicsRecoveringWait");
        LL_WARNS("Vulkan") << "#1547 recovery: the driver is slow to release the lost device; telling the user"
                           << LL_ENDL;
        if (mWindow && (mBits & 2u))
        {
            const char* t = SDL_GetWindowTitle(mWindow);
            mOldTitle = t ? t : "";
            SDL_SetWindowTitle(mWindow, ("Slipstream - " + msg).c_str());
        }
#if LL_WINDOWS
        if (mWindow && (mBits & 4u))
            if (HWND hwnd = (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(mWindow),
                                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr))
            {
                FLASHWINFO fi{sizeof(FLASHWINFO), hwnd, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
                FlashWindowEx(&fi);
            }
#endif
#if LL_LINUX
        if (!(mBits & 4u)) return;
        const char* argv[] = {"notify-send", "-a", "Slipstream", "-u", "normal", "-t", "15000",
                              "Slipstream", msg.c_str(), nullptr};
        pid_t pid = 0;
        if (posix_spawnp(&pid, "notify-send", nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0)
            mNotifyPid = pid;
#endif
    }

    SDL_Window* mWindow = nullptr;
    std::string mOldTitle;
    uint32_t    mBits = 7;
    bool        mShown = false;
#if LL_LINUX
    pid_t       mNotifyPid = 0;
#endif
};

// #1550: what the driver charges this process on the device-local heaps (VK_EXT_memory_budget heapUsage, a
// physical-device query: no VkDevice needed, so it is valid between a retired device's destroy and the next
// device). 0 = unknown.
uint64_t processDeviceLocalUsage(VkPhysicalDevice pd)
{
    if (!pd) return 0;
    VkPhysicalDeviceMemoryBudgetPropertiesEXT bud{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT};
    VkPhysicalDeviceMemoryProperties2 mp{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
    mp.pNext = &bud;
    vkGetPhysicalDeviceMemoryProperties2(pd, &mp);
    uint64_t used = 0;
    for (uint32_t h = 0; h < mp.memoryProperties.memoryHeapCount && h < VK_MAX_MEMORY_HEAPS; ++h)
        if (mp.memoryProperties.memoryHeaps[h].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) used += bud.heapUsage[h];
    return used;
}

// A small WSI allocation drop is not proof that a multi-GiB epoch came back.
// Allow at most a 10% driver residue; missing budget data stays unknown.
bool recoveryMemoryReleased(uint64_t expected, uint64_t remaining)
{
    return expected > 0 && remaining <= expected / 10u;
}

// One long-lived thread; never joined (a driver call on a dead device may not return: the process then
// ends without it, as with any thread left behind). Holds no lock the main thread waits for.
class RecoveryReaper
{
public:
    static RecoveryReaper& get()
    {
        static RecoveryReaper* r = new RecoveryReaper();   // intentionally leaked (see above)
        return *r;
    }
    void push(std::function<void()> job)
    {
        std::lock_guard<std::mutex> lk(mMutex);
        mJobs.push_back(std::move(job));
        if (!mStarted)
        {
            mStarted = true;
            std::thread([this] { run(); }).detach();
        }
        mCv.notify_one();
    }
private:
    void run()
    {
        vkDriverThreadName("gpu recovery reaper");
        for (;;)
        {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> lk(mMutex);
                mCv.wait(lk, [this] { return !mJobs.empty(); });
                job = std::move(mJobs.front());
                mJobs.erase(mJobs.begin());
            }
            job();
        }
    }
    std::mutex mMutex;
    std::condition_variable mCv;
    std::vector<std::function<void()>> mJobs;
    bool mStarted = false;
};
}

uint32_t LLVulkanContext::renderEpoch() const
{
    return vkEpochCurrent();
}

bool LLVulkanContext::takeRecoveryCompleted()
{
    if (!mRecoveryCompleted) return false;
    mRecoveryCompleted = false;
    return true;
}

// Epoch N, still current, torn down on the main thread (see the sequence above). False: it could not be
// (why says why); the caller abandons the epoch and the viewer takes the exit. What already went is gone;
// nothing of what is left is touched again.
bool LLVulkanContext::teardownLostEpoch(Grave& g, std::string& why)
{
    Impl& m = *mImpl;
    // (a) frame generation's present thread: stopped and joined (bounded). Left behind it still uses the
    // swapchain, the device and this Impl: none of them may go.
    if (!m.mFgPresent.stop())
    {
        why = "frame generation's present thread did not return from a driver call";
        return false;
    }
    // (b) a simulated loss: the device is healthy; every queue's earlier work proven done (bounded: a refused
    // submit can leave a wait that will never be signalled), then its gates open for a normal teardown.
    if (g.healthy)
    {
        if (!m.mDevice.boundedIdleProof(2000000000ull))
        {
            why = "the device of the simulated loss did not go idle within 2 s";
            return false;
        }
        // Exercise the real lost-storage path on an idle healthy device, without
        // deliberately hanging the GPU. Gates stay closed after the proof.
        if (gSavedSettings.getU32("RenderVKDebugRecoveryDevice") & 4u)
        {
            g.healthy = false;
            LL_INFOS("Vulkan") << "Recovery test: simulated device proven idle; exercising explicit lost-storage reap"
                               << LL_ENDL;
        }
        else
            vkEpochSetReaping(g.epoch, true);
    }
    vkSetPipelineProgressHook(nullptr, 0);   // (no saves through the retired device)
    vkSetPipelineBatchHook(nullptr);
    vkSetPipelineMemProbe(nullptr);
    vkSetPipelineMemBudget(nullptr);
    vkSetPipelineEventHook(nullptr);
    s_pipeSaveDev = nullptr;
    beginTexturePrepTeardown();
    LLVKFrameGen::instance().setReflexSwapchain(VK_NULL_HANDLE, false, false, false);
    if (g.healthy && vkDeviceWaitIdle(m.mDevice.device()) != VK_SUCCESS)
    {
        why = "the device of the simulated loss did not go idle";
        return false;
    }
    if (m.mTexturePrepService)
    {
        texturePrepRecordingDropped(true);
        m.mTexturePrepService->retireGpuUse(m.mTexturePrepSubmitSerial);
        m.mTexturePrepService->reapCompleted();
    }
    // (c) pop-out windows: closed normally on a healthy device; what is left (a lost device, a present not
    // retired) hands its swapchain and surface to the reaper. The floaters dock back (uiWindowFailed: the new
    // epoch knows none of these handles) and destroy their native windows after the reaper released the
    // surfaces (recoverFromDeviceLoss waits for that).
    if (g.healthy)
    {
        for (auto& w : m.mUIWindows)
            if (w) destroyUIWindow(w->surfaceID);
        pollUIWindowRetirement();
    }
    for (auto& w : m.mUIWindows)
    {
        if (!w) continue;
        if (w->swapchain.handle()) g.swapchains.push_back(w->swapchain.handle());
        if (w->surface) g.auxSurfaces.push_back(w->surface);
        w->surface = VK_NULL_HANDLE;
        w->failed = true;
    }
    // (d) RmlUi lets go of its GPU state before the world bridge: every serial counts as complete (the device
    // is idle, or lost). It registers again on the new epoch (slRmlUiTick).
    m.mMainUIRecorder = {};
    if (m.mUIRecorderShutdown)
    {
        m.mSurfaceCompleted = m.mSurfaceSerial;
        std::function<void(U64)> shutdownUi = std::move(m.mUIRecorderShutdown);
        m.mUIRecorderShutdown = {};
        shutdownUi(m.mSurfaceCompleted);
    }
    if (g.healthy)
    {
        if (m.mSurfaceCompletion) vkDestroySemaphore(m.mDevice.device(), m.mSurfaceCompletion, nullptr);
        if (m.mUIRetirement) vkDestroySemaphore(m.mDevice.device(), m.mUIRetirement, nullptr);
    }
    m.mSurfaceCompletion = m.mUIRetirement = VK_NULL_HANDLE;
    // (e) instant replay: the pre-loss clip is discarded (owner decision O5); the encoder starts again on the
    // new device by itself when the setting is on.
    LLInstantReplay::shutdown(!g.healthy);
    SLVideoRecorder::shutdown(!g.healthy);   // Video Studio's recorder (#1668), with it
    if (g.healthy) m.mFgPresent.shutdown(m.mDevice);
    vkSetDeviceIdleWait(nullptr, nullptr);
    m.vrReleaseTarget(false);
    if (g.healthy)
    {
        m.mDisplayEncode.destroy();
        m.mDisplayFrame.destroy();
        m.discardHdrOutputStage();
    }
    m.destroyDebugGpuHang(g.healthy);   // (a lost device: vkDestroyDevice takes them)
    m.mHdrOutputActive = false;
    m.mHdrOutputReady = false;
    // (f) the world renderer: its shutdown joins its compile jobs (which write into it), its fill worker and
    // the FX compile worker. Its GPU objects: destroyed on a healthy device, left to vkDestroyDevice on a
    // lost one (the #135 branches inside). First (#1548): what it held of the scene, CPU-side counts only.
    if (m.mWorld && m.mWorld->ready())
    {
        g.scene = m.mWorld->sceneCounts();
        g.haveScene = true;
        m.mWorld->texDemandSnapshot(g.texDemand, g.texSeen);
    }
    for (LLCleanRendererVK** slot : {&m.mWorld, &m.mDeferredWorld})
    {
        LLCleanRendererVK* w = *slot;
        if (!w) continue;
        w->shutdown();
        if (w->shutdownDeferred())
        {
            why = "the world renderer could not prove its device idle";
            return false;
        }
        delete w;
        *slot = nullptr;
    }
    if (LLFxRuntimeVK::instance().compileInDriver())
    {
        g.keepDevice = true;   // (a compile thread still inside the driver on this device)
        LL_WARNS("Vulkan") << "#1547 an effect compile is still inside the driver: renderer epoch " << g.epoch
                           << "'s device is kept (its video memory is not released until exit)" << LL_ENDL;
    }
    // (g) frame generation and DLSS (#1550): frame generation forgets its objects on a lost device (they go with
    // it); the DLSS add-on releases the device -- the features left on it, then its shutdown on that device -- while
    // the device still exists (bounded on a lost one), so the next device's initDevice binds it afresh. Not
    // released (the add-on did not return): DLSS and frame generation stay off, and the device is kept (a thread is
    // still inside the add-on on it). RenderVKDebugRecoveryDevice bit 2: the add-on is not entered on a lost device.
    LLVKFrameGen::instance().shutdownDirect(m.mDevice);
    {
        std::string nrWhy;
        const bool skipLostAddon = !g.healthy && (gSavedSettings.getU32("RenderVKDebugRecoveryDevice") & 2u);
        if (skipLostAddon)
        {
            LLVKNeuralRecon::instance().shutdown();   // (the lost branch: left on the dead device, unused)
            g.addonReleased = false;
            g.addonWhy = "RenderVKDebugRecoveryDevice bit 2: the DLSS add-on is not entered on a lost device";
        }
        else if (LLVKNeuralRecon::instance().releaseDevice(!g.healthy, kRecoveryAddonReleaseMs, nrWhy))
            g.addonReleased = true;
        else
        {
            g.addonReleased = false;
            g.addonWhy = nrWhy;
            g.keepDevice = true;
            LL_WARNS("Vulkan") << "#1550 the DLSS add-on did not release renderer epoch " << g.epoch << "'s device ("
                               << nrWhy << "): DLSS and frame generation stay off; the device is kept (its video "
                               << "memory is not released until exit)" << LL_ENDL;
        }
    }
    // (h) the UI: LLRender draws into its null backend until the new facade is up (no frame runs between).
    if (m.mRenderBackend)
    {
        LLRender::setBackend(nullptr);
        delete m.mRenderBackend;
        m.mRenderBackend = nullptr;
    }
    if (m.mFacadeReady)
    {
        if (g.healthy) gVK.shutdown();
        gVK.forgetDevice();
        m.mFacadeReady = false;
    }
    LLRenderVKBackend::epochReset();
    if (m.mTexturePrepService)
    {
        m.mTexturePrepService->shutdown();   // (joins its workers; a lost device's staging is forgotten)
        m.mTexturePrepService.reset();
    }
#if LL_WINDOWS
    if (g.healthy) (void)m.fseRelease("device loss recovery");
#endif
    // (i) the main swapchain: destroyed on a healthy device; a lost device's goes to the reaper (raw).
    if (g.healthy)
    {
        if (!m.mSwapchain.destroy(m.mDevice))
        {
            why = "the main swapchain's last present is still held by the presentation engine";
            return false;
        }
    }
    else if (m.mSwapchain.handle())
        g.swapchains.push_back(m.mSwapchain.handle());
    // (j) process-wide state that points at this device (the shared pipeline cache: saved first on a healthy
    // device, so the next one starts warm; never from a lost one).
    m.mDevice.releaseProcessStatics();
    return true;
}

// Epoch N+1 on epoch N's instance, surface and physical device: the device with the same requests as at
// startup except the features that stay off after a recovery, then everything init() does after the device
// that a recovered renderer needs. mImpl is the new Impl from the first line (a failure leaves it there,
// uninitialised, owning the instance; the viewer then exits).
bool LLVulkanContext::rebuildDeviceEpoch(Impl& old, bool driverReported,
                                         const std::function<void(double)>& waitPumping, std::string& why)
{
    Impl* n = new Impl();
    mImpl = n;
    n->mWindow = old.mWindow;
    n->mWidth = old.mWidth;
    n->mHeight = old.mHeight;
    n->mSurface = old.mSurface;   // (an instance child: not affected by the lost device; reused)
    old.mSurface = VK_NULL_HANDLE;
    n->mAutoTuneCaps = old.mAutoTuneCaps;
    n->mVrSafetyCaps = old.mVrSafetyCaps;
    n->mFseWanted = old.mFseWanted;
    n->mFseFocused = old.mFseFocused;
    n->mFseMinimized = old.mFseMinimized;
    VkDeviceContext& d = n->mDevice;
#if LL_WINDOWS
    if (old.mDevice.optionalExtensionEnabled(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME))
        d.requestOptionalExtension(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME);
#endif
    // #1550: every feature comes back as it was -- the startup device's requests (ray tracing, cluster LOD, async
    // compute, ...) and the DLSS add-on's / frame generation's device extensions; the world renderer then builds
    // RT, GI, DLSS and frame generation exactly as at login. VR never reaches here: TODO(#1551) rebuild the XR
    // session in place.
    applyDeviceRequests(d);
    installAddonDeviceProviders(d);
    // #1547 (RenderVKDebugRecoveryDevice, default 1): a rebuilt device is created WITHOUT VK_EXT_device_fault.
    // With it, a stalled vkDestroyDevice of a rebuilt device (GTX 1080 / 580.178.04: ~15-20 s once the driver
    // had been through many losses in one boot) let the device's event thread crash in the extension's
    // report path -- 13 of 13; without it, 0 crashes in 40 + 2 recoveries (fresh boot: 10-18 ms destroys).
    // The first device keeps the extension, so the first loss still reports its fault.
    const U32 recoveryDeviceBits = gSavedSettings.getU32("RenderVKDebugRecoveryDevice");
    d.setDeviceFaultRequested(!(recoveryDeviceBits & 1u));
    bool samePhysical = false;
    // Attempt 1 adopts epoch N's instance and messenger (initOnInstance); attempts 2..N run on it
    // (retryOnInstance) and own nothing else: the surface stays in this Impl, the instance in d whatever the
    // outcome. Only after a driver-reported loss: a Windows driver reset may still be in progress (TDR).
    const auto c0 = std::chrono::steady_clock::now();
    auto createMs = [&c0]()
    { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count(); };
    bool created = d.initOnInstance(old.mDevice, /*wantSwapchain=*/true, samePhysical);
    U32 attempts = 1;
    double backoffMs = kRecoveryCreateBackoffFirstMs;
    while (!created && driverReported && d.recoveryCreateRetryable(samePhysical)
           && createMs() + backoffMs < kRecoveryCreateRetryMs)
    {
        LL_WARNS("Vulkan") << "#1547 recovery: device attempt " << attempts << " failed at "
                           << llformat("%.0f", createMs()) << " ms ("
                           << (samePhysical ? vkResultString(d.lastDeviceCreateResult())
                                            : "physical device missing or changed")
                           << "); retrying in " << llformat("%.0f", backoffMs) << " ms" << LL_ENDL;
        waitPumping(backoffMs);
        backoffMs = std::min(backoffMs * 2.0, kRecoveryCreateBackoffMaxMs);
        ++attempts;
        created = d.retryOnInstance(/*wantSwapchain=*/true, samePhysical);
    }
    if (!created)
    {
        const VkRequirementsReport& r = d.requirementsReport();
        why = std::string(samePhysical ? "the new device could not be created" : "the physical device is lost or changed")
            + (r.failure.empty() ? std::string() : ": " + r.failure)
            + (attempts > 1 ? llformat(" (%u attempts over %.1f s)", attempts, createMs() / 1000.0) : std::string());
        LL_WARNS("Vulkan") << "#1547 recovery: no device after " << attempts << " attempt(s) in "
                           << llformat("%.0f", createMs()) << " ms ("
                           << (!driverReported ? "a simulated loss: not retried"
                               : d.recoveryCreateRetryable(samePhysical) ? "retry window over" : "not retryable")
                           << "): " << why << LL_ENDL;
        return false;
    }
    if (attempts > 1)
        LL_WARNS("Vulkan") << "#1547 recovery: device created on attempt " << attempts << " after "
                           << llformat("%.0f", createMs()) << " ms" << LL_ENDL;
    try
    {
        n->mTexturePrepService.reset(new (std::nothrow) LLVKTexturePrepService(&d));
    }
    catch (...)
    {
        n->mTexturePrepService.reset();
    }
    // #1550: the DLSS add-on on the new device (it released the old one in the teardown), then frame generation.
    initAddonAndFrameGen(d);
    vkdInitDeviceLimits(d.physicalDevice());
    vkSetPipelineCacheWarm(d.pipelineCacheWarm());
    s_pipeSaveDev = &d;
    vkSetPipelineProgressHook(&pipelineProgressSave, s_pipeProgressEvery);
    vkSetPipelineMemProbe(&pipelineMemProbe);
    vkSetPipelineBatchHook(&pipelineBatchReport);
    vkSetPipelineMemBudget(&pipelineMemBudget);
    vkSetPipelineEventHook(&pipelineEvent);
#if LL_WINDOWS
    if (d.optionalExtensionEnabled(VK_EXT_FULL_SCREEN_EXCLUSIVE_EXTENSION_NAME))
    {
        n->mFseCaps2 = (PFN_vkGetPhysicalDeviceSurfaceCapabilities2KHR)
            vkGetInstanceProcAddr(d.instance(), "vkGetPhysicalDeviceSurfaceCapabilities2KHR");
        n->mFseModes2 = (PFN_vkGetPhysicalDeviceSurfacePresentModes2EXT)
            vkGetInstanceProcAddr(d.instance(), "vkGetPhysicalDeviceSurfacePresentModes2EXT");
        n->mFseAcquire = (PFN_vkAcquireFullScreenExclusiveModeEXT)
            vkGetDeviceProcAddr(d.device(), "vkAcquireFullScreenExclusiveModeEXT");
        n->mFseRelease = (PFN_vkReleaseFullScreenExclusiveModeEXT)
            vkGetDeviceProcAddr(d.device(), "vkReleaseFullScreenExclusiveModeEXT");
        n->mFseExt = n->mFseCaps2 && n->mFseAcquire && n->mFseRelease;
        n->mSwapchain.setPresentModeQuery([n](std::vector<VkPresentModeKHR>& out) { return n->fsePresentModes(out); });
    }
#endif
    refreshDisplayHdrPolicy();
    // #1517 Reflex (frame generation): the main swapchain in low latency mode where Reflex can run, as at startup.
    n->mSwapchain.setLatencyMode(LLVKFrameGen::instance().reflexAvailable());
    if (!n->mSwapchain.init(d, n->mSurface, n->mWidth, n->mHeight))
    {
        why = "the new swapchain could not be created on the window";
        return false;
    }
    LLVKFrameGen::instance().setReflexSwapchain(n->mSwapchain.handle(), n->mSwapchain.latencyMode(),
                                                n->mSwapchain.frameGenImages() != 0, n->mSwapchain.ignoresVsync());
    n->logFrameGenChain();
    vkSetDeviceIdleWait([](void* impl, VkResult* result) { return static_cast<Impl*>(impl)->deviceIdleWait(result); }, n);
    n->mOutputColorSpace = n->mSwapchain.colorSpace();
    n->mNeedsRecreate = false;
    const uint32_t framesInFlight = n->mSwapchain.framesInFlight();
    n->mCmds.resize(framesInFlight, VK_NULL_HANDLE);
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = d.commandPool();
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = framesInFlight;
    if (vkAllocateCommandBuffers(d.device(), &ai, n->mCmds.data()) != VK_SUCCESS)
    {
        why = "the frame command buffers could not be allocated";
        return false;
    }
    mInitialized = true;
    if (!initRenderBackend())
    {
        mInitialized = false;
        why = "the UI facade could not be initialised on the new device";
        return false;
    }
    noteSwapchainMode(n->mSwapchain, false);
    return true;
}

bool LLVulkanContext::recoverFromDeviceLoss(std::string& why)
{
    const uint32_t epoch = vkEpochCurrent();
    if (!mImpl || !mInitialized)
    {
        why = "the renderer was not running";
        return false;
    }
    if (mXrHooked || mXrSession)
    {
        why = "VR: this device was created by the OpenXR runtime; rebuilding the XR session in place is #1551";
        return false;
    }
    const bool confirmed = vkEpochLostConfirmed(epoch);
    const bool simulated = vkEpochLostSimulated(epoch);
    if (!confirmed && !simulated)
    {
        // A 60 s hang verdict or a lost surface: the driver has not declared the device lost, so nothing of it
        // may be destroyed and a new device could be lost in the same reset (no watchdog: owner decision O1).
        why = "the driver did not report the device lost (a GPU hang verdict or a lost window surface)";
        return false;
    }
    pollRecoveryGraveyard();
    for (const Grave* old : mGraveyard)
        if (!old->swapchainsReleased.load(std::memory_order_acquire))
        {
            why = "the previous recovery's swapchain has not been released yet";
            return false;
        }
    const auto t0 = std::chrono::steady_clock::now();
    const double lossAt = LLFrameTimer::getElapsedSeconds();
    const bool rtBefore = mImpl->mDevice.rayTracingEnabled();   // (#1550: the recovered device must match it)
    auto msSince = [&t0]()
    { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count(); };
    LL_WARNS("Vulkan") << "#1547 recovery: renderer epoch " << epoch << " lost ("
                       << (confirmed ? "reported by the driver" : "simulated, the GPU is fine")
                       << "); threads inside driver calls: " << vkDriverCallsText() << LL_ENDL;
    Grave* g = new Grave();
    g->epoch = epoch;
    g->impl = mImpl;
    g->healthy = simulated && !confirmed;
    g->instance = mImpl->mDevice.instance();
    const VkDeviceContext::MemStats beforeTeardown = mImpl->mDevice.memStats();
    g->memoryUsageKnown = beforeTeardown.budgetExt;
    g->trackedBeforeTeardown = beforeTeardown.devLocalBytes;
    g->expectedDeviceBytes = processDeviceLocalUsage(mImpl->mDevice.physicalDevice());
    SDL_Window* const window = mImpl->mWindow;   // (for the wait notice; the window outlives the renderer)
    if (!teardownLostEpoch(*g, why))
    {
        vkEpochAbandon(epoch);   // (every gate shut for the exit; what is left stays to process exit)
        mInitialized = false;
        delete g;                // (not the Impl: it stays in mImpl, retained)
        return false;
    }
    const double teardownMs = msSince();
    mInitialized = false;
    mImpl = nullptr;   // (epoch N belongs to the graveyard from here)
    mGraveyard.push_back(g);
    RecoveryReaper::get().push([g]()
    {
        const VkEpochView view(g->epoch);   // (every #135 check on this thread is about epoch N)
        const VkDevice dev = g->impl->mDevice.device();
        {
            const VkDriverCallScope inDriver("vkDestroySwapchainKHR (retired epoch)");   // (seen by the exit)
            for (VkSwapchainKHR sc : g->swapchains)
                if (sc) vkDestroySwapchainKHR(dev, sc, nullptr);
        }
        for (VkSurfaceKHR s : g->auxSurfaces)
            if (s) SDL_Vulkan_DestroySurface(g->instance, s, nullptr);
        g->swapchainsReleased.store(true, std::memory_order_release);
        if (g->keepDevice) return;   // (held to process exit; logged at the teardown)
        const VkPhysicalDevice pd = g->impl->mDevice.physicalDevice();
        g->usageBeforeReap.store(processDeviceLocalUsage(pd));
        const auto r0 = std::chrono::steady_clock::now();
        {
            const VkDriverCallScope inDriver("vkDestroyDevice (retired epoch)");   // (seen by the exit)
            g->impl->mDevice.releaseDeviceForRecovery();
        }
        g->trackedAfterReap.store(g->impl->mDevice.memStats().devLocalBytes);
        vkEpochAbandon(g->epoch);   // (reaped: lost for every gate from now on, a healthy one included)
        g->deviceMs.store(std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - r0).count());
        g->usageAfterReap.store(processDeviceLocalUsage(pd));
        g->deviceReleased.store(true, std::memory_order_release);
    });
    // The surface takes a new swapchain only after the old one is gone: wait for the reaper's first step.
    RecoveryWaitNotice notice(window);
    while (!g->swapchainsReleased.load(std::memory_order_acquire) && msSince() - teardownMs < 5000.0)
    {
        notice.pump(msSince());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (!g->swapchainsReleased.load(std::memory_order_acquire))
    {
        why = "the reaper did not release the lost swapchain within 5 s";
        return false;
    }
    // ...and for the device itself (not when it is kept: then it is never destroyed in this session). The new
    // device is created only after the retired one's vkDestroyDevice has returned: the driver's event threads of
    // two devices on one GPU wait on one shared object, and the old device's thread handling an event inside its
    // destroy crashed the driver (see the overview above). The main thread would wait for most of it anyway: the loader
    // holds its lock across vkDestroyDevice, and vkCreateDevice / vkGetInstanceProcAddr take it.
    const double deviceWait0 = msSince();
    while (!g->keepDevice && !g->deviceReleased.load(std::memory_order_acquire)
           && msSince() - deviceWait0 < kRecoveryDeviceReleaseMs)
    {
        notice.pump(msSince());
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const double deviceWaitMs = msSince() - deviceWait0;
    if (!g->keepDevice && !g->deviceReleased.load(std::memory_order_acquire))
    {
        why = llformat("the reaper did not destroy renderer epoch %u's device within %.0f s (a new device beside a "
                       "retired one that is still being destroyed crashes the driver)",
                       epoch, kRecoveryDeviceReleaseMs / 1000.0);
        return false;
    }
    const uint32_t next = vkEpochBegin(confirmed ? "recovery after a driver-reported device loss"
                                                 : "recovery after a simulated device loss");
    // A refused device is retried (bounded) while the window and the wait notice stay alive.
    auto waitPumping = [&](double ms)
    {
        const double until = msSince() + ms;
        while (msSince() < until)
        {
            notice.pump(msSince());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    };
    if (!rebuildDeviceEpoch(*g->impl, confirmed, waitPumping, why))
    {
        vkEpochAbandon(next);
        mInitialized = false;
        return false;
    }
    mRecoveryOverlay = true;
    mRecoveryCompleted = false;
    mRecoveryStartedAt = LLFrameTimer::getElapsedSeconds();
    s_host_rates = HostMemRates{};   // (the rates were against the lost device's allocation counters)
    // #1548: the rebuilt renderer is measured against what the lost one held ("scene back" line).
    delete mRecoveryScene;
    mRecoveryScene = new RecoveryScene();
    mRecoveryScene->haveTarget = g->haveScene;
    mRecoveryScene->target = g->scene;
    mRecoveryScene->lossAt = lossAt;
    mRecoveryScene->rtRequested = mRayTracingRequested && mImpl->mDevice.rayTracingEnabled();
    mRecoveryScene->seedDemand = std::move(g->texDemand);
    mRecoveryScene->seedSeen = std::move(g->texSeen);
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(mImpl->mDevice.physicalDevice(), &props);
    LL_WARNS("Vulkan") << "#1547 recovered: renderer epoch " << next << " on " << props.deviceName
                       << " in " << llformat("%.0f", msSince()) << " ms (teardown " << llformat("%.0f", teardownMs)
                       << " ms); epoch " << epoch << "'s device "
                       << (g->keepDevice ? std::string("is kept (a thread still uses it)")
                                         : llformat("was destroyed by the reaper first (waited %.0f ms)", deviceWaitMs))
                       << LL_ENDL;
    // #1550: the features the new device carries, as at login (the world renderer builds RT, GI and DLSS on it).
    {
        const VkDeviceContext& d = mImpl->mDevice;
        const LLVKNeuralRecon& nr = LLVKNeuralRecon::instance();
        const LLVKFrameGen& fg = LLVKFrameGen::instance();
        LL_WARNS("Vulkan") << "#1550 recovered features: ray tracing "
                           << (d.rayTracingEnabled() ? "on" : "off") << (d.rayTracingEnabled() == rtBefore ? " (as before the loss)"
                                                                             : " (DIFFERENT from before the loss)")
                           << "; async compute " << (d.asyncComputeAvailable() ? "on" : "off") << " (" << d.asyncComputeStatus()
                           << "); DLSS " << (nr.available() ? "available" : "unavailable: " + nr.reason())
                           << (g->addonReleased ? "" : " [the add-on kept the old device: " + g->addonWhy + "]")
                           << "; frame generation " << (fg.directReady() ? "ready" : "not ready")
                           << (fg.reflexAvailable() ? ", Reflex on" : "") << LL_ENDL;
    }
    return true;
}

// Reaped epochs: their CPU objects freed (main thread: native windows and SDL objects among them), with
// this thread looking at the retired epoch -- every #135 check in a destructor finds it lost and calls
// nothing on the destroyed device.
void LLVulkanContext::pollRecoveryGraveyard()
{
    for (auto it = mGraveyard.begin(); it != mGraveyard.end();)
    {
        Grave* g = *it;
        if (!g->deviceReleased.load(std::memory_order_acquire))
        {
            ++it;
            continue;
        }
        // Compare against the WHOLE old epoch before teardown, not just the last
        // 64 MiB around vkDestroyDevice (the failed run freed only its swapchain).
        const uint64_t before = g->usageBeforeReap.load(), after = g->usageAfterReap.load();
        const uint64_t expected = g->expectedDeviceBytes;
        const uint64_t unattributed = expected > g->trackedBeforeTeardown ? expected - g->trackedBeforeTeardown : 0;
        const bool measured = g->memoryUsageKnown && expected > 0;
        LL_INFOS("Vulkan") << "#1547 renderer epoch " << g->epoch << " reaped: its device destroyed in "
                           << llformat("%.1f", g->deviceMs.load()) << " ms on the reaper; old epoch before teardown "
                           << (expected >> 20) << " MB (tracked " << (g->trackedBeforeTeardown >> 20)
                           << ", unattributed/driver " << (unattributed >> 20) << "); reaper entry "
                           << (before >> 20) << " MB -> after " << (after >> 20) << " MB (tracked "
                           << (g->trackedAfterReap.load() >> 20) << ", unattributed/driver "
                           << ((after > g->trackedAfterReap.load() ? after - g->trackedAfterReap.load() : 0) >> 20) << " MB)"
                           << (!measured ? " (release UNKNOWN: no memory-budget measurement)"
                               : recoveryMemoryReleased(expected, after) ? " (released: at least 90% returned)"
                               : " (NOT released: substantial old-epoch residue remains)")
                           << LL_ENDL;
        {
            const VkEpochView view(g->epoch);
            delete g->impl;
        }
        delete g;
        it = mGraveyard.erase(it);
    }
}

// #1544 AMD Anti-Lag (VK_AMD_anti_lag; nothing on a GPU without it): the main loop's INPUT stage, once a frame after its
// frame wait and before the frame reads input. A new frame index each time; renderFrame's PRESENT stage uses it.
// RenderVKAntiLag 0 turns it off (the driver is told: mode OFF).
void LLVulkanContext::antiLagInput()
{
    if (!mImpl || !mImpl->mDevice.antiLag()) return;
    static LLCachedControl<bool> s_anti_lag(gSavedSettings, "RenderVKAntiLag", true);
    mImpl->mDevice.antiLagUpdate(VK_ANTI_LAG_STAGE_INPUT_AMD, ++mImpl->mAntiLagFrame, s_anti_lag);
}
