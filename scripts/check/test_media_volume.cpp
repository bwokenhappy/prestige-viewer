// Tests for the media volume logic (#1610):
//   indra/media_plugins/cef/volume_catcher_util.h -- which audio streams belong to a plugin (its
//     process tree), the rate-limited log the plugin sends to the viewer;
//   indra/newview/llmediavolume.h -- a media's own volume x master Media volume x distance falloff.
// Built and run by scripts/check/checks/media_volume.py.
#include "volume_catcher_util.h"
#include "llmediavolume.h"

#include <cmath>
#include <cstdio>
#include <map>
#include <string>

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

using volume_catcher::isInProcessTree;
using volume_catcher::RateLimitedLog;

namespace {

// A process table: child -> parent.
struct Table
{
    std::map<uint64_t, uint64_t> parent;
    uint64_t operator()(uint64_t pid) const
    {
        auto it = parent.find(pid);
        return it == parent.end() ? 0 : it->second;
    }
};

void testProcessTree()
{
    // init(1) -> viewer(100) -> SLPlugin(200) -> zygote(300) -> renderer(400)
    //                                          -> audio service(210)   (Windows: direct child)
    //                        -> SLPlugin(500) -> audio service(510)    (another media)
    // unrelated: firefox(900) under init
    Table t;
    t.parent = { {100, 1}, {200, 100}, {300, 200}, {400, 300}, {210, 200}, {500, 100}, {510, 500}, {900, 1} };

    CHECK(isInProcessTree(200, 200, t));             // the plugin itself (in-process audio, waveOut era)
    CHECK(isInProcessTree(210, 200, t));             // Chromium's audio service: a child (#1610)
    CHECK(isInProcessTree(400, 200, t));             // two hops (zygote child)
    CHECK(!isInProcessTree(510, 200, t));            // another media's audio service: not ours
    CHECK(isInProcessTree(510, 500, t));             // ... but that media's
    CHECK(!isInProcessTree(100, 200, t));            // our parent (the viewer's own audio) is not ours
    CHECK(!isInProcessTree(900, 200, t));            // unrelated application
    CHECK(!isInProcessTree(0, 200, t));              // no process id (shared / system session)
    CHECK(!isInProcessTree(210, 0, t));
    CHECK(!isInProcessTree(1, 200, t));
    CHECK(!isInProcessTree(777, 200, t));            // unknown process (exited): parent 0

    // A stale table can loop: neither a cycle nor a self-parented entry may hang or match.
    Table cyc;
    cyc.parent = { {10, 11}, {11, 10}, {12, 12} };
    CHECK(!isInProcessTree(10, 200, cyc));
    CHECK(!isInProcessTree(12, 200, cyc));

    // Depth cap: a chain longer than max_depth stops.
    Table deep;
    for (uint64_t i = 1000; i < 1100; ++i)
        deep.parent[i + 1] = i;
    CHECK(isInProcessTree(1010, 1000, deep, 16));
    CHECK(!isInProcessTree(1050, 1000, deep, 16));
    CHECK(isInProcessTree(1050, 1000, deep, 64));
}

void testHelpers()
{
    CHECK(volume_catcher::volumePercent(0.f) == 0);
    CHECK(volume_catcher::volumePercent(-1.f) == 0);
    CHECK(volume_catcher::volumePercent(NAN) == 0);
    CHECK(volume_catcher::volumePercent(0.494f) == 49);
    CHECK(volume_catcher::volumePercent(0.5f) == 50);
    CHECK(volume_catcher::volumePercent(1.f) == 100);
    CHECK(volume_catcher::volumePercent(3.f) == 100);
    CHECK(volume_catcher::hexCode(0x80070005u) == "0x80070005");
    CHECK(volume_catcher::hexCode(0) == "0x00000000");
}

void testRateLimitedLog()
{
    RateLimitedLog log;
    CHECK(log.post("volume", "v 10", 1000, 2000));
    CHECK(!log.post("volume", "v 20", 1500, 2000));     // held back
    CHECK(!log.post("volume", "v 30", 2999, 2000));     // held back
    CHECK(log.post("other", "o", 1600, 2000));          // another key passes
    CHECK(log.post("volume", "v 40", 3000, 2000));      // interval over: passes, with the count
    auto lines = log.drain();
    CHECK(lines.size() == 3);
    if (lines.size() == 3)
    {
        CHECK(lines[0] == "v 10");
        CHECK(lines[1] == "o");
        CHECK(lines[2] == "v 40 (+2 similar)");
    }
    CHECK(log.drain().empty());

    // once-only keys
    CHECK(log.post("attach:7", "a", 0, volume_catcher::kLogOnce));
    CHECK(!log.post("attach:7", "a", 60000, volume_catcher::kLogOnce));
    log.drain();

    // Bounded: nobody draining never grows past kMaxQueued; the overflow is counted.
    RateLimitedLog full;
    const size_t extra = 10;
    for (size_t i = 0; i < RateLimitedLog::kMaxQueued + extra; ++i)
        full.post("k" + std::to_string(i % 8), "line " + std::to_string(i), i, 0);
    lines = full.drain();
    CHECK(lines.size() == RateLimitedLog::kMaxQueued + 1);
    if (!lines.empty())
        CHECK(lines[0] == std::to_string(extra) + " volume catcher log line(s) dropped (queue full)");
    if (lines.size() > 1)
        CHECK(lines[1] == "line " + std::to_string(extra));

    // Bounded keys too (one per stream over a long session).
    RateLimitedLog keys;
    for (size_t i = 0; i < 3 * RateLimitedLog::kMaxKeys; ++i)
    {
        keys.post("attach:" + std::to_string(i), "a", i, volume_catcher::kLogOnce);
        if (i % 32 == 0)
            keys.drain();
    }
    CHECK(keys.drain().size() <= RateLimitedLog::kMaxQueued + 1);
}

LLMediaVolumeInputs inputs(float requested, float master, double proximity)
{
    LLMediaVolumeInputs in;
    in.requested = requested;
    in.master = master;
    in.proximity = proximity;
    in.rolloffMin = 40.f;   // settings.xml defaults
    in.rolloffMax = 80.f;
    in.rolloffRate = 0.02f;
    return in;
}

bool approx(float a, float b) { return std::fabs(a - b) < 1e-5f; }

void testEffectiveVolume()
{
    // The master Media volume multiplies in; master mute (0) silences every media.
    CHECK(approx(llmedia_effective_volume(inputs(1.f, 1.f, 0)), 1.f));
    CHECK(approx(llmedia_effective_volume(inputs(0.5f, 0.5f, 0)), 0.25f));
    CHECK(approx(llmedia_effective_volume(inputs(1.f, 0.f, 0)), 0.f));
    // The media's own mute.
    CHECK(approx(llmedia_effective_volume(inputs(0.f, 1.f, 10)), 0.f));
    // Inside MediaRollOffMin: unchanged.
    CHECK(approx(llmedia_effective_volume(inputs(0.8f, 1.f, 39.9)), 0.8f));
    CHECK(approx(llmedia_effective_volume(inputs(0.8f, 1.f, 40.0)), 0.8f));
    // Beyond MediaRollOffMax: silent.
    CHECK(approx(llmedia_effective_volume(inputs(1.f, 1.f, 80.01)), 0.f));
    // Between: 1 / (1 + rate (d - min))^2, falling with distance, never louder.
    const float at60 = llmedia_effective_volume(inputs(1.f, 1.f, 60));
    CHECK(approx(at60, 1.f / (1.4f * 1.4f)));
    float previous = 1.f;
    for (double d = 40; d <= 80; d += 0.5)
    {
        const float v = llmedia_effective_volume(inputs(1.f, 1.f, d));
        CHECK(v <= previous + 1e-6f);
        CHECK(v <= 1.f);
        previous = v;
    }
    // The falloff scales the combined volume.
    CHECK(approx(llmedia_effective_volume(inputs(0.5f, 0.5f, 60)), 0.25f * at60));
    // A negative rate cannot make it louder.
    LLMediaVolumeInputs odd = inputs(0.5f, 1.f, 60);
    odd.rolloffRate = -0.02f;
    CHECK(approx(llmedia_effective_volume(odd), 0.5f));
    // Only another media is audible: silent.
    LLMediaVolumeInputs muted = inputs(1.f, 1.f, 0);
    muted.audible = false;
    CHECK(approx(llmedia_effective_volume(muted), 0.f));
}

} // namespace

int main()
{
    testProcessTree();
    testHelpers();
    testRateLimitedLog();
    testEffectiveVolume();
    if (failures)
    {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
