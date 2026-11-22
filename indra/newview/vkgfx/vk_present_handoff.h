// vkgfx -- how the renderer hands its real frames to frame generation's present thread (#1517).
// The renderer renders each real frame into a slot (its own backbuffer and generator inputs) and
// pushes the slot; the present thread takes the slots in the order they were pushed and runs a job
// for each (generate, fetch the swapchain images, present). A slot is in flight from push() until its
// job has returned; before the renderer renders into a slot again it waits for it (waitSlot) -- its
// only backpressure: an in-flight flag per backbuffer and a condition variable.
//
// Every wait for the thread has a bound, and a bound that passes only says so (the frame is skipped, the
// rebuild deferred, the thread left behind at shutdown): nothing here judges whether the thread is stuck,
// reports it or gives it up. A job may sit inside a driver call for as long as the compositor holds a
// present; whether the GPU hung is the device's own waits' to say (vk_hang_watchdog.h).
//
// Pure C++ (no Vulkan, no viewer): built and tested on the host by scripts/check/test_present_handoff.cpp,
// under ThreadSanitizer too.
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

class VkPresentHandoff {
public:
    // The job for one pushed slot, on the present thread. dropped: stop() came before it ran -- it
    // must release whatever the slot holds without showing anything. Every pushed slot gets exactly
    // one call.
    using Run = std::function<void(uint32_t slot, bool dropped)>;
    static constexpr uint32_t kMaxSlots = 4;
    // What a wait for a slot found: the slot is free (render into it), still in flight when the bound
    // passed (skip the frame; the slot is left as it was), or there is none to have (no thread, no such slot,
    // stop() begun).
    enum class SlotWait { Free, Busy, Gone };
    // How long stop() waits for the thread to end unless told otherwise.
    static constexpr std::chrono::milliseconds kStopBound{2000};

    VkPresentHandoff() = default;
    ~VkPresentHandoff() { stop(); }
    VkPresentHandoff(const VkPresentHandoff&) = delete;
    VkPresentHandoff& operator=(const VkPresentHandoff&) = delete;

    // Every call below is safe from any thread; the renderer (the main thread) is the one that pushes.
    // Spawns the present thread with `slots` slots (1..kMaxSlots), all free. False: bad arguments, it
    // already runs, or a thread was left behind (abandoned(): for good).
    bool start(uint32_t slots, Run run);
    bool running() const;
    // Waits at most `bound` for `slot` to be out of flight.
    SlotWait waitSlot(uint32_t slot, std::chrono::milliseconds bound);
    // Puts `slot` in flight and queues its job. False (nothing queued): no thread runs, stop() has
    // begun, no such slot, or the slot is still in flight (the caller skipped waitSlot).
    bool push(uint32_t slot);
    // Waits at most `bound` (0: looks only) for no job to be queued or running. True: none is (also with no
    // thread, and on the present thread: a job never waits for itself). False: the bound passed -- a job
    // still runs or waits; the caller defers what needed the thread idle.
    bool drain(std::chrono::milliseconds bound);
    // Takes no more jobs and wakes every waitSlot (Gone); the job running finishes and the jobs still queued
    // run as dropped. True: the thread ended within `bound` and was joined (or none ran). False: it did not --
    // it is left behind (detached) inside its job, for good: it keeps this object's state alive and whatever
    // its job uses must never be destroyed; it ends on its own if its call ever returns (the queued jobs then
    // run as dropped). A no-op on the present thread (true).
    bool stop(std::chrono::milliseconds bound = kStopBound);
    // A stop() left the thread behind.
    bool abandoned() const;
    // A present thread exists: running, stopping, or left behind (then for good).
    bool alive() const;
    // How long the job running now has run (0: none runs). Read from the clock: it judges nothing.
    std::chrono::milliseconds jobAge() const;
    // The caller is the present thread.
    bool onThread() const { return std::this_thread::get_id() == mS->threadId.load(); }

private:
    // What the thread shares with the waiting threads. The thread holds it too, so a thread left behind
    // that returns later still finds it.
    struct State
    {
        mutable std::mutex mutex;           // guards everything below but the thread's id
        std::condition_variable work;       // a job was queued, or stop()
        std::condition_variable done;       // a job finished, the thread ended, or stop()
        std::deque<uint32_t> queue;
        std::array<bool, kMaxSlots> inFlight{};
        uint32_t slots = 0;
        bool accepting = false;             // the thread runs and no stop has begun
        bool stopping = false;
        bool busy = false;                  // a job runs
        std::chrono::steady_clock::time_point busySince{};   // ... since then
        bool alive = false;                 // a thread exists (start() .. joined; left behind: for good)
        bool exited = false;                // the thread has left its loop
        bool abandoned = false;             // stop() left it behind
        Run run;
        std::atomic<std::thread::id> threadId{};
    };
    static void threadMain(State& s);

    const std::shared_ptr<State> mS = std::make_shared<State>();

    std::mutex mLife;                   // start / stop: one at a time
    std::thread mThread;
};
