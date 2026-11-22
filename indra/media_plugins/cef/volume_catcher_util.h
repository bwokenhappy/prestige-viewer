/**
 * @file volume_catcher_util.h
 * @brief Pure helpers shared by the volume catchers: matching an audio stream's process to this
 *        plugin's process tree, and the rate-limited log that reaches the viewer's log.
 *
 * Standard C++ only (no viewer or platform headers), so scripts/check/test_media_volume.cpp
 * builds it on the host. Checked by scripts/check/checks/media_volume.py.
 *
 * $LicenseInfo:firstyear=2026&license=viewerlgpl$
 * $/LicenseInfo$
 */

#ifndef VOLUME_CATCHER_UTIL_H
#define VOLUME_CATCHER_UTIL_H

#include <chrono>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace volume_catcher
{

// Does the process `pid` belong to the process tree rooted at `root` (root itself, a child, a
// grandchild...)? `parent_of(pid)` returns the parent's id, or 0 when unknown.
//
// Chromium plays a page's sound from its audio service, a utility process the browser process
// (SLPlugin) launches -- not from SLPlugin itself -- so a stream's process id is a descendant's,
// never SLPlugin's own (#1610). The walk stops at pid 0 / 1, at a self-parented process and after
// `max_depth` hops, so a stale or cyclic parent table can never loop.
template <class ParentOf>
bool isInProcessTree(uint64_t pid, uint64_t root, ParentOf parent_of, int max_depth = 16)
{
    if (pid == 0 || root == 0)
        return false;
    for (int depth = 0; depth <= max_depth; ++depth)
    {
        if (pid == root)
            return true;
        if (pid <= 1)
            return false;
        const uint64_t parent = parent_of(pid);
        if (parent == pid)
            return false;
        pid = parent;
    }
    return false;
}

// A volume as a whole percent, for "did it change enough to be worth a log line".
inline int volumePercent(float volume)
{
    if (!(volume > 0.f))    // (NaN too)
        return 0;
    if (volume >= 1.f)
        return 100;
    return (int)(volume * 100.f + 0.5f);
}

// An HRESULT / error code as 0x%08x.
inline std::string hexCode(uint32_t code)
{
    static const char digits[] = "0123456789abcdef";
    std::string out = "0x";
    for (int shift = 28; shift >= 0; shift -= 4)
        out += digits[(code >> shift) & 0xf];
    return out;
}

inline uint64_t nowMs()
{
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Thread-safe, rate-limited queue of log lines. The catchers post from any thread (PipeWire calls
// back on its own loop thread); the plugin drains on its main thread and sends each line to the
// viewer, which writes it to Slipstream.log (tag "MediaVolume"). The plugin's own LL_INFOS do not
// reach any log file, hence the round trip.
//
// Each `key` passes at most one line per `interval_ms`; lines held back are counted and the count
// is added to the next line that passes. At most kMaxQueued lines wait; past that the oldest go
// (and are counted), so nothing grows without bound if nobody drains.
class RateLimitedLog
{
public:
    static constexpr size_t kMaxQueued = 64;
    static constexpr size_t kMaxKeys = 256;

    // interval_ms 0: every line passes (still bounded by the queue).
    bool post(const std::string& key, const std::string& line, uint64_t now_ms, uint64_t interval_ms)
    {
        std::lock_guard<std::mutex> lock(mMutex);
        auto it = mKeys.find(key);
        if (it != mKeys.end() && interval_ms > 0 && now_ms - it->second.lastMs < interval_ms)
        {
            ++it->second.suppressed;
            return false;
        }
        if (it == mKeys.end())
        {
            if (mKeys.size() >= kMaxKeys)
                mKeys.clear();      // (keys are a handful of kinds plus one per stream)
            it = mKeys.emplace(key, KeyState()).first;
        }
        std::string text = line;
        if (it->second.suppressed)
            text += " (+" + std::to_string(it->second.suppressed) + " similar)";
        it->second.lastMs = now_ms;
        it->second.suppressed = 0;
        if (mQueue.size() >= kMaxQueued)
        {
            mQueue.pop_front();
            ++mDropped;
        }
        mQueue.push_back(std::move(text));
        return true;
    }

    std::vector<std::string> drain()
    {
        std::lock_guard<std::mutex> lock(mMutex);
        std::vector<std::string> out;
        if (mDropped)
        {
            out.push_back(std::to_string(mDropped) + " volume catcher log line(s) dropped (queue full)");
            mDropped = 0;
        }
        out.insert(out.end(), std::make_move_iterator(mQueue.begin()), std::make_move_iterator(mQueue.end()));
        mQueue.clear();
        return out;
    }

private:
    struct KeyState
    {
        uint64_t lastMs = 0;
        uint64_t suppressed = 0;
    };

    std::mutex mMutex;
    std::map<std::string, KeyState> mKeys;
    std::deque<std::string> mQueue;
    uint64_t mDropped = 0;
};

// The plugin process's one log (one media per SLPlugin process).
inline RateLimitedLog& logQueue()
{
    static RateLimitedLog s_log;
    return s_log;
}

// Rate-limit intervals by kind.
constexpr uint64_t kLogOnce = 24ull * 3600ull * 1000ull;    // backend choice, a stream attached
constexpr uint64_t kLogFailure = 60ull * 1000ull;           // the same failure, at most once a minute
constexpr uint64_t kLogVolume = 30ull * 1000ull;            // volume applied (moves every frame near a TV)

inline void post(const std::string& key, const std::string& line, uint64_t interval_ms)
{
    logQueue().post(key, line, nowMs(), interval_ms);
}

// Volume applied to N streams: logged when the whole percent or the stream count changes, at most
// every kLogVolume. Call it again on pump(): a change held back by the rate limit is logged then.
class AppliedVolumeLog
{
public:
    void applied(float volume, size_t streams, const char* backend)
    {
        const int percent = volumePercent(volume);
        if (percent == mLastPercent && streams == mLastStreams)
            return;
        if (send(percent, streams, backend))
        {
            mLastPercent = percent;
            mLastStreams = streams;
        }
    }

private:
    bool send(int percent, size_t streams, const char* backend)
    {
        return logQueue().post("volume", std::string(backend) + ": volume " + std::to_string(percent) + "% on "
                                        + std::to_string(streams) + " stream(s)",
                          nowMs(), kLogVolume);
    }

    int mLastPercent = -1;
    size_t mLastStreams = (size_t)-1;
};

} // namespace volume_catcher

#endif // VOLUME_CATCHER_UTIL_H
