/**
 * @file llvrbodysource.cpp
 * @brief VR full-body tracking: the body source selection (#1565 OpenXR trackers, #1582 SlimeVR,
 *        #1583 headset body tracking). See llvrbodysource.h.
 */
#include "llviewerprecompiledheaders.h"

#include "llvrbodysource.h"

#include "llviewercontrol.h"
#include "llvrbodyestimate.h"
#include "llvrbodyheadset.h"
#include "llvrbodyslime.h"
#include "llvrbodysourcepick.h"

#include <cctype>
#include <string>

namespace
{
enum class Mode { AUTO, OPENXR, SLIMEVR, HEADSET, ESTIMATE };

LLVRBodySource::Kind sActive = LLVRBodySource::NONE;
LLVRBodySource::Kind sLogged = LLVRBodySource::NONE;   // the last source logged (not NONE)
bool sModeRead = false;
Mode sMode = Mode::AUTO;
std::string sModeText;
F32 sEyeH = 1.6f, sFloorY = 0.f;   // #1586 (setBodyScale)
bool sHaveBodyYaw = false;         // #1584 (setBodyYaw)
F32 sBodyYaw = 0.f;

bool sameText(const std::string& a, const char* b)
{
    size_t i = 0;
    for (; i < a.size() && b[i]; ++i)
        if (std::tolower((unsigned char)a[i]) != std::tolower((unsigned char)b[i])) return false;
    return i == a.size() && !b[i];
}

Mode mode()
{
    static LLCachedControl<std::string> s_source(gSavedSettings, "SlipstreamVRBodySource", "Auto");
    const std::string& v = s_source();
    if (sModeRead && v == sModeText) return sMode;
    sModeText = v;
    if (sameText(v, "OpenXR")) sMode = Mode::OPENXR;
    else if (sameText(v, "SlimeVR")) sMode = Mode::SLIMEVR;
    else if (sameText(v, "Headset")) sMode = Mode::HEADSET;
    else if (sameText(v, "Estimate")) sMode = Mode::ESTIMATE;
    else
    {
        sMode = Mode::AUTO;
        if (!v.empty() && !sameText(v, "Auto"))
            LL_WARNS("VRBody") << "SlipstreamVRBodySource \"" << v << "\" is not Auto / OpenXR / SlimeVR / Headset / Estimate: using Auto"
                               << LL_ENDL;
    }
    sModeRead = true;
    return sMode;
}
} // namespace

const char* LLVRBodySource::shortName(Kind k)
{
    switch (k)
    {
    case OPENXR:   return "Trackers";
    case SLIMEVR:  return "SlimeVR";
    case HEADSET:  return "Headset";
    case ESTIMATE: return "3-point";
    default:       return "";
    }
}

const char* LLVRBodySource::kindName(Kind k)
{
    switch (k)
    {
    case OPENXR:  return "OpenXR trackers";
    case SLIMEVR: return "SlimeVR OSC";
    case HEADSET: return "headset body tracking";
    case ESTIMATE: return "3-point estimate";
    default:      return "none";
    }
}

