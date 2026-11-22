// vkgfx -- the thread that paces frame generation's presents. Per real frame the presenter queues
// the generated frames and the real one as presents; every present after the first waits on a
// timeline semaphore value (its gate), and this thread signals those values at each frame's due
// time so the frames reach the screen evenly spaced. A batch is paced from its GPU end time and
// the time since the previous batch ended (llvkframegenpolicy.h); with no usable history (the
// first batch, a hitch, a fence timeout, an unreadable clock) its gates open at once.
//
// Pure C++: every device call goes through Io (tests pass fakes), so this file includes no Vulkan
// or viewer header but the frame generation rules. Built and tested on the host by
// scripts/check/test_frame_pacer.cpp.
#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>

class VkFramePacer {
public:
    // The device-facing calls, injected (tests pass fakes). Called on the pacer's thread.
    struct Io {
        std::function<bool(uint64_t fenceToken, uint64_t timeoutNs)> waitFence;      // the batch's GPU work is done
        std::function<bool(uint32_t query, uint64_t* gpuTicks)> readTimestamp;       // when it ended
        std::function<bool(uint64_t* gpuTicks, int64_t* cpuNs)> calibrate;           // both clocks now
        std::function<int64_t()> nowNs;
        std::function<void(int64_t untilNs)> waitUntil;                              // sleep, then spin the last 2 ms;
                                                                                     // returns early once draining()
        std::function<void(uint64_t value)> signal;                                  // vkSignalSemaphore(gate, value)
        double nsPerGpuTick = 1.0;
    };
    // One real frame's presents: `gated` of them wait on values firstValue..firstValue+gated-1;
    // `generated` frames per real one set the spacing.
    struct Job { uint64_t fenceToken; uint32_t query; uint32_t gated; uint64_t firstValue; uint32_t generated; };
    struct Stats { uint64_t batches, paced, releasedAtOnce; int64_t worstLateNs; };

    VkFramePacer() = default;
    ~VkFramePacer() { stop(); }
    VkFramePacer(const VkFramePacer&) = delete;
    VkFramePacer& operator=(const VkFramePacer&) = delete;

    // push, drain, stop and nextValue: one thread at a time (#1517: the present thread pushes and takes
    // values while it runs a job; the main thread drains or stops only while that thread is drained).
    // drain() and stop() can take up to the fence timeout (1 s) when a job is in its fence wait.
    void start(Io io);            // spawns the thread (a no-op while it runs); forgets the history
    // After presentBatch: `gated` values firstValue..firstValue+gated-1. true: every one of them
    // will be signalled exactly once (queued for pacing; after stop(), signalled at once, in
    // order). false: the pacer has no way to signal (never started, or Io has no signal) and
    // signals nothing -- the caller must not gate that batch.
    bool push(const Job& j);
    void drain();                 // blocks until every pushed value is signalled (opens gates at once)
    void stop();                  // drain, then join (a no-op when not running)
    uint64_t nextValue(uint32_t gated);   // reserves `gated` consecutive timeline values, returns the first
    // Counted since the last takeStats(), which resets them. batches: jobs run; paced: jobs that
    // waited for due times; releasedAtOnce: jobs whose gates all opened at once; worstLateNs: the
    // latest a gate opened after its due time (0 if none).
    Stats takeStats();
    void runOne(const Job& j);    // the thread's body for one job (called directly by tests)
    bool draining() const { return mDrainers.load() > 0; }   // a drain or stop waits: release at once

private:
    void threadMain();
    void releaseAll(const Job& j, uint32_t from);   // signal the job's values from index `from` on

    Io mIo;
    std::thread mThread;

    // mMutex guards the queue, whether it takes jobs, and the counters below; it is never held
    // across an Io call. mWork wakes the thread; mDone wakes drain().
    std::mutex mMutex;
    std::condition_variable mWork, mDone;
    std::deque<Job> mQueue;
    bool mAccepting = false;                 // the thread runs and no stop has begun
    uint64_t mPushed = 0, mFinished = 0;     // jobs queued / run by the thread
    Stats mStats{0, 0, 0, 0};

    std::atomic<int> mDrainers{0};           // drain() / stop() calls in progress

    // Only the thread running runOne touches these.
    bool mHavePrev = false;
    int64_t mPrevDoneNs = 0;                 // the previous batch's end on the CPU clock

    uint64_t mNextValue = 1;                 // the pushing thread's; 0 means "no gate"
};
