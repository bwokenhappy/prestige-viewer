// vkgfx — swapchain + present, the one foundation piece the offscreen prototype did
// not need. Owns a VkSwapchainKHR over a caller-supplied VkSurfaceKHR, per-image
// views, and per-frame-in-flight sync objects (acquire semaphore, render-finished
// semaphore, in-flight fence). Presentation uses classic binary semaphores
// (timeline semaphores can't gate vkQueuePresentKHR).
//
// Usage per frame:
//   uint32_t idx;
//   if (!sc.acquire(&idx)) { sc.recreate(...); return; }   // OUT_OF_DATE -> recreate
//   ... record into a command buffer, targeting image(idx), using
//       imageAvailableSemaphore() as a wait and renderFinishedSemaphore() as a
//       signal on the graphics submit, and inFlightFence() on the submit ...
//   sc.present(idx);
//
// This file depends only on vk_device (which owns instance/device/queue) plus the
// Vulkan headers; it does NOT depend on SDL or the viewer, so it stays testable
// in isolation. The surface is created by the windowing layer (SDL) and handed in.
#pragma once

#include "vk_device.h"
#include "vk_display_hdr.h"
#include "llvkframegenpolicy.h"   // llfg::PresentMode (pure rules, no viewer)

#include <atomic>
#include <chrono>
#include <functional>
#include <utility>
#include <vector>

class VkSwapchain {
public:
    // surface must already be created from ctx.instance() by the window layer.
    // Chooses an SRGB/UNORM B8G8R8A8 format, FIFO present mode (vsync, always
    // available), and a clamped image count. Returns false if the surface has no
    // usable format (caller should fall back / report).
    static constexpr uint32_t kDefaultFramesInFlight = 2;
    bool init(VkDeviceContext& ctx, VkSurfaceKHR surface,
              uint32_t width, uint32_t height, uint32_t framesInFlight = kDefaultFramesInFlight);
    // False: nothing was destroyed -- an auxiliary chain not retired yet (its owner polls), or a main chain
    // whose present proofs the presentation engine still withholds after kDestroyProofWaitNs (below): the
    // owner keeps the chain, and everything it needs, until process exit. Also false, as ever, on a lost
    // device (nothing is touched there): destroyWithheld() says which -- true only for a withheld proof.
    bool destroy(VkDeviceContext& ctx);
    bool destroyWithheld() const { return mDestroyWithheld; }

    // Tear down and rebuild the swapchain (window resize / OUT_OF_DATE). Keeps
    // the surface. Main waits device-idle; auxiliary callers poll retired() first.
    // #1517: with a gate still shut (gatedPresentsPending()) the owner drains the pacer first; a logged
    // error is the only clue.
    // The main chain's present proofs (swapchain maintenance: a fence per present, signalled by the
    // presentation engine when it is done with the image) are waited with a bound, never through the hang
    // watchdog: a compositor that holds a hidden window's presents holds them too, and that is no GPU hang.
    // Not all there within kRebuildProofWaitNs: false with rebuildDeferred() -- the chain is left exactly as it
    // was and the owner asks again later. Asked again while they are still withheld, the proofs are only looked
    // at (vkGetFenceStatus): one bounded wait per row of deferrals, never one per try. rebuildDeferredNote():
    // this deferral is one to log -- the first of its row, then one a minute.
    bool recreate(VkDeviceContext& ctx, uint32_t width, uint32_t height);
    bool rebuildDeferred() const { return mRebuildDeferred; }
    bool rebuildDeferredNote() const { return mRebuildDeferred && mProofNote; }
    static constexpr uint64_t kRebuildProofWaitNs = 500000000ull;    // 0.5 s
    static constexpr uint64_t kDestroyProofWaitNs = 2000000000ull;   // 2 s
    // CPU wall time spent waiting for prior GPU work during the last recreate.
    double lastDeviceIdleWaitMs() const { return mLastDeviceIdleWaitMs; }
    // #1517: the last recreate()'s device wait succeeded -- no GPU work of any queue is still pending (init:
    // true, nothing ran yet). A wait that failed (no memory; a lost device never gets here) proves nothing.
    bool deviceIdled() const { return mDeviceIdled; }

    // RenderVSyncEnable. true: FIFO (vsync). false: MAILBOX (uncapped, no tearing)
    // when the surface offers it, else IMMEDIATE, else FIFO. Read at (re)creation.
    static bool sVSync;
    // RenderVKNoVsyncPresentMode (only with sVSync off): 0 = MAILBOX first (no tearing;
    // then IMMEDIATE, then FIFO), 1 = IMMEDIATE first (then MAILBOX, then FIFO). A
    // diagnostic: some Windows compositors pace MAILBOX unevenly. Read at (re)creation.
    static uint32_t sNoVsyncMode;

