// Bounded Conversations history: the scrollbar addresses message offsets, not an
// ever-growing DOM. At most kShown messages are materialised by llnewuichat.cpp.
#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
namespace slwin
{
struct HistoryThumb { float top = 0.f, height = 0.f; };
inline HistoryThumb historyThumb(size_t total, size_t back, float visible)
{
    if (total < 2 || !std::isfinite(visible) || visible <= 0.f || visible >= double(total)) return {};
    const double count = double(total);
    const double height = std::clamp(100.0 * visible / count, 8.0, 100.0);
    // back=0 always means the newest message, at the bottom of the track.
    const double fraction = 1.0 - double(std::min(back, total - 1)) / double(total - 1);
    return {float(fraction * (100.0 - height)), float(height)};
}
inline size_t historyDragBack(size_t start, float dyUp, float trackHeight, HistoryThumb thumb, size_t total)
{
    const double travel = double(trackHeight) * (1.0 - thumb.height / 100.0);
    if (total < 2 || !std::isfinite(dyUp) || !std::isfinite(travel) || travel <= 0.0) return std::min(start, total ? total - 1 : 0);
    return size_t(std::clamp(std::round(double(start) + double(dyUp) * double(total - 1) / travel), 0.0, double(total - 1)));
}
}
