#include "vkgfx/vk_gpu_scope_math.h"
#include <cassert>
#include <cmath>
#include <cstdint>
#include <limits>
using namespace VKGpuScopeMath;
int main()
{
    uint32_t count = 0; Pair pair{};
    assert(reserve(count, 12, 2, pair) && pair.begin == 0 && pair.end == 2 && count == 4);
    assert(reserve(count, 12, 4, pair) && pair.begin == 4 && pair.end == 8 && count == 12);
    assert(!reserve(count, 12, 1, pair) && count == 12); // atomic refusal
    count = std::numeric_limits<uint32_t>::max();
    assert(!reserve(count, 12, 1, pair));
    count = 0; assert(!reserve(count, 12, 0, pair) && count == 0);
    assert(elapsed(250, 3, 255, 1.0) == 9.0); // narrow family clock wrap
    assert(elapsed(UINT64_MAX - 3, 2, UINT64_MAX, 1.0) == 6.0);
    Sample samples[4]{}; uint32_t sampleCount = 0;
    assert(add(samples, sampleCount, 4, "pass", 1, false, false));
    assert(add(samples, sampleCount, 4, "pass", 2, false, false));
    assert(sampleCount == 1 && samples[0].ms == 3); // frame sum, not individual-dispatch worst
    assert(add(samples, sampleCount, 4, "pass", 4, true, false));
    assert(add(samples, sampleCount, 4, "pass", 5, false, true));
    assert(sampleCount == 3 && samples[0].ms == 3 && samples[1].ms == 4 && samples[2].ms == 5);
    assert(!add(samples, sampleCount, 4, "bad", std::numeric_limits<double>::quiet_NaN(), false, false));
    // Empty/cancelled pass table never invents pass cost; all elapsed time is unscoped.
    Interval spans[8]{}; double emitted = 0;
    auto emit = [&](const char*, double d) { emitted += d; };
    assert(gaps(spans, 0, 10, emit) == 10 && emitted == 10);
    // Recording order differs from execution endpoint order for nested reserved pairs.
    spans[0] = {4, 6, "b"}; spans[1] = {1, 3, "a"}; spans[2] = {1.5, 2, "child"};
    emitted = 0; assert(gaps(spans, 3, 8, emit) == 4 && emitted == 4);
    // Adjacent passes no gap; overlapping intervals only excluded once from complement.
    spans[0] = {0, 4, "a"}; spans[1] = {3, 5, "b"}; spans[2] = {5, 8, "c"};
    assert(gaps(spans, 3, 8, [](const char*, double) { assert(false); }) == 0);
    // Queue clocks are deliberately reduced independently, never cross-subtracted.
    Interval compute[] = {{1, 3, "async"}};
    assert(gaps(compute, 1, 4, [](const char*, double) {}) == 2);
    // The front end and later async segments share a compute clock; the
    // inter-submit interval is part of that queue's gap, never graphics wait.
    Interval computeFrame[] = {{0, 2, "frontend"}, {5, 7, "later"}};
    assert(gaps(computeFrame, 2, 8, [](const char*, double) {}) == 4);
    // Invalid/missing intervals cannot subtract time from the measured envelope.
    spans[0] = {3, 2, "bad"}; spans[1] = {0, 100, "outside"};
    assert(gaps(spans, 2, 8, [](const char*, double) {}) == 8);
}
