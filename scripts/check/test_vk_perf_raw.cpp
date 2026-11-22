// Actual raw capture storage: original-frame joins, missing samples, boundedness and
// operator-new accounting for both CPU collection and GPU interning/late harvest.
#include "llvkperfraw.h"
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <new>
#include <sstream>
#include <thread>

static std::atomic<bool> track{false};
static std::atomic<unsigned> allocations{0};
void* operator new(std::size_t bytes)
{
    if (track.load()) allocations.fetch_add(1);
    if (void* p = std::malloc(bytes ? bytes : 1)) return p;
    throw std::bad_alloc();
}
void* operator new[](std::size_t bytes) { return ::operator new(bytes); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }

int main()
{
    using namespace LLVKPerfRaw;
    Arena arena(8);
    arena.firstFrame = 100;
    Publication publication;
    publication.arena.store(&arena);
    track.store(true);
    for (uint64_t f = 100; f < 108; ++f)
    {
        Row* r = arena.cpuFrame(f, 1.0 + (f - 100) * 0.01, 10.f);
        assert(r);
        r->cpu = 8.f; r->wait = 1.f; r->present = 0.5f; r->sleep = 0.5f;
        arena.cpuSection(*r, 0, "vk_frame", 8.f);
        if (f % 2 == 0) arena.cpuSection(*r, 1, "vk_paceWait", 1.f);
        if (f == 100) arena.cpuSection(*r, 511, "zero_section", 0.f);
    }
    // Delayed values belong to 100 even when 107 is already closed on the CPU.
    arena.gpuRecorded(100);
    arena.gpuSample(100, "g_join_gi", 0.0, false);
    arena.gpuSample(100, "g_defer", 1.25, false);
    arena.gpuSample(100, "g_defer", 2.0, false);
    arena.gpuSample(100, "q_async_busy", 4.0, true);
    arena.gpuSample(100, "g_defer", 9.0, true); // distinct async column
    arena.gpuDone(100, 5.0, true);
    arena.gpuRecorded(101); arena.gpuDropped(101);
    // 102: no timestamp recording at all; 103: still pending at end.
    arena.gpuRecorded(103);
    arena.gpuRecorded(104); arena.gpuDone(104, 3.0, false);
    // Multiple recordings in one CPU frame remain explicitly accounted for.
    arena.gpuRecorded(105); arena.gpuDone(105, 3.0, true);
    arena.gpuRecorded(105); arena.gpuDone(105, 4.0, true);
    assert(arena.rows[105 - 100].gpuTotal == 7.0f);
    arena.gpuSample(99, "old", 1., false);
    arena.gpuSample(108, "future", 1., false);
    assert(!arena.cpuFrame(108, 2., 10.f));
    assert(arena.rowsUsed == 8 && arena.overflowFrames == 1);
    arena.lastFrame.store(107);
    arena.gpuSample(109, "outside_window", 1., false);
    arena.gpuSample(107, "invalid", std::numeric_limits<double>::infinity(), false);
    assert(arena.invalidValues == 1);
    {
        Publication::Read read(publication);
        assert(read.value == &arena && publication.readers.load() == 1);
        publication.arena.exchange(nullptr);
        // Detach/cancel cannot reclaim until this reader has left.
        assert(publication.readers.load() != 0);
    }
    assert(publication.readers.load() == 0);
    {
        Publication::Read read(publication);
        assert(!read.value);
    }
    track.store(false);
    assert(allocations.load() == 0);
    assert(finalState(arena.rows[0]) == GpuState::Complete);
    assert(finalState(arena.rows[1]) == GpuState::Dropped);
    assert(finalState(arena.rows[2]) == GpuState::Absent);
    assert(finalState(arena.rows[3]) == GpuState::Pending);
    assert(finalState(arena.rows[4]) == GpuState::Partial);
    assert(arena.gpuColumns == 4);
    assert(std::isnan(arena.rows[1].gpuTotal));

    // Export the fixture for the Python check's real CSV reader.
    writeCsv(std::cout, arena);

    Arena full(1);
    full.firstFrame = 1;
    track.store(true);
    for (unsigned i = 0; i < kGpuColumns + 10; ++i)
    {
        char name[64]; std::snprintf(name, sizeof(name), "mark_%u", i);
        full.gpuSample(1, name, 1., false);
    }
    track.store(false);
    assert(full.gpuColumns == kGpuColumns && full.droppedColumns == 10);
    assert(allocations.load() == 0);

    // Main CPU and GPU owner write disjoint fields; detach publication is safe while
    // the harvest reader is in flight (also used in the optional TSan invocation).
    Arena concurrent(1000);
    concurrent.firstFrame = 1;
    publication.arena.store(&concurrent);
    std::atomic<bool> entered{false}, release{false};
    std::thread harvest([&]
    {
        Publication::Read read(publication);
        entered.store(true);
        while (!release.load()) std::this_thread::yield();
        for (uint64_t f = 1; f <= 1000; ++f)
        {
            read.value->gpuRecorded(f);
            read.value->gpuSample(f, "g_join_gi", 0.125, false);
            read.value->gpuDone(f, 0.25, true);
        }
    });
    while (!entered.load()) std::this_thread::yield();
    assert(publication.arena.exchange(nullptr) == &concurrent);
    assert(publication.readers.load() == 1);
    release.store(true);
    for (uint64_t f = 1; f <= 1000; ++f)
    {
        Row* r = concurrent.cpuFrame(f, f, 1.f);
        concurrent.cpuSection(*r, 7, "cpu", 0.5f);
    }
    harvest.join();
    assert(publication.readers.load() == 0);
    assert(concurrent.rows[999].gpuTotal == 0.25f && concurrent.rows[999].sections[7] == 0.5f);
}
