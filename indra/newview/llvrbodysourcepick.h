/**
 * @file llvrbodysourcepick.h
 * @brief VR body tracking: which body source Auto uses (pure, no viewer types; LLVRBodySource and
 *        scripts/check/test_vr_source.cpp).
 *
 * Auto's order (SlipstreamVRBodySource "Auto"):
 *   1. SlimeVR while its packets arrive (VMC skeleton, else its OSC trackers). SlimeVR's SteamVR
 *      driver also lists the same trackers to the runtime (OpenXR trackers), so a SlimeVR user
 *      always has both; the direct stream is the richer one (a whole skeleton over VMC) and does
 *      not depend on SteamVR's tracker roles. (A tester: "SlimeVR tracking is nonfunctional;
 *      it's defaulting to the steam tracker system" -- Auto took the OpenXR trackers first.)
 *   2. OpenXR trackers the runtime lists (Vive / Tundra trackers; SlimeVR without its stream).
 *   3. The headset's own body tracking.
 *   4. The 3-point estimate (headset + controllers), while SlipstreamVRBodyEstimate is on.
 * An explicitly chosen source is used alone.
 */
#pragma once

namespace slsrc
{
enum Pick { P_NONE = 0, P_OPENXR, P_SLIMEVR, P_HEADSET, P_ESTIMATE };
struct Offer
{
    bool slime = false;       // SlimeVR packets arrive
    bool trackers = false;    // the runtime lists trackers
    bool headset = false;     // the headset tracks a body
    bool estimate = false;    // the estimate may run (setting on, head tracked)
};
inline Pick pickAuto(const Offer& o)
{
    if (o.slime) return P_SLIMEVR;
    if (o.trackers) return P_OPENXR;
    if (o.headset) return P_HEADSET;
    if (o.estimate) return P_ESTIMATE;
    return P_NONE;
}
} // namespace slsrc
