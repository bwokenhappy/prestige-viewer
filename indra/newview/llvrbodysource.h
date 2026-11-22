/**
 * @file llvrbodysource.h
 * @brief VR full-body tracking (#1565): where the body poses come from -- the one seam between
 *        the body sources and LLVRBody (calibration, smoothing, loss fade, IK, the body layer).
 *
 * Each VR frame LLVRBody calls LLVRBodySource::poll(hp, dt, frame) once: it picks the source
 * (SlipstreamVRBodySource), keeps the others off (a SlimeVR socket closed, a headset body tracker
 * released) and fills one LLVRBodyFrame from the chosen one.
 *
 * The frame: per-role poses in the ROOM (OpenXR app space, OpenXR axes, metres: the space
 * LLXrSession locates everything in), indexed by LLXrTrackerRole (waist, chest, feet, knees, ...),
 * each with active (the source has this role), valid (a pose this frame) and tracked (not inferred
 * / generated); connected (roles / devices it has); generation (bumps when the role set changes:
 * LLVRBody re-logs it and retries the saved calibration -- poll() folds the kind in, so two
 * sources' generations never compare equal); a per-role confidence 0..1 (below 1 LLVRBody smooths
 * up to 4x more: generated / unsure joints).
 *
 * bonePoses: the poses ARE the bones (position and an SL-convention rotation: front +X, up +Z,
 * left +Y once in body space) -- no tracker-to-bone offset, so no T-pose: LLVRBody maps such a
 * source with identity offsets at the measured eye height, and the wrist "Body" button only
 * measures your eye height. Otherwise (worn trackers) the T-pose calibration finds each offset,
 * after calibrate() lets the source align its own space (SlimeVR) on the same frame.
 *
 * skeleton: a whole solved skeleton (SlimeVR's VMC output): skelHave / skelRot give each Unity
 * humanoid bone's global rotation in the room (OpenXR app-space axes, aligned to the play space;
 * identity = standing in a T-pose facing -Z). LLVRBody then drives the SL joints from it directly
 * (llvrbodyvmc.h: no offsets, no IK) and places the body under the headset with the avatar's own
 * bone lengths. Such a frame is also bonePoses (Body measures the eye height) and lists the body
 * roles as active, never valid (the Body button).
 *
 * Sources (SlipstreamVRBodySource, a string, case-insensitive; unknown values act as Auto):
 *   "Auto"     SlimeVR while its packets arrive (its SteamVR driver lists the same trackers to the
 *              runtime: the direct stream wins), else OpenXR trackers while the runtime lists any,
 *              else the headset's body tracking where the runtime offers it (Auto back-off: the
 *              headset tracker is released after 10 s without a body, re-checked every 30 s),
 *              else the estimate (llvrbodysourcepick.h).
 *   "OpenXR"   XR_HTCX_vive_tracker_interaction (LLXrSession): SteamVR Vive / Tundra trackers.
 *   "SlimeVR"  SlimeVR server's VRChat OSC trackers over localhost UDP (#1582, llvrbodyslime.h).
 *   "Headset"  the headset's own body tracking (#1583, llvrbodyheadset.h: XR_FB_body_tracking /
 *              XR_META_body_tracking_full_body, XR_HTC_body_tracking, XR_BD_body_tracking).
 *   "Estimate" #1586: hips and feet estimated from the headset and the controllers
 *              (llvrbodyestimate.h). Auto uses it last, when no other source has a body, while
 *              SlipstreamVRBodyEstimate is on.
 *
 * Cost: everything here runs only from LLVRBody::update (VR frames); the desktop never calls in.
 * The SlimeVR socket exists only in VR with "SlimeVR", or "Auto" while the runtime lists no
 * trackers (one non-blocking read every 0.5 s until a stream starts). The headset tracker exists
 * only with "Headset", or "Auto" while neither trackers nor SlimeVR give a body and the runtime
 * offers body tracking.
 */
#pragma once

#include "llvrbodyvmc.h"
#include "stdtypes.h"
#include "vkgfx/llxrsession.h"

namespace LLVRBodySource
{
enum Kind : U32
{
    NONE = 0,
    OPENXR,
    SLIMEVR,
    HEADSET,
    ESTIMATE,   // #1586: the 3-point estimate (headset + controllers)
    KIND_COUNT
};
// Room for this many kinds in the generation fold (poll()).
constexpr U32 KIND_SLOTS = 16;
static_assert(KIND_COUNT <= KIND_SLOTS, "LLVRBodySource: raise KIND_SLOTS");
const char* kindName(Kind k);
// A word for the wrist panel's Body button ("SlimeVR", "Trackers", "Headset", "3-point").
const char* shortName(Kind k);
} // namespace LLVRBodySource

struct LLVRBodyFrame
{
    LLVRBodySource::Kind kind = LLVRBodySource::NONE;   // the source that filled it
    bool bonePoses = false;            // see the file comment
    LLXrTrackers poses;
    F32 confidence[LLXR_TRK_COUNT];    // 0..1 per role; below 1 smooths more (generated / unsure)
    bool skeleton = false;             // see the file comment
    bool skelHave[slvmc::U_COUNT] = {};
    slfbt::Q skelRot[slvmc::U_COUNT];
    LLVRBodyFrame()
    {
        for (F32& c : confidence) c = 1.f;
    }
};

namespace LLVRBodySource
{
// Every VR frame (LLVRBody::update; dt seconds): pick the source, keep the others off, fill
// `out` from the chosen one. False: no source has a body this frame (`out` is then empty).
bool poll(const LLXrHeadPose& hp, F32 dt, LLVRBodyFrame& out);
// The T-pose moment, before LLVRBody calibrates a worn-tracker source: the chosen source aligns
// its space (SlimeVR) and refreshes `out` with the aligned poses. False: it cannot now (logged).
bool calibrate(const LLXrHeadPose& hp, LLVRBodyFrame& out);
// The source the last poll() chose.
Kind active();
// #1586: your standing eye height and the floor's app-space y (LLVRBody's calibration, else the
// saved / set eye height), for the estimate; set before each poll().
void setBodyScale(F32 eyeH, F32 floorY);
// #1584: the avatar's facing in the room (radians, body space), the one body heading in VR: the
// estimate's hips face it. valid = false: none this frame (the estimate keeps its own).
void setBodyYaw(bool valid, F32 roomYaw);
// The VR session stopped: close whatever the sources hold (socket, body tracker).
void stop();
} // namespace LLVRBodySource