    // Auxiliary UI windows have their own pacing and need only a color attachment.
    // Set before init; never changes the main window's presentation policy.
    void setUIOnly(bool presentFences = false) { mUIOnly = true; mAcquireTimeout = 0; mMaintenance = presentFences; }
    bool failed() const { return mUIFailed; }

    // FRAME PACING: waits the current frame-in-flight slot's fence only (no reset, no
    // acquire), so the main loop can block at the TOP of its frame -- before input and
    // simulation -- instead of mid-frame inside acquire(). acquire() keeps its own wait
    // (then free). Safe at any point between frames: mFrame only advances in present(),
    // and a skipped frame (OUT_OF_DATE, minimized) leaves the fence signaled. No-op on a
    // lost device or without sync objects.
    void waitFrameSlot(VkDeviceContext& ctx);
    // #1517: waits every frame-in-flight slot's fence (no reset) -- all the renderer's frames the GPU was
    // given -- through the hooked wait (an infinite wait: sliced into the hang watchdog, which raises a GPU
    // that makes no progress). Between frames only (every fence is signalled or pending on a submit then).
    // False: the device is lost. The main chain's only (true at once on an auxiliary one).
    bool waitFrameSlots(VkDeviceContext& ctx);

    // Waits the current frame's in-flight fence, then acquires the next image.
    // Returns false and sets *outNeedsRecreate on OUT_OF_DATE/SUBOPTIMAL; the
    // caller should recreate() and skip the frame. #1517: refused (false, no recreate) on a chain
    // built for frame generation -- its images are the present thread's (acquireOne).
    bool acquire(VkDeviceContext& ctx, uint32_t* outImageIndex,
                 bool* outNeedsRecreate);

    // Presents the given image index. Returns false + sets *outNeedsRecreate on
    // OUT_OF_DATE/SUBOPTIMAL. #1517: refused on a chain built for frame generation (presentBatch,
    // the present thread's, presents its images).
    bool present(VkDeviceContext& ctx, uint32_t imageIndex,
                 bool* outNeedsRecreate);

    // #1074 VR only: a frame that renders for the headset with NO window image (the mirror
    // window is minimised, or its acquire failed). beginFrameWithoutImage waits and resets the
    // current slot's fence as acquire() does (true: a submit signalling inFlightFence() must
    // follow); endFrameWithoutPresent moves to the next slot as present() does.
    bool beginFrameWithoutImage(VkDeviceContext& ctx);
    void endFrameWithoutPresent() { if (mFramesInFlight) mFrame = (mFrame + 1) % mFramesInFlight; }
    // #1074 VR only: acquire()'s timeout (default: none, as on the desktop). A timed-out
    // acquire returns false without a recreate: the mirror never blocks the headset.
    void setAcquireTimeout(uint64_t ns) { mAcquireTimeout = ns; }

    // #1013: separate proof for GPU submissions, acquire and presentation.
    // Poll only. Reset-but-never-submitted fences do not count as pending work.
    bool retired(VkDevice device) const;
    bool needsPresentDrain() const { return !mMaintenance && mPresentPending; }

    // ---- #1517 frame generation: the present thread's presents ----
    // On a chain built for frame generation the renderer never touches a swapchain image: it renders
    // into its own backbuffer and takes its frame slot with beginFrameWithoutImage /
    // endFrameWithoutPresent. Frame generation's present thread (llvkframegenpresent) takes each image
    // with acquireOne just before that frame is shown and presents it with presentBatch, on
    // ctx.presentQueue() (Vulkan's external synchronisation: only that thread acquires and presents on
    // such a chain; the main thread recreates or destroys it only once that thread is drained). A
    // present may be held ("gated") until a timeline value is signalled (the frame pacer). A native
    // chain only: on a UI-only chain, or a device without a present queue, setFrameGen has no effect,
    // frameGenImages() is 0, and acquireOne / presentBatch refuse.
    static constexpr uint32_t kMaxBatchPresents = 6;   // 5 generated + the real frame

