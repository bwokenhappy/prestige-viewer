/**
 * @file llvkframegen.cpp
 * @brief Frame generation: the frames a generator makes, presented and paced by the viewer: see
 *        llvkframegen.h.
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
#include "linden_common.h"
#include "vk_enumerate.h"

#include "llvkframegen.h"
#include "llvkframegendlss.h"   // the DLSS generator (made here, used through LLVKFrameGenGenerator only)
#include "llvkframegenfsr.h"    // the FSR generator (likewise)
#include "vk_swapchain.h"   // VkSwapchain::kMaxBatchPresents: the most presents of one batch

#include "llerror.h"
#include "lltimer.h"
#include "../llviewercontrol.h"

#include <cstdio>

#if LL_WINDOWS
#include "llwin32headers.h"
#elif LL_LINUX
#include <time.h>
#if defined(__x86_64__) || defined(__i386__)
#include <immintrin.h>
#endif
#endif

namespace
{
const char* const kTimes = "\xC3\x97";   // the multiplication sign (UTF-8)
}

LLVKFrameGen& LLVKFrameGen::instance()
{
    static LLVKFrameGen s;
    return s;
}

void LLVKFrameGen::beginFrame(float capFps)
{
    reflexSleep(capFps);   // (VK_NV_low_latency2; a no-op unless Reflex paces this frame)
}

void LLVKFrameGen::marker(Marker m)
{
    reflexMarker(m);
}

void LLVKFrameGen::mainViewConstants(const llfg::GenCamera& c)
{
    // (kept for this frame's evaluates, takeConstants)
    mDirectConstants = c;
    mDirectConstantsHave = true;
}

#if LL_WINDOWS
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
namespace
{
// QueryPerformanceCounter in nanoseconds: the CPU clock of the pacer and of calibrate().
int64_t qpcFrequency()
{
    static const int64_t s_freq = []
    {
        LARGE_INTEGER f;
        return QueryPerformanceFrequency(&f) && f.QuadPart > 0 ? (int64_t)f.QuadPart : (int64_t)1;
    }();
    return s_freq;
}

int64_t qpcToNs(int64_t qpc)
{
    const int64_t f = qpcFrequency();
    return (qpc / f) * 1000000000LL + (qpc % f) * 1000000000LL / f;
}

int64_t qpcNowNs()
{
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return qpcToNs((int64_t)c.QuadPart);
}
} // namespace
#elif LL_LINUX
namespace
{
// The Linux pacer's CPU clock (llfg::pacerClock): CLOCK_MONOTONIC, or CLOCK_MONOTONIC_RAW where the device
// calibrates only against that one -- the clock of calibrate() and of the pacer's "now" and sleeps.
int64_t linuxNowNs(llfg::PacerClock c)
{
    timespec ts{};
    clock_gettime(c == llfg::PacerClock::MonotonicRaw ? CLOCK_MONOTONIC_RAW : CLOCK_MONOTONIC, &ts);
    return llfg::timespecNs((int64_t)ts.tv_sec, (int64_t)ts.tv_nsec);
}

timespec toTimespec(int64_t ns)
{
    int64_t sec = 0, nsec = 0;
    llfg::splitNs(ns, sec, nsec);
    timespec ts{};
    ts.tv_sec = (time_t)sec;
    ts.tv_nsec = (long)nsec;
    return ts;
}

void cpuRelax()
{
#if defined(__x86_64__) || defined(__i386__)
    _mm_pause();
#endif
}
} // namespace
#endif

int64_t LLVKFrameGen::cpuNowNs() const
{
#if LL_WINDOWS
    return qpcNowNs();
#elif LL_LINUX
    return linuxNowNs(mPacerClock);
#else
    return 0;
#endif
}

void LLVKFrameGen::directBlocked(llfg::Block b, const std::string& detail, const std::string& logDetail)
{
    mDirectReady = false;
    mBlock = b;
    mReason = std::string(llfg::blockText(b)) + (detail.empty() ? std::string() : " (" + detail + ")");
    mFrameBlock = b;
    mStatusText = mReason;
    LL_INFOS("Vulkan") << "Frame generation: unavailable: " << mReason
                       << (logDetail.empty() ? std::string() : " -- " + logDetail) << LL_ENDL;
}

void LLVKFrameGen::initDirect(VkDeviceContext& dev)
{
    if (mDirectReady) return;
#if LL_WINDOWS || LL_LINUX
    mDirectFailed = false;
    mCaps = llfg::Caps{};
    // Both generators say what they can do on this device (their init), or their block -- with a detail for
    // the player's line and one for the log only. Then the one that runs (llfg::chooseGenerator): DLSS where
    // it runs, else FSR, unless RenderVKFrameGenGenerator names one. Neither: the block of the one whose
    // reason helps the player.
    if (!mDlss.gen) mDlss.gen = std::make_unique<LLVKFrameGenDlss>();
    if (!mFsr.gen) mFsr.gen = std::make_unique<LLVKFrameGenFsr>();
    for (Backend* b : {&mDlss, &mFsr})
    {
        b->detail.clear();
        b->log.clear();
        b->block = b->gen->init(dev, b->caps, b->detail, b->log);
    }
    static LLCachedControl<U32> s_generator(gSavedSettings, "RenderVKFrameGenGenerator", 0);
    const llfg::GeneratorChoice choice = llfg::chooseGenerator((U32)s_generator, mDlss.block, mFsr.block);
    const Backend* const chosen = choice.generator == llfg::Generator::Dlss ? &mDlss
                                : choice.generator == llfg::Generator::Fsr  ? &mFsr : nullptr;
    mGen = chosen ? chosen->gen.get() : nullptr;
    mGeneratorAsked = (U32)s_generator;   // (a later change switches live: generatorChangeAsked / applyGenerator)
    for (const Backend* b : {&mDlss, &mFsr})
    {
        std::string state = b == chosen ? "is the generator in use" : "can run (not chosen)";
        if (b->block != llfg::Block::None)
            state = std::string("cannot run: ") + llfg::blockText(b->block) + (b->detail.empty() ? std::string() : " (" + b->detail + ")");
        LL_INFOS("Vulkan") << "Frame generation: " << llfg::generatorName(b->gen->kind()) << " " << state
                           << " (RenderVKFrameGenGenerator " << (U32)s_generator << ")" << LL_ENDL;
    }
    if (!chosen)
    {
        const Backend& why = choice.reasonOf == llfg::Generator::Fsr ? mFsr : mDlss;
        directBlocked(choice.reason, why.detail, why.log);
        return;
    }
    adopt(*chosen, dev);
    {
        std::lock_guard<std::mutex> lock(mPacingMutex);
        mPacing = llfg::PacingState{};
    }
    PFN_vkVoidFunction calibrate = nullptr;
    if (dev.calibratedTimestamps())
    {
        calibrate = vkGetDeviceProcAddr(dev.device(), "vkGetCalibratedTimestampsKHR");
        if (!calibrate) calibrate = vkGetDeviceProcAddr(dev.device(), "vkGetCalibratedTimestampsEXT");
    }
    // (#1544 D8: the present thread's queue -- its own, or the renderer's shared under one lock where the
    // graphics family has a single queue; blocked only where the device has no usable queue at all)
    const llfg::Block queueBlock =
        llfg::presentQueueBlock(llfg::presentQueueOf(dev.presentQueue() != VK_NULL_HANDLE, dev.presentQueueShared()));
    if (queueBlock != llfg::Block::None)
    {
        directBlocked(queueBlock, "no queue to present on");
        return;
    }
    if (dev.presentQueueShared())
        LL_INFOS("Vulkan") << "Frame generation: the present thread shares the renderer's graphics queue (one queue "
                              "in its family, or RenderVKFrameGenSharedQueue); generated frames need an IMMEDIATE "
                              "present mode" << LL_ENDL;
    if (!dev.calibratedTimestamps() || !calibrate)
    {
        // (the pacer's clock, not the queue: Block::NoPresentQueue is the queue's alone)
        directBlocked(llfg::Block::Unsupported, "no calibrated timestamps");
        return;
    }
    // Pacing needs each frame's GPU end on the CPU clock: timestamps on the graphics queue, and
    // both clocks calibrateable (else every batch would go out unpaced, silently). The CPU clock: the
    // performance counter on Windows; on Linux CLOCK_MONOTONIC, else CLOCK_MONOTONIC_RAW (llfg::pacerClock).
#if LL_WINDOWS
    constexpr bool kWindows = true;
    const char* const cpuClockName = "the performance counter";
#elif LL_LINUX
    constexpr bool kWindows = false;
    const char* const cpuClockName = "CLOCK_MONOTONIC or CLOCK_MONOTONIC_RAW";
#endif
    llfg::PacerClock cpuClock = llfg::PacerClock::None;
    {
        const VkPhysicalDevice pd = dev.physicalDevice();
        std::string clocks;
        uint32_t nf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &nf, nullptr);
        std::vector<VkQueueFamilyProperties> fam(nf);
        if (nf) vkGetPhysicalDeviceQueueFamilyProperties(pd, &nf, fam.data());
        if (dev.queueFamily() >= nf || fam[dev.queueFamily()].timestampValidBits == 0)
            clocks = "the graphics queue writes no timestamps";
        if (clocks.empty())
        {
            PFN_vkVoidFunction q = vkGetInstanceProcAddr(dev.instance(), "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR");
            if (!q) q = vkGetInstanceProcAddr(dev.instance(), "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
            const PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR domains = (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsKHR)q;
            bool gpu = false, qpc = false, mono = false, raw = false;
            std::vector<VkTimeDomainKHR> list;
            if (domains && vkenum::collect(list,
                [&](uint32_t* n, VkTimeDomainKHR* p) { return domains(pd, n, p); },
                "vkGetPhysicalDeviceCalibrateableTimeDomainsKHR", 16) == VK_SUCCESS)
            {
                for (const auto domain : list)
                {
                    gpu = gpu || domain == VK_TIME_DOMAIN_DEVICE_KHR;
                    qpc = qpc || domain == VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR;
                    mono = mono || domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR;
                    raw = raw || domain == VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR;
                }
            }
            cpuClock = llfg::pacerClock(kWindows, qpc, mono, raw);
            const bool cpu = cpuClock != llfg::PacerClock::None;
            if (!domains)
                clocks = "no calibrateable time domain query";
            else if (!gpu || !cpu)
                clocks = std::string("the device cannot calibrate ")
                       + (!gpu && !cpu ? std::string("its clock or ") + cpuClockName : !gpu ? std::string("its clock") : std::string(cpuClockName));
        }
        if (!clocks.empty())
        {
            directBlocked(llfg::Block::Unsupported, clocks);
            return;
        }
    }

    const VkDevice d = dev.device();
    auto timeline = [d](VkSemaphore& out)
    {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        ci.pNext = &type;
        return vkCreateSemaphore(d, &ci, nullptr, &out) == VK_SUCCESS;
    };
    VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
    qi.queryCount = kTimestampQueries;
    if (!timeline(mGate) || !timeline(mFrameDone) || !timeline(mGeneratedDone) ||
        vkCreateQueryPool(d, &qi, nullptr, &mQueryPool) != VK_SUCCESS)
    {
        if (mGate) vkDestroySemaphore(d, mGate, nullptr);
        if (mFrameDone) vkDestroySemaphore(d, mFrameDone, nullptr);
        if (mGeneratedDone) vkDestroySemaphore(d, mGeneratedDone, nullptr);
        mGate = mFrameDone = mGeneratedDone = VK_NULL_HANDLE;
        mQueryPool = VK_NULL_HANDLE;
        directBlocked(llfg::Block::Unsupported, "its semaphores or timestamp queries could not be created");
        return;
    }
    // (every query starts reset: the pacer may read one before its first write is done)
    const VkQueryPool pool = mQueryPool;
    dev.submitImmediate([pool](VkCommandBuffer c) { vkCmdResetQueryPool(c, pool, 0, kTimestampQueries); });
    mDirectDevice = d;
    mCalibrate = calibrate;
    mPacerClock = cpuClock;
    // The CPU side of each calibration: the pacer's clock (its values are what nowNs() reads).
    const VkTimeDomainKHR cpuDomain = cpuClock == llfg::PacerClock::Qpc       ? VK_TIME_DOMAIN_QUERY_PERFORMANCE_COUNTER_KHR
                                    : cpuClock == llfg::PacerClock::Monotonic ? VK_TIME_DOMAIN_CLOCK_MONOTONIC_KHR
                                                                           : VK_TIME_DOMAIN_CLOCK_MONOTONIC_RAW_KHR;
#if LL_WINDOWS
    // The pacer thread's sleeps (its only user): a high-resolution timer, else it spins.
    mWaitTimer = (void*)CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!mWaitTimer)
        LL_INFOS("Vulkan") << "Frame generation: no high-resolution waitable timer (error "
                           << (unsigned long)GetLastError() << "); the pacer spins while it waits" << LL_ENDL;
#elif LL_LINUX
    mWaitTimer = nullptr;   // (the Linux pacer sleeps on its clock itself: clock_nanosleep / nanosleep)
#endif
    mQueryNext.store(0);
    mFrameDoneNext = 1;
    mGeneratedDoneNext = 1;
    meteringReset();
    mEvalFailed.store(false);
    mEvalStopped.store(false);
    mImagesRefused = false;
    mDirectFrame = mEvalFrame = 0;
    mFeatureNew = true;
    mFeatureReady = false;
    mDirectAsked = mDirectGenerated = 0;
    mWinAsked = mWinGenerated = mWinShown = 0;
    mWinRefusal.clear();
    mLastRefusal.clear();

    VkFramePacer::Io io;
    io.waitFence = [this](uint64_t token, uint64_t timeoutNs)
    {
        // (the batch's generated frames drawn: the present thread's submit, contract (b))
        VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
        wi.semaphoreCount = 1;
        wi.pSemaphores = &mGeneratedDone;
        wi.pValues = &token;
        return vkWaitSemaphores(mDirectDevice, &wi, timeoutNs) == VK_SUCCESS;
    };
    io.readTimestamp = [this](uint32_t query, uint64_t* gpuTicks)
    {
        uint64_t v = 0;
        if (vkGetQueryPoolResults(mDirectDevice, mQueryPool, query % kTimestampQueries, 1, sizeof(v), &v, sizeof(v),
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return false;   // (not written yet, or reset again)
        // Read after the result: kTimestampQueries or more queries handed out since this one means
        // its slot was recorded again -- the value may be a later frame's.
        if (mQueryNext.load() - query - 1u >= kTimestampQueries) return false;
        *gpuTicks = v;
        return true;
    };
    io.calibrate = [this, cpuDomain](uint64_t* gpuTicks, int64_t* cpuNs)
    {
        VkCalibratedTimestampInfoKHR info[2] = {{VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR},
                                                {VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_KHR}};
        info[0].timeDomain = VK_TIME_DOMAIN_DEVICE_KHR;
        info[1].timeDomain = cpuDomain;
        uint64_t ts[2] = {0, 0}, deviation = 0;
        if (((PFN_vkGetCalibratedTimestampsKHR)mCalibrate)(mDirectDevice, 2, info, ts, &deviation) != VK_SUCCESS)
            return false;
        *gpuTicks = ts[0];
#if LL_WINDOWS
        *cpuNs = qpcToNs((int64_t)ts[1]);
#elif LL_LINUX
        *cpuNs = (int64_t)ts[1];   // (the CLOCK_MONOTONIC[_RAW] domains count nanoseconds, as clock_gettime)
#endif
        return true;
    };
#if LL_WINDOWS
    io.nowNs = [] { return qpcNowNs(); };
    io.waitUntil = [this](int64_t untilNs)
    {
        // The high-resolution timer in steps of at most 1 ms until 1 ms before (a drain is seen
        // between steps), then a spin; a drain releases at once. Without the timer: all spin
        // (bounded by llfg::kMaxPacedFrameNs) -- Sleep can be ~15 ms late.
        for (;;)
        {
            if (mPacer.draining()) return;
            const int64_t left = untilNs - qpcNowNs();
            if (left <= 0) return;
            const int64_t stepNs = left - 1000000 < 1000000 ? left - 1000000 : 1000000;
            if (mWaitTimer && stepNs >= 100)
            {
                LARGE_INTEGER due;
                due.QuadPart = -(LONGLONG)(stepNs / 100);   // relative, 100 ns units
                if (SetWaitableTimer((HANDLE)mWaitTimer, &due, 0, nullptr, nullptr, FALSE))
                    WaitForSingleObject((HANDLE)mWaitTimer, 2);
                else
                    YieldProcessor();
            }
            else
                YieldProcessor();
        }
    };
#elif LL_LINUX
    io.nowNs = [cpuClock] { return linuxNowNs(cpuClock); };
    io.waitUntil = [this, cpuClock](int64_t untilNs)
    {
        // Steps of at most 1 ms until 1 ms before (a drain is seen between them; llfg::pacerSleepNs), then a
        // spin on the same clock; a drain releases at once. CLOCK_MONOTONIC sleeps to each step's end
        // (clock_nanosleep, TIMER_ABSTIME: never longer for the time spent getting there); CLOCK_MONOTONIC_RAW
        // has no sleep of its own: a relative step (nanosleep). An interrupted sleep only loops again.
        for (;;)
        {
            if (mPacer.draining()) return;
            const int64_t now = linuxNowNs(cpuClock);
            const int64_t left = untilNs - now;
            if (left <= 0) return;
            const int64_t stepNs = llfg::pacerSleepNs(left);
            if (stepNs <= 0)
                cpuRelax();
            else if (llfg::pacerSleepsAbsolute(cpuClock))
            {
                const timespec until = toTimespec(now + stepNs);
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &until, nullptr);
            }
            else
            {
                const timespec step = toTimespec(stepNs);
                nanosleep(&step, nullptr);
            }
        }
    };
#endif
    io.signal = [this](uint64_t value)
    {
        // (a push after shutdownDirect signals on the main thread at once: the gate is gone, so is every waiter)
        if (!mGate || !mDirectDevice) return;
        VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        si.semaphore = mGate;
        si.value = value;
        vkSignalSemaphore(mDirectDevice, &si);
    };
    io.nsPerGpuTick = dev.timestampPeriodNs();
    mPacer.start(std::move(io));
    initReflex(dev);   // (without it frame generation runs all the same, paced as without Reflex)
    mDirectReady = true;
    mBlock = llfg::Block::None;
    mReason.clear();
    mFrameBlock = llfg::Block::None;
    mStatusText.clear();
    LL_INFOS("Vulkan") << "Frame generation: ready, up to " << (mCaps.maxGenerated + 1) << kTimes
                       << "; Reflex " << (mReflexOn ? "yes" : "no") << "; driver pacing "
                       << (mMeteringFeature ? "available (in fullscreen)" : "no (the driver has no present metering)") << "; pacer clock "
                       << llfg::pacerClockName(cpuClock) << LL_ENDL;
#else
    (void)dev;
    directBlocked(llfg::Block::Platform, std::string());
#endif
}

void LLVKFrameGen::adopt(const Backend& b, VkDeviceContext& dev)
{
    const llfg::GeneratorCaps& gc = b.caps;
    mGen = b.gen.get();
    mCaps.generator = b.gen->kind();
    mCaps.loaded = true;
    mCaps.reflex = gc.reflex;
    mCaps.frameGeneration = true;
    // (never more than one batch of presents carries: the present thread's per-batch arrays)
    mCaps.maxGenerated = llfg::maxGeneratedFor(gc.maxGenerated, VkSwapchain::kMaxBatchPresents);
    if (mCaps.maxGenerated != gc.maxGenerated)
        LL_INFOS("Vulkan") << "Frame generation: the driver offers " << gc.maxGenerated << " generated frames per "
                           << "rendered frame; this viewer runs at most " << mCaps.maxGenerated << LL_ENDL;
    mMeteringFeature = llfg::meteringFeature(dev.presentMetering(), gc);
}

const LLVKFrameGen::Backend* LLVKFrameGen::chosenFor(uint32_t setting) const
{
    const llfg::GeneratorChoice choice = llfg::chooseGenerator(setting, mDlss.block, mFsr.block);
    return choice.generator == llfg::Generator::Dlss ? &mDlss : choice.generator == llfg::Generator::Fsr ? &mFsr : nullptr;
}

bool LLVKFrameGen::generatorChangeAsked()
{
    static LLCachedControl<U32> s_generator(gSavedSettings, "RenderVKFrameGenGenerator", 0);
    const uint32_t setting = (U32)s_generator;
    if (setting == mGeneratorAsked) return false;
    mGeneratorAsked = setting;
    // (a session that stopped stays stopped; no generator at startup: nothing was set up to switch to)
    if (!mDirectReady || mDirectFailed || !mGen) return false;
    const Backend* next = chosenFor(setting);
    if (!next)
    {
        LL_INFOS("Vulkan") << "Frame generation: RenderVKFrameGenGenerator " << setting << " names a generator that "
                           << "cannot run here; " << llfg::generatorName(mGen->kind()) << " stays in use" << LL_ENDL;
        return false;
    }
    return next->gen.get() != mGen;
}

void LLVKFrameGen::applyGenerator(VkDeviceContext& dev, bool deviceIdle)
{
    static LLCachedControl<U32> s_generator(gSavedSettings, "RenderVKFrameGenGenerator", 0);
    if (!mDirectReady || mDirectFailed || !mGen) return;
    const Backend* next = chosenFor((U32)s_generator);
    if (!next || next->gen.get() == mGen) return;
    // (the old feature may be in use by work in flight: only after the rebuild's device wait succeeded)
    if (!deviceIdle) return;
    drainPacer();   // (the present thread is drained: the caller's drainFrameGen; no gate left shut)
    const llfg::Generator was = mGen->kind();
    if (mFeatureMade)
    {
        mGen->release(dev);
        mFeatureMade = false;
    }
    mFeatureReady = false;
    adopt(*next, dev);
    mFeatureNew = true;          // (the next frame's evaluate resets: a new generator has no history)
    mEvalStopped.store(false);   // (no evaluate of the old one is left: the present thread is drained)
    meteringReset();             // (another generator: driver pacing is asked again)
    LL_INFOS("Vulkan") << "Frame generation: the generator is now " << llfg::generatorName(mGen->kind()) << " (was "
                       << llfg::generatorName(was) << "; RenderVKFrameGenGenerator " << (U32)s_generator << "), up to "
                       << (mCaps.maxGenerated + 1) << kTimes << LL_ENDL;
}

uint64_t LLVKFrameGen::generatorBytes(VkExtent2D display, VkFormat colorFormat, VkFormat scratchFormat) const
{
    if (!mDirectReady || !mGen) return 0;
    llfg::GenSize size;
    size.displayW = display.width;
    size.displayH = display.height;
    size.colorFormat = colorFormat;
    size.scratchFormat = scratchFormat;
    size.slots = llfg::kMaxPresentSlots;
    return mGen->imageBytes(size);   // (render size not stated: the output size, the largest)
}

void LLVKFrameGen::shutdownDirect(VkDeviceContext& dev)
{
    mPacer.stop();   // (every value it was given is signalled; its thread is gone)
    // #1547 GPU-loss recovery: on a lost device nothing is waited for or destroyed (its objects go with the device)
    // and the add-on is not entered (LLVKNeuralRecon::fgRelease refuses); the state below is forgotten all the same.
    const VkDevice d = vkDeviceIsLost() ? VK_NULL_HANDLE : dev.device();
    if (d && (mFeatureMade || mGate || mFrameDone || mGeneratedDone || mQueryPool || mReflexSem)) vkDeviceWaitIdle(d);
    if (mFeatureMade && mGen) mGen->release(dev);
    // (what a generator keeps for the session: nothing of it is in use -- its feature is released, or the
    // generator never ran)
    // #1550 x #1544: on a lost device the generators are dropped, not shut down: FSR's pipelines, layouts and
    // sampler are the lost device's, and a recovery's initDirect on the new device would otherwise reuse them
    // (ensurePipelines: mPipelines still true). Their destructors make no Vulkan calls; initDirect makes new ones.
    for (Backend* b : {&mDlss, &mFsr})
    {
        if (!b->gen) continue;
        if (d)
            b->gen->shutdown(dev);
        else
            b->gen.reset();
    }
    if (!d) mGen = nullptr;
    if (d)
    {
        if (mReflexSem)
        {
            // (a sleep's signal the driver may still owe -- one cut at the timeout -- lands before the
            // semaphore goes: its last value, at most kReflexSleepTimeoutNs; past that it goes anyway)
            if (mReflexSemValue)
            {
                VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
                wi.semaphoreCount = 1;
                wi.pSemaphores = &mReflexSem;
                wi.pValues = &mReflexSemValue;
                if (vkWaitSemaphores(d, &wi, kReflexSleepTimeoutNs) != VK_SUCCESS)
                    LL_INFOS("Vulkan") << "Frame generation: the last Reflex sleep was never answered; "
                                       << "its semaphore goes regardless" << LL_ENDL;
            }
            vkDestroySemaphore(d, mReflexSem, nullptr);
        }
        if (mGate) vkDestroySemaphore(d, mGate, nullptr);
        if (mFrameDone) vkDestroySemaphore(d, mFrameDone, nullptr);
        if (mGeneratedDone) vkDestroySemaphore(d, mGeneratedDone, nullptr);
        if (mQueryPool) vkDestroyQueryPool(d, mQueryPool, nullptr);
    }
    mGate = mFrameDone = mGeneratedDone = VK_NULL_HANDLE;
    mQueryPool = VK_NULL_HANDLE;
    // (Reflex: the main swapchain was dropped first, setReflexSwapchain(VK_NULL_HANDLE))
    mReflexSem = VK_NULL_HANDLE;
    mReflexSemValue = 0;
    mReflexOn = false;
    mLlSetMode = nullptr;
    mLlSleep = nullptr;
    mLlMarker = nullptr;
    mReflexSwapchain = VK_NULL_HANDLE;
    mReflexLatencyChain = mReflexFgChain = false;
    mReflexBegun = mReflexOpen = false;
    mDirectDevice = VK_NULL_HANDLE;
    mCalibrate = nullptr;
#if LL_WINDOWS
    if (mWaitTimer) CloseHandle((HANDLE)mWaitTimer);
#elif LL_LINUX
    // (no timer object on Linux: the pacer slept on its clock)
#endif
    mWaitTimer = nullptr;
    mDirectReady = false;
    mFeatureMade = mFeatureReady = false;
    mDirectAsked = mDirectGenerated = 0;
    mDirectConstantsHave = false;
}

void LLVKFrameGen::drainPacer()
{
    if (mDirectReady) mPacer.drain();
}

bool LLVKFrameGen::driverPacing() const
{
    std::lock_guard<std::mutex> lock(mPacingMutex);
    return mDirectReady && llfg::driverPacing(mMeteringFeature, mPacing);
}

bool LLVKFrameGen::driverPacingAccepted() const
{
    std::lock_guard<std::mutex> lock(mPacingMutex);
    return mDirectReady && llfg::driverPacingAccepted(mMeteringFeature, mPacing, pacingNowNs());
}

const char* LLVKFrameGen::cappedMultiplierText() const
{
    static LLCachedControl<U32> s_fg_mult(gSavedSettings, "RenderVKFrameGen", 0);
    return mDirectReady ? llfg::cappedMultiplierText((U32)s_fg_mult, mCaps, driverPacingAccepted(), mMeteringFeature) : "";
}

uint32_t LLVKFrameGen::decideDirect(const llfg::Context& ctx, uint32_t swapchainImages)
{
    // The multiplier, live: on / off and 2x..Nx take effect on the next frame.
    static LLCachedControl<U32> s_fg_mult(gSavedSettings, "RenderVKFrameGen", 0);
    if (!mDirectReady)
    {
        mFrameBlock = mBlock;
        return mDirectAsked = mDirectGenerated = 0;
    }
    ++mDirectFrame;
    {
        // (the frame clock for llfg::GenInputs::frameMs: from this frame's decision to the one before)
        const int64_t now = (int64_t)pacingNowNs();
        mFrameMs = mDecideNs ? (float)((double)(now - mDecideNs) / 1.0e6) : 0.f;
        mDecideNs = now;
    }
    // Driver pacing: in fullscreen, where the driver can accept again without a new chain (a desktop overlay
    // over the window gone), it is asked again now and then after it stopped being asked.
    {
        static LLCachedControl<bool> s_fullscreen(gSavedSettings, "FullScreen", false);
        const uint64_t now = pacingNowNs();
        std::lock_guard<std::mutex> lock(mPacingMutex);
        if (mPacing.retryDue((bool)s_fullscreen, now)) mPacing.retry(now);
    }
    // (before recording: this frame's DLSS stores its own; a frame without one never gets an older frame's)
    mDirectConstantsHave = false;
    // The present thread's evaluate refused since the last frame: generation ends here, with its reason.
    if (mEvalFailed.exchange(false))
    {
        std::string why;
        {
            std::lock_guard<std::mutex> lock(mEvalMutex);
            why.swap(mEvalFailure);
        }
        directFailed(llfg::Stop::Evaluate, why);
    }
    uint32_t n = 0, want = 0;
    if (mDirectFailed)
        mFrameBlock = llfg::Block::Unsupported;
    else
    {
        mFrameBlock = llfg::blockReason(mCaps, ctx);
        if (mFrameBlock == llfg::Block::None)
        {
            // (above 4x only while the driver paces the presents: back to 4x the frame after a refusal)
            want = llfg::generatedFrames((U32)s_fg_mult, mCaps, driverPacingAccepted());
            n = llfg::generatedForImages(want, swapchainImages);
        }
    }
    if (n != mDirectAsked)
    {
        meteringReset();   // (a multiplier change asks the driver again)
        if ((n == 0) != (mDirectAsked == 0))
        {
            const char* why = mFrameBlock != llfg::Block::None ? llfg::blockText(mFrameBlock)
                            : want == 0                        ? "RenderVKFrameGen is below 2"
                                                               : "too few swapchain images";
            LL_INFOS("Vulkan") << "Frame generation: " << (n ? "on " : "off")
                               << (n ? std::to_string(n + 1) + kTimes : std::string(" (") + why + ")") << LL_ENDL;
        }
        mDirectAsked = n;
    }
    mDirectGenerated = n;
    mWinAsked += n;
    return n;
}

uint32_t LLVKFrameGen::chainGenerated(bool outputGenerates) const
{
    // (on / off live: the render loop rebuilds the main swapchain when this changes)
    static LLCachedControl<U32> s_fg_mult(gSavedSettings, "RenderVKFrameGen", 0);
    // An upscaler that writes the generator's inputs as the chosen one: the setting (1-6 FSR, 7-11 DLSS:
    // llfg::upscalerWritesInputs), not whether it ran last frame -- that changes at every login or teleport
    // and would rebuild the chain each time.
    static LLCachedControl<U32> s_upscaler(gSavedSettings, "RenderVKUpscaler", 0);
    const bool upscalerChosen = llfg::upscalerWritesInputs((U32)s_upscaler);
    // (the multiplier asked for: a chain of generated + 1 images; a higher one later rebuilds it)
    return (mDirectReady && !mDirectFailed && !imagesRefusedNow() && upscalerChosen && outputGenerates)
               ? llfg::generatedFrames((U32)s_fg_mult, mCaps, driverPacingAccepted()) : 0;
}

bool LLVKFrameGen::imagesRefusedNow() const
{
    // (a refusal holds for the multiplier it was made for; another one tries again)
    static LLCachedControl<U32> s_fg_mult(gSavedSettings, "RenderVKFrameGen", 0);
    return mImagesRefused && (U32)s_fg_mult == mImagesRefusedMult;
}

void LLVKFrameGen::imagesRefused(const char* why)
{
    static LLCachedControl<U32> s_fg_mult(gSavedSettings, "RenderVKFrameGen", 0);
    if (!mDirectReady || !why) return;
    mImagesRefused = true;
    mImagesRefusedMult = (U32)s_fg_mult;
    mWinRefusal = why;
    mStatusText = why;
    mLastRefusal = why;
    LL_WARNS("Vulkan") << "Frame generation: off: " << why << "; tried again when RenderVKFrameGen changes or the "
                       << "swapchain is rebuilt (a resize, the display)" << LL_ENDL;
}

void LLVKFrameGen::directRefused(const char* reason)
{
    if (!mDirectReady || !reason) return;
    mDirectGenerated = 0;
    mWinRefusal = reason;
    mStatusText = reason;
    if (mLastRefusal != reason)
    {
        mLastRefusal = reason;
        LL_INFOS("Vulkan") << "Frame generation: no generated frames: " << reason << LL_ENDL;
    }
}

void LLVKFrameGen::directFailed(llfg::Stop stop, const std::string& why)
{
    // What the player reads says it stopped and what brings it back (llfg::stoppedText: the Preferences
    // control, the status); the add-on's own error goes to the log.
    mDirectFailed = true;
    mDirectAsked = mDirectGenerated = 0;
    mBlock = llfg::Block::Unsupported;
    mReason = llfg::stoppedText(stop);
    mFrameBlock = llfg::Block::Unsupported;
    mStatusText = mReason;
    LL_WARNS("Vulkan") << "Frame generation: " << why << "; no frame generation for the rest of the session"
                       << LL_ENDL;
}

bool LLVKFrameGen::prepareFeature(VkDeviceContext& dev, const llfg::GenSize& size)
{
    if (!mDirectReady || mDirectFailed || !mGen) return false;
    if (featureFits(size)) return true;
    if (mFeatureMade)
    {
        // The feature in use by work in flight: the caller drained the present thread (no evaluate records on
        // it, and none will until this thread hands a frame over again); no gate left shut, then the GPU done
        // with what was submitted. A wait that failed -- a lost device, no memory -- releases nothing.
        drainPacer();
        if (vkDeviceWaitIdle(dev.device()) != VK_SUCCESS) return false;
        mGen->release(dev);
        mFeatureMade = mFeatureReady = false;
    }
    std::string err;
    const bool made = mGen->prepare(dev, size, err);
    mFeatureMade = true;   // (released at shutdown even after a refusal: the generator may hold a part)
    mFeatureNew = true;
    if (!made)
    {
        directFailed(llfg::Stop::Create, "the frame generation feature (" + std::to_string(size.displayW) + "x"
                     + std::to_string(size.displayH) + ") could not be created: " + err);
        return false;
    }
    mFeatureReady = true;
    mEvalStopped.store(false);   // (a new feature; the present thread is drained)
    LL_INFOS("Vulkan") << "Frame generation: feature created, " << size.displayW << "x" << size.displayH << ", format "
                       << (int)size.colorFormat << LL_ENDL;
    return true;
}

uint32_t LLVKFrameGen::generatorDebug() const
{
    static LLCachedControl<U32> s_conv(gSavedSettings, "RenderVKFrameGenConventionDebug", 0);
    static LLCachedControl<U32> s_fsr(gSavedSettings, "RenderVKFrameGenFsrDebug", 0);
    const llfg::Generator g = mGen ? mGen->kind() : llfg::Generator::None;
    return g == llfg::Generator::Dlss ? (uint32_t)(U32)s_conv : g == llfg::Generator::Fsr ? (uint32_t)(U32)s_fsr : 0u;
}

void LLVKFrameGen::frameSkipped()
{
    mEvalFrame = llfg::lastEvaluatedAfterSkip(mEvalFrame, mDirectFrame);
}

bool LLVKFrameGen::frameReset(bool rendererReset)
{
    // (every generated frame of this real frame takes the same answer)
    const bool reset = llfg::historyReset(rendererReset || mFeatureNew, mEvalFrame, mDirectFrame);
    mFeatureNew = false;
    mEvalFrame = mDirectFrame;
    ++mJobSerial;   // (frameSerial: this job's)
    return reset;
}

bool LLVKFrameGen::evaluate(VkCommandBuffer cmd, uint32_t slot, const llfg::GenInputs& in, uint32_t index, uint32_t count)
{
    // (the present thread: mDirectReady, mFeatureReady and mGen change only while it is drained or stopped;
    // after a failure, the jobs already queued never reach the generator again -- one failure, one report)
    if (!mDirectReady || !mFeatureReady || mEvalStopped.load() || !cmd || !mGen) return false;
    std::string err;
    if (mGen->evaluate(cmd, slot, in, index, count, err)) return true;
    {
        std::lock_guard<std::mutex> lock(mEvalMutex);
        mEvalFailure = "frame generation evaluate failed: " + err;
    }
    mEvalStopped.store(true);
    mEvalFailed.store(true);
    return false;
}

uint32_t LLVKFrameGen::timestampQuery(VkCommandBuffer cmd)
{
    const uint32_t q = mQueryNext.load();
    if (!mDirectReady || !cmd) return q;
    vkCmdResetQueryPool(cmd, mQueryPool, q % kTimestampQueries, 1);
    vkCmdWriteTimestamp2(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, mQueryPool, q % kTimestampQueries);
    mQueryNext.store(q + 1);   // (after recording; before the submit the GPU could run it)
    return q;
}

void LLVKFrameGen::batchPresented(uint32_t shown, uint32_t generated)
{
    if (!mDirectReady) return;
    ++mStatReal;
    mStatShown += shown;
    mWinGenerated += generated;
    mWinShown += shown ? shown - 1 : 0;
    const double now = LLTimer::getTotalSeconds();
    if (mStatStart == 0.0) mStatStart = now;
    if (now - mStatStart < 5.0) return;
    mShownPerRendered = mStatReal ? (float)mStatShown / (float)mStatReal : 0.f;
    // (only generated frames really shown count as working: asked for but none shown says why)
    if (mDirectFailed)
        mStatusText = mReason;
    else if (mWinShown)
        mStatusText = "ok";
    else if (!mWinRefusal.empty())
        mStatusText = mWinRefusal;
    else if (mWinAsked)
        mStatusText = "no frames generated";
    else
        mStatusText = mFrameBlock == llfg::Block::None ? "off" : llfg::blockText(mFrameBlock);
    // (log volume: the window is 5 s; the summary line goes out every sixth active window, 30 s -- a refusal
    // is logged on its own when it first happens)
    static uint32_t s_winLogs = 0;
    if (mWinAsked && (s_winLogs++ % 6u) == 0u) logDirectLine();
    if (mWinRefusal.empty()) mLastRefusal.clear();   // (a clean window: a refusal later is logged again)
    mWinAsked = mWinGenerated = mWinShown = 0;
    mWinRefusal.clear();
    mPresentCalls = mSlotWaits = 0;
    mPresentCallSumMs = mPresentCallWorstMs = mSlotWaitSumMs = mSlotWaitWorstMs = 0.0;
    mReflexWaits = 0;
    mReflexWaitSumMs = mReflexWaitWorstMs = 0.0;
    mStatReal = mStatShown = 0;
    mStatStart = now;
}

void LLVKFrameGen::releaseGates(uint64_t first, uint32_t count)
{
#if LL_WINDOWS || LL_LINUX
    if (!mGate || !mDirectDevice) return;
    // (a timeline only moves forward: a value already reached is not signalled again)
    uint64_t reached = 0;
    if (vkGetSemaphoreCounterValue(mDirectDevice, mGate, &reached) != VK_SUCCESS) reached = 0;
    for (uint32_t k = 0; k < count; ++k)
    {
        if (first + k <= reached) continue;
        VkSemaphoreSignalInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO};
        si.semaphore = mGate;
        si.value = first + k;
        vkSignalSemaphore(mDirectDevice, &si);
    }
#else
    (void)first;
    (void)count;
#endif
}

void LLVKFrameGen::notePresentCall(double ms)
{
    ++mPresentCalls;
    mPresentCallSumMs += ms;
    if (ms > mPresentCallWorstMs) mPresentCallWorstMs = ms;
}

void LLVKFrameGen::noteSlotWait(double ms)
{
    ++mSlotWaits;
    mSlotWaitSumMs += ms;
    if (ms > mSlotWaitWorstMs) mSlotWaitWorstMs = ms;
}

void LLVKFrameGen::logDirectLine()
{
    const VkFramePacer::Stats st = mPacer.takeStats();
    const bool driver = driverPacing();
    uint32_t refusedInARow = 0;
    {
        std::lock_guard<std::mutex> lock(mPacingMutex);
        refusedInARow = mPacing.refusedInARow;
    }
    // Reflex: whether it paces, and its sleeps' waits in this window (~0 ms under a frame cap below the
    // uncapped rate: the sleep does nothing).
    char reflex[96];
    if (mReflexGaveUp)
        std::snprintf(reflex, sizeof(reflex), "off (the driver did not answer)");
    else if (mReflexWaits)
        std::snprintf(reflex, sizeof(reflex), "%s (sleep avg %.2f ms, worst %.2f ms)", reflexCapsFrames() ? "on" : "off",
                      mReflexWaitSumMs / (double)mReflexWaits, mReflexWaitWorstMs);
    else
        std::snprintf(reflex, sizeof(reflex), "%s (no sleeps)", reflexCapsFrames() ? "on" : "off");
    // (generated frames that got no swapchain image in time, or could not be drawn, are made but not shown)
    char shownOf[64] = "";
    if (mWinShown != mWinGenerated)
        std::snprintf(shownOf, sizeof(shownOf), ", shown %llu of %llu", (unsigned long long)mWinShown,
                      (unsigned long long)mWinAsked);
    // The renderer's waits for a present slot (held back by the present thread) and the present thread's
    // present calls per job (a driver that blocks in them), average and worst over the window.
    const double slotAvg = mSlotWaits ? mSlotWaitSumMs / (double)mSlotWaits : 0.0;
    const double presentAvg = mPresentCalls ? mPresentCallSumMs / (double)mPresentCalls : 0.0;
    char line[768];
    std::snprintf(line, sizeof(line),
                  "Frame generation: %s, generated %llu of %llu asked%s, %.2f frames shown per real frame; pacer: %llu "
                  "batches paced, %llu released at once, worst %.2f ms late; driver pacing %s (refused %u in a row); "
                  "Reflex %s; slot wait avg %.2f ms, worst %.2f ms; present calls avg %.2f ms, worst %.2f ms",
                  llfg::statusPrefix(generator(), mDirectAsked).c_str(), (unsigned long long)mWinGenerated, (unsigned long long)mWinAsked, shownOf,
                  mShownPerRendered, (unsigned long long)st.paced, (unsigned long long)st.releasedAtOnce,
                  (double)st.worstLateNs / 1.0e6, driver ? "on" : "off", refusedInARow, reflex,
                  slotAvg, mSlotWaitWorstMs, presentAvg, mPresentCallWorstMs);
    std::string extra;
    // A present call is a queue operation: over about 1 ms the driver held the main thread in it.
    if (mPresentCallWorstMs > 1.0) extra += "; BLOCKED: the driver waited in a present call";
    if (!mWinRefusal.empty()) extra += "; refused: " + mWinRefusal;
    LL_INFOS("Vulkan") << line << extra << LL_ENDL;
}

// ---- Reflex, direct (VK_NV_low_latency2) ----
// The extension's sleep, sleep mode and markers belong to a swapchain: the main one, created with
// VkSwapchainLatencyCreateInfoNV (else the sleep never signals). The markers carry our own frame id as
// their presentID; the submits and presents carry none (no VkLatencySubmissionPresentIdNV, no
// VkPresentIdKHR): the driver then ties them to the frame by the markers alone, and tagging only some
// of a frame's submits would mislead it. On a frame generation chain the present thread presents on the
// present queue, marked out of band once (VK_OUT_OF_BAND_QUEUE_TYPE_PRESENT_NV), and -- as the reference
// implementation -- brackets its own work with the out-of-band markers under the frame's id: its generate-
// and-draw submit (OUT_OF_BAND_RENDERSUBMIT_START / END) and each present call (OUT_OF_BAND_PRESENT_START /
// END). The main thread's PresentStart / PresentEnd wrap the frame's hand-over to that thread, the frame's
// present as the renderer sees it.

static_assert((uint32_t)LLVKFrameGen::SimStart == (uint32_t)VK_LATENCY_MARKER_SIMULATION_START_NV
                  && (uint32_t)LLVKFrameGen::SimEnd == (uint32_t)VK_LATENCY_MARKER_SIMULATION_END_NV
                  && (uint32_t)LLVKFrameGen::RenderStart == (uint32_t)VK_LATENCY_MARKER_RENDERSUBMIT_START_NV
                  && (uint32_t)LLVKFrameGen::RenderEnd == (uint32_t)VK_LATENCY_MARKER_RENDERSUBMIT_END_NV
                  && (uint32_t)LLVKFrameGen::PresentStart == (uint32_t)VK_LATENCY_MARKER_PRESENT_START_NV
                  && (uint32_t)LLVKFrameGen::PresentEnd == (uint32_t)VK_LATENCY_MARKER_PRESENT_END_NV,
              "LLVKFrameGen::Marker must number the markers as VkLatencyMarkerNV does");
static_assert((uint32_t)LLVKFrameGen::OobRenderStart == (uint32_t)VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_START_NV
                  && (uint32_t)LLVKFrameGen::OobRenderEnd == (uint32_t)VK_LATENCY_MARKER_OUT_OF_BAND_RENDERSUBMIT_END_NV
                  && (uint32_t)LLVKFrameGen::OobPresentStart == (uint32_t)VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_START_NV
                  && (uint32_t)LLVKFrameGen::OobPresentEnd == (uint32_t)VK_LATENCY_MARKER_OUT_OF_BAND_PRESENT_END_NV,
              "LLVKFrameGen::OobMarker must number the markers as VkLatencyMarkerNV does");

void LLVKFrameGen::initReflex(VkDeviceContext& dev)
{
    mReflexOn = false;
#if LL_WINDOWS || LL_LINUX
    if (!dev.lowLatency2()) return;
    const VkDevice d = dev.device();
    // (once, the device's own entry points: a null one leaves Reflex off)
    mLlSetMode = (PFN_vkSetLatencySleepModeNV)vkGetDeviceProcAddr(d, "vkSetLatencySleepModeNV");
    mLlSleep = (PFN_vkLatencySleepNV)vkGetDeviceProcAddr(d, "vkLatencySleepNV");
    mLlMarker = (PFN_vkSetLatencyMarkerNV)vkGetDeviceProcAddr(d, "vkSetLatencyMarkerNV");
    const PFN_vkQueueNotifyOutOfBandNV outOfBand =
        (PFN_vkQueueNotifyOutOfBandNV)vkGetDeviceProcAddr(d, "vkQueueNotifyOutOfBandNV");
    std::string why;
    if (!mLlSetMode || !mLlSleep || !mLlMarker || !outOfBand)
        why = "the low latency entry points are missing";
    else if (!dev.presentQueue())
        why = "no present queue";
    // (#1544 D8: Reflex wants the present thread's queue marked out of band; a queue shared with the renderer
    // carries the frames' own work and cannot be. Without Reflex, as on every GPU that has none.)
    else if (dev.presentQueueShared())
        why = "the present thread shares the renderer's queue";
    else
    {
        VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type.initialValue = 0;
        VkSemaphoreCreateInfo ci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        ci.pNext = &type;
        if (vkCreateSemaphore(d, &ci, nullptr, &mReflexSem) != VK_SUCCESS)
        {
            mReflexSem = VK_NULL_HANDLE;
            why = "its semaphore could not be created";
        }
    }
    if (!why.empty())
    {
        mLlSetMode = nullptr;
        mLlSleep = nullptr;
        mLlMarker = nullptr;
        LL_INFOS("Vulkan") << "Frame generation: no Reflex: " << why << LL_ENDL;
        return;
    }
    mReflexSemValue = 0;
    mReflexNextFrame = 1;
    mReflexFrame = mReflexDirectFrame = 0;
    mReflexModeUs = -1;
    mReflexBegun = mReflexOpen = false;
    mReflexTimeoutLogged = false;
    mReflexTimeouts = 0;
    mReflexGaveUp = false;
    mReflexRefusedLogged = false;
    mReflexWaits = 0;
    mReflexWaitSumMs = mReflexWaitWorstMs = 0.0;
    VkOutOfBandQueueTypeInfoNV oob{VK_STRUCTURE_TYPE_OUT_OF_BAND_QUEUE_TYPE_INFO_NV};
    oob.queueType = VK_OUT_OF_BAND_QUEUE_TYPE_PRESENT_NV;
    outOfBand(dev.presentQueue(), &oob);
    mReflexOn = true;
#else
    (void)dev;
#endif
}

bool LLVKFrameGen::multiplierOn() const
{
    static LLCachedControl<U32> s_fg_mult(gSavedSettings, "RenderVKFrameGen", 0);
    return (U32)s_fg_mult >= 2;
}

void LLVKFrameGen::setReflexSwapchain(VkSwapchainKHR swapchain, bool latencyMode, bool frameGenChain, bool ignoresVsync)
{
    // #1559: the chain that goes has Reflex's sleep mode set off first -- left on, the driver paced the next
    // chain's presents at the last frame time (frame generation turned off: the frame held, the GPU idle).
    // (#1547: not on a lost device -- a GPU-loss recovery drops the chain with the device; no driver call then)
    if (mLlSetMode && !vkDeviceIsLost() && llfg::sleepModeOffBeforeChainGoes(mReflexModeUs, mReflexSwapchain != VK_NULL_HANDLE
                                                                         && mReflexSwapchain != swapchain))
    {
        VkLatencySleepModeInfoNV off{VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
        off.lowLatencyMode = VK_FALSE;
        off.lowLatencyBoost = VK_FALSE;
        off.minimumIntervalUs = 0;
        const VkResult r = mLlSetMode(mDirectDevice, mReflexSwapchain, &off);
        if (r != VK_SUCCESS)
            LL_WARNS("Vulkan") << "Frame generation: Reflex sleep mode could not be set off on the chain that goes ("
                               << (int)r << ")" << LL_ENDL;
    }
    mMainChainNoVsync = swapchain != VK_NULL_HANDLE && ignoresVsync;   // (chainForcesNoVsync)
    mReflexSwapchain = mReflexOn ? swapchain : VK_NULL_HANDLE;
    mReflexLatencyChain = mReflexSwapchain != VK_NULL_HANDLE && latencyMode;
    mReflexFgChain = mReflexSwapchain != VK_NULL_HANDLE && frameGenChain;
    mReflexModeUs = -1;                     // (a new chain: its sleep mode is set again)
    // A frame generation chain the driver refused in low latency mode (VkSwapchain tried once more
    // without it): no Reflex on it.
    if (mReflexFgChain && !mReflexLatencyChain && !mReflexRefusedLogged)
    {
        mReflexRefusedLogged = true;
        LL_INFOS("Vulkan") << "Frame generation: Reflex off on this swapchain: it was refused in low latency "
                           << "mode (logged once)" << LL_ENDL;
    }
    mReflexBegun = mReflexOpen = false;     // (no marker of a frame begun on the old chain goes to this one)
}

void LLVKFrameGen::reflexSleep(float capFps)
{
    mReflexBegun = false;
#if LL_WINDOWS || LL_LINUX
    // (the main loop asks only while reflexCapsFrames(); anything else is a no-op)
    if (!directReflex()) return;
    mReflexFrame = mReflexNextFrame++;
    mReflexDirectFrame = mDirectFrame;
    mReflexBegun = true;
    // The frame cap: Reflex spaces the real frames by (generated + 1) / cap (RenderFPSLimit counts the
    // frames on screen). Sent when it changed, and once per chain.
    const uint32_t us = llfg::frameLimitUs(mDirectAsked, capFps);
    if ((int64_t)us != mReflexModeUs)
    {
        VkLatencySleepModeInfoNV mode{VK_STRUCTURE_TYPE_LATENCY_SLEEP_MODE_INFO_NV};
        mode.lowLatencyMode = VK_TRUE;
        mode.lowLatencyBoost = VK_FALSE;
        mode.minimumIntervalUs = us;
        const VkResult r = mLlSetMode(mDirectDevice, mReflexSwapchain, &mode);
        if (r == VK_SUCCESS)
            mReflexModeUs = (int64_t)us;
        else
        {
            static uint32_t s_warned = 0;
            if (s_warned++ < 3)
                LL_WARNS("Vulkan") << "Frame generation: Reflex sleep mode refused (" << (int)r << ")" << LL_ENDL;
        }
    }
    // The sleep: the driver signals our timeline at the next value when this frame should start. A
    // value is never reused (one the driver still owes is passed by the next); a refused sleep is not waited.
    VkLatencySleepInfoNV sleep{VK_STRUCTURE_TYPE_LATENCY_SLEEP_INFO_NV};
    sleep.signalSemaphore = mReflexSem;
    sleep.value = ++mReflexSemValue;
    const VkResult sr = mLlSleep(mDirectDevice, mReflexSwapchain, &sleep);
    if (sr != VK_SUCCESS)
    {
        static uint32_t s_warned = 0;
        if (s_warned++ < 3) LL_WARNS("Vulkan") << "Frame generation: Reflex sleep refused (" << (int)sr << ")" << LL_ENDL;
        return;
    }
    VkSemaphoreWaitInfo wi{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
    wi.semaphoreCount = 1;
    wi.pSemaphores = &mReflexSem;
    wi.pValues = &sleep.value;
    const int64_t t0 = cpuNowNs();
    const VkResult wr = vkWaitSemaphores(mDirectDevice, &wi, kReflexSleepTimeoutNs);   // (a frame never waits longer)
    const double waitedMs = (double)(cpuNowNs() - t0) / 1.0e6;
    ++mReflexWaits;
    mReflexWaitSumMs += waitedMs;
    if (waitedMs > mReflexWaitWorstMs) mReflexWaitWorstMs = waitedMs;
    if (wr == VK_SUCCESS)
        mReflexTimeouts = 0;   // (answered: a cut sleep before it is forgiven)
    else if (wr == VK_TIMEOUT)
    {
        if (!mReflexTimeoutLogged)
        {
            mReflexTimeoutLogged = true;
            LL_INFOS("Vulkan") << "Frame generation: a Reflex sleep was not over after 100 ms; the frame went on "
                               << "(logged once)" << LL_ENDL;
        }
        // The driver does not answer (late or never): every frame would wait the full timeout. Reflex
        // steps aside for the session -- the frame slot wait and the CPU limiter pace as without it.
        if (++mReflexTimeouts >= kReflexMaxTimeouts)
        {
            mReflexGaveUp = true;
            mReflexBegun = false;   // (this frame's markers do not go out either)
            LL_WARNS("Vulkan") << "Frame generation: Reflex off for the rest of the session: the driver did "
                               << "not answer " << kReflexMaxTimeouts << " sleeps in a row ("
                               << kReflexSleepTimeoutNs / 1000000ull << " ms each)" << LL_ENDL;
        }
    }
#else
    (void)capFps;
#endif
}

void LLVKFrameGen::outOfBandMarker(uint64_t frame, OobMarker m)
{
#if LL_WINDOWS || LL_LINUX
    // (behind the same gate as the frame's own markers: a frame Reflex paced -- its id -- on a chain it
    // talks to; the chain changes only while the present thread is drained)
    if (!frame || !mReflexOn || !mLlMarker || mReflexSwapchain == VK_NULL_HANDLE) return;
    VkSetLatencyMarkerInfoNV info{VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV};
    info.presentID = frame;
    info.marker = (VkLatencyMarkerNV)(uint32_t)m;
    mLlMarker(mDirectDevice, mReflexSwapchain, &info);
#else
    (void)frame;
    (void)m;
#endif
}

void LLVKFrameGen::reflexMarker(Marker m)
{
    // Only a frame beginFrame slept for: SimStart right after the sleep; SimEnd (every frame, before
    // decideDirect) keeps the frame only when no decideDirect ran since that sleep (an older frame that
    // never reached its SimEnd lends this one nothing); PresentEnd ends it.
    bool send = false;
    if (m == SimStart)
        send = mReflexBegun;
    else if (m == SimEnd)
    {
        mReflexOpen = mReflexBegun && mReflexDirectFrame == mDirectFrame;
        mReflexBegun = false;
        send = mReflexOpen;
    }
    else
    {
        send = mReflexOpen;
        if (m == PresentEnd) mReflexOpen = false;
    }
#if LL_WINDOWS || LL_LINUX
    if (!send || !mReflexOn || mReflexSwapchain == VK_NULL_HANDLE) return;
    VkSetLatencyMarkerInfoNV info{VK_STRUCTURE_TYPE_SET_LATENCY_MARKER_INFO_NV};
    info.presentID = mReflexFrame;
    info.marker = (VkLatencyMarkerNV)(uint32_t)m;
    mLlMarker(mDirectDevice, mReflexSwapchain, &info);
#else
    (void)send;
#endif
}
