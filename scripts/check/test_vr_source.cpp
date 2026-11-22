// Fixtures for Auto's body source order (indra/newview/llvrbodysourcepick.h), run by
// scripts/check/checks/vr_source.py (host compiler only).
//   SlimeVR streaming wins over the trackers the runtime lists (its SteamVR driver lists the same
//   trackers: a tester's SlimeVR "defaulted to the steam tracker system"); then OpenXR trackers,
//   the headset's body tracking, the estimate.
#include "llvrbodysourcepick.h"

#include <cstdio>

using namespace slsrc;

static int g_fail = 0;
#define CHECK(cond, ...) do { if (!(cond)) { std::printf("FAIL %s:%d: ", __FILE__, __LINE__); std::printf(__VA_ARGS__); std::printf("\n"); ++g_fail; } } while (0)

static Offer offer(bool slime, bool trackers, bool headset, bool estimate)
{
    Offer o;
    o.slime = slime;
    o.trackers = trackers;
    o.headset = headset;
    o.estimate = estimate;
    return o;
}

int main()
{
    CHECK(pickAuto(offer(true, true, true, true)) == P_SLIMEVR, "SlimeVR streaming + its trackers listed: SlimeVR");
    CHECK(pickAuto(offer(false, true, true, true)) == P_OPENXR, "no stream: the runtime's trackers");
    CHECK(pickAuto(offer(false, false, true, true)) == P_HEADSET, "then headset body tracking");
    CHECK(pickAuto(offer(false, false, false, true)) == P_ESTIMATE, "then the estimate");
    CHECK(pickAuto(offer(false, false, false, false)) == P_NONE, "nothing");
    CHECK(pickAuto(offer(true, false, false, false)) == P_SLIMEVR, "SlimeVR alone");
    if (g_fail)
    {
        std::printf("%d failure(s)\n", g_fail);
        return 1;
    }
    std::printf("ok\n");
    return 0;
}