    // Frame generation on this chain from its next (re)creation: IMMEDIATE (else MAILBOX, else FIFO), no
    // vsync, at least llfg::swapchainImagesFor(maxGenerated, mode) images (never fewer than the surface's
    // minimum; clamped to its maximum). 0 = off.
    void setFrameGen(uint32_t maxGenerated);
    uint32_t frameGenImages() const { return mFgImages; }   // imageCount() when built for it, else 0
    uint32_t frameGenMax() const { return mFgMax; }         // the generated frames it was built for (0: none)
    // #1544 (D8): the present thread shares the renderer's queue and this surface offers no IMMEDIATE mode -- no
    // chain is built for frame generation here (llfg::sharedQueueHeld; found at every (re)creation).
    bool frameGenQueueHeld() const { return mFgQueueHeld; }
    // The present mode was chosen for frame generation (IMMEDIATE / MAILBOX whatever sVSync says), even
    // when its objects then failed (frameGenImages() 0): this chain ignores vsync. False after the #919
    // fallback to FIFO.
    bool ignoresVsync() const { return mFgPresentMode && mPresentMode != VK_PRESENT_MODE_FIFO_KHR; }
    // The present thread: the next image, signalling `sem` (the caller's binary semaphore, with no
    // pending operation) when it is free; waits at most timeoutNs. True: *outIndex is the image, and it
    // MUST be presented by a later presentBatch after a submit that waits `sem` (an image left
    // unpresented is lost to the chain until it is rebuilt). False: no image and `sem` untouched
    // (*outNeedsRecreate on OUT_OF_DATE; true with SUBOPTIMAL as well). Never moves the frame slot.
    bool acquireOne(VkDeviceContext& ctx, VkSemaphore sem, uint64_t timeoutNs, uint32_t* outIndex,
                    bool* outNeedsRecreate);
    struct BatchPresent { uint32_t image; uint64_t gateValue; };   // gateValue 0 = no gate
    // The present thread: queues each present, in order, on the present queue. Each waits
    // renderFinishedSemaphore(image) (signalled by the submit that drew it); a gated one also waits the
    // timeline `gate` at gateValue, through an empty submit. meteringFrames > 0 chains
    // VkSetPresentConfigNV on the first; *outFeedback gets the driver's answer (0: accepted; ~0u when not
    // metering, not queued, or left unset).
    // Every image must come from acquireOne and is presented once. Every present is queued even when an
    // earlier one fails (no acquired image or gate is left behind); a failed gate submit presents its
    // image ungated and returns false. A refused batch (no frame generation, bad count) presents nothing
    // and logs the images it leaves acquired; an image repeated within a batch is skipped (logged). A
    // present refused for lack of memory also asks for a recreate (its semaphore stays signalled until
    // a rebuild). Never moves the frame slot.
    bool presentBatch(VkDeviceContext& ctx, const BatchPresent* presents, uint32_t count, VkSemaphore gate,
                      uint32_t meteringFrames, uint32_t* outFeedback, bool* outNeedsRecreate);
    // A batch with gated presents was queued since the last gatesReleased(). The owner opens every
    // gate (drains the pacer) and then calls gatesReleased() BEFORE recreate() / destroy(): a present
    // held on a gate never completes, and they wait for the presents. (A device wait needs none of it
    // while frame generation's present thread exists: it never touches the present queue then.)
    bool gatedPresentsPending() const { return mGatesPending; }
    void gatesReleased() { mGatesPending = false; }

    void presentingQueueIdle() { mPresentPending = false; if (!mMaintenance) mUnresolvedPresent = false; }

    void submitted() { if (mFrame < mSubmissionArmed.size()) mSubmissionArmed[mFrame] = true; }

    // ---- per-frame accessors (indexed by the current frame-in-flight slot) ----
    // imageAvailable + the in-flight fence are per-frame-in-flight: acquire signals
    // the semaphore and the fence guards reuse of that slot's command buffer/VBO.
    VkSemaphore imageAvailableSemaphore() const { return mImageAvailable[mFrame]; }
    VkFence     inFlightFence()          const { return mInFlight[mFrame]; }
    // render-finished is per-IMAGE: it is signaled by the submit that renders
    // image `i` and waited by the present of image `i`. Indexing it by the
    // frame-in-flight slot (2) instead of the image (3 here) let a submit re-signal
    // a semaphore a pending present had not yet consumed — a WSI sync hazard.
    VkSemaphore renderFinishedSemaphore(uint32_t imageIndex) const { return mRenderFinished[imageIndex]; }
    // Current frame-in-flight slot (0..framesInFlight-1); the caller indexes its
    // per-slot command buffers / vertex buffers with this so reuse is guarded by
    // the same in-flight fence that acquire() waits on.
    uint32_t    frameSlot()      const { return mFrame; }
    uint32_t    framesInFlight() const { return mFramesInFlight; }
    // The last submission of frame-in-flight slot `slot` has completed (its fence is signaled;
    // non-blocking). Its fence is reset at that slot's next acquire.
    bool        slotDone(VkDevice dev, uint32_t slot) const;

