/** RDNA1/2 maintenance-driver compile admission. Pure policy, also used by the CPU fixture. */
#pragma once
#include <algorithm>
#include <cstdint>
#include <string_view>

namespace vkcompilegovernor
{
#if defined(_WIN32)
constexpr bool kWindows = true;
#else
constexpr bool kWindows = false;
#endif
constexpr uint64_t kHeavyBytes = 8ull << 30;
constexpr uint32_t kHeavyWords = 131072;
constexpr uint64_t kGrowthStep = 16ull << 20;
constexpr int64_t kPlateauMs = 2000;

// AMD's Vulkan driverVersion is NOT the Windows package version. Prefer an explicit
// package string, otherwise the PCI architecture rule: Windows Navi1x/Navi2x only.
// This deliberately does not penalize RDNA3 (Navi3x) or RADV on Linux.
inline const char* rule(uint32_t vendor, uint32_t device, bool windows, std::string_view info, bool force)
{
    if (force) return "debug force";
    if (!windows || vendor != 0x1002) return nullptr;
    if (info.find("32.0.21045.") != std::string_view::npos) return "Windows AMD 32.0.21045.x";
    if ((device >= 0x7310 && device <= 0x731f) || (device >= 0x7340 && device <= 0x734f)
        || (device >= 0x7360 && device <= 0x736f) || (device >= 0x73a0 && device <= 0x73ff)
        || (device >= 0x7420 && device <= 0x743f)) return "Windows RDNA1/2 PCI architecture";
    return nullptr;
}
inline bool heavy(uint32_t words, std::string_view name)
{
    return !words || words >= kHeavyWords || name.find("lit (") != std::string_view::npos
        || name.find("mirror") != std::string_view::npos || name.find("Mirror") != std::string_view::npos
        || name.find("forward") != std::string_view::npos || name.find("Forward") != std::string_view::npos;
}
inline uint64_t estimate(uint64_t estimated, uint64_t familyPeak)
{
    return std::max(kHeavyBytes, std::max(estimated, familyPeak));
}
struct Curve
{
    uint64_t high = 0;
    int64_t lastGrowth = 0;
    void start(uint64_t bytes, int64_t now) { high = bytes; lastGrowth = now; }
    void sample(uint64_t bytes, int64_t now)
    {
        // Cumulative growth, not per-sample delta: small steady allocations must not
        // look like a plateau. Track drops so regrowth below an old peak counts too.
        if (bytes < high) high = bytes;
        if (bytes > high && bytes - high >= kGrowthStep) { high = bytes; lastGrowth = now; }
    }
    bool plateau(int64_t now) const { return high != 0 && now - lastGrowth >= kPlateauMs; }
};
inline bool fits(uint64_t estimateBytes, uint64_t owed, uint64_t budget)
{
    return budget == UINT64_MAX || (owed <= budget && estimateBytes <= budget - owed);
}
inline bool abandon(bool pressure, uint64_t headroom, unsigned live, bool alreadyAbandoned)
{
    return live > 1 && !alreadyAbandoned && (pressure || headroom <= (512ull << 20));
}
// Range contains stable Running-like pointers; no allocation, scheduler mutex held.
template<class Range>
inline typename Range::value_type newestToAbandon(const Range& running, bool pressure, uint64_t headroom)
{
    typename Range::value_type newest = nullptr;
    bool alreadyAbandoned = false;
    unsigned live = 0;
    for (auto* r : running)
    {
        alreadyAbandoned |= r->abandon;
        if (!r->abandon)
        {
            ++live;
            if (!newest || r->serial > newest->serial) newest = r;
        }
    }
    return abandon(pressure, headroom, live, alreadyAbandoned) ? newest : nullptr;
}

}
