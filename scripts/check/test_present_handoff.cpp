// Tests for indra/newview/vkgfx/vk_present_handoff.{h,cpp}: how the renderer hands its real frames to
// frame generation's present thread (#1517). The renderer renders into a slot, pushes it, and before it
// renders into that slot again waits until the present thread is done with it; the present thread
// takes the slots in order. Every job here is a fake that records what it was given and, where a test
// says so, holds until the test lets it go.
// Built and run by scripts/check/checks/framegen_handoff.py (also under UBSan and TSan).
#include "vk_present_handoff.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(__linux__)
#include <csignal>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

namespace {

using namespace std::chrono_literals;

// What the present thread was given, and a hold the test opens.
struct Jobs
{
    std::mutex m;
    std::condition_variable cv;
    std::vector<uint32_t> slots;
    std::vector<bool> dropped;
    bool hold = false;         // jobs wait while set
    uint32_t holding = 0;      // jobs waiting in the hold now

    VkPresentHandoff::Run run()
    {
        return [this](uint32_t slot, bool drop)
        {
            std::unique_lock<std::mutex> lock(m);
            ++holding;
            cv.notify_all();
            cv.wait(lock, [this] { return !hold; });
            --holding;
            slots.push_back(slot);
            dropped.push_back(drop);
        };
    }
    void setHold(bool on)
    {
        std::lock_guard<std::mutex> lock(m);
        hold = on;
        cv.notify_all();
    }
    bool waitHolding(uint32_t n)   // a job reached the hold (bounded)
    {
        std::unique_lock<std::mutex> lock(m);
        return cv.wait_for(lock, 2s, [&] { return holding >= n; });
    }
    std::vector<uint32_t> ran()
    {
        std::lock_guard<std::mutex> lock(m);
        return slots;
    }
    std::vector<bool> drops()
    {
        std::lock_guard<std::mutex> lock(m);
        return dropped;
    }
};

// Runs f on another thread; done() says whether it returned.
struct Async
{
    std::atomic<bool> finished{false};
    std::atomic<bool> result{false};
    std::thread t;
    template<class F> explicit Async(F f) : t([this, f] { result = f(); finished = true; }) {}
    ~Async() { if (t.joinable()) t.join(); }
    bool doneWithin(std::chrono::milliseconds d)
    {
        const auto until = std::chrono::steady_clock::now() + d;
        while (!finished && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
        return finished;
    }
};

using SlotWait = VkPresentHandoff::SlotWait;

void testNotRunning()
{
    VkPresentHandoff h;
    CHECK(!h.running() && !h.alive() && !h.abandoned());
    CHECK(!h.push(0));          // nothing would run it
    CHECK(h.waitSlot(0, 20ms) == SlotWait::Gone);   // no thread: the renderer must not render as if one took it
    CHECK(h.drain(0ms));        // nothing to wait for
    CHECK(h.stop());            // a no-op
    CHECK(h.jobAge() == 0ms);
    Jobs j;
    CHECK(!h.start(0, j.run()));                                // no slots
    CHECK(!h.start(VkPresentHandoff::kMaxSlots + 1, j.run()));  // too many
    CHECK(!h.start(2, VkPresentHandoff::Run()));                // no job body
    CHECK(!h.running());
}

void testInOrder()
{
    VkPresentHandoff h;
    Jobs j;
    CHECK(h.start(2, j.run()));
    CHECK(h.running() && h.alive());
    CHECK(h.waitSlot(0, 2s) == SlotWait::Free);   // a slot never pushed is free at once
    CHECK(h.waitSlot(7, 2s) == SlotWait::Gone);   // no such slot
    j.setHold(true);
    CHECK(h.push(0));
    CHECK(h.push(1));
    CHECK(!h.push(0));          // still in flight: refused, never queued twice
    CHECK(!h.push(5));          // no such slot
    CHECK(j.waitHolding(1));
    j.setHold(false);
    CHECK(h.drain(2s));
    for (uint32_t k = 0; k < 6; ++k)
    {
        CHECK(h.waitSlot(k % 2, 2s) == SlotWait::Free);
        CHECK(h.push(k % 2));
    }
    CHECK(h.drain(2s));
    const std::vector<uint32_t> want{0, 1, 0, 1, 0, 1, 0, 1};
    CHECK(j.ran() == want);
    for (bool d : j.drops()) CHECK(!d);
    CHECK(h.stop());
    CHECK(!h.running() && !h.alive());
}

void testRendererWaitsUntilReleased()
{
    VkPresentHandoff h;
    Jobs j;
    CHECK(h.start(2, j.run()));
    j.setHold(true);
    CHECK(h.push(0));
    CHECK(j.waitHolding(1));
    {
        Async renderer([&] { return h.waitSlot(0, 5s) == SlotWait::Free; });
        CHECK(!renderer.doneWithin(50ms));   // the present thread still holds slot 0
        CHECK(h.waitSlot(1, 2s) == SlotWait::Free);   // the other slot is free
        j.setHold(false);
        CHECK(renderer.doneWithin(2s));
        CHECK(renderer.result);              // released: the renderer may use it
    }
    CHECK(h.stop());
}

void testDrainWaitsForQueueAndCurrentJob()
{
    VkPresentHandoff h;
    Jobs j;
    CHECK(h.start(2, j.run()));
    j.setHold(true);
    CHECK(h.push(0));
    CHECK(j.waitHolding(1));
    {
        // Nothing queued, one job running: drain waits for it.
        Async drainer([&] { return h.drain(5s); });
        CHECK(!drainer.doneWithin(50ms));
        j.setHold(false);
        CHECK(drainer.doneWithin(2s) && drainer.result);
    }
    CHECK(j.ran().size() == 1);
    j.setHold(true);
    CHECK(h.push(0));
    CHECK(h.push(1));
    CHECK(j.waitHolding(1));
    {
        Async drainer([&] { return h.drain(5s); });
        CHECK(!drainer.doneWithin(50ms));    // a job runs and one is queued
        j.setHold(false);
        CHECK(drainer.doneWithin(2s) && drainer.result);
    }
    CHECK(j.ran().size() == 3);              // both ran before drain returned
    CHECK(h.waitSlot(0, 0ms) == SlotWait::Free && h.waitSlot(1, 0ms) == SlotWait::Free);
    CHECK(h.stop());
}

void testStopWhileRendererWaits()
{
    VkPresentHandoff h;
    Jobs j;
    CHECK(h.start(2, j.run()));
    j.setHold(true);
    CHECK(h.push(0));
    CHECK(h.push(1));                        // queued behind the held job
    CHECK(j.waitHolding(1));
    Async renderer([&] { return h.waitSlot(1, 10s) == SlotWait::Gone; });
    CHECK(!renderer.doneWithin(30ms));
    {
        Async stopper([&] { return h.stop(10s); });
        CHECK(renderer.doneWithin(2s));      // stop() wakes the waiting renderer at once ...
        CHECK(renderer.result);              // ... with "no slot"
        CHECK(!h.push(0));                   // and takes no more jobs
        CHECK(!stopper.doneWithin(30ms));    // the running job is never cut short
        j.setHold(false);
        CHECK(stopper.doneWithin(2s) && stopper.result);   // it ended within the bound: joined
    }
    CHECK(!h.running() && !h.alive() && !h.abandoned());
    const std::vector<uint32_t> want{0, 1};
    CHECK(j.ran() == want);                  // every pushed job ran exactly once ...
    const std::vector<bool> drops = j.drops();
    CHECK(drops.size() == 2 && !drops[0] && drops[1]);   // ... the one still queued at stop() as dropped
    CHECK(h.waitSlot(0, 20ms) == SlotWait::Gone);
    CHECK(h.stop());                         // twice: a no-op
}

void testRestart()
{
    VkPresentHandoff h;
    Jobs j;
    CHECK(h.start(2, j.run()));
    CHECK(!h.start(2, j.run()));             // already running
    CHECK(h.push(1));
    CHECK(h.stop());
    CHECK(h.start(3, j.run()));              // a new run: every slot free again
    CHECK(h.waitSlot(2, 2s) == SlotWait::Free);
    CHECK(h.push(2));
    CHECK(h.drain(2s));
    const std::vector<uint32_t> want{1, 2};
    CHECK(j.ran() == want);
}

void testDrainFromTheJobItself()
{
    // A job that asks for a drain (a wait reached from the present thread) must not wait for itself.
    VkPresentHandoff h;
    std::atomic<bool> inside{false}, onThread{false}, drained{false};
    CHECK(h.start(1, [&](uint32_t, bool) { onThread = h.onThread(); drained = h.drain(10s); inside = true; }));
    CHECK(!h.onThread());
    CHECK(h.push(0));
    CHECK(h.drain(2s));
    CHECK(inside && onThread && drained);
    CHECK(h.stop());
}

void testBoundedSlotWait()
{
    // The renderer's wait for a slot ends after its bound (Busy: the frame is skipped, the main loop goes on
    // pumping events), never waits for the job, and leaves the slot as it was. Free when the job ends within
    // the bound; Gone with no thread, no such slot, or after stop().
    VkPresentHandoff h;
    Jobs j;
    CHECK(h.start(2, j.run()));
    CHECK(h.waitSlot(0, 20ms) == SlotWait::Free);
    j.setHold(true);
    CHECK(h.push(0));
    CHECK(j.waitHolding(1));
    {
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(h.waitSlot(0, 30ms) == SlotWait::Busy);
        const auto waited = std::chrono::steady_clock::now() - t0;
        CHECK(waited >= 25ms && waited < 1500ms);
    }
    {
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(h.waitSlot(0, 0ms) == SlotWait::Busy);     // (no bound at all: a look)
        CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
    }
    CHECK(!h.push(0));                                   // (still in flight: untouched)
    CHECK(h.waitSlot(1, 30ms) == SlotWait::Free);
    {
        std::thread releaser([&] { std::this_thread::sleep_for(20ms); j.setHold(false); });
        CHECK(h.waitSlot(0, 2000ms) == SlotWait::Free);   // the job ended within it
        releaser.join();
    }
    j.setHold(true);
    CHECK(h.push(0));
    CHECK(j.waitHolding(1));
    {
        Async stopper([&] { return h.stop(10s); });
        const auto until = std::chrono::steady_clock::now() + 2s;
        while (h.running() && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
        CHECK(h.waitSlot(0, 30ms) == SlotWait::Gone);   // (stopping)
        j.setHold(false);
        CHECK(stopper.doneWithin(2s) && stopper.result);
    }
    CHECK(j.ran().size() == 2);
}

void testABoundedDrainDefers()
{
    // (fix 4) A drain has a bound: it passes, false, and nothing else happens -- the job goes on, the caller
    // defers what needed the thread idle (a rebuild, a feature, shutdown). With bound 0 it only looks.
    VkPresentHandoff h;
    Jobs j;
    CHECK(h.start(2, j.run()));
    CHECK(h.drain(0ms));                                 // (idle: drained)
    j.setHold(true);
    CHECK(h.push(0));
    CHECK(h.push(1));
    CHECK(j.waitHolding(1));
    {
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!h.drain(40ms));
        const auto waited = std::chrono::steady_clock::now() - t0;
        CHECK(waited >= 35ms && waited < 1500ms);
    }
    {
        const auto t0 = std::chrono::steady_clock::now();
        CHECK(!h.drain(0ms));
        CHECK(std::chrono::steady_clock::now() - t0 < 1000ms);
    }
    CHECK(h.running() && h.alive() && !h.abandoned() && j.ran().empty());
    j.setHold(false);
    CHECK(h.drain(2s));                                  // (the job ended, the queued one ran: drained)
    CHECK(j.ran().size() == 2);
    CHECK(h.stop());
}

void testATimeoutNeverJudges()
{
    // (fix 4) However many bounds pass on a job that does not end -- a present the compositor holds, a driver
    // call that takes its time -- the handoff does nothing about it: no report, no give-up, the thread still
    // runs and takes work, and everything goes on when the job returns.
    VkPresentHandoff h;
    Jobs j;
    CHECK(h.start(3, j.run()));
    CHECK(h.jobAge() == 0ms);
    j.setHold(true);
    CHECK(h.push(0));
    CHECK(j.waitHolding(1));
    for (int k = 0; k < 100; ++k)
    {
        CHECK(h.waitSlot(0, 1ms) == SlotWait::Busy);
        CHECK(!h.drain(1ms));
    }
    CHECK(h.running() && h.alive() && !h.abandoned());
    CHECK(h.jobAge() >= 150ms && h.jobAge() < 60s);      // (the job has run that long: read, never judged)
    CHECK(h.waitSlot(1, 0ms) == SlotWait::Free && h.push(1));   // (work is still taken)
    CHECK(h.waitSlot(2, 0ms) == SlotWait::Free && h.push(2));
    j.setHold(false);
    CHECK(h.waitSlot(0, 2s) == SlotWait::Free);
    CHECK(h.drain(2s));
    const std::vector<uint32_t> want{0, 1, 2};
    CHECK(j.ran() == want);
    for (bool d : j.drops()) CHECK(!d);
    CHECK(h.jobAge() == 0ms);
    CHECK(h.stop());
}

void testStopLeavesAHeldThreadBehind()
{
    // (fix 4) stop() waits a bound for the thread to end. A thread still inside its job then (a driver call
    // that does not return: a present the compositor holds, at quit; any call, on a lost device) is left
    // behind, for good: stop() returns false, no wait waits for it again, a drain says "not idle" (whatever
    // the job uses must not be destroyed), and no new thread starts. If its call returns after all, the jobs
    // queued behind it run as dropped (their slots are given back) and it ends on its own.
    Jobs j;
    {
        VkPresentHandoff h;
        CHECK(h.start(2, j.run()));
        j.setHold(true);
        CHECK(h.push(0));
        CHECK(h.push(1));                                // (queued behind it)
        CHECK(j.waitHolding(1));
        {
            const auto t0 = std::chrono::steady_clock::now();
            CHECK(!h.stop(50ms));
            const auto waited = std::chrono::steady_clock::now() - t0;
            CHECK(waited >= 45ms && waited < 1500ms);
        }
        CHECK(h.abandoned() && h.alive() && !h.running());
        CHECK(h.waitSlot(0, 20ms) == SlotWait::Gone && !h.push(0));
        CHECK(!h.drain(0ms));                            // (still inside its job)
        CHECK(!h.stop(50ms));                            // (again: nothing to wait for, still left behind)
        CHECK(!h.start(2, j.run()));                     // (for good)
        CHECK(j.ran().empty());
    }                                                    // (the handoff goes; the thread keeps its own state)
    j.setHold(false);                                    // its call returns at last
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (j.ran().size() < 2 && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(50ms);                   // (the thread leaves its loop before `j` goes)
    const std::vector<uint32_t> want{0, 1};
    CHECK(j.ran() == want);
    const std::vector<bool> drops = j.drops();
    CHECK(drops.size() == 2 && !drops[0] && drops[1]);
}

#if defined(__linux__)
// The child of testAPausedProcessOnlyTimesOut: a renderer polling a slot (5 ms bounds) whose job is about 200 ms
// of work (200 steps of 1 ms: a pause stops it mid-way, so it is still running when the process resumes). It
// tells the parent when it is about to wait ('w' on stdout); the parent then stops the whole process for 1 s.
// Exit 0: the waits only timed out (Busy) until the slot came back free, and the thread was never taken for
// stuck -- it still ran, was not left behind, and stop() joined it.
int pausedChild()
{
    VkPresentHandoff h;
    const auto work = [](uint32_t, bool)
    {
        for (int k = 0; k < 200; ++k) std::this_thread::sleep_for(1ms);
    };
    if (!h.start(1, work) || !h.push(0)) return 2;
    if (write(STDOUT_FILENO, "w", 1) != 1) return 2;
    uint32_t busy = 0;
    SlotWait w;
    while ((w = h.waitSlot(0, 5ms)) == SlotWait::Busy) ++busy;
    const bool fine = w == SlotWait::Free && h.running() && !h.abandoned();
    const bool joined = h.stop(2000ms);
    std::printf("busy=%u free=%d fine=%d joined=%d\n", busy, w == SlotWait::Free ? 1 : 0, fine ? 1 : 0, joined ? 1 : 0);
    std::fflush(stdout);
    return fine && joined && busy > 0 ? 0 : 1;
}
#endif

void testAPausedProcessOnlyTimesOut()
{
    // A debugger break or a system sleep stops every thread of the viewer at once. The renderer waiting for the
    // present thread then wakes into a bound that passed, though the job made no progress only because nothing
    // ran. A bound that passes is only a timeout (the frame skipped): the job runs on, and the wait ends when
    // the job does.
#if defined(__linux__)
    int out[2];
    if (pipe(out) != 0) { CHECK(!"pipe"); return; }
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, out[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&fa, out[0]);
    char self[] = "/proc/self/exe", mode[] = "--paused-child";
    char* argv[] = {self, mode, nullptr};
    pid_t child = 0;
    const int spawned = posix_spawn(&child, self, &fa, nullptr, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(out[1]);
    CHECK(spawned == 0);
    if (spawned != 0) { close(out[0]); return; }
    char c = 0;
    CHECK(read(out[0], &c, 1) == 1 && c == 'w');
    std::this_thread::sleep_for(50ms);             // (the child is polling the slot, the job still running)
    kill(child, SIGSTOP);
    std::this_thread::sleep_for(1000ms);           // a long pause
    kill(child, SIGCONT);
    std::string said;
    char buf[128];
    for (ssize_t n; (n = read(out[0], buf, sizeof(buf))) > 0;) said.append(buf, size_t(n));
    close(out[0]);
    int status = 0;
    CHECK(waitpid(child, &status, 0) == child);
    if (!(WIFEXITED(status) && WEXITSTATUS(status) == 0)) std::printf("  the paused child said: %s", said.c_str());
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
#endif
}

} // namespace

int main(int argc, char** argv)
{
#if defined(__linux__)
    if (argc > 1 && std::strcmp(argv[1], "--paused-child") == 0) return pausedChild();
#else
    (void)argc;
    (void)argv;
#endif
    testNotRunning();
    testInOrder();
    testRendererWaitsUntilReleased();
    testDrainWaitsForQueueAndCurrentJob();
    testStopWhileRendererWaits();
    testRestart();
    testDrainFromTheJobItself();
    testBoundedSlotWait();
    testABoundedDrainDefers();
    testATimeoutNeverJudges();
    testStopLeavesAHeldThreadBehind();
    testAPausedProcessOnlyTimesOut();
    if (failures) std::printf("%d failure(s)\n", failures);
    return failures ? 1 : 0;
}
