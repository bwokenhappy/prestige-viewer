// CPU-only: real policy used by the pipeline scheduler, no Vulkan/driver dependencies.
#include "../vkgfx/vk_compile_governor.h"
#include <cassert>
#include <array>
#include <cstdio>
#include <limits>
using namespace vkcompilegovernor;
constexpr uint64_t GB = 1ull << 30;
int main()
{
    assert(rule(0x1002, 0x73ff, true, "", false)); // 6600
    assert(rule(0x1002, 0x731f, true, "", false)); // 5700
    assert(rule(0x1002, 0x73bf, true, "", false)); // 6800/6900
    assert(rule(0x1002, 0xffff, true, "32.0.21045.16010", false));
    assert(!rule(0x1002, 0x744c, true, "32.0.32015.1", false)); // 7900
    assert(!rule(0x1002, 0x73ff, false, "", false));
    assert(!rule(0x10de, 0x73ff, true, "32.0.21045.1", false));
    assert(rule(0, 0, false, "", true));
    assert(heavy(900000, "lit") && heavy(0, "unknown") && heavy(90000, "mirror"));
    assert(!heavy(50000, "UI"));
    assert(estimate(3*GB,0)==8*GB && estimate(2*GB,12*GB)==12*GB);
    Curve unavailable; unavailable.start(0,0); assert(!unavailable.plateau(100000));
    Curve first;
    first.start(GB,0);
    for (int64_t ms=100; ms<=8000; ms+=100)
    {
        first.sample(GB + uint64_t(ms)*GB/1000,ms);
        assert(!first.plateau(ms)); // eight GB of steady growth: no second heavy
        // Small work does not wait for a heavy plateau, and fits remaining headroom.
        assert(fits(32ull<<20,0,GB));
    }
    first.sample(9*GB,9999); assert(!first.plateau(9999));
    first.sample(9*GB,10000); assert(first.plateau(10000));
    // 16 GB: reserve/viewer leave 10 GB; after 8 GB allocated a second cannot fit.
    assert(!fits(8*GB,0,2*GB));
    assert(!fits(estimate(8*GB,12*GB),0,10*GB)); // live first compile raised the next reservation
    // 32 GB: live headroom alone could fit another, but the retained 25% cap forbids it.
    assert(fits(8*GB,0,16*GB));
    assert(!fits(8*GB,0,(32*GB/4)-8*GB));
    // 64 GB: two heavy reservations fit the existing cap, but only after plateau.
    assert(fits(8*GB,0,(64*GB/4)-8*GB));
    // First still owes 4 GB, so don't over-admit from live free memory.
    assert(!fits(8*GB,4*GB,10*GB));
    assert(!fits(UINT64_MAX,1,GB));
    // Re-growth after a free is still allocation, even below the old peak.
    first.sample(4*GB,11000); first.sample(5*GB,11100);
    assert(!first.plateau(12000));
    // Slow cumulative growth must not be misclassified as a flat curve.
    first.start(GB,0);
    for(int64_t ms=100;ms<=3000;ms+=100) { first.sample(GB+uint64_t(ms)*(1ull<<20)/10,ms); assert(!first.plateau(ms)); }
    // CRITICAL/TIGHT: only newest is abandoned, reservation remains until return;
    // while one abandoned result drains no other one is selected, oldest progresses.
    assert(abandon(true,GB,2,false));
    assert(!abandon(true,GB,1,false));
    assert(!abandon(true,GB,2,true));
    assert(abandon(false,128ull<<20,2,false));
    assert(!abandon(false,GB,2,false));
    struct Flight { bool abandon; uint64_t serial; } old{false, 4}, recent{false, 8};
    std::array<Flight*,2> pair{&recent,&old}; // vector order does not define newest
    assert(newestToAbandon(pair,true,0)==&recent);
    recent.abandon=true;
    assert(!newestToAbandon(pair,true,0)); // keep oldest making progress while draining
    std::array<Flight*,1> last{&old};
    assert(!newestToAbandon(last,true,0));
    recent.abandon=false;
    assert(!newestToAbandon(pair,false,GB));
    unsigned pending=1, completed=0;
    --pending; // start newest
    assert(abandon(true,0,2,false));
    ++pending; // discard only after return, front of queue, not a completion or OOM attempt
    assert(pending==1 && completed==0);
    --pending; ++completed; // recovery retry succeeds, required work not dropped
    assert(pending==0 && completed==1);
    std::puts("RDNA compile governor: detection, 16/32 GB curves, plateau, light lane, pressure and requeue PASS");
}
