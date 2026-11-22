// Tests for indra/newview/vkgfx/vk_frame_pacer.{h,cpp}: the thread that opens the gates of the
// generated frames' presents at their due times. Every device call is a fake: the GPU clock is
// the CPU clock (1 tick = 1 ns), a batch's done time is what its timestamp query reads, and
// waiting moves the fake clock to the wait's target.
// Built and run by scripts/check/checks/framegen_pacer.py.
#include "vk_frame_pacer.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

namespace {

using Values = std::vector<uint64_t>;
using Times = std::vector<int64_t>;

struct Fake
{
    std::vector<uint64_t> dones;        // query -> the batch's end, in GPU ticks
    int64_t now = 0;
    int64_t nowLag = 0;                 // nowNs() reads this far behind the calibrated clocks (they disagree)
    bool fenceOk = true;
    bool blockUntilDraining = false;    // waitUntil holds until the pacer drains (the real one polls draining())
    std::atomic<bool> inWait{false};
    const VkFramePacer* pacer = nullptr;
    Values signals;
    Times waits;

    VkFramePacer::Io io()
    {
        VkFramePacer::Io io;
        io.waitFence = [this](uint64_t, uint64_t) { return fenceOk; };
        io.readTimestamp = [this](uint32_t query, uint64_t* ticks) {
            if (query >= dones.size()) return false;
            *ticks = dones[query];
            now = (int64_t)dones[query];
            return true;
        };
        io.calibrate = [this](uint64_t* gpu, int64_t* cpu) { *gpu = (uint64_t)now; *cpu = now; return true; };
        io.nowNs = [this] { return now - nowLag; };
        io.waitUntil = [this](int64_t until) {
            waits.push_back(until);
            if (blockUntilDraining)
            {
                inWait = true;
                while (!pacer->draining())
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            if (until > now) now = until;
        };
        io.signal = [this](uint64_t v) { signals.push_back(v); };
        io.nsPerGpuTick = 1.0;
        return io;
    }

    // A batch that ended at `done`, run straight through runOne.
    void run(VkFramePacer& p, uint64_t done, uint32_t gated, uint64_t firstValue, uint32_t generated)
    {
        dones.push_back(done);
        p.runOne(VkFramePacer::Job{(uint64_t)dones.size(), (uint32_t)(dones.size() - 1), gated, firstValue, generated});
    }
};

} // namespace

int main()
{
    // 4x: three gated presents per batch (two generated frames after the first, then the real one)
    {
        Fake f;
        VkFramePacer p;
        p.start(f.io());

        // first batch: no history -> released at once, nothing waited
        f.run(p, 1'000'000, 3, 1, 3);
        CHECK((f.signals == Values{1, 2, 3}));
        CHECK(f.waits.empty());

        // second batch 16 ms later: paced at 4 ms steps after its end
        f.run(p, 17'000'000, 3, 4, 3);
        CHECK((f.signals == Values{1, 2, 3, 4, 5, 6}));
        CHECK((f.waits == Times{21'000'000, 25'000'000, 29'000'000}));
        const VkFramePacer::Stats s = p.takeStats();
        CHECK(s.batches == 2);
        CHECK(s.paced == 1);
        CHECK(s.releasedAtOnce == 1);
        CHECK(s.worstLateNs == 0);                      // the fake wakes exactly on time
        const VkFramePacer::Stats empty = p.takeStats();   // taking them resets them
        CHECK(empty.batches == 0 && empty.paced == 0 && empty.releasedAtOnce == 0 && empty.worstLateNs == 0);

        // a 250 ms hitch: released at once, and the batch after it has no history
        f.run(p, 267'000'000, 3, 7, 3);
        CHECK(f.waits.size() == 3);
        f.run(p, 283'000'000, 3, 10, 3);
        CHECK(f.waits.size() == 3);
        CHECK((f.signals == Values{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12}));

        // ... but the one after that is paced again
        f.run(p, 299'000'000, 3, 13, 3);
        CHECK((f.waits == Times{21'000'000, 25'000'000, 29'000'000, 303'000'000, 307'000'000, 311'000'000}));

        // fence timeout: released at once, and history is forgotten
        f.fenceOk = false;
        f.run(p, 315'000'000, 3, 16, 3);
        CHECK(f.waits.size() == 6);
        CHECK((f.signals == Values{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18}));
        f.fenceOk = true;
        f.run(p, 331'000'000, 3, 19, 3);
        CHECK(f.waits.size() == 6);
        CHECK(f.signals.size() == 21 && f.signals.back() == 21);
        p.stop();
    }

    // 2x: one gated present (the real frame) at +8 ms
    {
        Fake f;
        VkFramePacer p;
        p.start(f.io());
        f.run(p, 1'000'000, 1, 1, 1);
        f.run(p, 17'000'000, 1, 2, 1);
        CHECK((f.waits == Times{25'000'000}));
        CHECK((f.signals == Values{1, 2}));
    }

    // a due time more than kMaxPacedFrameNs ahead of now (the clocks disagree): the batch is
    // paceable (16 ms after the last) but its gates open at once; its end is still remembered, so
    // the next batch is paced from it
    {
        Fake f;
        VkFramePacer p;
        p.start(f.io());
        f.run(p, 1'000'000'000, 3, 1, 3);
        f.nowLag = 150'000'000;                         // nowNs() reads 150 ms behind the batch's end
        f.run(p, 1'016'000'000, 3, 4, 3);
        CHECK(f.waits.empty());
        CHECK((f.signals == Values{1, 2, 3, 4, 5, 6}));
        VkFramePacer::Stats s = p.takeStats();
        CHECK(s.batches == 2 && s.paced == 0 && s.releasedAtOnce == 2);
        f.nowLag = 0;
        f.run(p, 1'032'000'000, 3, 7, 3);               // history kept: 16 ms -> 4 ms steps
        CHECK((f.waits == Times{1'036'000'000, 1'040'000'000, 1'044'000'000}));
        CHECK((f.signals == Values{1, 2, 3, 4, 5, 6, 7, 8, 9}));
        s = p.takeStats();
        CHECK(s.batches == 1 && s.paced == 1);
    }

    // a timestamp or calibration that cannot be read: released at once
    {
        Fake f;
        VkFramePacer p;
        p.start(f.io());
        f.run(p, 1'000'000, 3, 1, 3);
        p.runOne(VkFramePacer::Job{99, 99, 3, 4, 3});   // no such query
        CHECK((f.signals == Values{1, 2, 3, 4, 5, 6}));
        f.run(p, 33'000'000, 3, 7, 3);                  // no history after the failed read
        CHECK(f.waits.empty());
        CHECK(f.signals.size() == 9);
    }

    // lateness: the wait wakes 300 us after its target
    {
        Fake f;
        VkFramePacer::Io io = f.io();
        io.waitUntil = [&f](int64_t until) { f.waits.push_back(until); f.now = until + 300'000; };
        VkFramePacer p;
        p.start(io);
        f.run(p, 1'000'000, 1, 1, 1);
        f.run(p, 17'000'000, 1, 2, 1);
        CHECK(p.takeStats().worstLateNs == 300'000);
    }

    // drain() with two queued jobs on the real thread: the second holds in its wait until the
    // pacer drains; drain returns with every value signalled, in order
    {
        Fake f;
        f.dones = {1'000'000, 17'000'000};
        f.blockUntilDraining = true;
        VkFramePacer p;
        f.pacer = &p;
        p.start(f.io());
        const uint64_t first = p.nextValue(3);
        const uint64_t second = p.nextValue(3);
        CHECK(p.push(VkFramePacer::Job{1, 0, 3, first, 3}));
        CHECK(p.push(VkFramePacer::Job{2, 1, 3, second, 3}));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while (!f.inWait && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::microseconds(100));
        CHECK(f.inWait);                                // the second batch is held in its wait
        CHECK(!p.draining());
        p.drain();
        CHECK(!p.draining());
        CHECK((f.signals == Values{1, 2, 3, 4, 5, 6}));
        CHECK(f.waits.size() == 1);                     // the rest of the batch did not wait
        p.stop();
        p.stop();                                       // a second stop is a no-op
    }

    // stop() with jobs still queued signals them all before the thread ends
    {
        Fake f;
        f.dones = {1'000'000, 17'000'000, 33'000'000};
        VkFramePacer p;
        p.start(f.io());
        CHECK(p.push(VkFramePacer::Job{1, 0, 2, 1, 2}));
        CHECK(p.push(VkFramePacer::Job{2, 1, 2, 3, 2}));
        CHECK(p.push(VkFramePacer::Job{3, 2, 2, 5, 2}));
        p.stop();
        CHECK((f.signals == Values{1, 2, 3, 4, 5, 6}));
    }

    // a pacer that was never started: push cannot release (the caller must not gate that batch),
    // and stop() and the destructor do nothing
    {
        VkFramePacer p;
        CHECK(!p.push(VkFramePacer::Job{1, 0, 3, 1, 3}));
        CHECK(p.takeStats().batches == 0);
        p.stop();
        p.stop();
    }

    // after stop(): push signals the batch's values at once, in order
    {
        Fake f;
        f.dones = {1'000'000};
        VkFramePacer p;
        p.start(f.io());
        CHECK(p.push(VkFramePacer::Job{1, 0, 1, 1, 1}));
        p.stop();
        CHECK(p.push(VkFramePacer::Job{2, 0, 3, 2, 3}));
        CHECK((f.signals == Values{1, 2, 3, 4}));
        CHECK(f.waits.empty());
    }

    // nextValue: consecutive timeline values, starting at 1 (0 is "no gate")
    {
        VkFramePacer p;
        CHECK(p.nextValue(3) == 1);
        CHECK(p.nextValue(1) == 4);
        CHECK(p.nextValue(2) == 5);
    }

    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("frame pacer: all tests pass\n");
    return 0;
}
