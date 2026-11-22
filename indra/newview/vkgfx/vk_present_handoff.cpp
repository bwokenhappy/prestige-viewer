#include "vk_present_handoff.h"

bool VkPresentHandoff::start(uint32_t slots, Run run)
{
    if (slots == 0 || slots > kMaxSlots || !run) return false;
    std::lock_guard<std::mutex> life(mLife);
    if (mThread.joinable()) return false;
    State& s = *mS;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (s.abandoned) return false;   // (it may still run its job on this state)
        s.queue.clear();
        s.inFlight.fill(false);
        s.slots = slots;
        s.stopping = false;
        s.busy = false;
        s.exited = false;
        s.run = std::move(run);
        s.alive = true;
        s.accepting = true;
    }
    mThread = std::thread([state = mS] { threadMain(*state); });
    return true;
}

bool VkPresentHandoff::running() const
{
    std::lock_guard<std::mutex> lock(mS->mutex);
    return mS->accepting;
}

bool VkPresentHandoff::abandoned() const
{
    std::lock_guard<std::mutex> lock(mS->mutex);
    return mS->abandoned;
}

bool VkPresentHandoff::alive() const
{
    std::lock_guard<std::mutex> lock(mS->mutex);
    return mS->alive;
}

std::chrono::milliseconds VkPresentHandoff::jobAge() const
{
    std::lock_guard<std::mutex> lock(mS->mutex);
    if (!mS->busy) return std::chrono::milliseconds(0);
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - mS->busySince);
}

VkPresentHandoff::SlotWait VkPresentHandoff::waitSlot(uint32_t slot, std::chrono::milliseconds bound)
{
    State& s = *mS;
    std::unique_lock<std::mutex> lock(s.mutex);
    if (slot >= s.slots || !s.accepting) return SlotWait::Gone;
    // (the bound passed: Busy and nothing else -- the slot, the job and the thread are left as they are)
    if (!s.done.wait_for(lock, bound, [&] { return !s.accepting || !s.inFlight[slot]; })) return SlotWait::Busy;
    return s.accepting ? SlotWait::Free : SlotWait::Gone;
}

bool VkPresentHandoff::push(uint32_t slot)
{
    State& s = *mS;
    {
        std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.accepting || slot >= s.slots || s.inFlight[slot]) return false;
        s.inFlight[slot] = true;
        s.queue.push_back(slot);
    }
    s.work.notify_one();
    return true;
}

bool VkPresentHandoff::drain(std::chrono::milliseconds bound)
{
    if (onThread()) return true;   // (a job waiting for itself)
    State& s = *mS;
    std::unique_lock<std::mutex> lock(s.mutex);
    return s.done.wait_for(lock, bound, [&] { return s.queue.empty() && !s.busy; });
}

bool VkPresentHandoff::stop(std::chrono::milliseconds bound)
{
    if (onThread()) return true;   // (the thread cannot join itself)
    std::lock_guard<std::mutex> life(mLife);
    State& s = *mS;
    std::unique_lock<std::mutex> lock(s.mutex);
    if (!mThread.joinable()) return !s.abandoned;
    s.accepting = false;
    s.stopping = true;
    s.work.notify_all();
    s.done.notify_all();   // (a waitSlot returns Gone now)
    // The job running finishes, the queued ones run as dropped and the thread ends -- within the bound, or
    // it is left behind inside its job (a driver call that does not return): never joined, never waited for
    // again, and no verdict on why.
    if (!s.done.wait_for(lock, bound, [&] { return s.exited; }))
    {
        s.abandoned = true;
        lock.unlock();
        mThread.detach();
        return false;
    }
    lock.unlock();
    mThread.join();
    lock.lock();
    s.alive = false;
    s.stopping = false;
    s.slots = 0;
    s.run = nullptr;
    return true;
}

void VkPresentHandoff::threadMain(State& s)
{
    s.threadId.store(std::this_thread::get_id());
    std::unique_lock<std::mutex> lock(s.mutex);
    for (;;)
    {
        s.work.wait(lock, [&] { return s.stopping || !s.queue.empty(); });
        if (s.queue.empty()) break;   // (stopping, nothing left)
        const uint32_t slot = s.queue.front();
        s.queue.pop_front();
        const bool dropped = s.stopping;
        s.busy = true;
        s.busySince = std::chrono::steady_clock::now();
        lock.unlock();
        s.run(slot, dropped);
        lock.lock();
        s.busy = false;
        s.inFlight[slot] = false;
        s.done.notify_all();
    }
    s.exited = true;
    s.done.notify_all();
    lock.unlock();
    s.threadId.store(std::thread::id());
}
