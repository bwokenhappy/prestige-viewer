// vkgfx -- the thread that paces frame generation's presents (vk_frame_pacer.h).
#include "vk_frame_pacer.h"

#include "llvkframegenpolicy.h"

#include <utility>

namespace {
constexpr uint64_t kFenceTimeoutNs = 1'000'000'000;   // a batch not done within 1 s is not paced
}

void VkFramePacer::start(Io io)
{
    if (mThread.joinable()) return;
    mIo = std::move(io);
    mHavePrev = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mAccepting = true;
    }
    mThread = std::thread(&VkFramePacer::threadMain, this);
}

bool VkFramePacer::push(const Job& j)
{
    if (!mIo.signal) return false;   // never started: nothing could open the gates
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mAccepting)
        {
            mQueue.push_back(j);
            ++mPushed;
            mWork.notify_one();
            return true;
        }
    }
    // No thread to pace it (stopped): a gate left closed would hold its present forever.
    releaseAll(j, 0);
    std::lock_guard<std::mutex> lock(mMutex);
    ++mStats.batches;
    ++mStats.releasedAtOnce;
    return true;
}

void VkFramePacer::drain()
{
    ++mDrainers;    // the thread stops waiting for due times
    {
        std::unique_lock<std::mutex> lock(mMutex);
        const uint64_t target = mPushed;
        mDone.wait(lock, [&] { return mFinished >= target; });
    }
    --mDrainers;
}

void VkFramePacer::stop()
{
    if (!mThread.joinable()) return;
    ++mDrainers;    // what is still queued is released at once
    {
        std::lock_guard<std::mutex> lock(mMutex);
        mAccepting = false;
        mWork.notify_one();
    }
    mThread.join(); // the thread runs out the queue first
    --mDrainers;
}

uint64_t VkFramePacer::nextValue(uint32_t gated)
{
    const uint64_t first = mNextValue;
    mNextValue += gated;
    return first;
}

VkFramePacer::Stats VkFramePacer::takeStats()
{
    std::lock_guard<std::mutex> lock(mMutex);
    const Stats s = mStats;
    mStats = Stats{0, 0, 0, 0};
    return s;
}

void VkFramePacer::threadMain()
{
    for (;;)
    {
        Job j{};
        {
            std::unique_lock<std::mutex> lock(mMutex);
            mWork.wait(lock, [&] { return !mQueue.empty() || !mAccepting; });
            if (mQueue.empty()) return;   // stopping, and nothing left to release
            j = mQueue.front();
            mQueue.pop_front();
        }
        runOne(j);
        {
            std::lock_guard<std::mutex> lock(mMutex);
            ++mFinished;
        }
        mDone.notify_all();
    }
}

void VkFramePacer::releaseAll(const Job& j, uint32_t from)
{
    for (uint32_t k = from; k < j.gated; ++k)
        mIo.signal(j.firstValue + k);
}

void VkFramePacer::runOne(const Job& j)
{
    // Released at once, and the next batch is treated like the first.
    auto releaseForget = [&] {
        mHavePrev = false;
        releaseAll(j, 0);
        std::lock_guard<std::mutex> lock(mMutex);
        ++mStats.batches;
        ++mStats.releasedAtOnce;
    };

    // A drain skips the batch's timing, so the next one has no previous end to measure from.
    if (draining() || !mIo.waitFence(j.fenceToken, kFenceTimeoutNs)) return releaseForget();
    uint64_t doneTicks = 0, refTicks = 0;
    int64_t refCpuNs = 0;
    if (!mIo.readTimestamp(j.query, &doneTicks) || !mIo.calibrate(&refTicks, &refCpuNs)) return releaseForget();
    const int64_t doneNs = llfg::gpuToCpuNs(doneTicks, refTicks, refCpuNs, mIo.nsPerGpuTick);

    if (!mHavePrev)
    {
        releaseForget();
        mHavePrev = true;
        mPrevDoneNs = doneNs;
        return;
    }
    const int64_t frameToFrameNs = doneNs - mPrevDoneNs;
    if (!llfg::paceable(frameToFrameNs)) return releaseForget();
    mPrevDoneNs = doneNs;

    const int64_t spacingNs = llfg::presentSpacingNs(frameToFrameNs, j.generated);
    int64_t worstLateNs = 0;
    bool waited = false;
    for (uint32_t k = 0; k < j.gated; ++k)
    {
        // Present index k + 1: the batch's first present (index 0) has no gate.
        const int64_t dueNs = llfg::dueNs(doneNs, spacingNs, k + 1);
        if (draining() || dueNs - mIo.nowNs() > llfg::kMaxPacedFrameNs)
        {
            releaseAll(j, k);   // a drain, or clocks that disagree: never hold a frame that long
            break;
        }
        mIo.waitUntil(dueNs);
        waited = true;
        const int64_t lateNs = mIo.nowNs() - dueNs;
        if (lateNs > worstLateNs) worstLateNs = lateNs;
        mIo.signal(j.firstValue + k);
    }

    std::lock_guard<std::mutex> lock(mMutex);
    ++mStats.batches;
    ++(waited ? mStats.paced : mStats.releasedAtOnce);
    if (worstLateNs > mStats.worstLateNs) mStats.worstLateNs = worstLateNs;
}