bool LLVRBodySource::poll(const LLXrHeadPose& hp, F32 dt, LLVRBodyFrame& out)
{
    static LLCachedControl<U32> s_port(gSavedSettings, "SlipstreamVRBodySlimePort", 9000);
    static LLCachedControl<U32> s_vmc_port(gSavedSettings, "SlipstreamVRBodySlimeVMCPort", 39539);
    static LLCachedControl<bool> s_on(gSavedSettings, "SlipstreamVRBodyTracking", true);
    LLXrSession& xr = LLXrSession::instance();
    LLVRBodySlime& slime = LLVRBodySlime::instance();
    const Mode m = mode();
    const bool autoMode = m == Mode::AUTO;
    out = LLVRBodyFrame{};

    // 1. SlimeVR first in Auto (llvrbodysourcepick.h): its socket listens in Auto and SlimeVR
    // modes (one non-blocking read every 0.5 s until a stream starts), also while the runtime
    // lists trackers -- SlimeVR's SteamVR driver lists the very same trackers there.
    const bool slimeWanted = m == Mode::SLIMEVR || autoMode;
    if (slimeWanted) slime.pump(s_port, s_vmc_port);
    else if (slime.listening()) slime.stop();
    const bool slimeOn = slimeWanted && slime.streaming();
    // 2. OpenXR trackers (one test without them), unless SlimeVR streams (Auto).
    const bool listed = (autoMode || m == Mode::OPENXR) && xr.trackersListed();
    slsrc::Offer offer;
    offer.slime = slimeOn;
    offer.trackers = listed;
    const bool trackers = listed && (!autoMode || slsrc::pickAuto(offer) == slsrc::P_OPENXR);
    // 3. The headset's body tracking (poll(false) releases its tracker; Auto backs off).
    const bool headsetWanted = s_on && !trackers && !slimeOn &&
                               (m == Mode::HEADSET || (autoMode && LLVRBodyHeadset::offered()));
    const bool headset = LLVRBodyHeadset::poll(headsetWanted, autoMode, dt, out);
    // 4. #1586: the estimate from the headset and the controllers, when nothing else has a body.
    static LLCachedControl<bool> s_estimate(gSavedSettings, "SlipstreamVRBodyEstimate", true);
    const bool estimateWanted = s_on && !trackers && !slimeOn && !headset && (m == Mode::ESTIMATE || (autoMode && s_estimate));
    LLVRBodyFrame est;
    const bool estimate = LLVRBodyEstimate::poll(estimateWanted, hp, dt, sEyeH, sFloorY, sHaveBodyYaw, sBodyYaw, est);

    Kind k = NONE;
    bool have = false;
    if (trackers)
    {
        out = LLVRBodyFrame{};
        have = xr.trackerState(out.poses);
        k = OPENXR;
    }
    else if (slimeOn)
    {
        have = slime.poll(hp, out);
        k = SLIMEVR;
    }
    else if (headset)
    {
        have = true;
        k = HEADSET;
    }
    else if (estimate)
    {
        out = est;
        have = true;
        k = ESTIMATE;
    }
    if (!have) out = LLVRBodyFrame{};
    out.kind = have ? k : NONE;
    // Generations of different sources never compare equal (a switch re-logs the list).
    out.poses.generation = out.poses.generation * KIND_SLOTS + (U32)out.kind;
    sActive = out.kind;
    if (sActive != NONE && sActive != sLogged)
    {
        // Why this one (Auto): the order is SlimeVR, OpenXR trackers, headset, estimate.
        std::string why;
        if (autoMode && sActive == SLIMEVR && listed)
            why = "; preferred over the trackers the runtime lists (SlimeVR's SteamVR driver shows the same trackers there)";
        else if (autoMode && sActive == OPENXR)
            why = "; no SlimeVR stream on 127.0.0.1 (VMC / OSC ports in SlipstreamVRBodySlimeVMCPort / SlipstreamVRBodySlimePort)";
        LL_INFOS("VRBody") << "body source: " << kindName(sActive) << " (SlipstreamVRBodySource " << sModeText << why << ")"
                           << LL_ENDL;
        sLogged = sActive;
    }
    return have;
}

bool LLVRBodySource::calibrate(const LLXrHeadPose& hp, LLVRBodyFrame& out)
{
    if (sActive != SLIMEVR) return sActive != NONE;
    LLVRBodySlime& slime = LLVRBodySlime::instance();
    if (!slime.calibrate(hp)) return false;
    const U32 gen = out.poses.generation;
    const LLVRBodySource::Kind kind = out.kind;
    if (!slime.poll(hp, out)) return false;
    out.poses.generation = gen;   // (the same role set: no re-log)
    out.kind = kind;
    return true;
}

LLVRBodySource::Kind LLVRBodySource::active()
{
    return sActive;
}

void LLVRBodySource::setBodyScale(F32 eyeH, F32 floorY)
{
    sEyeH = eyeH;
    sFloorY = floorY;
}

void LLVRBodySource::setBodyYaw(bool valid, F32 roomYaw)
{
    sHaveBodyYaw = valid;
    sBodyYaw = roomYaw;
}

void LLVRBodySource::stop()
{
    LLVRBodySlime& slime = LLVRBodySlime::instance();
    if (slime.listening()) slime.stop();
    LLVRBodyFrame unused;
    LLVRBodyHeadset::poll(false, false, 0.f, unused);   // (no-op without a tracker)
    LLXrHeadPose none;
    LLVRBodyEstimate::poll(false, none, 0.f, 1.6f, 0.f, false, 0.f, unused);   // #1586 (forgets its state)
    sActive = NONE;
}
