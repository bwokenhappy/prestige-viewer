#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Queue-local elapsed intervals only. Complement is unscoped queue time: it may
// contain dependency waits, scheduling gaps or commands not yet instrumented.
// Timestamp intervals cannot distinguish internal shader stalls from execution.
namespace VKGpuScopeMath
{
struct Pair { uint32_t begin; uint32_t end; };
inline bool reserve(uint32_t& count, uint32_t capacity, uint32_t views, Pair& pair)
{
    if (views == 0u || views > 4u || count > capacity || 2u * views > capacity - count) return false;
    pair = {count, count + views};
    count += 2u * views;
    return true;
}
inline double elapsed(uint64_t beginTick, uint64_t endTick, uint64_t mask, double scale)
{ return static_cast<double>((endTick - beginTick) & mask) * scale; }
struct Sample { const char* name; double ms; double worst = 0.0; bool async = false; bool gap = false; };
inline bool add(Sample* samples, uint32_t& count, uint32_t capacity,
                const char* name, double ms, bool async, bool gap)
{
    if (!name || !(ms >= 0.0 && ms < 1000.0)) return false;
    for (uint32_t j = 0; j < count; ++j)
        if (samples[j].async == async && samples[j].gap == gap && !std::strcmp(samples[j].name, name))
        { samples[j].ms += ms; return true; }
    if (count >= capacity) return false;
    samples[count++] = {name, ms, 0.0, async, gap};
    return true;
}
struct Interval { double begin; double end; const char* name; };
template<class Gap>
double gaps(Interval* spans, std::size_t count, double envelope, Gap emit)
{
    std::size_t validCount = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (spans[i].begin >= 0.0 && spans[i].end >= spans[i].begin && spans[i].end <= envelope)
            spans[validCount++] = spans[i];
    count = validCount;
    const auto earlier = [](const Interval& a, const Interval& b)
    { return a.begin < b.begin || (a.begin == b.begin && a.end < b.end); };
    std::make_heap(spans, spans + count, earlier);
    std::sort_heap(spans, spans + count, earlier);
    double cursor = 0.0, total = 0.0;
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto& span = spans[i];
        if (!(span.begin >= 0.0 && span.end >= span.begin && span.end <= envelope)) continue;
        if (span.begin > cursor) { const double d = span.begin - cursor; total += d; emit(span.name, d); }
        cursor = std::max(cursor, span.end);
    }
    if (envelope > cursor) { const double d = envelope - cursor; total += d; emit("frame_end", d); }
    return total;
}
}