    // ---- swapchain image accessors (indexed by acquired image index) ----
    VkImage     image(uint32_t i)     const { return mImages[i]; }
    VkImageView imageView(uint32_t i) const { return mImageViews[i]; }
    VkFormat    format()              const { return mFormat.format; }
    VkExtent2D  extent()              const { return mExtent; }
    uint32_t    imageCount()          const { return (uint32_t)mImages.size(); }
    bool        valid()               const { return mSwapchain != VK_NULL_HANDLE; }
    VkColorSpaceKHR  colorSpace()     const { return mFormat.colorSpace; }
    // Set at startup / explicit output or monitor changes, before recreation. Readiness
    // requires the complete float-world + linear UI + SDR capture path, not just shaders.
    void setDisplayPolicy(vkdisplay::Request request, vkdisplay::Capability capability,
                          bool linearOutputReady) {
        mDisplayRequest = vkdisplay::sanitized(request);
        mDisplayCapability = capability;
        mLinearOutputReady = linearOutputReady;
    }
    const vkdisplay::Status& displayStatus() const { return mDisplayStatus; }
    // Bounded, allocation-local query used to stage a renderer output bank before
    // retiring the currently visible swapchain. Does not mutate the live chain.
    bool previewDisplayStatus(VkDeviceContext& ctx, vkdisplay::Status& out) const;
    // Predict the exact extent selected by the next createSwapchainObjects call.
    // Used to admit output targets before the current WSI chain is retired.
    bool previewExtent(VkDeviceContext& ctx, uint32_t width, uint32_t height,
                       VkExtent2D& out) const;
    VkPresentModeKHR presentMode()    const { return mPresentMode; }
    // That mode as frame generation's rules name it (#1517 Task 11b: the log, the present thread's stall rule).
    llfg::PresentMode fgPresentMode() const
    {
        return mPresentMode == VK_PRESENT_MODE_IMMEDIATE_KHR ? llfg::PresentMode::Immediate
             : mPresentMode == VK_PRESENT_MODE_MAILBOX_KHR   ? llfg::PresentMode::Mailbox
                                                             : llfg::PresentMode::Fifo;
    }
    // VRAM accounting (#407): the device memory the swapchain images take -- MEASURED
    // (vkGetDeviceImageMemoryRequirements on the swapchain's own image create info, so
    // tiling / alignment / compression metadata are included) x the image count the
    // driver actually returned. The WSI allocates these, not VkDeviceContext, so they
    // are not in its tracker. 0 = unknown (no swapchain).
    uint64_t    memoryBytes()         const { return mMemBytes; }
    VkSwapchainKHR handle()           const { return mSwapchain; }

    // #919 full-screen exclusive (Windows, VK_EXT_full_screen_exclusive). The owner keeps
    // these platform structs; this file stays platform-free:
    //  - createChain: chained into VkSwapchainCreateInfoKHR::pNext at the next
    //    (re)creation (nullptr = none). Must stay valid until then.
    //  - presentModeQuery: replaces vkGetPhysicalDeviceSurfacePresentModesKHR while set (the
    //    modes valid WITH the chain: vkGetPhysicalDeviceSurfacePresentModes2EXT). Returns
    //    false to fall back to the plain query.
    void setCreateChain(const void* createChain) { mCreateChain = createChain; }
    const void* createChain() const { return mCreateChain; }
    void setPresentModeQuery(std::function<bool(std::vector<VkPresentModeKHR>&)> q) { mPresentModeQuery = std::move(q); }
    // An acquire or present returned VK_ERROR_FULL_SCREEN_EXCLUSIVE_MODE_LOST_EXT since the
    // last call (cleared by the call). Those calls return false WITHOUT needsRecreate (the
    // owner re-acquires the mode, or rebuilds without the chain); the frame is skipped
    // (acquire) or was still queued (present). Never a device loss.
    bool takeExclusiveLost() { return mExclusiveLost.exchange(false); }   // (#1517: the present thread sets it too)
    // The last (re)creation was refused WITH the chain and fell back to a plain swapchain.
    bool chainRejected() const { return mChainRejected; }
    // #1517 Reflex (VK_NV_low_latency2, enabled on the device): VkSwapchainLatencyCreateInfoNV
    // {latencyModeEnable} chained in front of createChain at the next (re)creation of a chain built for
    // frame generation (setFrameGen) only; a creation refused with it is tried once more without it,
    // before the exclusive chain's own fallback. latencyMode(): the chain in use was built with it.
    void setLatencyMode(bool on) { mLatencyWanted = on; }
    bool latencyMode() const { return mLatencyBuilt; }

private:
    bool createSwapchainObjects(VkDeviceContext& ctx, uint32_t width,
                                uint32_t height);
    bool checkResult(VkResult result) const;
    void destroySwapchainObjects(VkDeviceContext& ctx);
    // A present-proof slot that is free (never armed, or its proof completed) and not reserved
    // (mNextPresentFence, mExtra); mPresentFences.size() if none.
    size_t freePresentFence(VkDevice device) const;
    void createFrameGenObjects(VkDeviceContext& ctx, uint32_t images);
    void destroyFrameGenObjects(VkDeviceContext& ctx);
    void gatesPendingError(const char* where) const;
    // Every armed present proof signalled within totalNs (bounded waits only; 0: a look, no wait at all).
    bool waitPresentProofs(VkDevice device, uint64_t totalNs) const;

