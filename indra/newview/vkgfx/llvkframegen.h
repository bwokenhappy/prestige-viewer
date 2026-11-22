/**
 * @file llvkframegen.h
 * @brief Frame generation (#1517, #1544): the frames are made by a generator behind
 *        LLVKFrameGenGenerator -- DLSS Frame Generation through the DLSS add-on (LLVKFrameGenDlss) or FSR
 *        frame generation (LLVKFrameGenFsr), chosen at startup (llfg::chooseGenerator,
 *        RenderVKFrameGenGenerator) -- and the viewer presents and paces them itself
 *        (VkSwapchain::presentBatch, VkFramePacer).
 *
 *   initDirect()                 after the DLSS add-on's initDevice: the generator's caps, the block, the objects
 *   decideDirect() / prepareFeature() / frameReset()   per frame (the renderer, main thread)
 *   evaluate() / timestampQuery() / pacer().push()     per job (frame generation's present thread,
 *                                                      LLVKFrameGenPresent)
 *   shutdownDirect()             before the DLSS add-on shuts down and before the device goes
 * Only initDirect, prepareFeature, evaluate and shutdownDirect reach the generator; the pacer, its
 * gate and frame-done semaphores, the timestamps and the statistics never call it. Any
 * failure (no add-on, an old one, a GPU that cannot, no second queue, a create / evaluate error)
 * leaves the viewer as without frame generation, DLSS included, with a reason for the player.
 * Reflex (VK_NV_low_latency2, the main swapchain) paces the frames where the driver offers it.
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

#ifndef LL_LLVKFRAMEGEN_H
#define LL_LLVKFRAMEGEN_H

#include "llvkframegenpolicy.h"
#include "llvkframegengenerator.h"   // llfg::GenCamera, GenInputs, GenSize; the generator's interface
#include "vk_device.h"
#include "vk_frame_pacer.h"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

class LLVKFrameGen
{
public:
    static LLVKFrameGen& instance();

    // Reflex applies the frame cap (the CPU limiter steps aside): Reflex runs on the main swapchain
    // (VK_NV_low_latency2, reflexAvailable(); the chain registered, built with low latency mode and for
    // frame generation) and generation is asked for (decideDirect's last answer).
    bool reflexCapsFrames() const { return directReflex(); }
    llfg::Block startupBlock() const { return mBlock; }
    const std::string& reason() const { return mReason; }   // why not (empty when ready)
    // A failure in mid-session stopped frame generation until the viewer starts again; reason() is then the
    // line for the player (llfg::stoppedText), while directReady() stays true (the control stays usable).
    bool stopped() const { return mDirectFailed; }

    // ---- per frame ----
    enum Marker : uint32_t { SimStart = 0, SimEnd = 1, RenderStart = 2, RenderEnd = 3, PresentStart = 4, PresentEnd = 5 };
    // The main loop, at the top of a frame (in place of the frame slot wait; only while
    // reflexCapsFrames()): a new frame id, the swapchain's sleep mode when it changed (capFps:
    // RenderFPSLimit counts frames on screen -- Reflex limits the real ones to (generated + 1) / cap),
    // vkLatencySleepNV and a bounded wait on it.
    void beginFrame(float capFps);
    // vkSetLatencyMarkerNV with the frame's id, for a frame beginFrame slept for only.
    void marker(Marker m);
    // The id the open frame's markers carry (VK_NV_low_latency2's presentID), 0 when this frame sends none
    // (Reflex does not pace it). Main thread, between RenderStart and PresentEnd: frame generation's job
    // carries it to the present thread.
    uint64_t reflexFrame() const { return mReflexOpen ? mReflexFrame : 0; }
    // The present thread's work for a frame is out of band (Reflex counts it apart from the frame's own): its
    // generate-and-draw submit between OobRenderStart / OobRenderEnd, each of its present calls between
    // OobPresentStart / OobPresentEnd, with the frame's id (reflexFrame(); 0: nothing is sent). The frame's
    // own PRESENT_START / END stay on the main thread around the hand-over. Present thread; the main
    // swapchain it reaches changes only while that thread is drained (setReflexSwapchain).
    enum OobMarker : uint32_t { OobRenderStart = 8, OobRenderEnd = 9, OobPresentStart = 10, OobPresentEnd = 11 };
    void outOfBandMarker(uint64_t frame, OobMarker m);
    // The main view's camera of this frame in the renderer's own terms (llfg::GenCamera), from the upscaler
    // that ran on it, before its evaluate (LLCleanDlssRRVK::frameGenConstants): with that upscaler's reset and
    // the jitter sign it applied. Kept for this frame's evaluates (takeConstants); each generator derives its
    // own conventions from it.
    void mainViewConstants(const llfg::GenCamera& c);
    // The main view's camera of this frame, stored by mainViewConstants (taken once).
    bool takeConstants(llfg::GenCamera& out)
    {
        if (!mDirectConstantsHave) return false;
        out = mDirectConstants;
        mDirectConstantsHave = false;
        return true;
    }
    // The last 5 s: frames shown per real frame (1: none generated; 0: unknown) and the status ("ok",
    // or why not). For the Preferences tooltip and the debug bridge.
    float shownPerRendered() const { return mShownPerRendered; }
    const std::string& statusText() const { return mStatusText; }

    // ---- generated by the generator, presented and paced by the viewer ----
    // After the device and the DLSS add-on are up. Asks both generators what they can do (their init: DLSS --
    // the add-on's caps), picks the one that runs (llfg::chooseGenerator: RenderVKFrameGenGenerator, read here);
    // decides the block (Platform, the generator's own -- AddonOff, NoAddon, OldAddon, Unsupported, ... --,
    // NoPresentQueue); creates the gate and frame-done timeline semaphores and the timestamp query pool, and
    // starts the pacer.
    void initDirect(VkDeviceContext& dev);
    // Pacer stopped, the feature released, the objects destroyed. Safe without / after a failed initDirect.
    void shutdownDirect(VkDeviceContext& dev);
    bool directReady() const { return mDirectReady; }   // initDirect succeeded
    // The generator in use (None before initDirect, or when neither can run).
    llfg::Generator generator() const { return mGen ? mGen->kind() : llfg::Generator::None; }
    // That generator can run on this device (its startup block is None): Preferences lists it.
    bool generatorCanRun(llfg::Generator g) const
    {
        const Backend* b = g == llfg::Generator::Dlss ? &mDlss : g == llfg::Generator::Fsr ? &mFsr : nullptr;
        return b && b->gen && b->block == llfg::Block::None;
    }
    // #1544, live: RenderVKFrameGenGenerator changed since the last call and names (through llfg::chooseGenerator)
    // another generator that can run -- the render loop then asks for a rebuild of the main swapchain, where
    // applyGenerator makes the switch. Main thread, every frame; true once per change of the setting.
    bool generatorChangeAsked();
    // Main thread, in the main swapchain's rebuild: the present thread drained (drainFrameGen), the device idle
    // when deviceIdle (the rebuild's own wait: VkSwapchain::deviceIdled). The generator the setting names, when
    // it can run and is not the one in use: the old one's feature released, the new one's caps, a new feature
    // on the next frame (a reset), driver pacing asked again. Not idle: nothing (the next rebuild tries again).
    void applyGenerator(VkDeviceContext& dev, bool deviceIdle);
    // The video memory the generator in use takes for its own images at this display size and formats, at the
    // largest render size it can be fed (the output size): what the present slots' admission adds (D7). 0: none.
    uint64_t generatorBytes(VkExtent2D display, VkFormat colorFormat, VkFormat scratchFormat) const;
    // This frame: the serial its job's evaluates carry (llfg::GenInputs::serial) -- the count of rendered frames that
    // generated, this one included (frameReset counts it), so a frame skipped or not generated for leaves no gap:
    // those breaks are frameReset's to judge (a skip carries the history over, a frame without generation resets
    // it). And the milliseconds since the frame before (llfg::GenInputs::frameMs; 0 on the first).
    uint64_t frameSerial() const { return mJobSerial + 1; }
    // The generated frames this frame asked for (decideDirect's last answer; 0: none) -- the status's multiplier.
    uint32_t generatedNow() const { return mDirectAsked; }
    float frameMs() const { return mFrameMs; }
    uint32_t maxGenerated() const { return mDirectReady ? mCaps.maxGenerated : 0; }
    // This frame: how many frames to generate (0 = none) under the rules, the live multiplier
    // (RenderVKFrameGen) and the swapchain images at hand. Before recording: advances the frame
    // counter and forgets constants an earlier frame left (takeConstants gives this frame's only).
    uint32_t decideDirect(const llfg::Context& ctx, uint32_t swapchainImages);
    // The generated frames the main swapchain is built for (VkSwapchain::setFrameGen): those
    // RenderVKFrameGen asks for (within maxGenerated()) while every LASTING condition holds -- direct
    // frame generation ready and not failed, RenderVKFrameGen 2 or more, an upscaler chosen that writes the
    // generator's inputs (llfg::upscalerWritesInputs: RenderVKUpscaler 1-11, FSR or DLSS), and the output in
    // use one it runs on (outputGenerates: llfg::outputGenerates,
    // HDR10 or #1536 8-bit SDR) -- else 0, a normal chain
    // (vsync honoured). A higher multiplier than the chain in use was built for rebuilds it (the render
    // loop); per-frame conditions (DLSS ran last frame, a snapshot, ...) are decideDirect's: they never
    // rebuild the chain.
    uint32_t chainGenerated(bool outputGenerates) const;
    // The main swapchain in use ignores vsync for frame generation (setReflexSwapchain's ignoresVsync:
    // VkSwapchain::ignoresVsync), whatever RenderVSyncEnable says (Preferences greys vsync).
    bool chainForcesNoVsync() const { return mMainChainNoVsync; }
    // RenderVKFrameGen asks for generated frames (2 or more), read live: the status bar shows no
    // multiplied rate once it is off (the 5 s statistics would linger).
    bool multiplierOn() const;
    // This frame asked for generated frames (decideDirect) but cannot make them, for `reason` (plain
    // words, a string literal): it becomes the status (Preferences, the status line) and is logged once
    // per change of reason.
    void directRefused(const char* reason);
    // This frame (decideDirect ran) is not rendered at all: the present thread still had its slot when the
    // bounded wait for it passed. The generator skipped nothing: its history carries over the frame
    // (llfg::lastEvaluatedAfterSkip), so the next frame is no reset.
    void frameSkipped();
    // The contract between the render loop, the present thread (LLVKFrameGenPresent) and the pacer:
    //  (a) the renderer's last submission of a real frame signals frameDone() at a value it took with
    //      nextFrameDoneValue() (main thread); the present thread's submits for that frame wait it;
    //  (b) the present thread records a batch's generated frames, then timestampQuery(cmd) as the last
    //      command of that command buffer, and submits it signalling generatedDone() at a value it took
    //      with nextGeneratedDoneValue(): that value is the pacer job's fenceToken, the timestamp its
    //      GPU end (the batch's "done": the end of the present thread's generate and draw work);
    //  (c) the pacer job is pushed only AFTER that submit, after the batch's first (ungated) present and
    //      BEFORE any gated present (a present call may idle the queue: a gate without a job would hang);
    //      push(), nextValue(), meteringResult() and evaluate() run on the present thread; drain() and
    //      stop() only while it is drained or stopped (no two threads use the pacer at once);
    //  (d) drainPacer() (after the present thread is drained: the render loop's drainFrameGen) before every
    //      swapchain rebuild, feature or input change, mode release and shutdown. A device wait needs neither
    //      (it never touches the present queue: vkSetDeviceIdleWait). The pacer holds each gate at most
    //      llfg::kMaxPacedFrameNs after its batch's GPU end (its fence wait at most 1 s);
    //  (e) the feature is (re)created only on the main thread (prepareFeature), with the present thread
    //      drained; evaluate only records.
    // The present thread: generated frame `index` (1-based) of `count` for the rendered frame in present slot
    // `slot` into in.output, recorded into cmd (the feature exists: prepareFeature). A failure is noted once
    // (no more generation this session: the next decideDirect makes it the block, with the generator's error
    // as the status) and returns false; every evaluate after it returns false without reaching the generator.
    bool evaluate(VkCommandBuffer cmd, uint32_t slot, const llfg::GenInputs& in, uint32_t index, uint32_t count);
    // Main thread, before a frame's job is pushed, with the present thread drained when it returns false
    // from featureFits: the generator's feature for `size`, made on first use and again when the generator
    // says it no longer fits (pacer drained, GPU idle). False: refused -- no more generation this session.
    bool featureFits(const llfg::GenSize& size) const { return mFeatureReady && mGen && mGen->fits(size); }
    bool prepareFeature(VkDeviceContext& dev, const llfg::GenSize& size);
    // The generator's own debug setting (llfg::GenInputs::debug), read on the main thread for a frame's job:
    // DLSS -- RenderVKFrameGenConventionDebug; FSR -- RenderVKFrameGenFsrDebug.
    uint32_t generatorDebug() const;
    // Main thread, once for a real frame whose job generates: whether the generator drops its history
    // for it (the renderer's reset, a new feature, or a frame without generated frames before it).
    bool frameReset(bool rendererReset);
    // A create or evaluate refused: no more generation this session (main thread).
    void directFailed(llfg::Stop stop, const std::string& why);
    // Frame generation's images or present objects were refused for a chain (no memory: `why`, a literal):
    // chainGenerated() is 0 -- a normal chain -- until RenderVKFrameGen changes or rearmImages() (the main
    // swapchain rebuilt for another reason). Main thread.
    void imagesRefused(const char* why);
    void rearmImages() { mImagesRefused = false; }
    bool imagesRefusedNow() const;
    void drainPacer();   // before any device wait / swapchain rebuild / quit (a no-op when not ready)
    // The presents' gate (timeline; VkFramePacer signals it, VkSwapchain::presentBatch waits on it).
    VkSemaphore gate() const { return mGate; }
    // Timeline semaphore the frame's render submit signals at nextFrameDoneValue(); that value is
    // the pacer job's fenceToken. Main thread.
    VkSemaphore frameDone() const { return mFrameDone; }
    uint64_t nextFrameDoneValue() { return mFrameDoneNext++; }
    // The last value nextFrameDoneValue() gave (0: none yet). Main thread.
    uint64_t lastFrameDoneValue() const { return mFrameDoneNext - 1; }
    // Timeline the present thread's generate-and-draw submit of a batch signals at
    // nextGeneratedDoneValue() (present thread only); the pacer waits it.
    VkSemaphore generatedDone() const { return mGeneratedDone; }
    uint64_t nextGeneratedDoneValue() { return mGeneratedDoneNext++; }
    VkFramePacer& pacer() { return mPacer; }
    // Driver pacing's answers (llfg::PacingState, under mPacingMutex): the present thread reports each metered
    // batch; a new chain, another batch size or a multiplier change asks again (meteringReset), and so does
    // decideDirect now and then in fullscreen after the driver stopped being asked (PacingState::retryDue).
    void meteringResult(bool accepted)
    {
        std::lock_guard<std::mutex> lock(mPacingMutex);
        mPacing.result(accepted, pacingNowNs());
    }
    void meteringReset()
    {
        std::lock_guard<std::mutex> lock(mPacingMutex);
        mPacing.reset(pacingNowNs());
    }
    void meteringBatchChanged()
    {
        std::lock_guard<std::mutex> lock(mPacingMutex);
        mPacing.batchChanged(pacingNowNs());
    }
    // The driver is asked to pace the presents (llfg::driverPacing: the device has present metering and the
    // driver is still asked; it paces only in fullscreen). Any thread.
    bool driverPacing() const;
    // ... and accepted it for long enough (llfg::driverPacingAccepted): only then do 5x and 6x run and does
    // Preferences show them. It starts false: generation runs at 4x at most until the driver paces.
    bool driverPacingAccepted() const;
    // Preferences offers this multiplier (llfg::multiplierOffered).
    bool multiplierOffered(uint32_t multiplier) const
    {
        return mDirectReady && llfg::multiplierOffered(multiplier, mCaps, driverPacingAccepted());
    }
    // RenderVKFrameGen holds 5x / 6x but 4x runs, the driver not pacing: the status's words ("" otherwise).
    const char* cappedMultiplierText() const;
    // At the frame's end, on its command buffer (outside a render pass): resets and writes the next of the pool's
    // timestamps (vkCmdWriteTimestamp2). Returns the pacer job's query: its pool slot is
    // query % kTimestampQueries; the rest tells the pacer's read whether the slot was used again.
    uint32_t timestampQuery(VkCommandBuffer cmd);
    // After a real frame's presents: `shown` = the generated frames really shown for it + 1
    // (shownPerRendered(), statusText()); `generated` = the frames generated for it (some may not be
    // shown: no swapchain image in time). While frames are asked for, one "Frame generation:" line
    // every 5 s: the multiplier, generated of asked (and shown of asked when that differs), frames
    // shown per real frame, the pacer's statistics, driver pacing, Reflex, the renderer's waits for a
    // present slot and the present thread's present calls (average / worst; BLOCKED when a present call
    // took over 1 ms: the driver waited in it), and why frames were refused.
    void batchPresented(uint32_t shown, uint32_t generated);
    // The pacer could not take a batch (VkFramePacer::push returned false): its gate values
    // first..first+count-1 are signalled now, in order (a shut gate holds its present for ever).
    void releaseGates(uint64_t first, uint32_t count);
    // A job's present calls took this long, on the present thread (before its batchPresented): the 5 s
    // line's average and worst.
    void notePresentCall(double ms);
    // The renderer waited this long for a present slot (LLVKFrameGenPresent::waitSlot): the 5 s line's
    // average and worst (the renderer held back by the present thread).
    void noteSlotWait(double ms);
    static constexpr uint32_t kTimestampQueries = 8;
    // Reflex, direct (VK_NV_low_latency2; Windows and Linux): the device has it, its entry points were found and
    // the present queue is marked out of band (initDirect). The main swapchain is then built with low
    // latency mode (VkSwapchain::setLatencyMode).
    bool reflexAvailable() const { return mReflexOn; }
    // The main swapchain Reflex talks to: after every (re)creation of it, and VK_NULL_HANDLE before it
    // is destroyed (nothing is sent to a destroyed swapchain). latencyMode: VkSwapchain::latencyMode();
    // frameGenChain: built for frame generation (frameGenImages() != 0). ignoresVsync:
    // VkSwapchain::ignoresVsync() (chainForcesNoVsync, Reflex or not). A frame open on the old chain
    // ends here, and the new chain gets its sleep mode again.
    void setReflexSwapchain(VkSwapchainKHR swapchain, bool latencyMode, bool frameGenChain, bool ignoresVsync);

private:
    LLVKFrameGen() = default;

    uint64_t mStatReal = 0, mStatShown = 0;   // since the last status line
    double mStatStart = 0.0;
    float mShownPerRendered = 0.f;
    std::string mStatusText;
    llfg::Caps mCaps;
    llfg::Block mBlock = llfg::Block::None;
    std::string mReason;
    llfg::Block mFrameBlock = llfg::Block::None;

    // The startup block, its text (with `detail`), the log line (with `logDetail` too: the log only).
    void directBlocked(llfg::Block b, const std::string& detail, const std::string& logDetail = std::string());
    bool mDirectReady = false;
    bool mDirectFailed = false;
    bool mMeteringFeature = false;     // llfg::meteringFeature at initDirect: the device's present metering, for a
                                       // generator whose presents the driver may pace
    VkDevice mDirectDevice = VK_NULL_HANDLE;
    // Read by the pacer thread (through its Io); created before it starts, destroyed after it stopped.
    VkSemaphore mGate = VK_NULL_HANDLE;
    VkSemaphore mFrameDone = VK_NULL_HANDLE;
    VkSemaphore mGeneratedDone = VK_NULL_HANDLE;
    VkQueryPool mQueryPool = VK_NULL_HANDLE;
    PFN_vkVoidFunction mCalibrate = nullptr;   // vkGetCalibratedTimestampsKHR / EXT
    void* mWaitTimer = nullptr;                // Windows: the pacer's high-resolution waitable timer (HANDLE; null: it spins)
    // The pacer's CPU clock (llfg::pacerClock; initDirect, before the pacer starts): what calibrate() pairs
    // with the GPU's clock and what its "now" and sleeps read. Linux: CLOCK_MONOTONIC or CLOCK_MONOTONIC_RAW.
    llfg::PacerClock mPacerClock = llfg::PacerClock::None;
    int64_t cpuNowNs() const;                  // that clock now, in nanoseconds (Reflex's sleep statistics too)
    std::atomic<uint32_t> mQueryNext{0};   // the next query timestampQuery hands out
    uint64_t mFrameDoneNext = 1;
    uint64_t mGeneratedDoneNext = 1;   // the present thread's
    VkFramePacer mPacer;
    mutable std::mutex mPacingMutex;
    llfg::PacingState mPacing;                   // driver pacing's answers (under mPacingMutex)
    static uint64_t pacingNowNs()                // steady clock, for PacingState
    {
        return (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
    }
    // The present thread's evaluate refused (mEvalFailure, under mEvalMutex, says why); decideDirect
    // turns it into directFailed.
    std::atomic<bool> mEvalFailed{false};
    std::atomic<bool> mEvalStopped{false};   // an evaluate failed: no more reach the add-on (until a new feature)
    std::mutex mEvalMutex;
    std::string mEvalFailure;
    uint64_t mDirectFrame = 0;         // decideDirect's frame counter
    uint64_t mEvalFrame = 0;           // the last frame a job generated for (frameReset; 0: none)
    uint64_t mJobSerial = 0;           // rendered frames that generated (frameReset; frameSerial)
    // The generators: made at initDirect, kept until the process ends, each with what its init answered.
    // mGen is the one in use (null before initDirect and when neither can run); it changes only on the main
    // thread with the present thread drained.
    struct Backend
    {
        std::unique_ptr<LLVKFrameGenGenerator> gen;
        llfg::Block block = llfg::Block::Platform;   // its startup block (None: it can run)
        llfg::GeneratorCaps caps;
        std::string detail, log;                     // with the block: for the player's line / the log only
    };
    Backend mDlss, mFsr;
    LLVKFrameGenGenerator* mGen = nullptr;
    // (initDirect and applyGenerator) `b` becomes the generator in use: mGen, its caps, driver pacing's feature.
    void adopt(const Backend& b, VkDeviceContext& dev);
    // The backend llfg::chooseGenerator picks for this RenderVKFrameGenGenerator (null: neither can run).
    const Backend* chosenFor(uint32_t setting) const;
    uint32_t mGeneratorAsked = 0;      // RenderVKFrameGenGenerator at the last generatorChangeAsked / initDirect
    int64_t mDecideNs = 0;             // decideDirect's last call (steady clock; 0: none yet)
    float mFrameMs = 0.f;              // frameMs()
    bool mFeatureNew = true;           // the feature was (re)created: the next frame's evaluate resets
    bool mFeatureMade = false;         // a prepare was recorded (the generator's release at shutdown / re-create)
    bool mFeatureReady = false;        // made and accepted (prepareFeature; the present thread reads it)
    uint32_t mDirectAsked = 0;         // decideDirect's last answer (its on / off line, the metering reset)
    uint32_t mDirectGenerated = 0;     // this frame's: decideDirect's answer, 0 once directRefused
    // The 5 s window (batchPresented): frames asked for, really generated, generated and shown, the last
    // refusal's reason.
    uint64_t mWinAsked = 0, mWinGenerated = 0, mWinShown = 0;
    std::string mWinRefusal;
    std::string mLastRefusal;          // logged; forgotten after a window without a refusal
    // notePresentCall / noteSlotWait in the 5 s window: count, sum, worst.
    uint64_t mPresentCalls = 0, mSlotWaits = 0;
    double mPresentCallSumMs = 0.0, mPresentCallWorstMs = 0.0, mSlotWaitSumMs = 0.0, mSlotWaitWorstMs = 0.0;
    bool mImagesRefused = false;       // imagesRefused(), until rearmImages() or another multiplier
    uint32_t mImagesRefusedMult = 0;   // RenderVKFrameGen when they were refused
    void logDirectLine();              // the 5 s line (batchPresented)
    llfg::GenCamera mDirectConstants{};
    bool mDirectConstantsHave = false;

    // ---- Reflex, direct (VK_NV_low_latency2) ----
    void initReflex(VkDeviceContext& dev);   // initDirect: the entry points, the sleep's semaphore, out of band
    void reflexSleep(float capFps);          // beginFrame, direct
    void reflexMarker(Marker m);             // marker, direct
    bool directReflex() const
    {
        return mReflexOn && !mReflexGaveUp && mDirectReady && !mDirectFailed && mDirectAsked > 0
            && mReflexSwapchain != VK_NULL_HANDLE && mReflexLatencyChain && mReflexFgChain;
    }
    // A sleep's wait is cut here (a frame never waits longer); this many cut in a row and Reflex steps
    // aside for the rest of the session (the driver does not answer: the slot wait and the CPU limiter return).
    static constexpr uint64_t kReflexSleepTimeoutNs = 100000000ull;   // 100 ms
    static constexpr uint32_t kReflexMaxTimeouts = 3;
    bool mReflexOn = false;            // reflexAvailable()
    PFN_vkSetLatencySleepModeNV mLlSetMode = nullptr;
    PFN_vkLatencySleepNV mLlSleep = nullptr;
    PFN_vkSetLatencyMarkerNV mLlMarker = nullptr;
    VkSemaphore mReflexSem = VK_NULL_HANDLE;   // timeline: vkLatencySleepNV signals it, beginFrame waits
    uint64_t mReflexSemValue = 0;      // the last value handed to vkLatencySleepNV
    VkSwapchainKHR mReflexSwapchain = VK_NULL_HANDLE;   // setReflexSwapchain (VK_NULL_HANDLE: none)
    bool mReflexLatencyChain = false;  // that chain was built with low latency mode
    bool mReflexFgChain = false;       // ... and for frame generation
    bool mMainChainNoVsync = false;    // the main chain in use ignores vsync for frame generation (any Reflex)
    int64_t mReflexModeUs = -1;        // minimumIntervalUs last set on that chain (-1: not yet)
    uint64_t mReflexFrame = 0;         // the markers' frame id (presentID) of the last frame slept for
    uint64_t mReflexNextFrame = 1;     // monotonic, from 1
    uint64_t mReflexDirectFrame = 0;   // decideDirect's counter when beginFrame slept (the frame's own test)
    bool mReflexBegun = false;         // beginFrame slept for this frame (until its SimEnd)
    bool mReflexOpen = false;          // this frame's markers go out (SimEnd .. PresentEnd)
    bool mReflexTimeoutLogged = false;
    uint32_t mReflexTimeouts = 0;      // sleeps cut at kReflexSleepTimeoutNs in a row (a normal wake: 0)
    bool mReflexGaveUp = false;        // kReflexMaxTimeouts reached: Reflex off for the session
    bool mReflexRefusedLogged = false; // a frame generation chain refused in low latency mode (logged once)
    // The 5 s window (logDirectLine): the sleeps' waits.
    uint64_t mReflexWaits = 0;
    double mReflexWaitSumMs = 0.0, mReflexWaitWorstMs = 0.0;
};

#endif // LL_LLVKFRAMEGEN_H
