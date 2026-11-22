/**
 * @file llvkframegenpresent.h
 * @brief Frame generation's present thread (#1517). On a main
 *        swapchain built for frame generation the renderer never touches a swapchain image: it renders
 *        each real frame (the display encode's output, the interface included) into one of this
 *        object's backbuffers -- one per slot -- and hands the slot over (push). The present thread
 *        then, per real frame: for each generated frame takes ONE swapchain image (VkSwapchain::
 *        acquireOne), generates the frame into a scratch image (LLVKFrameGen::evaluate) and draws the
 *        generated world with the real interface over it into that image (VkDisplayEncode::
 *        recordFgComposite: the generator never sees the interface); submits; presents the first at
 *        once and the others each held until the pacer opens its gate; then takes one more image,
 *        copies the backbuffer into it and presents it, held until its own gate. It owns the swapchain's
 *        acquires and presents and the present queue (Vulkan's external synchronisation). Generator-
 *        neutral: it reaches the generator only through LLVKFrameGen.
 *        (#1544 D8: on a GPU with one graphics queue the present queue is the renderer's own, shared: every
 *        call on it, either thread's, is made inside VkDeviceContext::QueueLock; the chain is IMMEDIATE, and
 *        this thread waits each gate itself before the present instead of leaving the wait in the queue.)
 *
 *        It also owns every image of frame generation, admitted as ONE set against the device's free
 *        video memory (never one slot admitted and another refused): per slot the backbuffer and the
 *        generator's inputs (the world alone, written by the encode; copies of the upscaler's depth and
 *        motion vectors), the frame's interface layer (the encode's copy of the UI layer, RGBA8: only the
 *        composites read it) and the scratch image. 3 slots when the set for 3
 *        fits (the reference keeps at least 3 backbuffers), else 2, else none.
 *
 *   prepare()        main thread, after every (re)creation of a frame generation chain (device idle)
 *   prepareInputs()  main thread, when the upscaler's depth / motion change size (device idle)
 *   waitSlot() / backbuffer() / beginInputs() / endInputs() / push()   the render loop, per frame
 *   drain()          main thread, before anything the thread uses is released or rebuilt (true: no job
 *                    queued or running; false: not within the bound -- the caller defers)
 *   waitAccepted()   any thread: the present queue's part of a device wait (the queue itself is the thread's)
 *   setWindowHidden() main thread, each frame on Linux (never waits)
 *   settleOwedRelease() main thread, the thread drained, before the chain's rebuild
 *   releaseChain()   main thread, the chain in use is a normal one now (device idle): the idle thread stopped
 *   stop()           main thread, the viewer's exit (before its window goes) or a lost device
 *   shutdown()       main thread, with the device idle: the thread joined, the objects gone -- or the thread
 *                    left behind inside a call that does not return, and nothing destroyed
 *
 *        THE STALL RULE (Task 7c fix 4). The present thread never raises a lost device and never judges: its
 *        own waits are bounded (an image: 1 s; the slot's last job: 1 s) and it reports what it showed. The
 *        main thread never waits for it without a bound, and a bound that passes only drops (a frame) or
 *        defers (a rebuild, a feature, shutdown). Whether a long wait is a GPU hang is decided only by the
 *        device's sliced waits (vk_hang_watchdog.h), on work a queue ACCEPTED: once the thread is held (one
 *        job running for llfg::kHeldJobMs) every skipped frame waits the frames in flight and the present
 *        queue's accepted values (checkGpu). They complete: the GPU is idle or advancing, the thread sits in
 *        a present, an acquire or the generator (a compositor hold, a hidden window, a pause) -- never a
 *        hang, logged once a minute, never ended by the viewer. They do not: the device's watchdog raises
 *        "GPU hang" as for any other wait.
 *        (fix 5) Once stop() has begun the thread takes no image and presents nothing more (mStopping, read as
 *        the hidden flag is: before every acquire and every present); left behind, the call it was inside may
 *        return after its window is gone -- what it answers then is not reported (vkLostDeviceReported).
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

#ifndef LL_LLVKFRAMEGENPRESENT_H
#define LL_LLVKFRAMEGENPRESENT_H

#include "llvkframegenpolicy.h"   // llfg::kMaxPresentSlots, presentSlotsFor, the wait bounds
#include "llvkframegengenerator.h"   // llfg::GenInputs: a frame's inputs, in no generator's own terms
#include "vk_device.h"
#include "vk_present_handoff.h"
#include "vk_swapchain.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <vector>

class LLVKFrameGen;
class VkDisplayEncode;

class LLVKFrameGenPresent
{
public:
    // Real frames the renderer may have handed over and not had back: one backbuffer and one set of
    // generator inputs each. slots() of them are in use (decided at prepare()).
    static constexpr uint32_t kMaxSlots = llfg::kMaxPresentSlots;
    // (The generator's world images: VkDisplayEncode::fgHudlessFormat(the chain) -- HDR10's 10-bit format, or
    // (#1536) an 8-bit SDR chain's own; the scratch image: llfg::scratchFormat(the generator, that format) -- #1544
    // FSR writes RGBA8 where the chain is BGRA8; the interface layer: VkDisplayEncode::kFgUiFormat.)
    // What prepare() admits for the upscaler's depth and motion copies before it knows them: their
    // formats at the output size (DLAA, the largest the upscaler renders at).
    static constexpr VkFormat kDepthAdmitFormat = VK_FORMAT_R32_SFLOAT;
    static constexpr VkFormat kMotionAdmitFormat = VK_FORMAT_R16G16_SFLOAT;
    // hudless / ui: written by the encode; hudless read by the generator (it may copy it) and, like ui,
    // by the composites.
    static constexpr VkImageUsageFlags kInputUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                     VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    // #1536 SDR: the slot's world image is the world's render target itself (no encode writes it): a swapchain
    // image's usage -- rendered, cleared and copied (captures) through -- and sampled by the composites.
    static constexpr VkImageUsageFlags kSdrWorldUsage = kInputUsage | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    // Depth/motion copies: copied, optionally rastered by world overlays, read by the generator.
    static constexpr VkImageUsageFlags kInputCopyUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                                         VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;

    // One real frame, filled by the render loop before push (the present thread reads it until the
    // slot is released).
    struct Job
    {
        uint64_t done = 0;             // the frame's frame-done value (LLVKFrameGen::frameDone())
        uint32_t generated = 0;        // generated frames to show before it (0: the real frame alone)
        bool dropped = false;          // the frame's work was dropped (#697): nothing of it is shown
        bool showGenerated = false;    // RenderVKFrameGenShowGenerated: the real frame shows the last generated one
        uint64_t reflexFrame = 0;      // the frame's Reflex id when its markers went out (0: none)
        llfg::GenInputs eval;          // the generator's inputs (this slot's; output: the scratch image)
        VkImageView ui = VK_NULL_HANDLE;   // this slot's copy of the frame's UI layer, for the generated frames (GENERAL)
        bool labels = false;           // #1517 Task 7e: the slot's label atlas holds this frame's labels
        float paperWhiteNits = 0.f;    // every composite: the interface (and the labels) composed in nits as
        float peakNits = 0.f;          // the real frame's -- the encode's sanitized paper white and peak
    };
    // What a job showed, for the render loop's statistics (takeResults).
    struct Result
    {
        uint32_t shown = 0;            // frames presented (the real one included); 0: none
        uint32_t generated = 0;        // generated frames made (some may have had no image in time)
        double presentMs = 0.0;        // its present calls
        const char* refused = nullptr; // why it showed fewer generated frames than asked for (a literal)
    };
    // The device's free video memory, asked after this object let go of the images it replaces. False:
    // nothing to check against (no VK_EXT_memory_budget). margin: what a large request leaves free.
    using Budget = std::function<bool(uint64_t& freeBytes, uint64_t& margin)>;

    LLVKFrameGenPresent() = default;
    ~LLVKFrameGenPresent() { mHandoff.stop(); }
    LLVKFrameGenPresent(const LLVKFrameGenPresent&) = delete;
    LLVKFrameGenPresent& operator=(const LLVKFrameGenPresent&) = delete;

    // Main thread, the present thread drained and the device idle (a swapchain rebuild): the images for
    // the chain's size and format (admitted as one set; kept when they still fit), the command buffers and
    // semaphores, and the thread. Clears a rebuild request. False: refused (refusal() says why, logged) --
    // no frame can be shown on that chain. Refused untouched when the rebuild's device wait failed
    // (!chain.deviceIdled(): work may still be pending on what this would release) or the thread was left
    // behind.
    bool prepare(VkDeviceContext& dev, VkSwapchain& chain, VkDisplayEncode& encode, const Budget& budget);
    const char* refusal() const { return mRefusal; }
    uint32_t slots() const { return mSlotCount; }
    // Main thread, the same conditions: the chain in use is no longer built for frame generation. The thread
    // -- idle: the caller drained it -- is stopped (prepare() starts it again), so the present queue is the
    // device's again and a device wait is the driver's own, as before frame generation was used
    // (ownsPresentQueue()); every image goes; a rebuild request is forgotten. The images are kept when the
    // present queue's last work could not be waited for (the thread left behind, or not idle; an owed release).
    void releaseChain();
    // Frame generation's images exist (prepare() made them, no releaseChain() since): a rebuild whose device
    // wait failed must be asked again -- they may still be in use; without them it need not.
    bool holdsImages() const { return mImagesFormat != VK_FORMAT_UNDEFINED; }
    // Main thread, the present thread drained: a release the present thread could not signal (its submit and
    // its own queue wait both refused), settled now -- the thread stopped (prepare() starts it again), the
    // present queue waited for (with no present thread it is this thread's), the last job's value signalled.
    // Before the chain's rebuild: no accepted value stands for that job's last commands on the present queue,
    // so the rebuild's device wait would not cover them and the old chain's images would go under them. A
    // release the queue accepted is not owed (the GPU signals it; a device wait waits it). False: it could not
    // be settled -- the caller defers the rebuild (prepare refuses; the next try settles it).
    bool settleOwedRelease(VkDeviceContext& dev);
    // Main thread, the device idle: the jobs still queued are dropped (their slots released), the thread
    // joined, every object destroyed. A no-op when never prepared; nothing destroyed once the device is lost,
    // nor when the thread did not end (stop() false).
    void shutdown(VkDeviceContext& dev);
    // Main thread (a lost device, a shutdown that cannot wait): the thread is told to stop -- the jobs still
    // queued dropped -- and joined if it ends within llfg::kDrainBoundMs. False: it did not (inside a driver
    // call that does not return: a present the compositor holds; any call, on a lost device). It is left
    // behind, with no verdict: it still uses this object, the swapchain, the device and the generator, so none
    // of them may be destroyed or deleted any more, and the process must end without static destructors
    // (vkNoteThreadAbandoned). Nothing is destroyed here either way. From its first moment the present thread
    // takes no image and presents nothing more (stoppingNow()): the call it is inside returns, if it does, and
    // the rest of its job only gives the slot back.
    bool stop();
    // prepare() succeeded for the chain in use.
    bool ready() const { return mReady; }

    // ---- the generator's inputs ----
    // Every slot holds depth / motion copies of this size and format.
    bool inputsFit(VkExtent2D depthExtent, VkFormat depthFormat, VkExtent2D motionExtent, VkFormat motionFormat) const;
    // Copies of this size and format were refused for `generated` frames (no memory): not tried again
    // until the size, the format or the multiplier changes, or the chain is rebuilt.
    bool inputsRefused(VkExtent2D depthExtent, VkFormat depthFormat, VkExtent2D motionExtent, VkFormat motionFormat,
                       uint32_t generated) const;
    // Main thread, the present thread drained and the device idle: every slot's depth / motion copies for
    // this size and format, all or none (admitted against `budget`). False: refused (inputsRefused).
    bool prepareInputs(VkExtent2D depthExtent, VkFormat depthFormat, VkExtent2D motionExtent, VkFormat motionFormat,
                       uint32_t generated, const Budget& budget);
    // The slot's world without the interface: HDR10, written by the encode; #1536 SDR, the world's render
    // target itself (the frame draws the world into it).
    const GpuImage& fgHudless(uint32_t slot) const { return mSlots[slot < mSlotCount ? slot : 0].hudless; }
    // The slot's interface layer: HDR10, the encode's copy of the UI layer; #1536 SDR, the UI layer itself (the
    // frame draws the interface into it, no copy).
    const GpuImage& fgUi(uint32_t slot) const { return mSlots[slot < mSlotCount ? slot : 0].ui; }
    const GpuImage& fgDepth(uint32_t slot) const { return mSlots[slot < mSlotCount ? slot : 0].depth; }
    const GpuImage& fgMotion(uint32_t slot) const { return mSlots[slot < mSlotCount ? slot : 0].motion; }
    const GpuImage& fgScratch() const { return mScratch; }
    // #1544: what a generator's feature is made for, as far as this object knows it -- every slot's world and
    // depth / motion copies, the scratch image, the slot count, and imagesSerial (counted every time any of them
    // is made again: a new image can get an old handle back). The caller adds the sizes and formats.
    void fillGenSize(llfg::GenSize& size) const;

    // ---- moving labels (#1517 Task 7e) ----
    // Main thread, after waitSlot(slot) returned Ready: `slot`'s label atlas (RGBA8, COLOR_ATTACHMENT |
    // SAMPLED) at `atlas` and its label buffer (device-local STORAGE | TRANSFER_DST, written with
    // vkCmdUpdateBuffer in the frame's command buffer: llfgl::bufferBytes for the chain's size). A new
    // size replaces the atlas; the old one is retired, freed only once every frame up to `lastFrameDone`
    // on `frameDone` and every job pushed so far have finished on the GPU (never waits). Admitted against
    // `budget`. False: none for this slot (refused, or no chain): its labels draw with the interface.
    bool ensureLabels(uint32_t slot, VkExtent2D atlas, VkSemaphore frameDone, uint64_t lastFrameDone,
                      const Budget& budget);
    // Main thread, every frame after waitSlot(slot) returned Free, labels or not (never waits): frees the
    // retired atlases the GPU is done with; and, `idle` -- no frame has drawn labels for llfgl::kIdleShrinkFrames
    // frames (the setting off, snapshots, an SDR frame...) -- retires `slot`'s atlas as a resize does: freed
    // once every frame up to `lastFrameDone` on `frameDone` and every job pushed so far have finished on the
    // GPU. Only the slot the caller owns (the present thread may be reading another slot's job), so every
    // slot's atlas has gone after one round of the slots. The label buffer stays (well under 1 MB). Labels
    // again: ensureLabels makes a new atlas.
    void tendLabels(uint32_t slot, bool idle, VkSemaphore frameDone, uint64_t lastFrameDone);
    const GpuImage& labelAtlas(uint32_t slot) const { return mSlots[slot < mSlotCount ? slot : 0].labelAtlas; }
    const GpuBuffer& labelBuffer(uint32_t slot) const { return mSlots[slot < mSlotCount ? slot : 0].labelBuffer; }
    static constexpr VkImageUsageFlags kLabelAtlasUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    static constexpr VkFormat kLabelAtlasFormat = VK_FORMAT_R8G8B8A8_UNORM;
    // fgHudless / fgUi of `slot`: UNDEFINED -> COLOR_ATTACHMENT before the encode; after it -> GENERAL.
    void beginInputs(VkCommandBuffer cmd, uint32_t slot) const;
    void endInputs(VkCommandBuffer cmd, uint32_t slot) const;

    // ---- the render loop ----
    uint32_t nextSlot() const { return mNext; }
    // Waits until the present thread is done with `slot` (the renderer's only backpressure), at most
    // llfg::slotWaitBoundMs on any chain. Busy: the bound passed -- the frame is not rendered (the main loop
    // goes on pumping events; the slot is left as it was). Then, once the thread is held(), the GPU is checked
    // (the stall rule above): a GPU that hung is raised by the device's watchdog inside this call. Gone: no
    // thread, or it is stopping.
    VkPresentHandoff::SlotWait waitSlot(uint32_t slot);
    // One job has run on the present thread for llfg::kHeldJobMs: it sits inside a call (a present the
    // compositor holds, an acquire, the generator). Any thread.
    bool held() const { return mHandoff.jobAge().count() >= mBounds.heldMs; }
    // What the thread waits for while it is held (a literal, for the status): the call it is inside, or the
    // neutral text. It names; it judges nothing. Any thread.
    const char* heldReason() const { return llfg::heldText(static_cast<llfg::HeldIn>(mHeldIn.load())); }
    // The real frame's target in `slot`: the chain's format and size, usage as a swapchain image's. The
    // frame leaves it in TRANSFER_SRC_OPTIMAL.
    const GpuImage& backbuffer(uint32_t slot) const { return mSlots[slot < mSlotCount ? slot : 0].backbuffer; }
    // The frame's first submission waits released() at releasedValue(slot) (0: none): the present thread
    // has read that slot's backbuffer and generator inputs for the last time.
    VkSemaphore released() const { return mReleased; }
    uint64_t releasedValue(uint32_t slot) const { return slot < kMaxSlots ? mSlots[slot].serial : 0; }
    // Hands `slot` (waitSlot'ed) to the present thread with `job`, and moves to the next slot. False:
    // refused (nothing queued; the slot is free again, its release value unchanged).
    bool push(uint32_t slot, const Job& job);
    // True: no job is queued or running (always on the present thread itself). One try of at most
    // llfg::kDrainBoundMs -- none while the thread is held(): it only looks then -- ended within 50 ms by a
    // lost device. False: not drained; the caller defers what needed it (a rebuild, the generator's feature
    // or inputs, a mode release, shutdown) and releases nothing.
    bool drain();
    // The present queue is the present thread's own (Vulkan's external synchronisation) for as long as a
    // thread exists -- running, stopping or left behind: no other thread may wait it idle, so a device wait
    // then waits every other queue and, here, what the present queue ACCEPTED.
    bool ownsPresentQueue() const { return mHandoff.alive(); }
    // Any thread: waits, through the device's sliced waits (UINT64_MAX: the hang watchdog), until the GPU has
    // finished what the present queue accepted -- the last release and the last generated-done value whose
    // submit succeeded (published by the present thread after the submit; never a value merely issued: a
    // thread held before its submit has nothing pending on the GPU). VK_SUCCESS: complete. The queue is never
    // touched.
    VkResult waitAccepted(VkDevice device) const;
    // A present or acquire on THIS frame generation chain asked for it to be rebuilt (OUT_OF_DATE /
    // SUBOPTIMAL, a batch the chain has too few images for, a refused submit): jobs are dropped until
    // prepare() runs for the new chain. Never true on a normal chain (releaseChain, stop, shutdown forget it).
    bool needsRecreate() const { return mReady && mNeedsRecreate.load(); }
    // The finished jobs' results since the last call (main thread).
    void takeResults(std::vector<Result>& out);
    // Main thread, each frame on Linux (#1517 Task 11b): the main window cannot be seen (llfg::windowHidden). Told
    // before anything waits for this thread, and never waits itself. On a MAILBOX / FIFO chain the present thread
    // reads it before every acquire and every present: while hidden it takes no image, drops the jobs it starts
    // (their slots released) and presents nothing -- the images of a batch already drawn stay acquired and the
    // chain is rebuilt, after the window shows again (llfg::deferRebuild).
    void setWindowHidden(bool hidden) { mWindowHidden.store(hidden); }

private:
    // Present thread: driver pacing's answer to a metered batch (llfg::meteringAccepted), reported to fg.
    void meteringAnswer(LLVKFrameGen& fg, uint32_t answer);
    struct Slot
    {
        GpuImage backbuffer, hudless, ui, depth, motion;
        GpuImage labelAtlas;       // #1517 Task 7e (ensureLabels; main thread)
        GpuBuffer labelBuffer;
        Job job;
        uint64_t serial = 0;       // the release value of the slot's last job (main thread writes)
        uint64_t prevSerial = 0;   // ... of the job before it on this slot (the present thread waits it)
        VkCommandBuffer gen = VK_NULL_HANDLE, real = VK_NULL_HANDLE;   // the present thread's
        // ... a refused submit's replacement, one per submit of a job (both may be refused in one job)
        VkCommandBuffer blankGen = VK_NULL_HANDLE, blankReal = VK_NULL_HANDLE;
        VkSemaphore acquire[VkSwapchain::kMaxBatchPresents] = {};      // one per image a job takes
    };
    // A submit of the present thread: its commands ran (Drawn), or only a transition of its images to
    // PRESENT_SRC did, with the same waits and signals (Blank: the commands were refused for lack of
    // memory), or nothing was submitted (None: its images must not be presented).
    enum class Outcome { Drawn, Blank, None };
    void runJob(uint32_t slot, bool dropped);           // the present thread: one job, then its release
    // The present thread: the window cannot be seen and this chain's presents may be held by the compositor
    // (MAILBOX / FIFO): no image is taken and nothing presented now.
    bool hiddenNow() const { return mWindowHidden.load() && llfg::presentCanBlockOnCompositor(mPresentMode); }
    // The present thread: stop() has begun (the viewer's exit, a lost device) -- on any chain, no image is taken
    // and nothing presented any more.
    bool stoppingNow() const { return mStopping.load(); }
    void presentJob(uint32_t slot, Result& r);          // ... the job itself
    void signalRelease(uint64_t serial);                // ... the slot's release, on the present queue
    bool waitReleased(uint64_t value);
    Outcome submit(VkCommandBuffer cmd, VkCommandBuffer blank, const VkSemaphoreSubmitInfo* waits, uint32_t waitCount,
                   const VkSemaphoreSubmitInfo* signals, uint32_t signalCount, const uint32_t* images, uint32_t imageCount);
    // Main thread, a slot wait that passed its bound while the thread is held: the frames in flight and the
    // present queue's accepted work are waited through the device's sliced waits (a GPU that hung is raised
    // there); they complete -- not a hang -- and that is logged, at first and once a minute.
    void checkGpu();
    bool makeObjects(VkDeviceContext& dev);
    bool makeAcquireSemaphores(VkDeviceContext& dev);
    void destroyObjects(VkDeviceContext& dev);
    // format: the chain's (the backbuffers); hudless: the generator's world (VkDisplayEncode::fgHudlessFormat);
    // scratch: what the generator writes (#1544 llfg::scratchFormat of the generator in use and hudless).
    bool imagesFit(VkExtent2D ext, VkFormat format, VkFormat hudless, VkFormat scratch) const;
    bool makeImages(VkDeviceContext& dev, uint32_t slots, VkExtent2D ext, VkFormat format, VkFormat hudless,
                    VkFormat scratch, bool sdr);
    void releaseImages();
    void releaseInputCopies();
    void collectRetiredLabels(bool all);                // frees retired atlases the GPU is done with (all: device idle)

    VkDeviceContext* mDev = nullptr;
    VkSwapchain* mChain = nullptr;
    VkDisplayEncode* mEncode = nullptr;
    VkPresentHandoff mHandoff;
    Slot mSlots[kMaxSlots];
    GpuImage mScratch;                              // the generated world, one frame at a time
    uint32_t mSlotCount = 0;                        // slots() (main thread; the present thread reads it while a job runs)
    VkExtent2D mImagesExtent{0, 0};                 // the backbuffers' size and format
    VkFormat mImagesFormat = VK_FORMAT_UNDEFINED;
    VkFormat mImagesHudless = VK_FORMAT_UNDEFINED;  // the generator's world images' format
    VkFormat mImagesScratch = VK_FORMAT_UNDEFINED;  // the scratch image's (#1544: the generator's own)
    uint64_t mImagesGeneratorBytes = 0;             // the generator's own images the set was admitted with (#1544)
    uint64_t mImagesSerial = 0;                     // fillGenSize's imagesSerial (main thread)
    struct InputKey { VkExtent2D depth{0, 0}; VkFormat depthFormat = VK_FORMAT_UNDEFINED;
                      VkExtent2D motion{0, 0}; VkFormat motionFormat = VK_FORMAT_UNDEFINED; uint32_t generated = 0; };
    bool mInputsRefused = false;                    // prepareInputs refused mRefusedInputs
    InputKey mRefusedInputs;
    VkCommandPool mPool = VK_NULL_HANDLE;           // the present thread's command buffers (graphics family)
    VkSemaphore mReleased = VK_NULL_HANDLE;         // timeline: each job's release, in job order
    uint64_t mSerial = 0;                           // main thread: the last job's release value
    uint32_t mNext = 0;                             // main thread: the next slot
    bool mReady = false;                            // main thread
    const char* mRefusal = nullptr;                 // main thread: why prepare() refused
    std::atomic<bool> mNeedsRecreate{false};
    // What the present queue accepted (waitAccepted): the timeline and the last value a submit that SUCCEEDED
    // signals on it. The present thread stores the value after the submit; the main thread the semaphore
    // (prepare; gone with the objects). Read by any thread.
    struct Accepted
    {
        std::atomic<VkSemaphore> semaphore{VK_NULL_HANDLE};
        std::atomic<uint64_t> value{0};
    };
    Accepted mReleaseAccepted;                      // the slots' releases (mReleased)
    Accepted mGeneratedAccepted;                    // the generated batches (LLVKFrameGen::generatedDone())
    // (main thread, prepare -- while the thread is drained; the present thread reads it during its jobs)
    llfg::PresentMode mPresentMode = llfg::PresentMode::Immediate;
    std::atomic<bool> mWindowHidden{false};         // setWindowHidden (main thread); the present thread reads it
    std::atomic<bool> mStopping{false};             // stop() has begun (main thread); the present thread reads it
    std::atomic<uint8_t> mHeldIn{0};                // llfg::HeldIn: the call the present thread is inside (it writes)
    // The main thread's bounds for its waits for the thread: llfg's (the slot wait's by the chain's present
    // mode, at prepare). Members only so the host test can shorten them.
    struct Bounds
    {
        uint32_t slotMs = llfg::kSlotWaitBoundImmediateMs;
        uint32_t drainMs = llfg::kDrainBoundMs;
        int64_t heldMs = llfg::kHeldJobMs;
    } mBounds;
    uint32_t mSkipped = 0;                          // main thread: frames skipped in a row (a bounded slot wait)
    int64_t mHeldLoggedMinute = -1;                 // main thread: the held thread's last log line (-1: none)
    bool mAbandonedSaid = false;                    // main thread: stop() left the thread behind (said once)
    // Present thread (written by prepare while it is drained): an acquire semaphore was signalled and never
    // waited (a submit refused outright) -- they are made again with the next chain.
    std::atomic<bool> mAcquireStale{false};
    // The present thread's only: the generator did not see the last real frame (a dropped job, an evaluate
    // never submitted, a frame without generated frames), so the next evaluate resets its history.
    bool mHistoryBroken = true;
    uint32_t mLastShown = 0;                        // the present thread's: the last batch's generated frames shown
    uint32_t mLastMeteringAnswer = ~0u;             // the present thread's: the last answer judged (logged on change)
    uint32_t mMeteringAnswerRun = 0;                // the present thread's: batches in a row with that answer
    std::mutex mResultMutex;
    std::vector<Result> mResults;
    struct RetiredLabels
    {
        GpuImage atlas;
        VkSemaphore frameDone = VK_NULL_HANDLE;
        uint64_t frameDoneValue = 0;   // every frame that could have drawn it or sampled it
        uint64_t releaseValue = 0;     // every job that could have sampled it
    };
    std::vector<RetiredLabels> mRetiredLabels;      // main thread
};

#endif // LL_LLVKFRAMEGENPRESENT_H