    VkSurfaceKHR       mSurface   = VK_NULL_HANDLE; // owned by window layer
    VkSwapchainKHR     mSwapchain = VK_NULL_HANDLE;
    VkSurfaceFormatKHR mFormat{};
    VkPresentModeKHR   mPresentMode = VK_PRESENT_MODE_FIFO_KHR;
    VkExtent2D         mExtent{0, 0};
    uint64_t           mMemBytes = 0;   // #407: measured image memory, all images
    double             mLastDeviceIdleWaitMs = 0.0;
    bool               mDeviceIdled = true;     // deviceIdled()
    bool               mRebuildDeferred = false; // rebuildDeferred()
    // The row of deferred rebuilds in progress (0: none): how many, since when, and the last minute of it
    // that was logged (rebuildDeferredNote()).
    uint32_t           mProofDeferrals = 0;
    std::chrono::steady_clock::time_point mProofDeferredSince{};
    int64_t            mProofNotedMinute = -1;
    bool               mProofNote = false;
    bool               mDestroyWithheld = false; // destroyWithheld()
    vkdisplay::Request mDisplayRequest;
    vkdisplay::Capability mDisplayCapability;
    vkdisplay::Status mDisplayStatus;
    bool mLinearOutputReady = false;
    const void*        mCreateChain = nullptr;   // #919
    std::function<bool(std::vector<VkPresentModeKHR>&)> mPresentModeQuery;   // #919
    std::atomic<bool>  mExclusiveLost{false};   // #919
    bool               mChainRejected = false;   // #919
    bool               mLatencyWanted = false;   // #1517 setLatencyMode
    bool               mLatencyBuilt = false;    // #1517 latencyMode()
    bool               mUIOnly = false;
    bool               mUIFailed = false;
    uint64_t           mAcquireTimeout = UINT64_MAX;   // #1074 (VR sets a short one)

    std::vector<VkImage>     mImages;
    std::vector<VkImageView> mImageViews;

    // #1517 frame generation (all empty / 0 unless built for it).
    uint32_t mFgWanted = 0;      // setFrameGen, applied at the next (re)creation
    uint32_t mFgMax = 0;         // generated frames per batch this chain was built for
    bool mFgQueueHeld = false;   // frameGenQueueHeld()
    uint32_t mFgImages = 0;      // frameGenImages()
    bool mFgPresentMode = false; // the present mode was chosen for frame generation (ignoresVsync)
    // Per IMAGE, as mRenderFinished: signalled by the gate submit of image i, waited by its present.
    std::vector<VkSemaphore> mGateRelease;
    // The present thread's: images acquireOne gave and no presentBatch presented yet.
    struct ExtraImage { uint32_t image; size_t fence; };   // fence: reserved proof slot (~0 = none)
    ExtraImage mExtra[kMaxBatchPresents] = {};
    uint32_t mExtraCount = 0;
    bool mGatesPending = false;  // gatedPresentsPending()

    uint32_t mFramesInFlight = kDefaultFramesInFlight;
    uint32_t mFrame = 0;
    std::vector<VkSemaphore> mImageAvailable; // size == framesInFlight
    std::vector<VkSemaphore> mRenderFinished;
    std::vector<VkFence>     mInFlight;
    std::vector<VkFence>     mAcquireFences;
    std::vector<VkFence>     mPresentFences; // bounded pool; reuse only after the PE proof completes
    std::vector<bool>        mAcquireArmed, mPresentArmed, mSubmissionArmed;
    bool mMaintenance = false;
    bool mPresentPending = false;
    bool mUnresolvedPresent = false;
    size_t mNextPresentFence = 0;

};
